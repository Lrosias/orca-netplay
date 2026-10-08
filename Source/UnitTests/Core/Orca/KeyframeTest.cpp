// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <zstd.h>

#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Core/Orca/Session/Keyframe.h"

using namespace Orca::Net;

namespace
{
ReplayArchive Replay(size_t count)
{
  ReplayArchive replay;
  replay.origin_hash = 0x123456789abcdef0;
  replay.target_hash = 0xfedcba9876543210;
  auto ports = std::make_shared<const std::vector<Orca::Events::PortInfo>>(
      std::vector<Orca::Events::PortInfo>{{0, "player", false, {1, 2, 3}, {4, 5}},
                                        {2, "friend", true, {6, 7}, {8}}});
  std::mt19937 rng(1);
  replay.frames.resize(count);
  for (auto& frame : replay.frames)
  {
    for (auto& pad : frame.pads)
      for (auto& byte : pad)
        byte = static_cast<u8>(rng());
    frame.ports = ports;
  }
  replay.boundary.ports = ports;
  replay.boundary.header = ReplayHeader{2, 1, 1, 2, 0x11223344};
  replay.boundary.clear_ready = true;
  return replay;
}

std::vector<u8> Raw(const std::vector<u8>& blob)
{
  std::vector<u8> raw(ZSTD_getFrameContentSize(blob.data(), blob.size()));
  EXPECT_EQ(ZSTD_decompress(raw.data(), raw.size(), blob.data(), blob.size()), raw.size());
  return raw;
}

std::vector<u8> Compressed(const std::vector<u8>& raw)
{
  std::vector<u8> blob(ZSTD_compressBound(raw.size()));
  const auto size = ZSTD_compress(blob.data(), blob.size(), raw.data(), raw.size(), 1);
  EXPECT_FALSE(ZSTD_isError(size));
  blob.resize(size);
  return blob;
}

void U32(std::vector<u8>& raw, size_t offset, u32 value)
{
  std::memcpy(raw.data() + offset, &value, sizeof(value));
}
}  // namespace

TEST(OrcaKeyframe, ReplayRoundTripsInputsAndTypedEvents)
{
  auto original = Replay(4321);
  original.frames[42].header = ReplayHeader{1, 2, 0, 3, 55};
  original.frames[99].clear_ready = true;
  const auto blob = PackKeyframe(4321, original);
  ASSERT_FALSE(blob.empty());
  int frame = -1;
  ReplayArchive decoded;
  ASSERT_TRUE(UnpackKeyframe(blob, &frame, &decoded));
  EXPECT_EQ(frame, 4321);
  EXPECT_EQ(decoded.origin_hash, original.origin_hash);
  EXPECT_EQ(decoded.target_hash, original.target_hash);
  ASSERT_EQ(decoded.frames.size(), original.frames.size());
  for (size_t i = 0; i < decoded.frames.size(); ++i)
  {
    EXPECT_EQ(decoded.frames[i].pads, original.frames[i].pads);
    EXPECT_EQ(decoded.frames[i].header, original.frames[i].header);
    EXPECT_EQ(decoded.frames[i].clear_ready, original.frames[i].clear_ready);
    EXPECT_EQ(decoded.frames[i].ports->at(0).controls, original.frames[i].ports->at(0).controls);
    EXPECT_EQ(decoded.frames[i].ports->at(1).queue, original.frames[i].ports->at(1).queue);
  }
  EXPECT_EQ(decoded.frames[0].ports, decoded.frames[1].ports);
  EXPECT_EQ(decoded.boundary.header, original.boundary.header);
  EXPECT_TRUE(decoded.boundary.clear_ready);
}

TEST(OrcaKeyframe, RejectsLegacyStatesTruncationAndTrailingData)
{
  const auto blob = PackKeyframe(2, Replay(2));
  auto raw = Raw(blob);
  int frame = 77;
  ReplayArchive decoded;
  decoded.origin_hash = 99;
  for (size_t n = 0; n < raw.size(); ++n)
  {
    const std::vector<u8> truncated(raw.begin(), raw.begin() + n);
    EXPECT_FALSE(UnpackKeyframe(Compressed(truncated), &frame, &decoded));
  }
  EXPECT_EQ(frame, 77);
  EXPECT_EQ(decoded.origin_hash, 99);
  std::memcpy(raw.data(), "OKF2", 4);
  EXPECT_FALSE(UnpackKeyframe(Compressed(raw), &frame, &decoded));
  raw = Raw(blob);
  raw.push_back(0);
  EXPECT_FALSE(UnpackKeyframe(Compressed(raw), &frame, &decoded));
}

