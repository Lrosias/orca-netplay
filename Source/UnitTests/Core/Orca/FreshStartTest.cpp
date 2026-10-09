// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

// The origin and fresh starts (Core/Rollback/OnlineMatch.h, ORCA.md "Drop-in"): every Orca boots
// the same way to its origin at the built main menu, and a host goes back there before a queue
// match, or before a friend joins a game it has played alone for a while, so a join replays only the
// match's own frames.

#include <optional>

#include <gtest/gtest.h>

#include "Core/Orca/Session/PadCodec.h"
#include "Core/Orca/Session/Replay.h"
#include "Core/Rollback/OnlineMatch.h"

using namespace Rollback::OnlineMatch;

namespace
{
// A solo host on the menus with an origin, nobody arriving.
FreshInputs SoloHost(int history_frames = 10 * 60 * 60)
{
  FreshInputs in;
  in.enabled = true;
  in.origin_ready = true;
  in.history_frames = history_frames;
  return in;
}
}  // namespace

TEST(OrcaCanonicalBoot, EveryBootPlaysPort1WithNothingPressedAndNobodyElse)
{
  const Orca::Net::Pads pads = CanonicalBootPads();
  EXPECT_EQ(pads[0], Orca::Net::Pad{});
  const GCPadStatus one = Orca::Net::DecodePad(pads[0]);
  EXPECT_TRUE(one.isConnected);
  EXPECT_EQ(one.button, 0);
  // As values: the header's constants have no definition to bind to.
  EXPECT_EQ(int{one.stickX}, int{GCPadStatus::MAIN_STICK_CENTER_X});
  EXPECT_EQ(int{one.stickY}, int{GCPadStatus::MAIN_STICK_CENTER_Y});
  for (int port = 1; port < Orca::Net::MAX_SEATS; ++port)
  {
    EXPECT_EQ(pads[port], Orca::Net::UNPLUGGED_PAD) << port;
    EXPECT_FALSE(Orca::Net::DecodePad(pads[port]).isConnected) << port;
  }
}

TEST(OrcaCanonicalBoot, TheOriginIsTheMainMenuBuiltForHalfASecond)
{
  // 30 frames of a built main menu make the origin (Brawl Rev 2 measures 1524, Project+ 230).
  EXPECT_EQ(DecideOrigin(1441, 0, std::nullopt), OriginStep::Wait);
  EXPECT_EQ(DecideOrigin(1470, ORIGIN_SETTLE_FRAMES - 1, std::nullopt), OriginStep::Wait);
  EXPECT_EQ(DecideOrigin(1471, ORIGIN_SETTLE_FRAMES, std::nullopt), OriginStep::Capture);
  // Project+: after its JIT clear (frame 2).
  EXPECT_EQ(DecideOrigin(229, ORIGIN_SETTLE_FRAMES, 2u), OriginStep::Capture);
  EXPECT_EQ(DecideOrigin(2, ORIGIN_SETTLE_FRAMES, 2u), OriginStep::Wait);
  // No built menu in time: no origin.
  EXPECT_EQ(DecideOrigin(ORIGIN_DEADLINE - 1, 3, std::nullopt), OriginStep::Wait);
  EXPECT_EQ(DecideOrigin(ORIGIN_DEADLINE, 3, std::nullopt), OriginStep::Fail);
  EXPECT_EQ(DecideOrigin(ORIGIN_DEADLINE, ORIGIN_SETTLE_FRAMES, std::nullopt), OriginStep::Capture);
}

TEST(OrcaFreshStart, AQueueRoomsHostFreshStartsOnceItsWelcomeCame)
{
  FreshInputs in = SoloHost();
  in.queue_armed = true;
  // `host` came, the welcome hasn't: no keyframe yet.
  EXPECT_EQ(DecideFreshStart(in), FreshStep::Hold);
  in.waiting = true;  // the opponent's hello came first
  EXPECT_EQ(DecideFreshStart(in), FreshStep::Hold);
  in.in_queue_room = true;
  EXPECT_EQ(DecideFreshStart(in), FreshStep::Now);
  // However short the host's history.
  in.history_frames = 3;
  EXPECT_EQ(DecideFreshStart(in), FreshStep::Now);
  // Under way (or a keyframe being made): wait for it.
  in.busy = true;
  EXPECT_EQ(DecideFreshStart(in), FreshStep::Hold);
  // Done (the arm is spent): the opponent joins in place, never a second fresh start in that room.
  in.busy = false;
  in.queue_armed = false;
  EXPECT_EQ(DecideFreshStart(in), FreshStep::None);
  in.history_frames = 40 * 60 * 60;
  EXPECT_EQ(DecideFreshStart(in), FreshStep::None);
}

