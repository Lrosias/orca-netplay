// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>

#include "Common/CommonTypes.h"
#include "Core/Orca/UX/Results.h"
#include "Core/Orca/UX/SetBlock.h"

namespace Core
{
class CPUThreadGuard;
}

// Tracks a ranked set in emulated memory. The frame hook judges each fight of a ranked match on
// its results screen and keeps the score in the set block. Every write is a pure function of
// emulated memory and the frame number, and only changed bytes are written, so a rollback re-run
// writes nothing new. See ORCA.md, "Ranked sets".
//
// Brawl time-outs go to the ledge-grab limit, then stocks, then percent. A level game or a sudden
// death is a tie, replayed as a 1 stock, 3:00 tiebreak game. Five ties in a row void the set.
// Project+'s own codeset already settles time-outs, so its results screen's winner stands.
// The set ends at 2 wins, or is void after SetBlock::MAX_RECORDS games.
namespace Orca::UX::RankedSet
{
constexpr int WINS = 2;
constexpr int TIEBREAK_STREAK_LIMIT = 5;
constexpr int LEDGE_LIMIT = 35;
constexpr int LEDGE_LIMIT_MK = 20;
constexpr int LEDGE_LIMIT_TIEBREAK = 11;
constexpr int LEDGE_LIMIT_TIEBREAK_MK = 6;
// gmCharacterKind of Meta Knight in the result info.
constexpr u8 META_KNIGHT = 0x18;

// The fight's live state on one frame.
struct Live
{
  bool valid = false;
  bool time_up = false;  // a timed stock match's clock reached 0
  std::array<u8, 2> stocks{0, 0};
  std::array<u16, 2> percent{0, 0};
  std::array<bool, 2> on_ledge{false, false};
  bool operator==(const Live&) const = default;
};

// What the hook reads on one frame.
struct Facts
{
  Reading::Scene scene = Reading::Scene::Other;
  ResultBlock block;  // the result info, on the results screen
  u8 decision = 0;    // gmResultInfo +0x1378; 1 = time up
  Live live;
  // Live state was read. If it is invalid, the fighters are gone (between or after fights).
  bool live_read = false;
};

// The verdict on one finished fight, or nullopt for a void game that isn't recorded.
std::optional<SetBlock::GameRecord> Judge(SetBlock::Ruleset ruleset, const SetBlock::SetState& set,
                                          const ResultBlock& block, u8 decision);

// The set after one frame.
SetBlock::SetState Step(SetBlock::SetState set, SetBlock::Ruleset ruleset, const Facts& facts,
                        u32 frame);

// Writes a Brawl ranked game's rules on the menus: 3 stocks and 8:00, or 1 stock and 3:00 for a
// tiebreak. Returns how many bytes changed.
int WriteSetRules(GuestMemory& memory, const SetBlock::SetState& set, u8 stocks = 3,
                  u8 minutes = 8);

// Read-only. Invalid where the fight can't be read.
Live ReadLiveFight(const GuestMemory& memory);

// Read-only. Reads the live fight only when `live` is set.
Facts ReadFacts(const GuestMemory& memory, bool live);

// Frame hook entry. `resimulating` only quiets the log.
void Frame(const Core::CPUThreadGuard& guard, int frame, bool resimulating);

// The overlay's set score line, local player first ("Set over · 2–1"). On an ordinary game's
// results screen it names the game just played ("Game 1 · 1–0"). Otherwise empty for an ordinary
// game, since the dots show it, and outside a ranked set.
std::string ScoreLine(const SetBlock::SetState& set, int local_port, bool results = false);
// A matchmade game's result toast, local player first ("You won · 1–0"). `out` is the room's
// verdict for this player: "won", "lost", "draw", or anything else for a void game.
std::string ResultToast(std::string_view out, int mine, int theirs);
// The last line the hook published, for the local player. Any thread; empty during a fight.
std::string CurrentScoreLine();
void SetLocalPort(int port);

// The overlay's per-game dots. games[i] is the winning port of the i-th decided game, -1 if not
// played yet (ties don't count). `current` is the 0-based game in play, -1 once the set is over.
// `on` only in a ranked set on the menus.
struct Dots
{
  bool on = false;
  std::array<int, 3> games{-1, -1, -1};
  int current = -1;
  bool operator==(const Dots&) const = default;
};
Dots DotsOf(const SetBlock::SetState& set);
// The last dots the hook published. Any thread; off during a fight.
Dots CurrentDots();
}  // namespace Orca::UX::RankedSet