TEST(OrcaKeyframe, BoundsCountsSeatsAndPayloadLengthsBeforeAllocation)
{
  const auto blob = PackKeyframe(1, Replay(1));
  const auto original = Raw(blob);
  int frame;
  ReplayArchive decoded;
  // Archive count, port count, first seat, name length, controls length, second seat.
  for (const auto [offset, value] : std::vector<std::pair<size_t, u32>>{
           {4, 0xffffffff}, {56, 0xffffffff}, {60, 4}, {64, 129}, {74, 65}, {95, 0}})
  {
    auto raw = original;
    U32(raw, offset, value);
    EXPECT_FALSE(UnpackKeyframe(Compressed(raw), &frame, &decoded)) << offset;
  }
}

TEST(OrcaKeyframe, BoundsDecompressionAndTypedHeaderValues)
{
  int frame;
  ReplayArchive decoded;
  std::vector<u8> oversized((64 << 20) + 1, 0);
  EXPECT_FALSE(UnpackKeyframe(Compressed(oversized), &frame, &decoded));
  auto replay = Replay(0);
  replay.boundary.ports = std::make_shared<const std::vector<Orca::Events::PortInfo>>();
  const auto original = Raw(PackKeyframe(0, replay));
  // Header presence, mode, ruleset, coin, flags, and clear-ready presence.
  for (const auto [offset, value] : std::vector<std::pair<size_t, u32>>{
           {60, 2}, {64, 3}, {68, 3}, {72, 2}, {76, 4}, {84, 2}})
  {
    auto raw = original;
    U32(raw, offset, value);
    EXPECT_FALSE(UnpackKeyframe(Compressed(raw), &frame, &decoded)) << offset;
  }
}

TEST(OrcaKeyframe, ReplayRecordingRetainsWinningInputsAndEventsAcrossRollback)
{
  ReplayArchive archive;
  ReplayRecordingScope recording(&archive);
  auto* frame = ReplayRecordingScope::Frame(0);
  frame->header = ReplayHeader{1, 1, 0, 0, 7};
  frame->clear_ready = true;
  Pads predicted{};
  ReplayRecordingScope::RecordPads(0, predicted);
  Pads corrected{};
  corrected[2][3] = 99;
  ReplayRecordingScope::RecordPads(0, corrected);
  ASSERT_EQ(archive.frames.size(), 1u);
  EXPECT_EQ(archive.frames[0].pads, corrected);
  EXPECT_TRUE(archive.frames[0].clear_ready);
  EXPECT_EQ(archive.frames[0].header->room, 7u);
  EXPECT_EQ(ReplayRecordingScope::Frame(MAX_REPLAY_FRAMES + 1), nullptr);
}

TEST(OrcaKeyframe, BoundsDecodedMetadataWhenCompressedDataIsSmall)
{
  ReplayArchive replay;
  replay.boundary.ports = std::make_shared<const std::vector<Orca::Events::PortInfo>>(
      std::vector<Orca::Events::PortInfo>{{0, "a", false, {}, {}}, {1, "b", false, {}, {}},
                                        {2, "c", false, {}, {}}, {3, "d", false, {}, {}}});
  const auto single = Raw(PackKeyframe(0, replay));
  constexpr u32 count = 80000;
  const size_t record_size = single.size() - 24;
  std::vector<u8> raw(24 + (count + 1) * record_size);
  std::copy_n(single.begin(), 24, raw.begin());
  U32(raw, 4, count);
  for (u32 i = 0; i <= count; ++i)
  {
    std::copy(single.begin() + 24, single.end(), raw.begin() + 24 + i * record_size);
    raw[24 + i * record_size + 44] = static_cast<u8>('a' + i % 26);
  }
  const auto blob = Compressed(raw);
  EXPECT_LT(blob.size(), 1u << 20);
  int frame = -1;
  ReplayArchive decoded;
  EXPECT_FALSE(UnpackKeyframe(blob, &frame, &decoded));
  EXPECT_EQ(frame, -1);
}