TEST(OrcaFreshStart, AFriendAfterTenSecondsFreshStartsTheHost)
{
  FreshInputs in = SoloHost(FRESH_FRIENDS_AFTER + 1);
  // Nobody arriving: nothing.
  EXPECT_EQ(DecideFreshStart(in), FreshStep::None);
  in.waiting = true;
  EXPECT_EQ(DecideFreshStart(in), FreshStep::Now);
  // A host that just booted or just fresh-started takes the friend in place.
  in.history_frames = FRESH_FRIENDS_AFTER;
  EXPECT_EQ(DecideFreshStart(in), FreshStep::None);
  in.history_frames = 120;
  EXPECT_EQ(DecideFreshStart(in), FreshStep::None);
  // The test knob's threshold.
  in.fresh_after = 60;
  EXPECT_EQ(DecideFreshStart(in), FreshStep::Now);
  in.fresh_after = 0;
  in.history_frames = 1;
  EXPECT_EQ(DecideFreshStart(in), FreshStep::Now);
}

TEST(OrcaFreshStart, AFriendTakenInPlaceStaysInPlace)
{
  // Arrived at 9 s of history: in place. Its keyframe takes a second to store, and the history
  // passes 10 s meanwhile: still in place, no fresh start under it.
  FreshInputs in = SoloHost(FRESH_FRIENDS_AFTER - 60);
  in.waiting = true;
  EXPECT_EQ(DecideFreshStart(in), FreshStep::None);
  in.in_place = true;
  in.history_frames = FRESH_FRIENDS_AFTER + 60;
  EXPECT_EQ(DecideFreshStart(in), FreshStep::None);
  // The next friend, once nobody waits, decides again.
  in.in_place = false;
  EXPECT_EQ(DecideFreshStart(in), FreshStep::Now);
}

TEST(OrcaFreshStart, ASinglePlayerModeHoldsTheFriendFirst)
{
  FreshInputs in = SoloHost();
  in.waiting = true;
  in.single_player = true;
  EXPECT_EQ(DecideFreshStart(in), FreshStep::Hold);
  in.single_player = false;
  EXPECT_EQ(DecideFreshStart(in), FreshStep::Now);
}

TEST(OrcaFreshStart, NeverInASessionOrWhileJoining)
{
  for (int which = 0; which < 3; ++which)
  {
    FreshInputs in = SoloHost();
    in.waiting = true;
    in.queue_armed = which == 0;
    in.in_queue_room = which == 0;
    in.session = which == 0;
    in.joining = which == 1;
    in.seated_or_plugging = which == 2;
    EXPECT_EQ(DecideFreshStart(in), FreshStep::None) << which;
  }
}

TEST(OrcaFreshStart, NoOriginMeansWaitingAndTheKnobTurnsItOff)
{
  FreshInputs in = SoloHost();
  in.waiting = true;
  in.origin_ready = false;
  EXPECT_EQ(DecideFreshStart(in), FreshStep::Hold);
  in.origin_ready = true;
  in.busy = true;
  EXPECT_EQ(DecideFreshStart(in), FreshStep::Hold);
  // ORCA_TEST_FRESH=off, or a game whose origin is its first frame: joins in place.
  in.busy = false;
  in.enabled = false;
  EXPECT_EQ(DecideFreshStart(in), FreshStep::None);
  in.queue_armed = true;
  in.in_queue_room = true;
  EXPECT_EQ(DecideFreshStart(in), FreshStep::None);
}

TEST(OrcaFreshStart, AFriendInAQueueRoomIsTheQueuesBusiness)
{
  // A queue room's opponent never takes the friends path.
  FreshInputs in = SoloHost();
  in.waiting = true;
  in.in_queue_room = true;
  EXPECT_EQ(DecideFreshStart(in), FreshStep::None);
}

