// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <charconv>
#include <optional>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/Orca/Session/Events.h"
#include "Core/Orca/Session/Session.h"

namespace Orca::Net
{
// Network joins contain only controller inputs and typed Orca UI events. They never contain
// Dolphin device state, RAM images, NAND files, host addresses or arbitrary memory writes.
constexpr size_t MAX_REPLAY_FRAMES = 60 * 60 * 60;

inline bool ValidReplayId(std::string_view id)
{
  if (!id.starts_with("replay-"))
    return false;
  id.remove_prefix(7);
  const auto dash = id.find('-');
  if (dash == std::string_view::npos || dash == 0 || dash > 6 || id.size() != dash + 9 ||
      (dash > 1 && id.front() == '0'))
    return false;
  unsigned frame = 0;
  const auto parsed = std::from_chars(id.data(), id.data() + dash, frame);
  if (parsed.ec != std::errc{} || parsed.ptr != id.data() + dash || frame > MAX_REPLAY_FRAMES)
    return false;
  for (const char c : id.substr(dash + 1))
  {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
      return false;
  }
  return true;
}

struct ReplayHeader
{
  u8 mode = 0;
  u8 ruleset = 0;
  u8 coin = 0;
  u8 flags = 0;
  u32 room = 0;
  bool operator==(const ReplayHeader&) const = default;
};

struct ReplayFrame
{
  Pads pads{};
  std::shared_ptr<const std::vector<Events::PortInfo>> ports;
  std::optional<ReplayHeader> header;
  bool clear_ready = false;
};

struct ReplayArchive
{
  u64 origin_hash = 0;
  u64 target_hash = 0;
  std::vector<ReplayFrame> frames;
  // The host's pre-frame hook has already run at the boundary being offered.
  ReplayFrame boundary;
};

// CPU-thread-only context for the existing trusted UI handlers. Exogenous changes are recorded
// when first applied and replayed at exactly the same phase; resimulation retains those events.
class ReplayScope
{
public:
  ReplayScope(ReplayFrame* frame, bool playback, bool network = false)
      : m_previous(s_current), m_playback(s_playback), m_network(s_network)
  {
    s_current = frame;
    s_playback = playback;
    s_network = network;
  }
  ~ReplayScope()
  {
    s_current = m_previous;
    s_playback = m_playback;
    s_network = m_network;
  }
  static ReplayFrame* Current() { return s_current; }
  static bool Playing() { return s_current && s_playback; }
  static bool NetworkPlaying() { return Playing() && s_network; }

private:
  ReplayFrame* m_previous;
  bool m_playback;
  bool m_network;
  static inline thread_local ReplayFrame* s_current = nullptr;
  static inline thread_local bool s_playback = false;
  static inline thread_local bool s_network = false;
};

class ReplayRecordingScope
{
public:
  explicit ReplayRecordingScope(ReplayArchive* archive) : m_previous(s_archive) { s_archive = archive; }
  ~ReplayRecordingScope() { s_archive = m_previous; }
  static ReplayFrame* Frame(int frame)
  {
    if (!s_archive || frame < 0 || static_cast<size_t>(frame) > MAX_REPLAY_FRAMES)
      return nullptr;
    if (s_archive->frames.size() <= static_cast<size_t>(frame))
      s_archive->frames.resize(static_cast<size_t>(frame) + 1);
    return &s_archive->frames[frame];
  }
  static void RecordPads(int frame, const Pads& pads)
  {
    if (auto* target = Frame(frame))
      target->pads = pads;
  }

private:
  ReplayArchive* m_previous;
  static inline thread_local ReplayArchive* s_archive = nullptr;
};
}  // namespace Orca::Net
