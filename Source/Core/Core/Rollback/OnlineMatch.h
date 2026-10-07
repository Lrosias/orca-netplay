// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

// Online drop-in play: friends join this game through a YouGame room. The host plays solo from
// boot; a friend who joins loads the host's keyframe, replays the host's inputs since then, and
// takes the next free port. From there a rollback session over the room's transport runs both
// games. See ORCA.md, "Drop-in".

#pragma once

#include <chrono>
#include <functional>
#include <optional>

#include "Core/Orca/Session/Events.h"
#include "Core/Orca/Session/Session.h"

namespace Core
{
class System;
}

namespace Rollback::OnlineMatch
{
// Runs at each frame boundary. The first call joins the room; a joiner blocks here until it has
// loaded the host's keyframe (or emulation stops). `local_pad(seat)` returns this player's pad.
// Returns the frame the state was rewound to (a rollback, or a joiner's keyframe); the state is
// then at the start of that frame.
// `on_frame` runs first at every boundary, before anything is saved: `ports` are the controllers
// plugged in at that frame, and `alone` is AloneAt for it (false on re-runs).
using FrameHook = std::function<void(int frame, bool resimulating,
                                     const std::vector<Orca::Events::PortInfo>& ports, bool alone)>;
std::optional<int> OnBoundary(Core::System& system,
                              const std::function<Orca::Net::Pad(int local_seat)>& local_pad,
                              const FrameHook& on_frame);

// A keyframe at most this old (30 s at 60 fps) still serves a new joiner, who replays the frames
// since.
constexpr int KEYFRAME_FRESH_FRAMES = 30 * 60;

// True when `frame` belongs to this player alone: playing solo, no drop-in pending, and no fresh
// keyframe a joining friend would replay from. Only then may the UI announce things.
bool AloneAt(int frame, bool solo_idle, bool drop_in_pending, std::optional<int> stored_keyframe);

// Whether the frame hook may write the online rules' header at `frame`: no other game runs this
// frame from an earlier state. Looser than AloneAt: a friend arriving or a keyframe being wanted
// doesn't block it, since that keyframe is made after the hook and so carries the header.
bool HeaderFreeAt(int frame, bool solo_quiet, std::optional<int> stored_keyframe);

// The host's wait for its room's header before it makes or offers a keyframe: the keyframe carries
// the header in game memory, and the joiner replays it.
enum class HeaderWait
{
  Ready,  // make or offer the keyframe
  Wait,   // not yet; the header is written at a later free boundary
  Fail,   // queue room only: the match can't start and the room is left
};
// A friends room whose game never gets its header (say, a session was already running when it
// changed) makes the keyframe anyway after this many boundaries (2 s) with a friend waiting.
constexpr int HEADER_WAIT_BOUNDARIES = 120;
// A queue room never makes a keyframe without the room's header: after this many boundaries (5 s)
// with the opponent waiting, the join fails.
constexpr int HEADER_FAIL_BOUNDARIES = 300;
// One boundary of the wait. `in_place`: the header is in game memory (HeaderInPlace);
// `someone_waiting`: a friend waits for a keyframe; `queue_room`: this game hosts a matchmade room.
// `*waited` counts the boundaries waited so far.
HeaderWait StepHeaderWait(int* waited, bool in_place, bool someone_waiting, bool queue_room);

// True while this process plays online (solo in its room, or with friends).
bool Active();
// Logs the session's stats.
void LogStats(const char* when);
// Logs the stats and the reason, ends the session and leaves the room. Safe to call more than once;
// also runs when emulation stops or the session ends.
void End(const char* reason);

// Whether the player's pause may go ahead. Allowed in a session without a room (tests), or online
// while this game plays alone with no drop-in under way or pending, so the pause stops only this
// player's game. Any thread.
bool SoloPauseAllowed();
// Core::SetState(Paused) in a session: whether the pause may go ahead. From a yes until EndPause,
// boundaries take no friend in, so a boundary that runs while the CPU thread winds down never
// starts a session it can't step.
bool BeginPause();
// Core::SetState(Running): the pause, if any, is over.
void EndPause();
// True between a successful BeginPause and EndPause: the core is paused by the player, not by boot
// stepping, an HLE reload or a panic.
bool Pausing();
// Host time spent paused since the last call. The stats line leaves it out, so its fps counts
// running time only.
std::chrono::nanoseconds TakePausedTime();
}  // namespace Rollback::OnlineMatch
