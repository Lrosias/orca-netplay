// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <optional>
#include <span>
#include <string>

#include "Common/CommonTypes.h"
#include "Core/Orca/UX/MatchBlock.h"
#include "InputCommon/GCPadStatus.h"

// Two cursors on a matchmade stage select, one per player. The game's stage select has one cursor
// driven by one port. While these cursors run, the stage flows (BrawlStages.h, RankedPPlus.h) take
// every pad away from it and drive it themselves, so the game only sees what the flows decide.
//
// Each cursor is synced state in the match block, in fixed point (1/16 of the game's cursor units,
// integer math only). The input gate's latch writes it from ports 1-2's raw pads as a pure function
// of memory and those pads, on every pass over a frame (first runs, rollback re-runs, a joiner's
// first frame), so every machine reads the same cursors. Presses are edges against the last seen
// buttons, also in the block, so re-running a frame changes nothing. The overlay draws the cursors
// from what the frame hook publishes on first runs.
//
// Match block bytes (MatchBlock.h):
//   +0xF4..+0xFF   shared: tag (on), game, which cursor the native one follows, page turn
//   +0x1B4..+0x1BF two cursors, 6 bytes each: s16 x, s16 y, u8 buttons now, u8 buttons seen
namespace Core
{
class CPUThreadGuard;
}

namespace Orca::UX
{
class GuestMemory;
}

