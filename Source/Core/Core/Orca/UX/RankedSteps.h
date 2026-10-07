// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"

// Stage striking, banning and picking for a ranked set, shared by Brawl and Project+. Pure logic
// with no memory access or state. Each game's adapter keeps the set and step state in the match
// block and maps the ruleset's stage indices to its own stage select. Bit i of a StageMask is the
// ruleset's legal stage i, starters first. See ORCA.md, "Ranked steps".
//
// Game 1 is 1-2-1 on the starters. Later games, the winner strikes or bans, then the loser picks
// (never a stage they already won on). Casual games skip strikes: both players name a stage at
// once and a seeded coin decides between two different ones. In ranked, either player may propose
// a stage at any time, and matching proposals end the steps.
namespace Orca::UX::Ranked
{
using StageMask = u16;
constexpr int MAX_STAGES = 16;

enum class StepKind : u8
{
  Strike = 1,
  Ban = 2,
  Pick = 3,
  // A casual game's preferred stage, named by both players at once.
  Prefer = 4,
};

// The port of a step both players take at once (a casual Prefer).
constexpr u8 BOTH = 2;

struct Step
{
  StepKind kind = StepKind::Pick;
  u8 port = 0;            // 0, 1, or BOTH
  u8 count = 1;           // stages to strike or ban (1 for a pick)
  bool optional = false;  // a ban the player may skip
  u16 seconds = 30;       // turn timer
  bool operator==(const Step&) const = default;
};

// Whose turn it is on the stage select, as the screen labels it ("P2 STRIKES 2", "P1 PICKS").
struct Turn
{
  StepKind kind = StepKind::Pick;
  u8 port = 0;   // 0, 1, or BOTH
  int left = 1;  // strikes or bans still to make in this step (1 for a pick)
  bool operator==(const Turn&) const = default;
};

struct Ruleset
{
  u8 id = 0;  // MatchBlock's ruleset byte
  std::string_view name;
  int stage_count = 0;
  StageMask starters = 0;
  StageMask counterpicks = 0;
  int wins_needed = 2;  // best of 3
  // The winner's step in later games.
  StepKind later_kind = StepKind::Strike;
  u8 later_count = 2;
  bool later_optional = false;
  bool dsr = true;  // full DSR: the loser never picks a stage they won a game on
  // Brawl clauses. The stage Ice Climbers never play on, and the character kinds the clauses look
  // for. -1 means no such clause.
  int ic_banned_stage = -1;
  int ice_climbers = -1;
  int meta_knight = -1;
  // Turn timers in seconds: game 1's three steps, a later strike or ban, a later pick.
  std::array<u16, 3> game1_seconds{30, 30, 10};
  u16 later_seconds = 30;
  u16 pick_seconds = 30;
  // Grace after each timer before its default applies, shown as 0:00.
  u16 grace_frames = 180;
  // Casual games give both players 15 seconds to name a stage, with a shorter grace.
  u16 casual_seconds = 15;
  u16 casual_grace_frames = 30;
  // Short stage names for the overlay and logs.
  std::array<std::string_view, MAX_STAGES> stage_names{};