TEST(OrcaKeyframe, RejectsLegacyAndRedirectingDownloadIds)
{
  EXPECT_TRUE(ValidReplayId("replay-0-01234567"));
  EXPECT_TRUE(ValidReplayId("replay-216000-abcdef01"));
  for (const std::string id : {"kf-1-01234567", "../thumb?u=host", "replay-01-01234567",
                               "replay-216001-01234567", "replay-1-0123456g",
                               "replay-1-01234567?u=host", "replay-1-01234567/../x"})
    EXPECT_FALSE(ValidReplayId(id)) << id;
}

TEST(OrcaKeyframe, EncryptionRoundTripsAndRefusesTampering)
{
  const std::vector<u8> packed = PackKeyframe(77, Replay(77));
  std::vector<u8> sealed = packed;
  std::string key;
  ASSERT_TRUE(EncryptKeyframe(77, &sealed, &key));
  EXPECT_EQ(key.size(), 64u);
  EXPECT_EQ(sealed.size(), packed.size() + 28);
  std::string other_key;
  std::vector<u8> twice = packed;
  ASSERT_TRUE(EncryptKeyframe(77, &twice, &other_key));
  EXPECT_NE(key, other_key);
  EXPECT_NE(twice, sealed);
  std::vector<u8> opened = sealed;
  ASSERT_TRUE(DecryptKeyframe(77, key, &opened));
  EXPECT_EQ(opened, packed);
  opened = sealed;
  EXPECT_FALSE(DecryptKeyframe(77, other_key, &opened));
  opened = sealed;
  EXPECT_FALSE(DecryptKeyframe(78, key, &opened));
  opened = sealed;
  opened[opened.size() / 2] ^= 1;
  EXPECT_FALSE(DecryptKeyframe(77, key, &opened));
  opened = sealed;
  EXPECT_FALSE(DecryptKeyframe(77, key.substr(0, 63) + "g", &opened));
}

