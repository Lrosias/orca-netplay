// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Common/FileUtil.h"
#include "Common/IOFile.h"

#include "Core/Orca/Session/Keyframe.h"

using namespace Orca::Net;

namespace
{
Rollback::MachineImage Image(u32 seed, size_t ram)
{
  std::mt19937 rng(seed);
  Rollback::MachineImage image;
  image.state.resize(70000);
  image.mem1.resize(ram);
  image.mem2.resize(ram * 2);
  image.l1_cache.resize(16384);
  for (auto* v : {&image.state, &image.mem1, &image.mem2, &image.l1_cache})
  {
    // Compressible, like RAM: runs of one random byte.
    for (size_t i = 0; i < v->size(); i += 64)
      std::fill(v->begin() + i, v->begin() + std::min(v->size(), i + 64), static_cast<u8>(rng()));
  }
  return image;
}

std::vector<NandEntry> Nand()
{
  return {{"shared2", true, {}},
          {"title/00010000/52534245/data/autosv0.bin", false, std::vector<u8>(5000, 7)},
          {"tmp/2d13", false, std::vector<u8>{1, 2, 3}},
          {"fst.bin", false, {}}};
}
}  // namespace

TEST(OrcaKeyframe, PackUnpackRoundTrips)
{
  const auto image = Image(1, 1 << 20);
  const std::vector<u8> blob = PackKeyframe(4321, image, Nand());
  ASSERT_FALSE(blob.empty());
  EXPECT_LT(blob.size(), image.mem1.size());
  EXPECT_EQ(KeyframeHash(blob).size(), 16u);
  int frame = 0;
  Rollback::MachineImage got;
  std::vector<NandEntry> nand;
  ASSERT_TRUE(UnpackKeyframe(blob, &frame, &got, &nand));
  EXPECT_EQ(frame, 4321);
  EXPECT_EQ(got.state, image.state);
  EXPECT_EQ(got.mem1, image.mem1);
  EXPECT_EQ(got.mem2, image.mem2);
  EXPECT_EQ(got.l1_cache, image.l1_cache);
  ASSERT_EQ(nand.size(), Nand().size());
  for (size_t i = 0; i < nand.size(); ++i)
  {
    EXPECT_EQ(nand[i].path, Nand()[i].path);
    EXPECT_EQ(nand[i].directory, Nand()[i].directory);
    EXPECT_EQ(nand[i].data, Nand()[i].data);
  }
}

TEST(OrcaKeyframe, RefusesDamagedAndHostileKeyframes)
{
  std::vector<u8> blob = PackKeyframe(10, Image(2, 4096), Nand());
  int frame;
  Rollback::MachineImage image;
  std::vector<NandEntry> nand;
  std::vector<u8> cut(blob.begin(), blob.begin() + blob.size() / 2);
  EXPECT_FALSE(UnpackKeyframe(cut, &frame, &image, &nand));
  blob[blob.size() / 2] ^= 0xff;
  EXPECT_FALSE(UnpackKeyframe(blob, &frame, &image, &nand));
  // Paths that would leave the NAND's folder never unpack.
  for (const std::string path : {"../etc/passwd", "/abs", "a/../b", "a//b", ""})
  {
    std::vector<NandEntry> bad = {{path, false, {1}}};
    const std::vector<u8> packed = PackKeyframe(1, Image(3, 4096), bad);
    EXPECT_FALSE(UnpackKeyframe(packed, &frame, &image, &nand)) << path;
  }
}

// Encryption: the store only ever holds ciphertext; the right key and frame open it, nothing else.
TEST(OrcaKeyframe, EncryptionRoundTripsAndRefusesTampering)
{
  const std::vector<u8> packed = PackKeyframe(77, Image(5, 8192), Nand());
  std::vector<u8> sealed = packed;
  std::string key;
  ASSERT_TRUE(EncryptKeyframe(77, &sealed, &key));
  EXPECT_EQ(key.size(), 64u);
  EXPECT_EQ(sealed.size(), packed.size() + 28);
  EXPECT_EQ(std::search(sealed.begin(), sealed.end(), packed.begin() + 8, packed.begin() + 40),
            sealed.end());
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
  EXPECT_FALSE(DecryptKeyframe(78, key, &opened)) << "the frame is authenticated";
  opened = sealed;
  opened[opened.size() / 2] ^= 1;
  EXPECT_FALSE(DecryptKeyframe(77, key, &opened));
  opened = sealed;
  EXPECT_FALSE(DecryptKeyframe(77, key.substr(0, 63) + "g", &opened));
}

