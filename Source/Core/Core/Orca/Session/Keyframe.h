// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/Orca/Session/YouGameRoom.h"
#include "Core/Orca/Session/Replay.h"
#include "Core/Rollback/Rollback.h"

// Network drop-in transfers are bounded controller-input replays with typed UI events. Each
// machine restores its own local boot snapshot and executes the replay before joining live play.
// NAND helpers below serve local snapshots only; no NAND or Dolphin state is accepted from peers.
namespace Orca::Net
{
// One file or folder of the session NAND, by its '/'-separated path under the NAND root.
struct NandEntry
{
  std::string path;
  bool directory = false;
  std::vector<u8> data;
  // A file the joiner's own boot also writes (Profile::boot_nand_frame): the keyframe carries only
  // its XXH3-64, and the joiner uses its own copy if the hash matches.
  bool reference = false;
  u64 hash = 0;
};

// A file's hash as a reference carries it.
u64 NandHash(const std::vector<u8>& data);
// The NAND's files under `root` as {path: hash} (no folders), for references.
std::map<std::string, u64> HashNandTree(const std::string& root);
// Replaces each reference in `entries` with the joiner's own file at that path under `root`. False,
// with the path in `error`, if one is missing or differs.
bool ResolveNandReferences(const std::string& root, std::vector<NandEntry>* entries,
                           std::string* error);

// The NAND's files under `root`, in name order. False if the tree can't be read.
bool ReadNandTree(const std::string& root, std::vector<NandEntry>* entries);
// Replaces everything under `root` with `entries`. `root` must be a temporary session NAND; the
// caller checks.
bool ReplaceNandTree(const std::string& root, const std::vector<NandEntry>& entries);

// Packs and unpacks a keyframe. `hash` is the hex XXH3 of the compressed bytes.
std::vector<u8> PackKeyframe(int frame, const ReplayArchive& replay);
bool UnpackKeyframe(const std::vector<u8>& blob, int* frame, ReplayArchive* replay);
std::string KeyframeHash(const std::vector<u8>& blob);

// AES-256-GCM over a packed keyframe, so YouGame's store only holds ciphertext. The key is random
// per keyframe and reaches the joiner only in the room's `kf` message (KeyframeInfo::key, 64 hex).
// Body: IV (12) | ciphertext | tag (16); the frame number is authenticated too.
bool EncryptKeyframe(int frame, std::vector<u8>* blob, std::string* key_hex);
bool DecryptKeyframe(int frame, const std::string& key_hex, std::vector<u8>* blob);

// A joiner's checks on the host's encrypted replay, whether downloaded from the store or inline in
// the offer (KeyframeInfo::inline_blob): the size and hash the offer named, decryption with its
// key, and unpacking at its frame. False, with `error` saying which, on any mismatch.
bool OpenKeyframe(const KeyframeInfo& info, std::vector<u8> blob, int* frame, ReplayArchive* replay,
                  std::string* error);

// Where keyframes travel: the host puts one, the joiner gets it, and it's deleted once loaded. Put
// and Get block, so call them on their own thread; the progress callback can return false to abort.
// Delete never blocks.
class KeyframeStore
{
public:
  virtual ~KeyframeStore() = default;
  virtual bool Put(const KeyframeInfo& info, const std::vector<u8>& data, std::string* error) = 0;
  virtual std::optional<std::vector<u8>> Get(const KeyframeInfo& info,
                                             const std::function<bool(u64, u64)>& progress,
                                             std::string* error) = 0;
  virtual void Delete(const std::string& id) = 0;
  // Makes Put and Get give up as soon as they can (emulation is stopping).
  virtual void Cancel() {}
};

// Supplies a room ticket (a newly minted one when `fresh`) and the store's URL for the room.
using TicketSource = std::function<bool(bool fresh, std::string* ticket, std::string* store_url,
                                        std::string* error)>;

// ORCA_TEST_KEYFRAME_DIR: a folder shared by two processes on one machine (tests). Otherwise
// YouGame's keyframe endpoint, authorized with room tickets.
std::unique_ptr<KeyframeStore> MakeKeyframeStore(TicketSource tickets);

// The HTTP store alone (tests point it at a local server).
std::unique_ptr<KeyframeStore> MakeHttpKeyframeStore(TicketSource tickets);

// After a Put or Get on this thread failed: why, when YouGame's store refused for good (its code,
// such as "signed_out"). Empty when another try could work (a network error, 429, 5xx, a 4xx
// without YouGame's code) or the call was cancelled.
std::string LastRefusal();
}  // namespace Orca::Net