  StageMask All() const { return static_cast<StageMask>((1u << stage_count) - 1); }
};

// MatchBlock's ruleset ids.
constexpr u8 RULESET_NONE = 0;
constexpr u8 RULESET_BRAWL_SUPERNOVA_2025 = 1;
constexpr u8 RULESET_PPLUS_2024 = 2;

// Project+ 2024: BF, PS2, SV, LM, ToT; counterpicks GHZ, BC, FH, DL. Winner strikes 2.
const Ruleset& PPlus2024();
// Brawl, Supernova 2025: BF, PS1, LC, SV, YI; counterpicks FD, Delfino. Winner may ban 1. Meta
// Knight clause; Ice Climbers never on FD.
const Ruleset& Brawl2025();
// nullptr for none or an unknown id.
const Ruleset* RulesetById(u8 id);

// What a game's steps depend on.
struct SetView
{
  int game = 1;          // 1-based; a draw replays the same game number
  int last_winner = -1;  // port of the last counted game's winner; -1 before any
  int coin = 0;          // game 1's first striker, from the match block's fair coin
  std::array<StageMask, 2> won_on{};      // stages each port has won on
  std::array<int, 2> characters{-1, -1};  // character kinds; -1 unknown
  bool operator==(const SetView&) const = default;
};

struct Plan
{
  std::vector<Step> steps;
  // Stages in play when the steps start. The rest show as struck throughout.
  StageMask available = 0;
  // Stages the pick step also excludes (DSR).
  StageMask pick_excluded = 0;
  // Brawl's Meta Knight clause applies: one pick of any legal stage.
  bool mk_clause = false;
  bool casual = false;
  u32 seed = 0;  // casual games' random choices (CasualSeed)
  // Stages the players may agree on with Propose. DSR is ignored, and this game's strikes and
  // bans come off it as they are made.
  StageMask agreeable = 0;
};

Plan PlanGame(const Ruleset& rules, const SetView& set);

// ---- Casual games ----
// Mixes the room code hash, the match block's coin and the game number. Neither player can predict
// it, and every machine, re-run and drop-in keyframe computes the same value.
u32 CasualSeed(u32 room, u8 coin, int game);
// One Prefer step for both players on the whole legal list. No Meta Knight clause.
Plan PlanCasual(const Ruleset& rules, const SetView& set, u32 seed);
// The random legal stage a player gets if their Prefer timer runs out. -1 if none is allowed.
int CasualDefault(const Plan& plan, int port);
// Which port's stage is played when the two differ (one bit of the seed).
int CasualChoice(const Plan& plan);
// The stage played from both preferences. -1 for a port that named none uses its default.
int CasualResult(const Plan& plan, std::array<s8, 2> prefs);

// Stages the player at `step` may act on, given this game's strikes and bans so far.
StageMask Allowed(const Plan& plan, int step, StageMask struck);
// Timer defaults. The strikes still owed are the first allowed stages in list order (none for an
// optional ban). A pick defaults to the first allowed stage. 0 or -1 when nothing is allowed.
StageMask DefaultStrikes(const Plan& plan, int step, StageMask struck, int done);
int DefaultPick(const Plan& plan, int step, StageMask struck);
// A step's grace, and its full timer in frames including the grace.
u16 GraceFrames(const Ruleset& rules, const Step& step);
u32 StepFrames(const Ruleset& rules, const Step& step);

int CountBits(StageMask mask);
// The lowest `n` set bits of `mask`.
StageMask FirstBits(StageMask mask, int n);

// ---- Driving a game's steps one action and one frame at a time ----
// Used by a stage select that sees each action as it happens and counts its own frames (Brawl's).
// The state lives in the match block. Project+ keeps its own deadline instead (RankedPPlus.h).
struct Progress
{
  u8 step = 0;           // index into the plan; plan.steps.size() when done
  u8 done = 0;           // strikes or bans made in this step
  StageMask struck = 0;  // stages struck or banned this game
  u16 frames = 0;        // left on this step's timer, grace included
  s8 picked = -1;        // -1 until a stage is picked
  // Each port's proposed (ranked) or preferred (casual) stage, -1 for none.
  std::array<s8, 2> prefs{-1, -1};
  bool operator==(const Progress&) const = default;
};

// Starts the first step's timer, and picks at once if only one stage is left.
Progress Begin(const Ruleset& rules, const Plan& plan);
// The current step; nullptr once a stage is picked or the steps are done.
const Step* Current(const Plan& plan, const Progress& progress);
// Stages the current step allows; 0 when there is no step.
StageMask Allowed(const Plan& plan, const Progress& progress);
// Strikes or bans `stage` for the current player. Returns false and changes nothing unless the
// step allows it and it is not the last stage standing.
bool Strike(const Ruleset& rules, const Plan& plan, Progress* progress, int stage);
// Skips an optional ban. False if the step isn't optional.
bool Skip(const Ruleset& rules, const Plan& plan, Progress* progress);
// Records the game's pick. False if the step doesn't allow `stage`.
bool Pick(const Plan& plan, Progress* progress, int stage);
// `port` proposes `stage`, or names it in a casual game. Two matching ranked proposals pick it;
// two casual preferences pick CasualResult. False and no change if the stage isn't Proposable or
// is already this port's proposal.
bool Propose(const Plan& plan, Progress* progress, int port, int stage);
// Takes back `port`'s proposal. False if it had none or a stage is picked.
bool Withdraw(Progress* progress, int port);
// What a player may propose now; 0 once the stage is picked.
StageMask Proposable(const Plan& plan, const Progress& progress);
// A pick step with exactly one stage allowed picks it.
void AutoPick(const Plan& plan, Progress* progress);
// Advances the current step's timer one frame, applying its default when it runs out.
void Tick(const Ruleset& rules, const Plan& plan, Progress* progress);
// Whole seconds the timer shows, 0 during the grace. The first form uses the ruleset's grace.
int SecondsShown(const Ruleset& rules, u16 frames);
int SecondsShown(const Ruleset& rules, const Step& step, u16 frames);
}  // namespace Orca::UX::Ranked
