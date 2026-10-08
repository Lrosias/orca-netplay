// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <picojson.h>

#include "Common/CommonTypes.h"
#include "Core/Orca/Session/Session.h"

// A private YouGame room carrying an Orca drop-in session. The host plays from boot and friends
// join by room code (up to four players). Each player's seat is its room slot (seat 0, the host, is
// port 1), and session packets travel as relayed room messages. Tickets come from the YouGame
// desktop app's loopback bridge (YOUGAME_BRIDGE / YOUGAME_TOKEN), or for tests from the site as an
// anonymous dev game (ORCA_TEST_DEV_GAME), whose rooms never meet real players. All network work
// runs on one thread.
namespace Orca::Net
{
struct RoomOptions
{
  // 6-12 characters [a-z0-9]. Empty: the desktop app's invite (hello.room), else a new code.
  std::string room_code;
  // Lobby compatibility key: players with different keys can't share a room.
  std::string compatibility;
  std::string player_name = "Player";
  // The room's mode, one per game (e.g. "orca-rsbe01"). Both players must match.
  std::string mode = "orca-test";
  // This Orca joins a friend's running game (ORCA_JOIN=1, or the app opened an invite).
  bool joining = false;
  // A joiner's own controls (Orca::Events::OwnControls), sent in its hello so the host puts them in
  // its port (KeyframeInfo::Name::controls). Empty: none.
  std::vector<u8> controls;
  // A joiner's queue identity (Orca::Events::OwnQueue), sent like the controls. Empty: none.
  std::vector<u8> queue;
  // With no room_code, ask the app whether the player opened an invite. Only at launch: a room
  // opened later, after a leave, is always new.
  bool ask_app_for_room = true;
};

// Where the host's keyframe is, for a joining friend (Orca::Net::KeyframeStore).
struct KeyframeInfo
{
  int frame = -1;
  std::string id;
  u64 size = 0;
  std::string hash;
  // The keyframe's AES-256-GCM key (64 hex); only ever sent in the room's `kf` message.
  std::string key;
  // The host's values for the game's ports (who plays each, with which controls), each from the
  // frame it applies. Joiners name ports exactly as the host does, never from their own view of the
  // roster.
  struct Name
  {
    int seat = 0;
    int from = 0;
    std::string name;
    // The player's own controls (at most MAX_CONTROLS bytes; empty: none).
    std::vector<u8> controls;
    // The player's queue identity (at most MAX_QUEUE bytes; empty: none).
    std::vector<u8> queue;
    bool operator==(const Name&) const = default;
  };
  std::vector<Name> names;
  // The host's count of changes to `names` (Session::RequireValues). Players acknowledge the newest
  // they hold, and the host plugs a newcomer in only once every player holds its values.
  int names_version = 0;
};

// Max wire size of a port's controls (UX/NameTags.h's profile is 46).
constexpr size_t MAX_CONTROLS = 64;
// Max wire size of a port's queue identity (UX/Queue.h's is 13).
constexpr size_t MAX_QUEUE = 32;

// Something another player in the room did.
struct PeerEvent
{
  enum class Kind
  {
    // It said hello with our compatibility key: a player of this game at `seat`.
    Arrived,
    // It left the room or its connection dropped.
    Left,
    // The host offered its keyframe (to a joiner).
    Keyframe,
    // The host's port names changed (a friend was taken in): `keyframe.names` only.
    Names,
    // The host starts or stops holding this joiner's join (`holding`), e.g. while its game is in a
    // single-player mode. Sent only to a joiner without a keyframe yet.
    Hold,
  };
  Kind kind = Kind::Arrived;
  int seat = -1;
  // The player is the game's host (the Orca that started it).
  bool host = false;
  std::string name;
  // Arrived: the joiner's own controls from its hello (empty: none).
  std::vector<u8> controls;
  KeyframeInfo keyframe;
  // Left: the reason it gave ("desync" or "stalled" from its bye; "mismatch" from a host whose
  // queue room's match can't start; empty: it just left).
  std::string reason;
  // Arrived: the joiner's queue identity from its hello (empty: none).
  std::vector<u8> queue;
  // Hold: whether the host holds the join now.
  bool holding = false;
};

// How other players' inputs reach this one (DirectLink.h): the worst of them.
struct DirectSummary
{
  // 0 relay only, 1 direct link, 2 direct link through TURN (`orca stats` "tx").
  int transport = 0;
  // Round trip on that link in ms (-1: relay, or not measured yet).
  int rtt_ms = -1;
  // For logs: "direct host/host 1 ms", "TURN relay/host 38 ms", "relay".
  std::string line = "relay";
};

// A game's result as this Orca read it (UX/Results.h), for a matchmade room's report.
struct GameReport
{
  enum class Kind
  {
    Win,
    Draw,
    Void,
  };
  Kind kind = Kind::Void;
  // Win: the winner's seat (its room slot).
  int winner_seat = -1;
  // Void: why ("nocontest", "ports").
  std::string why;
  // Win and Draw: {"f":..,"st":..,"c":[..],"s":[..],"to":..}, ports in order.
  std::string detail;
  // A ranked set's game (UX/RankedSet.h): its fight's first frame (the report id "g<start>", the
  // same on both machines; -1 for the legacy "g<n>"), its number in the set, and for the game that
  // ended the set, how (1: won by `set_winner_seat`, 2: void).
  int start = -1;
  int number = 0;
  int set_done = 0;
  int set_winner_seat = -1;
};

enum class RoomState
{
  Connecting,
  // In the room.
  Waiting,
  Ended,
};

class YouGameRoom final : public Transport
{
public:
  explicit YouGameRoom(RoomOptions options);
  ~YouGameRoom() override;

