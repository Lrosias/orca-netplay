// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/UX/RankedSteps.h"

#include <algorithm>

namespace Orca::UX::Ranked
{
namespace
{
constexpr StageMask Bit(int i)
{
  return static_cast<StageMask>(1u << i);
}

Ruleset MakePPlus()
{
  Ruleset r;
  r.id = RULESET_PPLUS_2024;
  r.name = "Project+ 2024";
  r.stage_count = 9;
  r.starters = 0x001F;      // BF, PS2, SV, LM, ToT
  r.counterpicks = 0x01E0;  // GHZ, BC, FH, DL
  r.later_kind = StepKind::Strike;
  r.later_count = 2;
  r.later_optional = false;
  r.stage_names = {"Battlefield",     "Pokémon Stadium 2", "Smashville",
                   "Luigi's Mansion", "Temple of Time",    "Green Hill Zone",
                   "Bowser's Castle", "Frigate Husk",      "Dream Land"};
  return r;
}

Ruleset MakeBrawl()
{
  Ruleset r;
  r.id = RULESET_BRAWL_SUPERNOVA_2025;
  r.name = "Brawl Supernova 2025";
  r.stage_count = 7;
  r.starters = 0x001F;      // BF, PS1, LC, SV, YI
  r.counterpicks = 0x0060;  // FD, Delfino
  r.later_kind = StepKind::Ban;
  r.later_count = 1;
  r.later_optional = true;
  r.ic_banned_stage = 5;  // Final Destination
  r.ice_climbers = 0x10;  // gmCharacterKind
  r.meta_knight = 0x18;
  r.stage_names = {"Battlefield",    "Pokémon Stadium",   "Lylat Cruise", "Smashville",
                   "Yoshi's Island", "Final Destination", "Delfino Plaza"};
  return r;
}
}  // namespace

const Ruleset& PPlus2024()
{
  static const Ruleset r = MakePPlus();
  return r;
}

const Ruleset& Brawl2025()
{
  static const Ruleset r = MakeBrawl();
  return r;
}

const Ruleset* RulesetById(u8 id)
{
  if (id == RULESET_PPLUS_2024)
    return &PPlus2024();
  if (id == RULESET_BRAWL_SUPERNOVA_2025)
    return &Brawl2025();
  return nullptr;
}

int CountBits(StageMask mask)
{
  int n = 0;
  for (; mask; mask &= static_cast<StageMask>(mask - 1))
    ++n;
  return n;
}

StageMask FirstBits(StageMask mask, int n)
{
  StageMask out = 0;
  for (int i = 0; i < MAX_STAGES && n > 0; ++i)
  {
    if (mask & Bit(i))
    {
      out |= Bit(i);
      --n;
    }
  }
  return out;
}

Plan PlanGame(const Ruleset& rules, const SetView& set)
{
  Plan plan;
  const StageMask all = rules.All();
  StageMask legal = all;
  const auto is = [&](int port, int character) {
    return character >= 0 && set.characters[port] == character;
  };
  if (rules.ic_banned_stage >= 0 && (is(0, rules.ice_climbers) || is(1, rules.ice_climbers)))
  {
    legal &= static_cast<StageMask>(~Bit(rules.ic_banned_stage));
  }
  const int coin = set.coin == 1 ? 1 : 0;
  // Meta Knight clause: exactly one Meta Knight; the other player picks any legal stage.
  if (rules.meta_knight >= 0 && is(0, rules.meta_knight) != is(1, rules.meta_knight))
  {
    const u8 other = is(0, rules.meta_knight) ? 1 : 0;
    plan.available = legal;
    plan.agreeable = legal;
    plan.mk_clause = true;
    plan.steps.push_back({StepKind::Pick, other, 1, false, rules.pick_seconds});
    return plan;
  }
  if (set.game <= 1 || (set.last_winner != 0 && set.last_winner != 1))
  {
    // Game 1, or no counted game yet: 1-2-1 on the starters, ending in a pick.
    plan.available = rules.starters & legal;
    plan.agreeable = plan.available;
    const u8 first = static_cast<u8>(coin);
    const u8 second = static_cast<u8>(1 - coin);
    const int n = CountBits(plan.available);
    // With fewer than 5 starters, each step shrinks so one stage is always left to pick.
    const int s1 = std::min(1, std::max(0, n - 1));
    const int s2 = std::min(2, std::max(0, n - 1 - s1));
    if (s1 > 0)
      plan.steps.push_back(
          {StepKind::Strike, first, static_cast<u8>(s1), false, rules.game1_seconds[0]});
    if (s2 > 0)
      plan.steps.push_back(
          {StepKind::Strike, second, static_cast<u8>(s2), false, rules.game1_seconds[1]});
    plan.steps.push_back({StepKind::Pick, first, 1, false, rules.game1_seconds[2]});
    return plan;
  }
  const u8 winner = static_cast<u8>(set.last_winner);
  const u8 loser = static_cast<u8>(1 - set.last_winner);
  plan.available = legal;
  // Agreement ignores DSR.
  plan.agreeable = legal;
  if (rules.dsr)
    plan.available &= static_cast<StageMask>(~set.won_on[loser]);
  // Never more strikes than leave one stage to pick.
  const int n = CountBits(plan.available);
  const int count = std::min<int>(rules.later_count, std::max(0, n - 1));
  if (count > 0)
  {
    plan.steps.push_back({rules.later_kind, winner, static_cast<u8>(count), rules.later_optional,
                          rules.later_seconds});
  }
  plan.steps.push_back({StepKind::Pick, loser, 1, false, rules.pick_seconds});
  return plan;
}

namespace
{
// murmur3's 32-bit finalizer.
constexpr u32 Mix(u32 h)
{
  h ^= h >> 16;
  h *= 0x85EBCA6Bu;
  h ^= h >> 13;
  h *= 0xC2B2AE35u;
  h ^= h >> 16;
  return h;
}
}  // namespace

u32 CasualSeed(u32 room, u8 coin, int game)
{
  return Mix(room ^ Mix(static_cast<u32>(game) * 2u + (coin & 1u) + 0x9E3779B9u));
}

Plan PlanCasual(const Ruleset& rules, const SetView& set, u32 seed)
{
  Plan plan;
  plan.casual = true;
  plan.seed = seed;
  plan.available = rules.All();
  const auto is = [&](int port, int character) {
    return character >= 0 && set.characters[port] == character;
  };
  if (rules.ic_banned_stage >= 0 && (is(0, rules.ice_climbers) || is(1, rules.ice_climbers)))
    plan.available &= static_cast<StageMask>(~Bit(rules.ic_banned_stage));
  plan.agreeable = plan.available;
  // Both players at once: one step, one timer.
  plan.steps.push_back({StepKind::Prefer, BOTH, 1, false, rules.casual_seconds});
  return plan;
}

int CasualDefault(const Plan& plan, int port)
{
  const int n = CountBits(plan.available);
  if (n == 0)
    return -1;
  int k = static_cast<int>(Mix(plan.seed ^ (0x27D4EB2Fu * static_cast<u32>(port + 1))) %
                           static_cast<u32>(n));
  for (int i = 0; i < MAX_STAGES; ++i)
  {
    if ((plan.available & Bit(i)) && k-- == 0)
      return i;
  }
  return -1;
}

int CasualChoice(const Plan& plan)
{
  return static_cast<int>(Mix(plan.seed ^ 0x165667B1u) & 1);
}

int CasualResult(const Plan& plan, std::array<s8, 2> prefs)
{
  for (int port = 0; port < 2; ++port)
  {
    if (prefs[port] < 0)
      prefs[port] = static_cast<s8>(CasualDefault(plan, port));
  }
  if (prefs[0] == prefs[1])
    return prefs[0];
  const int chosen = prefs[CasualChoice(plan)];
  return chosen >= 0 ? chosen : std::max<int>(prefs[0], prefs[1]);
}

StageMask Allowed(const Plan& plan, int step, StageMask struck)
{
  if (step < 0 || step >= static_cast<int>(plan.steps.size()))
    return 0;
  StageMask allowed = plan.available & static_cast<StageMask>(~struck);
  if (plan.steps[step].kind == StepKind::Pick)
    allowed &= static_cast<StageMask>(~plan.pick_excluded);
  return allowed;
}

StageMask DefaultStrikes(const Plan& plan, int step, StageMask struck, int done)
{
  if (step < 0 || step >= static_cast<int>(plan.steps.size()))
    return 0;
  const Step& s = plan.steps[step];
  if (s.kind == StepKind::Pick || s.kind == StepKind::Prefer || s.optional)
    return 0;
  const StageMask allowed = Allowed(plan, step, struck);
  // Never strike the last stage standing.
  const int owed = std::min(s.count - done, CountBits(allowed) - 1);
  return owed > 0 ? FirstBits(allowed, owed) : 0;
}

int DefaultPick(const Plan& plan, int step, StageMask struck)
{
  const StageMask allowed = Allowed(plan, step, struck);
  for (int i = 0; i < MAX_STAGES; ++i)
  {
    if (allowed & Bit(i))
      return i;
  }
  return -1;
}

u16 GraceFrames(const Ruleset& rules, const Step& step)
{
  return step.kind == StepKind::Prefer ? rules.casual_grace_frames : rules.grace_frames;
}

u32 StepFrames(const Ruleset& rules, const Step& step)
{
  return static_cast<u32>(step.seconds) * 60 + GraceFrames(rules, step);
}

namespace
{
void Enter(const Ruleset& rules, const Plan& plan, Progress* progress, size_t step)
{
  progress->step = static_cast<u8>(step);
  progress->done = 0;
  progress->frames =
      step < plan.steps.size() ? static_cast<u16>(StepFrames(rules, plan.steps[step])) : u16(0);
}
}  // namespace

Progress Begin(const Ruleset& rules, const Plan& plan)
{
  Progress progress;
  Enter(rules, plan, &progress, 0);
  AutoPick(plan, &progress);
  return progress;
}

const Step* Current(const Plan& plan, const Progress& progress)
{
  if (progress.picked >= 0 || progress.step >= plan.steps.size())
    return nullptr;
  return &plan.steps[progress.step];
}

StageMask Allowed(const Plan& plan, const Progress& progress)
{
  return Current(plan, progress) ? Allowed(plan, progress.step, progress.struck) : StageMask(0);
}

bool Strike(const Ruleset& rules, const Plan& plan, Progress* progress, int stage)
{
  const Step* step = Current(plan, *progress);
  if (!step || step->kind == StepKind::Pick || step->kind == StepKind::Prefer || stage < 0 ||
      stage >= MAX_STAGES)
  {
    return false;
  }
  const StageMask allowed = Allowed(plan, *progress);
  if (!(allowed & Bit(stage)) || CountBits(allowed) <= 1)
    return false;
  progress->struck = static_cast<StageMask>(progress->struck | Bit(stage));
  ++progress->done;
  // A proposal on a struck stage is dropped.
  for (s8& pref : progress->prefs)
  {
    if (pref == stage)
      pref = -1;
  }
  if (progress->done >= step->count || CountBits(Allowed(plan, *progress)) <= 1)
    Enter(rules, plan, progress, progress->step + 1);
  AutoPick(plan, progress);
  return true;
}

bool Skip(const Ruleset& rules, const Plan& plan, Progress* progress)
{
  const Step* step = Current(plan, *progress);
  if (!step || !step->optional)
    return false;
  Enter(rules, plan, progress, progress->step + 1);
  AutoPick(plan, progress);
  return true;
}

bool Pick(const Plan& plan, Progress* progress, int stage)
{
  const Step* step = Current(plan, *progress);
  if (!step || step->kind != StepKind::Pick || stage < 0 || stage >= MAX_STAGES)
    return false;
  if (!(Allowed(plan, *progress) & Bit(stage)))
    return false;
  progress->picked = static_cast<s8>(stage);
  return true;
}

StageMask Proposable(const Plan& plan, const Progress& progress)
{
  if (progress.picked >= 0)
    return 0;
  if (plan.casual)
    return Allowed(plan, progress);
  return static_cast<StageMask>(plan.agreeable & ~progress.struck);
}

bool Propose(const Plan& plan, Progress* progress, int port, int stage)
{
  if (port < 0 || port > 1 || stage < 0 || stage >= MAX_STAGES)
    return false;
  if (!(Proposable(plan, *progress) & Bit(stage)) || progress->prefs[port] == stage)
    return false;
  progress->prefs[port] = static_cast<s8>(stage);
  const auto& prefs = progress->prefs;
  if (plan.casual && prefs[0] >= 0 && prefs[1] >= 0)
    progress->picked = static_cast<s8>(CasualResult(plan, prefs));
  else if (!plan.casual && prefs[0] >= 0 && prefs[0] == prefs[1])
    progress->picked = prefs[0];  // matching proposals skip the remaining steps
  return true;
}

bool Withdraw(Progress* progress, int port)
{
  if (port < 0 || port > 1 || progress->picked >= 0 || progress->prefs[port] < 0)
    return false;
  progress->prefs[port] = -1;
  return true;
}

void AutoPick(const Plan& plan, Progress* progress)
{
  const Step* step = Current(plan, *progress);
  if (!step || step->kind != StepKind::Pick)
    return;
  const StageMask allowed = Allowed(plan, *progress);
  if (CountBits(allowed) == 1)
    progress->picked = static_cast<s8>(DefaultPick(plan, progress->step, progress->struck));
}

void Tick(const Ruleset& rules, const Plan& plan, Progress* progress)
{
  const Step* step = Current(plan, *progress);
  if (!step)
    return;
  if (progress->frames > 0)
    --progress->frames;
  if (progress->frames > 0)
    return;
  if (step->kind == StepKind::Pick)
  {
    const int stage = DefaultPick(plan, progress->step, progress->struck);
    progress->picked = static_cast<s8>(stage >= 0 ? stage : 0);
    return;
  }
  if (step->kind == StepKind::Prefer)
  {
    // Whoever named nothing gets a random legal stage, then the coin decides between the two.
    for (int port = 0; port < 2; ++port)
    {
      if (progress->prefs[port] < 0)
        progress->prefs[port] = static_cast<s8>(CasualDefault(plan, port));
    }
    const int stage = CasualResult(plan, progress->prefs);
    progress->picked =
        static_cast<s8>(stage >= 0 ? stage : std::max(0, DefaultPick(plan, progress->step, 0)));
    return;
  }
  if (step->optional)
  {
    Skip(rules, plan, progress);
    return;
  }
  const StageMask defaults =
      DefaultStrikes(plan, progress->step, progress->struck, progress->done);
  for (int i = 0; i < MAX_STAGES; ++i)
  {
    if ((defaults & Bit(i)) && Current(plan, *progress) == step)
      Strike(rules, plan, progress, i);
  }
  // Nothing left to strike: the step ends anyway.
  if (Current(plan, *progress) == step)
  {
    Enter(rules, plan, progress, progress->step + 1);
    AutoPick(plan, progress);
  }
}

int SecondsShown(const Ruleset& rules, u16 frames)
{
  if (frames <= rules.grace_frames)
    return 0;
  return (frames - rules.grace_frames + 59) / 60;
}

int SecondsShown(const Ruleset& rules, const Step& step, u16 frames)
{
  const u16 grace = GraceFrames(rules, step);
  if (frames <= grace)
    return 0;
  return (frames - grace + 59) / 60;
}
}  // namespace Orca::UX::Ranked