TEST(OrcaFreshStart, AFriendLandsOnTheSelectTheHostIsOn)
{
  EXPECT_EQ(FriendsFreshExit(std::nullopt), Orca::Net::MENU_EXIT_FRIENDS);
  EXPECT_EQ(FriendsFreshExit(false), Orca::Net::MENU_EXIT_CASUAL);
  EXPECT_EQ(FriendsFreshExit(true), Orca::Net::MENU_EXIT_RANKED);
  for (const u8 exit : {Orca::Net::MENU_EXIT_FRIENDS, Orca::Net::MENU_EXIT_CASUAL,
                        Orca::Net::MENU_EXIT_RANKED})
  {
    EXPECT_TRUE(Orca::Net::ValidMenuExit(exit));
  }
  EXPECT_FALSE(Orca::Net::ValidMenuExit(0));
  EXPECT_FALSE(Orca::Net::ValidMenuExit(24));
}

// A queue room's host runs its hold for the room's header and its own pick being put back unseen
// and unthrottled, like the tail, but never longer than FRESH_UNSEEN_LIMIT frames; a friend's fresh
// start has neither, so nothing of it runs unseen past the tail.
TEST(OrcaFreshStart, AQueueHostsHoldAndSteerRunUnseenUpToALimit)
{
  for (int frames = 0; frames < FRESH_UNSEEN_LIMIT; ++frames)
    EXPECT_TRUE(FreshUnseen(true, frames)) << frames;
  EXPECT_FALSE(FreshUnseen(true, FRESH_UNSEEN_LIMIT));
  EXPECT_FALSE(FreshUnseen(true, FRESH_UNSEEN_LIMIT + 1000));
  EXPECT_FALSE(FreshUnseen(false, 0));
  // About two seconds at 60 fps, well past the ~70 frames the steer takes, far short of the
  // steer's own 10 s limit, which would freeze the picture for seconds.
  EXPECT_GE(FRESH_UNSEEN_LIMIT, 100);
  EXPECT_LE(FRESH_UNSEEN_LIMIT, 180);
}

// The header goes in during the unseen hold (it is a first run, so the hook writes and records it),
// never in a tail (a re-run) or a joiner's rebuild.
TEST(OrcaFreshStart, TheHeaderGoesInOnlyDuringTheUnseenHold)
{
  EXPECT_TRUE(HeaderFreeWhileCatchingUp(true, false, false));
  EXPECT_FALSE(HeaderFreeWhileCatchingUp(false, false, false));
  EXPECT_FALSE(HeaderFreeWhileCatchingUp(true, true, false));
  EXPECT_FALSE(HeaderFreeWhileCatchingUp(true, false, true));
  EXPECT_FALSE(HeaderFreeWhileCatchingUp(false, true, true));
}

// The queue room's header limit counts only boundaries that run at the game's pace: a tail's and
// an unseen hold's don't count, so the join never fails sooner in real time because they ran
// unthrottled. Once the header is in, the hold is ready at once, unseen or not.
TEST(OrcaFreshStart, UnseenBoundariesNeverCountTowardTheHeaderLimit)
{
  int waited = 0;
  for (int i = 0; i < 10 * HEADER_FAIL_BOUNDARIES; ++i)
  {
    ASSERT_EQ(HeaderWaitAt(&waited, true, false, false, true, true), HeaderWait::Wait);
    ASSERT_EQ(HeaderWaitAt(&waited, false, true, false, true, true), HeaderWait::Wait);
  }
  EXPECT_EQ(waited, 0);
  EXPECT_EQ(HeaderWaitAt(&waited, false, true, true, true, true), HeaderWait::Ready);
  // Shown again with the header still not in: the usual limit, counted from there.
  for (int i = 0; i < HEADER_FAIL_BOUNDARIES; ++i)
    ASSERT_EQ(HeaderWaitAt(&waited, false, false, false, true, true), HeaderWait::Wait) << i;
  EXPECT_EQ(HeaderWaitAt(&waited, false, false, false, true, true), HeaderWait::Fail);
  // Unchanged outside a fresh start: a friends room makes the keyframe anyway after its wait.
  waited = 0;
  for (int i = 0; i < HEADER_WAIT_BOUNDARIES; ++i)
    ASSERT_EQ(HeaderWaitAt(&waited, false, false, false, true, false), HeaderWait::Wait) << i;
  EXPECT_EQ(HeaderWaitAt(&waited, false, false, false, true, false), HeaderWait::Ready);
}