// The HTTP store against Tools/orca/fake_keyframes.py (set ORCA_TEST_KEYFRAME_SERVER to its URL):
// put, get with progress, delete, retries past 503s. A stale ticket is replaced once, a second
// refusal is final, and a repeated PUT of the same id counts as done.
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
    KeyframeInfo info{1200, "kf-1200-" + hash.substr(0, 8), blob.size(), hash, ""};
    std::string error;
    ASSERT_TRUE(store->Put(info, blob, &error)) << error;
    EXPECT_EQ(minted, 1);
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
  }
  // A GET with a stale ticket gets a new one too, and the refusal's body never counts as progress.
  {
    minted = 0;
    auto putter = store_for("room2", "good", "good");
    const std::vector<u8> blob(50000, 3);
    const std::string hash = KeyframeHash(blob);
    const KeyframeInfo info{7, "kf-7-" + hash.substr(0, 8), blob.size(), hash, ""};
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
    EXPECT_FALSE(store->Put({3, "kf-3-" + hash.substr(0, 8), blob.size(), hash, ""}, blob, &error));
    EXPECT_EQ(error, "cancelled");
  }
  // A ticket the store refuses even when new: final.
  auto refused = store_for("room1", "bad", "bad");
  std::string error;
  const std::vector<u8> blob(1000, 1);
  const std::string hash = KeyframeHash(blob);
  EXPECT_FALSE(refused->Put({1, "kf-1-" + hash.substr(0, 8), blob.size(), hash, ""}, blob, &error));
  EXPECT_NE(error.find("401"), std::string::npos) << error;
}

// NAND files the joiner's own boot writes travel as hashes: the joiner fills them in from its NAND
// and refuses a keyframe whose named file it lacks or holds differently.
TEST(OrcaKeyframe, NandReferencesResolveFromTheJoinersOwnNand)
{
  const std::string root = File::CreateTempDir();
  ASSERT_FALSE(root.empty());
  const std::vector<u8> pack(200000, 9);
  File::CreateFullPath(root + "/tmp/");
  File::IOFile(root + "/tmp/2d13", "wb").WriteBytes(pack.data(), pack.size());

  std::vector<NandEntry> nand = Nand();
  nand.push_back({"tmp/2d13", false, {}, true, NandHash(pack)});
  const std::vector<u8> with_reference = PackKeyframe(5, Image(4, 4096), nand);
  std::vector<NandEntry> full = Nand();
  full.push_back({"tmp/2d13", false, pack, false, 0});
  EXPECT_LT(with_reference.size(), PackKeyframe(5, Image(4, 4096), full).size())
      << "a reference costs a hash, not the file";

  int frame;
  Rollback::MachineImage image;
  std::vector<NandEntry> got;
  ASSERT_TRUE(UnpackKeyframe(with_reference, &frame, &image, &got));
  ASSERT_TRUE(got.back().reference);
  std::string error;
  ASSERT_TRUE(ResolveNandReferences(root, &got, &error)) << error;
  EXPECT_FALSE(got.back().reference);
  EXPECT_EQ(got.back().data, pack);

  // A different file, or none, refuses.
  ASSERT_TRUE(UnpackKeyframe(with_reference, &frame, &image, &got));
  const std::vector<u8> other(200000, 8);
  File::IOFile(root + "/tmp/2d13", "wb").WriteBytes(other.data(), other.size());
  EXPECT_FALSE(ResolveNandReferences(root, &got, &error));
  EXPECT_NE(error.find("differs"), std::string::npos) << error;
  File::Delete(root + "/tmp/2d13");
  EXPECT_FALSE(ResolveNandReferences(root, &got, &error));
  EXPECT_NE(error.find("missing"), std::string::npos) << error;
  File::DeleteDirRecursively(root);
}