  RoomState GetState() const;
  // This Orca's room slot (its seat), once in the room (-1 before).
  int Seat() const;
  // Whether this Orca joins someone else's game, once the ticket is in (nullopt before, or if the
  // room ended first).
  std::optional<bool> Joining() const;
  // What other players did since the last call.
  std::vector<PeerEvent> TakePeerEvents();
  // Whether the untaken events include a friend arriving (Arrived on a seat after the host's, from
  // a player that isn't the game's host).
  bool ArrivalPending() const;
  // Whether the untaken events include the game's host leaving, so a joiner stalled on the host's
  // inputs hears it at once rather than after its silence limit.
  bool HostLeftPending() const;
  // Seats of players whose leaving is among the untaken events. Any thread.
  std::vector<int> LeftSeatsPending() const;
  // Host: offer `seat`'s player the keyframe (resent every second until its first packet arrives).
  void OfferKeyframe(int seat, const KeyframeInfo& info);
  // Host: send every port's values so far (KeyframeInfo::names) and their version to every player.
  void BroadcastNames(const std::vector<KeyframeInfo::Name>& names, int version);
  // Host: tell `seat`'s player it was unplugged and why ("desync", "stalled", "network"), so its
  // own report matches. Its room ends with that code.
  void DropPeer(int seat, const std::string& reason);
  // Host: whether to hold every join without a keyframe yet (its game is in a single-player mode).
  // Joiners hear it at once, every second while held, and once when it ends.
  void HoldJoins(bool holding);
  // A ticket for this room (`fresh`: a newly minted one, after the store refused the last) and the
  // keyframe store's URL (".../api/orca/keyframes/<room>"). Any thread; minting blocks on HTTP.
  bool FreshTicket(bool fresh, std::string* ticket, std::string* store_url, std::string* error);
  // The room code in use (for showing and sharing), once known.
  std::string Code() const;
  // The roster's name for a seat (this player's included); "" when nobody holds it.
  std::string SeatName(int seat) const;
  // A line for the UI or log: what the room is doing, or why it ended.
  std::string Status() const;
  // Once Ended: the app's error code (Orca::Status), empty for a normal end. One code is internal:
  // "taken", when a hosting Orca found someone else hosting its room (it opens another and reports
  // "network").
  std::string ErrorCode() const;
  // Latest round trip to the room server in ms (-1 before the first). `sequence`, if given,
  // receives a count that changes with each new measurement: feed Session::OnRoundTrip only when it
  // changed.
  int RoundTripMs(u32* sequence = nullptr) const;
  // The latest RTT_SAMPLES round trips, oldest first, for a session starting now, and in `sequence`
  // the count RoundTripMs gives with the newest.
  std::vector<int> RecentRoundTrips(u32* sequence) const;
  // The direct links to the other players (refreshed a few times a second).
  DirectSummary Direct() const;
  // Tests only: `test-direct off|on|in|out|rebuild` (DirectLink::Test).
  void TestDirect(const std::string& command);

