// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/Orca/Session/Events.h"

namespace Core
{
class CPUThreadGuard;
}

// The game's own Online menu. Patches (Data/Sys/Orca/<profile>.patches) make Brawl's Wi-Fi button
// and Project+'s Play Online open an offline ONLINE page. Each choice leaves the main menu for the
// local Versus character select with an exit code: 30 Casual, 31 Ranked, 25 With Friends. This
// file reads that pick and prints it on stdout ("orca menu online casual local", ...) for the app.
//
// Picks are read only on first runs, never on rollback re-runs, and announced only while this game
// plays alone. After a resync (Events NoteResync) the reader starts over silently, so nothing stale
// is printed.
//
// Scene manager at 0x805A0060: +4 the scene, whose name is at +0 ("muMenuMain", "scMemoryChange"
// between scenes); +0x284 the main menu's exit code, written as the menu leaves.
namespace Orca::UX
{
class GuestMemory;

enum class OnlinePick
{
  Casual,
  Ranked,
  Friends,
};

// What one frame shows of the main menu.
struct MenuState
{
  enum class Where
  {
    Other,
    Menu,     // muMenuMain
    Leaving,  // scMemoryChange: between two scenes
  };
  Where where = Where::Other;
  u32 exit_code = 0;
};

MenuState ReadMenuState(const GuestMemory& memory);

// The pick an exit code stands for, if any.
std::optional<OnlinePick> PickForExitCode(u32 exit_code);

// The words after "orca menu" on stdout: "online casual" or "online ranked" when the app's queue
// searches ("host" capability), with " local" appended when nothing searches; "online friends".
std::string_view MenuEventText(OnlinePick pick, bool queue = false);

// The current scene's name ("" when unreadable).
std::string ReadSceneName(const GuestMemory& memory);

// Matchmaking search state after a Casual or Ranked pick, for the overlay. The search ends when the
// app matches this game (`host <code>` or `join <code>`), the app cancels (`queue-cancel`), or the
// player backs out to the main menu, which prints "orca menu cancel" once. Any thread.
namespace Search
{
enum class State
{
  None,
  Searching,
  Found,
};
void Begin(bool ranked);
// The app matched this game: Found when hosting, None when joining.
void Matched(bool hosting);
// The opponent plugged in, the app ended the search, or the room went away.
void End();
State Current();
bool Ranked();
// The overlay's two lines (empty: nothing to show).
std::pair<std::string, std::string> Lines();
}  // namespace Search

class OnlineMenuReader
{
public:
  // One frame. `resyncs` is Events::Resyncs(). Returns the pick to announce, if any.
  std::optional<OnlinePick> Boundary(const MenuState& state, bool resimulating, bool alone,
                                     u64 resyncs);

private:
  bool m_started = false;
  u64 m_resyncs = 0;
  // The previous scene (or this one) was the main menu.
  bool m_from_menu = false;
  // This exit's code was already seen.
  bool m_seen = false;
};

// Frame hook: reads the menu and prints "orca menu ...". Reads emulated memory only.
void ReadOnlineMenu(const Core::CPUThreadGuard& guard, bool resimulating, bool alone);

// ---- Friends drop in from the menus ----
// A friend who joins while the host is in the menus moves both games to With Friends' character
// select, as if the host had picked it. A friend who joins during a single-player mode waits until
// the host is back (`orca state friend-holding` / `friend-waiting`).
//
// gfSceneManager (0x805A0060): +0x4 scene, +0x10 sequence (names at +0), +0x284 exit code, +0x288
// process step. Every menu exit writes the code, then step 2.

// The current sequence's name ("sqMenuMain", "sqVsMelee", ...; "" when unreadable).
std::string ReadSequenceName(const GuestMemory& memory);

// Whether a friend must wait in this sequence: single-player modes and the Vault. Everything else,
// including the boot, takes a friend in.
bool SequenceHoldsDropIn(std::string_view sequence);

// Whether the host's game was in such a sequence at its last first-run frame. Any thread.
bool DropInHeld();

namespace FriendsMove
{
// The scene manager's process step while a scene runs, and the step that ends it.
constexpr u32 STEP_RUNNING = 1;
constexpr u32 STEP_EXIT = 2;
// With Friends: goes to sqVsMelee, and B from its character select returns to the ONLINE page.
constexpr u32 EXIT_WITH_FRIENDS = 25;
// No friend plugs in before this frame, because a host never makes a keyframe in its first 300
// frames (Rollback/OnlineMatch.cpp FIRST_KEYFRAME_FRAME).
constexpr int FIRST_FRIEND_FRAME = 300;
// The friend sees the host's built menu for half a second before both move.
constexpr u32 MENU_SETTLE_FRAMES = 30;
// muMenuMain's "pages built" byte (same in both games). Its files load over many frames, then all
// pages are built in one frame and this becomes 1. Leaving the menu before then calls through an
// unfilled proc table and Brawl hangs, so never exit until it is set. Meaningless at step 0.
constexpr u32 MENU_BUILT = 0xAC8;

// What one frame reads.
struct Inputs
{
  // The block's memory is present (MatchBlock::Present and the friends bytes mapped).
  bool present = false;
  // Ports plugged in this frame (bit n: port n+1).
  u8 plugged = 0;
  // A casual or ranked header is present: queue matches never go to With Friends.
  bool queue = false;
  std::string scene;
  std::string sequence;
  u32 step = 0;
  u32 exit_code = 0;
  // On muMenuMain with MENU_BUILT set (only then can it be left).
  bool menu_built = false;
  int frame = 0;
  // The block's friends bytes (MatchBlock FRIENDS_*).
  u8 tag = 0;
  u8 seen = 0;
  u8 pending = 0;
  // Frame + 1 since which the built main menu has run with a friend pending (0: not yet).
  u32 menu_since = 0;
};

// What that frame writes.
struct Outputs
{
  // Write the friends bytes. False when nothing was kept and no friend is plugged in, so solo play
  // never writes them.
  bool keep = false;
  u8 seen = 0;
  u8 pending = 0;
  u32 menu_since = 0;
  // Leave the main menu for With Friends (exit code plus STEP_EXIT, like the menu's own exit).
  bool move = false;
};

// Pure function of its inputs, so a rollback re-run at the same frame writes the same thing.
Outputs Decide(const Inputs& in);

// Frame hook, first runs and re-runs, Brawl rev 2 and Project+ only. Returns whether the host left
// the main menu for With Friends this frame.
bool Frame(const Core::CPUThreadGuard& guard, int frame, bool resimulating,
           const std::vector<Events::PortInfo>& ports);
}  // namespace FriendsMove
}  // namespace Orca::UX
