// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/UX/UX.h"

#include <chrono>
#include <cstdlib>
#include <utility>
#include <vector>

#include "Common/Logging/Log.h"
#include "Core/Orca/Session/Events.h"
#include "Core/Orca/UX/BrawlStages.h"
#include "Core/Orca/UX/CharOrder.h"
#include "Core/Orca/UX/ControllerSource.h"
#include "Core/Orca/UX/CssTitle.h"
#include "Core/Orca/UX/GamePatches.h"
#include "Core/Orca/UX/Kit.h"
#include "Core/Orca/UX/MenuText.h"
#include "Core/Orca/UX/NameTags.h"
#include "Core/Orca/UX/NativeText.h"
#include "Core/Orca/UX/OnlineRules.h"
#include "Core/Orca/UX/OnlineMenu.h"
#include "Core/Orca/UX/Overlay.h"
#include "Core/Orca/UX/Probe.h"
#include "Core/Orca/UX/Queue.h"
#include "Core/Orca/UX/RankedPPlus.h"
#include "Core/Orca/UX/RankedSet.h"
#include "Core/Orca/UX/Relabel.h"
#include "Core/Orca/UX/Results.h"
#include "Core/Orca/UX/StageCursors.h"
#include "Core/Rollback/InputGate.h"

namespace Orca::UX
{
namespace
{
// ORs `more` into `masks`. An input any rule takes is taken.
void Merge(Rollback::InputGate::Masks& masks, const Rollback::InputGate::Masks& more)
{
  for (int port = 0; port < Rollback::InputGate::PORTS; ++port)
  {
    masks[port].buttons |= more[port].buttons;
    masks[port].press |= more[port].press;
    masks[port].a_as |= more[port].a_as;
    masks[port].main_stick = masks[port].main_stick || more[port].main_stick;
    masks[port].c_stick = masks[port].c_stick || more[port].c_stick;
    masks[port].a_centres_stick = masks[port].a_centres_stick || more[port].a_centres_stick;
    if (more[port].steer)
    {
      masks[port].steer = true;
      masks[port].steer_x = more[port].steer_x;
      masks[port].steer_y = more[port].steer_y;
    }
  }
}
}  // namespace

void SetGameDisc(std::string path)
{
  Kit::SetGameDisc(std::move(path));
}

void Init()
{
  StartControllerStreamFromEnvironment();
  Orca::Events::SetLocalPadSource([](bool for_frame) { return LocalPad(for_frame); });
  Orca::Events::SetInputAgeSource([] { return GetInputAge(); });
  InitOverlay();
  // The input gate's masks for the frame about to run: every rule's masks ORed, from memory alone.
  Rollback::InputGate::SetSource([](const Core::CPUThreadGuard& guard) {
    Rollback::InputGate::Masks masks{};
    if (Rules::ProfileRuleset() == Rules::Ruleset::None)
      return masks;
    GuardMemory memory(guard);
    // No Z on the results screen in any session. Its replay save is a dead end online.
    Merge(masks, Rules::SessionMasks(memory));
    Merge(masks, Rules::GateMasks(memory));
    Merge(masks, RankedPPlus::Masks(memory));
    BrawlStages::GateMasks(guard, &masks);
    Rollback::InputGate::Masks order = CharOrder::GateFrame(guard);
    // On a queue room's character select the ready timer presses Start, so the character order's
    // own Start is dropped.
    if (Queue::OwnsStart(guard))
    {
      for (Rollback::InputGate::Mask& mask : order)
        mask.press &= static_cast<u16>(~PAD_BUTTON_START);
    }
    Merge(masks, order);
    Merge(masks, Queue::GateFrame(guard));
    return masks;
  });
  // Each frame's raw buttons go into the match block before it runs, so the queue's character
  // select still sees presses the gate hides from the game (Start, B, Z).
  Rollback::InputGate::SetLatch(
      [](const Core::CPUThreadGuard& guard,
         const std::array<std::optional<GCPadStatus>, Rollback::InputGate::PORTS>& raw) {
        Queue::LatchFrame(guard, raw);
        // On the stage select, each player's stick moves their own cursor.
        StageCursors::LatchFrame(guard, raw);
      });
  Orca::Events::SetUXCompatVersion(kCompatVersion);
  // Runs at every frame boundary, on first runs and rollback re-runs, before the session saves
  // state there. Every memory write below must be a pure function of emulated memory, the frame
  // number and the synced ports, so a re-run writes exactly what the first run did.
  Orca::Events::SetFrameCallback([](const Core::CPUThreadGuard& guard, int frame, bool resimulating,
                                    const std::vector<Orca::Events::PortInfo>& ports, bool alone) {
    static bool s_logged = false;
    if (!std::exchange(s_logged, true))
      NOTICE_LOG_FMT(ROLLBACK, "Orca UX: frame hook live at frame {}", frame);
    ApplyGamePatches(guard);
    // The main menu's text about going online.
    WriteMenuText(guard);
    // Read only. Tells the app what the player picked in the game's Online menu.
    ReadOnlineMenu(guard, resimulating, alone);
    // A friend who joins while the host is in the menus moves both to the With Friends character
    // select.
    FriendsMove::Frame(guard, frame, resimulating, ports);
    // Tracks a ranked set in the match block.
    RankedSet::Frame(guard, frame, resimulating);
    // Read only. Reports each game's result in a matchmade room once its frames are final.
    ReadResultsFrame(guard, frame);
    // Project+'s ranked stage flow.
    RankedPPlus::Frame(guard, frame, resimulating, ports, alone);
    // A ranked set's stage flow on vanilla Brawl's stage select.
    BrawlStages::FrameHook(guard, resimulating, ports);
    // Read only, first runs only. Publishes the stage select cursors for the overlay.
    if (!resimulating && Rules::ProfileRuleset() != Rules::Ruleset::None)
    {
      GuardMemory memory(guard);
      StageCursors::View view = Rules::ProfileRuleset() == Rules::Ruleset::Brawl ?
                                    BrawlStages::CursorView(memory, ports) :
                                    RankedPPlus::CursorView(memory, ports, frame);
      StageCursors::Publish(std::move(view));
    }
    ProbeFrame(guard, frame);
    // Local display only. The overlay matches the session's game.
    Kit::SetLook(Rules::ProfileRuleset() == Rules::Ruleset::PPlus ? Kit::Look::ProjectPlus :
                                                                    Kit::Look::Brawl);
    // Read only. Captures this player's own controls while playing alone, so the session can carry
    // them into a game it joins or hosts.
    if (alone)
      ReadOwnControlsFrame(guard, ports);
    // Test only, ORCA_UX_TEST_LATE_NAME=<frame>. Simulates a friend's name arriving late: first
    // runs before that frame see it empty and re-runs see it, to show the desync the session's
    // synced value channel prevents.
    static const int s_late = [] {
      const char* v = std::getenv("ORCA_UX_TEST_LATE_NAME");
      return v ? std::atoi(v) : -1;
    }();
    // Checked every frame because a host that boots solo can have a friend join later.
    if (s_late >= 0 && !resimulating && frame < s_late && TestKnobsAllowed())
    {
      std::vector<Orca::Events::PortInfo> early = ports;
      // PortInfo.remote differs per machine: the friend is whoever is not on port 1.
      for (Orca::Events::PortInfo& p : early)
      {
        if (p.port != 0)
        {
          p.name.clear();
          p.controls.clear();
        }
      }
      WriteNameTags(guard, early);
    }
    else
    {
      WriteNameTags(guard, ports);
    }
    // The online rules: rule, stage and ready-timer locks for queue rooms.
    u8 plugged = 0;
    for (const Orca::Events::PortInfo& p : ports)
    {
      if (p.port >= 0 && p.port < 4)
        plugged |= static_cast<u8>(1 << p.port);
    }
    Rules::OnFrame(guard, frame, resimulating, alone, plugged);
    // A ranked set's character order for later games. The input gate reads its state from the
    // match block.
    CharOrder::Frame(guard, frame, resimulating, ports);
    // The queue's character select: ready flags, timer and the joiner's pick. Runs after the
    // character order, which a ranked later game's ready timer waits for.
    Queue::Frame(guard, frame, resimulating, ports, alone);
    // The character select's title: CASUAL, RANKED or FRIENDS.
    CssTitle::Frame(guard, resimulating, ports);
    // Relabels the game's big titles, such as READY TO FIGHT! and STAGE SELECT.
    const Relabel::Shown labels = Relabel::Frame(guard, resimulating, ports);
    // Orca's text in the character select's own text boxes: player names and the rules bar line.
    NativeText::Frame(guard, resimulating, ports, Relabel::ClockInBand(labels));
    // Must run last. Toggled patch groups follow the state the writers above left, so a re-run of
    // this boundary decides the same and writes nothing new.
    ApplyToggledGamePatches(guard);
  });
  // While the game waits for a friend, re-present the last frame with "Waiting for <name>...".
  Orca::Events::SetStallCallback([](int stalled_ms) { RepresentDuringStall(stalled_ms); });
  Orca::Events::SetBoundaryCallback([](bool shown) { FrameBoundary(shown); });
  // For joined/left toasts only. Never a source for memory writes; those use the frame callback.
  Orca::Events::SetPlugInCallback([](int frame, const std::vector<Orca::Events::PortInfo>& ports) {
    const double now = std::chrono::duration<double, std::milli>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count();
    Overlay().OnPorts(frame, ports, now);
  });
}

void Shutdown()
{
  Rollback::InputGate::SetSource({});
  Rollback::InputGate::SetLatch({});
  Orca::Events::SetStallCallback({});
  Orca::Events::SetBoundaryCallback({});
  Orca::Events::SetFrameCallback({});
  Orca::Events::SetPlugInCallback({});
  Orca::Events::SetLocalPadSource({});
  Orca::Events::SetInputAgeSource({});
  ShutdownOverlay();
  StopControllerStream();
}
}  // namespace Orca::UX