  // ---- Matchmade rooms (ORCA.md "Matchmaking") ----
  // The room's queue from its welcome: "private" (a friends room, and until the welcome), "casual"
  // or "ranked".
  std::string Queue() const;
  // A room match is running and its result isn't in yet (for ranked, the whole set).
  bool MatchLive() const;
  // A ranked set's room after its verdict (`set-over`): it stays open, with relay and chat, until
  // the players leave (five minutes at most). Nothing more is rated or forfeited.
  bool SetOver() const;
  // Host: the opponent is plugged in past its catch-up, so the room's match may begin; false once
  // it unplugs.
  void SetOpponentPlugged(bool plugged);
  // Reports a game's result to the room's match once it has one (held 60 s at most).
  void ReportGame(GameReport report);
  // A ranked set's game began (`start`: its fight's first frame): tells the room's match with a
  // `game-start` (GameStartMessage), queued with the reports, once per game.
  void ReportGameStart(int start);
  // The games desynced: report the match in progress void (a ranked set, or a casual game).
  void ReportDesync();
  // The opponent's inputs stopped mid ranked set: send a `finish` naming this player the winner,
  // once, if the set is still open.
  void ReportStall();

  void Send(const Packet& packet) override;
  std::vector<Packet> Receive() override;
  bool Connected() const override;

  // Leaves the room (reporting a running match void).
  void Leave();
  // Why this player is about to leave, told to the friend first so both report the same thing (an
  // Orca::Status code; only "desync" is passed on).
  void SetLeaveReason(std::string reason);
  // Tests only: drop the connection as a network failure would (no goodbye; the server holds the
  // seat). The room ends with "network".
  void TestDropConnection();

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

// A ranked set's tally (exposed for tests): the room's game verdicts in order. The set ends once a
// player wins SET_WINS games, or void ("limit") after SET_GAMES games without that. Both Orcas see
// the same verdicts, so both end the set the same way.
class SetTally
{
public:
  static constexpr int SET_WINS = 2;
  static constexpr int SET_GAMES = 7;
  struct End
  {
    // The set's winner (a participant id); empty: the set is void.
    std::string winner;
    std::string why;
  };
  // One game's verdict: the winner's participant id, or "" for a draw or void game. Returns the
  // set's end, once (nothing after it).
  std::optional<End> Game(const std::string& winner);
  int Wins(const std::string& id) const;
  void Reset();

private:
  std::map<std::string, int> m_wins;
  int m_games = 0;
  bool m_ended = false;
};

// A ranked game's `game-start` for the room's match (exposed for tests):
// {"t":"game-start","matchId":"<match>","id":"g<start>"}, the id the game's `game-report` carries.
// Empty when none may go: a room other than a ranked queue's, no room match, the set already
// decided (this side sent its finish, or the verdict came), or no fight frame.
std::string GameStartMessage(const std::string& queue, const std::string& match, bool decided,
                             int start);
// Packets as room messages (exposed for tests).
std::string EncodePacket(const Packet& packet);
bool DecodePacket(const std::string& json, Packet* packet);
// Port values in the room's "nm" and "kf" messages (exposed for tests): each entry is [seat, from,
// name], plus controls hex (empty for none) and then queue hex when present. At most 16 entries are
// read; oversized or non-hex controls or queue identities are treated as none.
picojson::value NamesToJson(const std::vector<KeyframeInfo::Name>& names);
std::vector<KeyframeInfo::Name> NamesFromJson(const picojson::object& message);
// A packet from a direct link to the player at `slot`: as through the relay, only for that player's
// own seat, and never with drop-in history (only the relay carries that, in order).
bool DecodeDirectPacket(const std::string& payload, int slot, Packet* packet);
// YouGame's direct-link switch in a ticket reply: top-level `direct`, false turning links off.
// Absent or not a boolean: on.
bool TicketAllowsDirect(const picojson::object& reply);
// What to tell the player when the room refused the connection (WebSocket::Connect's error), and
// the app's error code for it.
std::string RefusalReason(const std::string& connect_error);
std::string RefusalCode(const std::string& connect_error);
// Queues `packet` in place of one not yet sent, keeping the older one's earlier inputs and
// checksum.
void CoalescePacket(std::optional<Packet>* outgoing, const Packet& packet);
}  // namespace Orca::Net