namespace Orca::UX::StageCursors
{
constexpr u32 SHARED = MatchBlock::STAGE_CURSORS_SHARED;
constexpr u32 SHARED_SIZE = MatchBlock::STAGE_CURSORS_SHARED_SIZE;
constexpr u32 CURSORS = MatchBlock::STAGE_CURSORS;
constexpr u32 CURSOR_SIZE = 6;
constexpr u32 CURSORS_SIZE = 2 * CURSOR_SIZE;
static_assert(CURSORS_SIZE == MatchBlock::STAGE_CURSORS_SIZE);
// Tag value while the two cursors run (anything else: off).
constexpr u8 TAG_ON = 0xC2;

// The buttons the flows read, one bit each.
enum Button : u8
{
  BTN_A = 0x01,
  BTN_B = 0x02,
  BTN_X = 0x04,
  BTN_Y = 0x08,
  BTN_START = 0x10,
  BTN_Z = 0x20,
};
u8 ButtonsOf(u16 pad_buttons);

// MatchBlock's ruleset ids.
constexpr u8 GAME_BRAWL = 1;
constexpr u8 GAME_PPLUS = 2;

// In cursor units x 16: x0 <= x < x1, y0 <= y < y1 (up is +y).
struct Rect
{
  s16 x0, y0, x1, y1;
  constexpr bool Contains(int x, int y) const { return x >= x0 && x < x1 && y >= y0 && y < y1; }
};
// A legal stage's tile: ruleset stage index (RankedSteps.h), page, and rectangle.
struct Tile
{
  s8 stage;
  u8 page;
  Rect rect;
};
struct Point
{
  s16 x, y;
};

struct Layout
{
  u8 game = 0;
  // The cursor's range (the game's own cursor never leaves it), x 16.
  Rect bounds{};
  // Movement per frame at full tilt (x 16), and the stick dead zone (of 128).
  s16 speed = 16;
  s16 dead_zone = 24;
  std::span<const Tile> tiles;
  // Brawl's Melee Stages / Brawl Stages page button. Without pages it is never hovered.
  bool pages = false;
  Rect page_button{};
  std::array<Point, 2> start{};
  // Overlay only, never synced: where cursor (0, 0) sits in the picture (0..1 across and down),
  // and how far one cursor unit moves across and down.
  float u0 = 0.5f, du = 0.01f, v0 = 0.5f, dv = -0.01f;
};
const Layout& BrawlLayout();
const Layout& PPlusLayout();
const Layout* LayoutFor(u8 game);

// What a cursor is on: a legal stage's index, PAGE_BUTTON, or NOTHING.
constexpr int NOTHING = -1;
constexpr int PAGE_BUTTON = -2;
int Hovered(const Layout& layout, u32 page, int x, int y);
// The tile of `stage` on `page` (nullptr: not on that page).
const Tile* TileOf(const Layout& layout, int stage, u32 page);
// The middle of a rectangle in the game's cursor units (not x 16).
float MidX(const Rect& r);
float MidY(const Rect& r);

struct Cursor
{
  s16 x = 0;
  s16 y = 0;
  u8 now = 0;   // buttons of the frame that just ran
  u8 seen = 0;  // the buttons the frame hook last saw
  bool operator==(const Cursor&) const = default;
};

struct State
{
  u8 game = 0;
  // The port the game's own cursor follows (the last to move), so its hover shows the preview.
  u8 follower = 0;
  // Page turn requested: 0 none, else 1 + port. Frames since then (it times out).
  u8 page_turn = 0;
  u8 page_frames = 0;
  std::array<Cursor, 2> cursors{};
  bool operator==(const State&) const = default;
};

// The two cursors, or nullopt while the tag is off.
std::optional<State> Read(const GuestMemory& memory);
// Writes `state` with the tag on, only the bytes that differ. Returns bytes changed.
int Write(GuestMemory& memory, const State& state);
// Turns the tag off if it was on. Returns bytes changed.
int Clear(GuestMemory& memory);
// A fresh pair of cursors at the layout's start points.
State Start(const Layout& layout);

// Presses `port` made since the frame hook last looked (`now` against `seen`). Marks them seen.
u8 TakePresses(State* state, int port);

// While the tag is on, ports 1-2's raw sticks move their cursors (integer math, clamped to the
// bounds) and record the frame's buttons. Returns bytes changed.
int Latch(GuestMemory& memory, const std::array<std::optional<GCPadStatus>, 4>& raw);
// The CPU thread's latch (InputGate.h).
void LatchFrame(const Core::CPUThreadGuard& guard,
                const std::array<std::optional<GCPadStatus>, 4>& raw);

// ---- Overlay view (UI only, published by the stage flows on first runs) ----
struct View
{
  bool on = false;
  u8 game = 0;
  u32 page = 0;
  struct Player
  {
    bool present = false;
    s16 x = 0, y = 0;
    s8 proposal = -1;  // proposed stage (ruleset index), -1 none
    bool local = false;
    std::string name;
  };
  std::array<Player, 2> players;
  s8 picked = -1;    // the stage the flow took (-1: not yet)
  s8 turn = -1;      // whose turn it is (-1: nobody's or both)
  // The current step: kind (Ranked::StepKind, 0 none), strikes or bans still owed, whether a ban
  // is optional, Brawl's Meta Knight clause (one pick of any legal stage), a step for both players
  // at once (a casual pick), and the timer's seconds (-1: none).
  u8 kind = 0;
  u8 left = 0;
  bool optional = false;
  bool mk_clause = false;
  bool both = false;
  s16 seconds = -1;
  bool operator==(const View&) const = default;
};

// The turn tag drawn by a player's cursor (UI only).
struct TurnCue
{
  enum class Mode : u8
  {
    Idle,     // nothing to say: no tag, no blink
    Acting,   // this player has to act: the cursor blinks, the tag says what to do
    Waiting,  // the local player waits for the other: a small grey tab, no blink
  };
  Mode mode = Mode::Idle;
  bool local = false;  // the cursor belongs to this machine's player
  std::string title;   // "YOUR TURN", "BO'S TURN", "WAITING FOR BO"
  std::string detail;  // "X STRIKES A STAGE · 2 LEFT" (may be empty)
  int seconds = -1;    // the turn's timer (-1: none)
  bool operator==(const TurnCue&) const = default;
};
// The turn tag for `port`'s cursor (0 or 1), from this machine's point of view.
TurnCue TurnText(const View& view, int port);
// The banner line under "META KNIGHT CLAUSE" ("BO PICKS ANY STAGE · NO STRIKES", or "YOU PICK ..."
// on the picker's screen). Empty when the clause doesn't apply or the stage is picked.
std::string MkClauseNote(const View& view);
// Publish runs on every first-run frame. Current returns the view from the frame before the last,
// which is the frame on screen.
void Publish(View view);
View Current();
}  // namespace Orca::UX::StageCursors