// The HTTP store against Tools/orca/fake_keyframes.py (set ORCA_TEST_KEYFRAME_SERVER to its URL):
// put, get with progress, delete, retries past 503s. A stale ticket is replaced once, a second
// refusal is final (LastRefusal names it), and a repeated PUT of the same id counts as done.
TEST(OrcaKeyframe, HttpStoreAgainstAFakeServer)
{
  const char* server = std::getenv("ORCA_TEST_KEYFRAME_SERVER");
  if (!server || !*server)
    GTEST_SKIP() << "start Tools/orca/fake_keyframes.py and set ORCA_TEST_KEYFRAME_SERVER";
  int minted = 0;
  const auto store_for = [&](const std::string& room, const std::string& cached,
                             const std::string& fresh_ticket) {
    return MakeHttpKeyframeStore(
        [=, &minted](bool fresh, std::string* t, std::string* url, std::string*) {
          // Like YouGameRoom::FreshTicket: a minted ticket is the cached one from then on.
          minted += fresh;
          *t = minted ? fresh_ticket : cached;
          *url = std::string(server) + "/api/orca/keyframes/" + room;
          return true;
        });
  };
  for (const auto& [room, size] : std::vector<std::pair<std::string, size_t>>{
           {"room1", 300000}, {"room1", 3 << 20}, {"flaky", 200000}})
  {
    SCOPED_TRACE(room + " " + std::to_string(size));
    // The cached ticket went stale: one refusal, then a new one.
    minted = 0;
    auto store = store_for(room, "stale", "good");
    std::vector<u8> blob(size);
    std::mt19937 rng(static_cast<u32>(size));
    for (u8& b : blob)
      b = static_cast<u8>(rng());
    const std::string hash = KeyframeHash(blob);
    KeyframeInfo info{1200, "replay-1200-" + hash.substr(0, 8), blob.size(), hash, ""};
    std::string error;
    ASSERT_TRUE(store->Put(info, blob, &error)) << error;
    EXPECT_EQ(minted, 1);
    EXPECT_EQ(LastRefusal(), "") << "503s and a stale ticket aren't refusals";
    auto good = store_for(room, "good", "good");
    ASSERT_TRUE(good->Put(info, blob, &error)) << "409 exists is done: " << error;
    int calls = 0;
    u64 last = 0;
    const auto got = good->Get(
        info,
        [&](u64 done, u64 total) {
          ++calls;
          EXPECT_EQ(total, info.size);
          EXPECT_GE(done, last);
          last = done;
          return true;
        },
        &error);
    ASSERT_TRUE(got) << error;
    EXPECT_EQ(*got, blob);
    EXPECT_GT(calls, 0);
    EXPECT_EQ(last, info.size);
    good->Delete(info.id);
    good.reset();  // waits for the delete
    auto again = store_for(room, "good", "good");
    EXPECT_FALSE(again->Get(info, [](u64, u64) { return true; }, &error));
    EXPECT_NE(error.find("404"), std::string::npos) << error;
    EXPECT_NE(error.find("gone"), std::string::npos) << "the store's sentence: " << error;
    EXPECT_EQ(LastRefusal(), "gone");
  }
  // A GET with a stale ticket gets a new one too, and the refusal's body never counts as progress.
  {
    minted = 0;
    auto putter = store_for("room2", "good", "good");
    const std::vector<u8> blob(50000, 3);
    const std::string hash = KeyframeHash(blob);
    const KeyframeInfo info{7, "replay-7-" + hash.substr(0, 8), blob.size(), hash, ""};
    std::string error;
    ASSERT_TRUE(putter->Put(info, blob, &error)) << error;
    minted = 0;
    auto getter = store_for("room2", "stale", "good");
    u64 most = 0;
    const auto got =
        getter->Get(info, [&](u64 done, u64) { most = std::max(most, done); return true; }, &error);
    ASSERT_TRUE(got) << error;
    EXPECT_EQ(*got, blob);
    EXPECT_EQ(minted, 1);
    EXPECT_EQ(most, blob.size());
    getter->Delete(info.id);
  }
  // Cancelled (emulation stopping): no more tries.
  {
    auto store = store_for("flaky", "good", "good");
    store->Cancel();
    const std::vector<u8> blob(1000, 2);
    const std::string hash = KeyframeHash(blob);
    std::string error;
    EXPECT_FALSE(store->Put({3, "replay-3-" + hash.substr(0, 8), blob.size(), hash, ""}, blob, &error));
    EXPECT_EQ(error, "cancelled");
    EXPECT_EQ(LastRefusal(), "");
  }
  // A ticket minted while signed out: one new ticket (the player may have signed in since), then
  // the refusal is final.
  {
    const std::vector<u8> blob(2000, 4);
    const std::string hash = KeyframeHash(blob);
    const KeyframeInfo info{9, "replay-9-" + hash.substr(0, 8), blob.size(), hash, ""};
    std::string error;
    minted = 0;
    ASSERT_TRUE(store_for("room3", "guest", "good")->Put(info, blob, &error)) << error;
    EXPECT_EQ(minted, 1);
    minted = 0;
    EXPECT_FALSE(store_for("room4", "guest", "guest")->Put(info, blob, &error));
    EXPECT_EQ(minted, 1);
    EXPECT_NE(error.find("403"), std::string::npos) << error;
    EXPECT_EQ(LastRefusal(), "signed_out");
    // A 4xx without YouGame's code (an edge page) fails this try but isn't a refusal.
    EXPECT_FALSE(store_for("room4", "edge", "edge")->Put(info, blob, &error));
    EXPECT_NE(error.find("403"), std::string::npos) << error;
    EXPECT_EQ(LastRefusal(), "");
  }
  // A ticket the store refuses even when new: final.
  auto refused = store_for("room1", "bad", "bad");
  std::string error;
  const std::vector<u8> blob(1000, 1);
  const std::string hash = KeyframeHash(blob);
  EXPECT_FALSE(refused->Put({1, "replay-1-" + hash.substr(0, 8), blob.size(), hash, ""}, blob, &error));
  EXPECT_NE(error.find("401"), std::string::npos) << error;
  EXPECT_EQ(LastRefusal(), "ticket");
}
