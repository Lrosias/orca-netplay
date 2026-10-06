// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Rollback/OnlineMatch.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>
#include <picojson.h>

#ifdef __APPLE__
#include <sys/clonefile.h>
#endif

#include "Common/FileUtil.h"
#include "Common/HookableEvent.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "Common/Timer.h"
#include "Core/Core.h"
#include "Core/HW/CPU.h"
#include "Core/IOS/FS/HostBackend/FS.h"
#include "Core/IOS/IOS.h"
#include "Core/Orca/Profile.h"
#include "Core/Orca/Session/Events.h"
#include "Core/Orca/Session/Keyframe.h"
#include "Core/Orca/Session/Online.h"
#include "Core/Orca/Session/PadCodec.h"
#include "Core/Orca/Status.h"
#include "Core/Orca/UX/OnlineMenu.h"
#include "Core/Orca/UX/OnlineRules.h"
#include "Core/Orca/UX/Queue.h"
#include "Core/Orca/UX/RankedSet.h"
#include "Core/Orca/UX/Results.h"
#include "Core/Orca/UX/SetEnd.h"
#include "Core/Rollback/SessionPort.h"
#include "Core/System.h"
#include "Core/WiiRoot.h"
#include "VideoCommon/OnScreenDisplay.h"

namespace Rollback::OnlineMatch
{
namespace
{
using Orca::Net::Pad;
using Orca::Net::Pads;
using Clock = std::chrono::steady_clock;

double MsSince(Clock::time_point start)
{
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

// The host's keyframe: captured at a boundary, then packed, compressed and stored on a thread.
struct KeyframeJob
{
  std::thread thread;
  std::atomic<bool> done{false};
  bool ok = false;
  int frame = -1;
  Orca::Net::KeyframeInfo info;
  double capture_ms = 0, pack_ms = 0, put_ms = 0;
  u64 raw_size = 0;
  // NAND bytes sent as hashes only; the joiner's own boot writes them.
  u64 referenced = 0;
  // The header generation wanted at capture (UX/OnlineRules.h WantedGeneration).
  u64 generation = 0;
  std::string error;
  ~KeyframeJob()
  {
    if (thread.joinable())
      thread.join();
  }
};

// The host's NAND file hashes at Profile::boot_nand_frame, read on a thread from a copy.
struct BootNandJob
{
  std::thread thread;
  std::atomic<bool> done{false};
  std::map<std::string, u64> hashes;
  // Boot /tmp files, so a later join can resolve hash references after the game rewrites /tmp.
  std::map<std::string, std::vector<u8>> tmp_files;
  ~BootNandJob()
  {
    if (thread.joinable())
      thread.join();
  }
};

// A joining player's download of the host's keyframe, unpacked on its own thread.
struct Download
{
  std::thread thread;
  std::atomic<bool> done{false};
  std::atomic<int> percent{0};
  std::atomic<bool> cancel{false};
  bool ok = false;
  std::string error;
  Orca::Net::KeyframeInfo info;
  int frame = -1;
  MachineImage image;
  std::vector<Orca::Net::NandEntry> nand;
  double transfer_ms = 0, unpack_ms = 0;
  ~Download()
  {
    cancel = true;
    if (thread.joinable())
      thread.join();
  }
};

// A player who arrived for a seat (its hello): its name and own controls (empty if none).
struct Arrival
{
  std::string name;
  std::vector<u8> controls;
  std::vector<u8> queue;
};

struct Match
{
  bool started = false;
  bool finished = false;
  // Emulation stopped since this match started: the next boot plays a new match.
  bool stale = false;
  std::unique_ptr<RingPort> port;
  std::unique_ptr<Orca::Net::Session> session;
  std::unique_ptr<Orca::Net::KeyframeStore> store;
  bool joining = false;
  int local_seat = 0;
  // The frame the current boundary completes (-1 at the first boundary).
  int running = -1;

  // Host: every seat's pads since the newest keyframe. The session owns the log while friends play.
  int log_base = 0;
  std::vector<Pads> log;
  // Local inputs the session had already sampled ahead (its input delay) when it ended.
  std::deque<Pad> queued_local;
  std::unique_ptr<KeyframeJob> job;
  std::optional<Orca::Net::KeyframeInfo> keyframe;
  // The header generation `keyframe` was captured under (KeyframeJob::generation).
  u64 keyframe_generation = 0;
  double keyframe_capture_ms = 0;
  bool keyframe_wanted = false;
  // Boundaries a friend has waited for this room's header (StepHeaderWait).
  int header_waited = 0;
  // Players waiting for a keyframe, and seats the session took in (until the room says they left).
  std::map<int, Arrival> waiting;
  std::set<int> seated;
  // Seats taken in but not plugged in yet; the app hears "friend-joined" once all have.
  std::set<int> plugging;
  // Host: hold waiting friends while in a single-player mode (Orca::UX::DropInHeld).
  bool holding = false;
  // Host: its room ended without this player leaving it. Reported once, then reopened with a
  // growing backoff; `home_code` is the last room that worked.
  bool room_lost = false;
  Clock::time_point room_lost_at{}, reopen_at{};
  std::chrono::seconds reopen_backoff{0};
  std::string home_code;
  // A new room to open shortly after a leave, so a quit right after it never requests a ticket.
  std::optional<Clock::time_point> reopen_pending;
  // A former joiner not on port 1 has no room; the app is told once, at the next boundary.
  bool tell_no_room = false;

  // A joining player.
  std::unique_ptr<Download> download;
  int host_seat = 0;
  std::string host_name;
  bool host_here = false;
  // The host holds this join while its game is in a single-player mode.
  bool host_holding = false;
  Clock::time_point boot_time{}, offered_time{}, loaded_time{};
  double load_ms = 0;
  bool joined = false;
  int leave_at = -1;
  bool leave_requested = false;
  // The joiner runs its own boot to this frame before loading (Profile::boot_nand_frame).
  int boot_ahead_to = 0;
  std::string join_status;
  // Host: NAND hashes at Profile::boot_nand_frame; keyframes reference those files, not send them.
  std::unique_ptr<BootNandJob> boot_nand;
  int last_percent = -1;

  u32 round_trip_sequence = 0;
  std::optional<Clock::time_point> last_return;
  Common::EventHook on_state_changed;

  // A join started during play ("join <code>"): this game plays solo until the keyframe is in.
  bool join_in_play = false;
  // Why the join failed (PumpJoin), for the caller to report.
  std::string join_error_code, join_error;

  // For the "orca stats" line: values at the last line, deepest rollback and stall time since.
  Clock::time_point stats_time{};
  int stats_frame = -1;
  Orca::Net::Stats stats_last{};
  int stats_rbmax = 0;
  double stats_stall_ms = 0;
  // The local controller's input-age totals at the last line (Orca::Events::InputAge).
  Orca::Events::InputAge stats_input{};

  // Each port's player name and own controls, and the frame they apply from. The host decides them
  // and sends them with keyframe offers, so every game writes the same values each frame. A short
  // history per seat lets a re-run frame get the values it had.
  std::map<int, std::vector<Orca::Net::KeyframeInfo::Name>> port_names;
  // Version of port_names (Session::RequireValues). A joiner holds the host's newest, keeping one
  // heard mid-download for after the load.
  int names_version = 0;
  std::optional<Orca::Net::KeyframeInfo> later_names;
  // The ports the UI was last told of.
  std::vector<Orca::Events::PortInfo> told_ports;
  // A joiner's host left during a re-run; handled at the next boundary that isn't one.
  std::optional<std::string> host_gone;
  // Host: frames its session has run with nobody else in it (IDLE_GRACE_FRAMES).
  int idle_frames = 0;

  // A ranked room still deciding the set after the opponent left: stay in it, solo, until the
  // verdict closes it or this time passes. See ORCA.md, "Matchmaking".
  std::optional<Clock::time_point> linger_until;
  // Host: the app was told the opponent left ("friend-left <why>") since they joined.
  bool friend_left_told = false;

  // Queue image: this player's own game (its character select) when the queue matched it, restored
  // once that room is over. Armed once the room was actually entered.
  struct QueueImage
  {
    MachineImage image;
    std::vector<Orca::Net::NandEntry> nand;
    bool armed = false;
    Clock::time_point taken{};
  };
  std::unique_ptr<QueueImage> queue_image;
  bool want_queue_image = false;
  // A fight was played in the ranked room this queue image is for: the restored character select
  // comes back not ready, so Start searches again.
  bool ranked_fought = false;

  // A Casual or Ranked pick handed over while this game wasn't alone, at a boundary the app's
  // commands may not take (TakeLobbyPickNow): retried at the next one while it still stands
  // (UX/OnlineMenu.h PickStands).
  std::optional<Orca::UX::OnlinePick> lobby_pick;
  // A pick this host announced unarmed to keep its friends, armed once they are gone and it still
  // stands (ArmKeptPick).
  std::optional<Orca::UX::OnlinePick> kept_pick;
  // The pick whose queue this host ended for a friend, and whether a friend came in since: armed
  // again once they are all gone (RearmFriendsPick).
  std::optional<Orca::UX::OnlinePick> friends_pick;
  bool friends_pick_played = false;
  // Set over: after a ranked verdict this game stays in the room until the player leaves
  // (SetOverLeave). Tracks the leave, Z held since, and the last buttons (to spot presses).
  bool set_over_left = false;
  bool set_over_hint = false;  // the overlay's leave hint is shown
  std::optional<Clock::time_point> set_over_z_since;
  u16 set_over_buttons = 0xFFFF;
};

// Desyncs this process has seen across all matches (the stats line's "ds").
int s_desyncs = 0;

// The local pad the last boundary read, for set over's Z and Start.
std::optional<Orca::Net::Pad> s_local_pad;

// Solo pause. s_solo_idle: the last boundary left this game alone (false while a boundary runs).
// s_pausing: a pause is in effect, so boundaries take nobody in. Sequentially consistent: either
// the pause sees a running boundary, or the boundary sees the pause.
std::atomic<bool> s_solo_idle{false};
std::atomic<bool> s_pausing{false};
// Paused host time, for the stats line. Pause and resume arrive on the host thread; 0 = not paused.
std::atomic<s64> s_paused_since_ns{0};
std::atomic<s64> s_paused_ns{0};

s64 NowNs()
{
  return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
      .count();
}

void AddName(Match& match, Orca::Net::KeyframeInfo::Name name);
std::vector<Orca::Net::KeyframeInfo::Name> AllNames(const Match& match);
void SetNames(Match& match, const Orca::Net::KeyframeInfo& from_host);
void RefreshOwnValues(Match& match, int frame);
std::vector<Orca::Events::PortInfo> PortsAt(Match& match, int frame);
void Leave(Match& match);

Match& TheMatch()
{
  static Match match;
  return match;
}

// This game plays alone with no drop-in under way (SoloPauseAllowed).
bool SoloIdle(const Match& match)
{
  return match.started && !match.finished && match.port && !match.session && !match.joining &&
         !match.download && !match.job && !match.keyframe_wanted && match.waiting.empty() &&
         match.plugging.empty() && !match.port->CatchingUp();
}

// Emulation is stopping, or the host asked the CPU thread to pause and waits for it: a loop holding
// the CPU thread must give up rather than deadlock.
bool Stopping(Core::System& system)
{
  return Core::GetState(system) == Core::State::Stopping ||
         system.GetCPU().GetState() != CPU::State::Running;
}

// Two frame periods at 60 Hz: a frame slower than this was a hitch.
constexpr std::chrono::microseconds HITCH_TIME{33'334};
// A peer frozen with its connection open: after this long the host unplugs it, a joiner stops.
constexpr std::chrono::seconds PEER_SILENCE_LIMIT{10};
constexpr char PEER_SILENT_SENTENCE[] = "Your friend's game stopped responding";
// A joiner past its unplug frame waits this long for the host's final acknowledgement
// (Session::LeftDone), then leaves anyway.
constexpr std::chrono::seconds LEAVE_SILENCE_LIMIT{3};
// Host: frames the session keeps running after everyone left, so a leaving friend still gets the
// host's last acknowledgement if a packet is lost.
constexpr int IDLE_GRACE_FRAMES = 12;
// The rollback window (Config::max_rollback); RingPort keeps this many snapshots plus two.
constexpr int MAX_ROLLBACK = 7;
// No keyframe in a boot's first frames: Project+'s frames 3-9 after its launcher run differently
// when the JIT holds other blocks, as a joiner's does. Early friends wait for frame 300 (5 s).
constexpr int FIRST_KEYFRAME_FRAME = 300;
// A joiner far behind the host runs unthrottled.
constexpr int CATCH_UP_BEHIND = 12;
// How long a joiner waits for the host's keyframe before it gives up.
constexpr std::chrono::seconds JOIN_LIMIT{90};
// How long the rooms Worker holds a disconnected player's seat (AWAY_MS).
constexpr std::chrono::seconds ROOM_HOLD{90};
// How long to wait in a ranked room for the verdict after the opponent left (forfeit after 15 s).
constexpr std::chrono::seconds VERDICT_WAIT{30};
// Ranked: the opponent's inputs stopped this long, so this player claims the set (ReportStall).
constexpr std::chrono::seconds RANKED_STALL_LIMIT{15};
// Set over: holding Z this long on the results screen leaves the room.
constexpr std::chrono::milliseconds SET_OVER_Z_HOLD{1500};

// The input delay the player chose ("delay N" with "caps delay"), or nullopt for the adaptive one.
std::optional<int> ChosenDelay()
{
  return Orca::Status::Cap("delay") ? Orca::Online::ChosenDelay() : std::nullopt;
}

// A round-trip sample the session has not seen yet (the room numbers its samples).
std::optional<int> NewRoundTrip(u32* last_sequence)
{
  u32 sequence = 0;
  const int round_trip_ms = Orca::Online::RoundTripMs(&sequence);
  if (round_trip_ms < 0 || sequence == *last_sequence)
    return std::nullopt;
  *last_sequence = sequence;
  return round_trip_ms;
}

// A new session takes the room's recent round trips, so its input delay fits from the first frame.
void SeedRoundTrips(Match& match)
{
  for (const int round_trip_ms : Orca::Online::RecentRoundTrips(&match.round_trip_sequence))
    match.session->OnRoundTrip(round_trip_ms);
}

// A joiner's single status line: each message replaces the last, and the join's end clears it.
void ShowJoinMessage(std::string message, u32 ms)
{
  if (std::ranges::all_of(message, Common::IsPrintableCharacter))
    OSD::AddTypedMessage(OSD::MessageType::OrcaJoin, std::move(message), ms);
}

void StopEmulation(Core::System& system, const std::string& message)
{
  Core::DisplayMessage(message, 10000);
  if (!Stopping(system))
    Core::QueueHostJob([](Core::System& host_system) { Core::Stop(host_system); });
}

IOS::HLE::FS::HostFileSystem* HostNand(Core::System& system)
{
  IOS::HLE::EmulationKernel* ios = system.GetIOS();
  return ios ? static_cast<IOS::HLE::FS::HostFileSystem*>(ios->GetFS().get()) : nullptr;
}

Orca::Net::Config SessionConfig()
{
  Orca::Net::Config config;
  config.seats = Orca::Net::MAX_SEATS;
  config.max_rollback = MAX_ROLLBACK;
  // Snapshot spacing is local only (it changes no frame's result); a test may fix it.
  config.snapshot_every = Orca::TestSnapshotEvery();
  // So is the input delay.
  config.fixed_delay = ChosenDelay();
  return config;
}

// ---- Host ----

// Captures the state at the start of `frame` and stores it on a thread. Only from the boundary
// hook, with every earlier frame final.
void StartKeyframe(Core::System& system, Match& match, int frame)
{
  auto job = std::make_unique<KeyframeJob>();
  job->frame = frame;
  const auto start = Clock::now();
  MachineImage image;
  if (!SnapshotRing::Capture(system, &image, !match.session))
  {
    ERROR_LOG_FMT(ROLLBACK, "Drop-in: capturing a keyframe at frame {} failed", frame);
    return;
  }
  // The NAND as of this boundary: a copy-on-write clone where possible, else read now.
  IOS::HLE::FS::HostFileSystem* nand = HostNand(system);
  std::string clone;
  std::vector<Orca::Net::NandEntry> entries;
  bool nand_ok = nand != nullptr;
#ifdef __APPLE__
  if (nand)
  {
    clone = File::GetUserPath(D_USER_IDX) + fmt::format("OrcaKeyframeNand-{}", frame);
    File::DeleteDirRecursively(clone);
    // Flush and close open files first, or buffered writes miss the clone.
    nand->CloseHostFiles();
    if (clonefile(nand->HostRoot().c_str(), clone.c_str(), CLONE_NOFOLLOW) != 0)
      clone.clear();
    nand->ReopenHostFiles();
  }
#endif
  if (nand && clone.empty())
  {
    // Flush and close the game's open files first: Windows can't read them while they're open.
    nand->CloseHostFiles();
    nand_ok = Orca::Net::ReadNandTree(nand->HostRoot(), &entries);
    nand->ReopenHostFiles();
  }
  job->capture_ms = MsSince(start);
  if (!nand_ok)
  {
    ERROR_LOG_FMT(ROLLBACK, "Drop-in: reading the NAND for a keyframe failed");
    return;
  }

  KeyframeJob* const raw = job.get();
  Orca::Net::KeyframeStore* const store = match.store.get();
  std::map<std::string, u64> boot;
  if (match.boot_nand && match.boot_nand->done)
    boot = match.boot_nand->hashes;
  raw->thread = std::thread([raw, store, clone, image = std::move(image),
                             entries = std::move(entries), boot = std::move(boot)]() mutable {
    if (!clone.empty())
    {
      if (!Orca::Net::ReadNandTree(clone, &entries))
        raw->error = "reading the NAND clone failed";
      File::DeleteDirRecursively(clone);
    }
    // /tmp files the joiner's own boot also writes travel as hashes. Saves always travel: Project+
    // writes Brawl's saves during boot, and they can depend on the host's inputs.
    for (Orca::Net::NandEntry& entry : entries)
    {
      if (entry.directory || !entry.path.starts_with("tmp/"))
        continue;
      const auto it = boot.find(entry.path);
      const u64 hash = Orca::Net::NandHash(entry.data);
      if (it == boot.end() || it->second != hash)
        continue;
      raw->referenced += entry.data.size();
      entry.reference = true;
      entry.hash = hash;
      entry.data.clear();
      entry.data.shrink_to_fit();
    }
    const auto pack_start = Clock::now();
    u64 raw_size = image.state.size() + image.mem1.size() + image.mem2.size();
    for (const auto& e : entries)
      raw_size += e.data.size();
    raw->raw_size = raw_size;
    std::vector<u8> blob =
        raw->error.empty() ? Orca::Net::PackKeyframe(raw->frame, image, entries) : std::vector<u8>{};
    if (raw->error.empty() && blob.empty())
      raw->error = "compression failed";
    // The store sees ciphertext only; the key goes to the joiner inside the room.
    if (raw->error.empty() && !Orca::Net::EncryptKeyframe(raw->frame, &blob, &raw->info.key))
      raw->error = "encryption failed";
    raw->pack_ms = MsSince(pack_start);
    if (raw->error.empty())
    {
      raw->info.frame = raw->frame;
      raw->info.size = blob.size();
      raw->info.hash = Orca::Net::KeyframeHash(blob);
      raw->info.id = fmt::format("kf-{}-{}", raw->frame, raw->info.hash.substr(0, 8));
      const auto put_start = Clock::now();
      raw->ok = store->Put(raw->info, blob, &raw->error);
      raw->put_ms = MsSince(put_start);
    }
    raw->done = true;
  });
  match.job = std::move(job);
  NOTICE_LOG_FMT(ROLLBACK, "Drop-in: keyframe of frame {} captured in {:.1f} ms", frame,
                 match.job->capture_ms);
}

// Host: at Profile::boot_nand_frame, hash what the boot wrote; a joiner's boot writes the same.
void MaybeHashBootNand(Core::System& system, Match& match)
{
  const Orca::Profile* profile = Orca::ActiveProfile();
  if (!profile || !profile->boot_nand_frame || match.boot_nand ||
      match.running + 1 != static_cast<int>(*profile->boot_nand_frame))
  {
    return;
  }
  IOS::HLE::FS::HostFileSystem* nand = HostNand(system);
  if (!nand)
    return;
  auto job = std::make_unique<BootNandJob>();
  std::string clone;
#ifdef __APPLE__
  clone = File::GetUserPath(D_USER_IDX) + "OrcaBootNand";
  File::DeleteDirRecursively(clone);
  // Flushed and closed first, so the hashes match this boundary.
  nand->CloseHostFiles();
  if (clonefile(nand->HostRoot().c_str(), clone.c_str(), CLONE_NOFOLLOW) != 0)
    clone.clear();
  nand->ReopenHostFiles();
#endif
  if (clone.empty())
  {
    // No clone: copy now with files flushed and closed (Windows can't read open files).
    const auto copy_start = Clock::now();
    clone = File::GetUserPath(D_USER_IDX) + "OrcaBootNand";
    File::DeleteDirRecursively(clone);
    nand->CloseHostFiles();
    const bool copied = File::Copy(nand->HostRoot(), clone);
    nand->ReopenHostFiles();
    NOTICE_LOG_FMT(ROLLBACK, "Drop-in: boot NAND copied for hashing in {:.1f} ms ({})",
                   MsSince(copy_start), copied ? "ok" : "failed");
    if (!copied)
    {
      File::DeleteDirRecursively(clone);
      clone.clear();
    }
  }
  // No copy: hash the live NAND. A file written meanwhile hashes wrong and is just sent whole.
  BootNandJob* const raw = job.get();
  const std::string root = clone.empty() ? nand->HostRoot() : clone;
  raw->thread = std::thread([raw, root, cloned = !clone.empty()] {
    raw->hashes = Orca::Net::HashNandTree(root);
    std::vector<Orca::Net::NandEntry> entries;
    if (Orca::Net::ReadNandTree(root, &entries))
    {
      for (Orca::Net::NandEntry& entry : entries)
      {
        if (!entry.directory && entry.path.starts_with("tmp/"))
          raw->tmp_files[entry.path] = std::move(entry.data);
      }
    }
    if (cloned)
      File::DeleteDirRecursively(root);
    raw->done = true;
  });
  match.boot_nand = std::move(job);
}

// A solo frame: this player's pad on its port (port 1 for a host), every other port unplugged.
void RunSolo(Match& match, const std::function<Pad(int)>& local_pad)
{
  Pad pad;
  if (!match.queued_local.empty())
  {
    pad = match.queued_local.front();
    match.queued_local.pop_front();
  }
  else
  {
    pad = local_pad(match.local_seat);
  }
  Pads pads;
  pads.fill(Orca::Net::UNPLUGGED_PAD);
  pads[match.local_seat] = pad;
  const int frame = match.running + 1;
  match.port->SetPads(frame, pads);
  match.log.push_back(pads);
  match.running = frame;
  // No keyframe stored or in progress: only the newest frames can serve the next, so cap the log.
  if (!match.keyframe && !match.job && match.log.size() > 4 * KEYFRAME_FRESH_FRAMES)
  {
    const int drop = static_cast<int>(match.log.size()) - KEYFRAME_FRESH_FRAMES;
    match.log.erase(match.log.begin(), match.log.begin() + drop);
    match.log_base += drop;
  }
}

// Host: unplug every friend still in the session, telling each (and the app) why.
void DropFriends(Match& match, const std::string& reason, bool announce = true)
{
  for (int seat = 0; seat < Orca::Net::MAX_SEATS; ++seat)
  {
    if (seat == match.local_seat || match.session->Plan()[seat].live_from == Orca::Net::NEVER)
      continue;
    Orca::Online::DropPeer(seat, reason);
    match.session->DropSeat(seat);
  }
  match.seated.clear();
  match.plugging.clear();
  Orca::Online::SetOpponentPlugged(false);
  if (reason == "desync")
    Orca::Online::ReportDesync();
  if (announce)
  {
    Orca::Status::State("friend-left " + reason);
    match.friend_left_told = true;
  }
}

void ConfirmResults(Match& match);

void GoSolo(Match& match, const char* why, const char* state = "playing")
{
  // Report results already final; readings after them used predicted inputs and start over.
  ConfirmResults(match);
  Orca::Events::NoteResync();
  match.log = match.session->TakeLog(&match.log_base);
  const std::vector<Pad> pending = match.session->PendingLocal();
  match.queued_local.assign(pending.begin(), pending.end());
  Orca::Net::Stats stats = match.session->GetStats();
  NOTICE_LOG_FMT(ROLLBACK, "Drop-in: back to solo play at frame {} ({}): {} rollbacks, {} checksums matched",
                 match.running + 1, why, stats.rollbacks, stats.checksums_matched);
  match.session.reset();
  match.plugging.clear();
  match.idle_frames = 0;
  match.port->SetCatchingUp(false);
  // A session that ended mid re-run left the port in re-run mode (unthrottled, unrendered, muted).
  match.port->SetResimulating(false);
  if (state)
    Orca::Status::State(state);
}

// With "caps join", a failed join, the host leaving or a desync leaves this player solo.
bool Soft()
{
  return Orca::Status::Cap("join");
}

std::unique_ptr<Orca::Net::KeyframeStore> MakeStore()
{
  return Orca::Net::MakeKeyframeStore(
      [](bool fresh, std::string* ticket, std::string* store_url, std::string* error) {
        return Orca::Online::FreshTicket(fresh, ticket, store_url, error);
      });
}

// Drops drop-in work for the room being left; the next room gets a new store.
void ResetDropIn(Match& match)
{
  if (match.keyframe && match.store)
    match.store->Delete(match.keyframe->id);
  match.keyframe.reset();
  if (match.store)
    match.store->Cancel();
  match.waiting.clear();
  match.seated.clear();
  match.plugging.clear();
  match.keyframe_wanted = false;
  match.header_waited = 0;
  // Wind down the job, the download and the old store (a delete can retry for seconds) on a
  // detached thread; job and download first, since they use the store.
  std::thread([job = std::move(match.job), download = std::move(match.download),
               store = std::move(match.store)]() mutable {
    job.reset();
    download.reset();
    store.reset();
  }).detach();
  match.store = MakeStore();
}

// The online rules' mode for a room's queue (UX/OnlineRules.h): casual or ranked, else none.
Orca::UX::Rules::Mode QueueMode(const std::string& queue)
{
  if (queue == "casual")
    return Orca::UX::Rules::Mode::Casual;
  if (queue == "ranked")
    return Orca::UX::Rules::Mode::Ranked;
  return Orca::UX::Rules::Mode::None;
}

// Queue flags for a queue room's header (FLAG_QUEUE2 with `queue2`); host sets, joiner checks.
u8 RoomFlags()
{
  return Orca::Status::Cap("queue2") ? Orca::UX::MatchBlock::FLAG_QUEUE2 : u8{0};
}

// The header this game should carry, written only where HeaderFreeAt allows: a queue room's while
// hosting it on port 1, or while waiting for or staying after a ranked verdict; the queue's own
// character select's (FLAG_SOLO) while waiting there with `queue2`; otherwise none.
std::pair<Orca::UX::Rules::Mode, u8> WantedRulesMode(const Match& match)
{
  using Orca::UX::Rules::Mode;
  namespace MB = Orca::UX::MatchBlock;
  if (match.joining)
    return {Mode::None, 0};
  const Mode mode =
      Orca::Online::RoomEnded() ? Mode::None : QueueMode(Orca::Online::RoomQueue());
  if (mode != Mode::None &&
      (match.local_seat == 0 ||
       ((match.linger_until || Orca::Online::SetOver()) && mode == Mode::Ranked)))
  {
    return {mode, RoomFlags()};
  }
  if (mode == Mode::None && Orca::UX::Queue::Active() && Orca::Status::Cap("queue2") &&
      match.local_seat == 0 && !match.session && !match.linger_until)
  {
    return {Orca::UX::Queue::Ranked() ? Mode::Ranked : Mode::Casual,
            static_cast<u8>(MB::FLAG_SOLO | MB::FLAG_QUEUE2)};
  }
  return {Mode::None, 0};
}

// Tells the online rules which header to carry now. Runs before the frame hook and again before a
// keyframe, since a room welcome can arrive in between.
void UpdateWanted(const Match& match)
{
  const auto [mode, flags] = WantedRulesMode(match);
  const bool room = mode != Orca::UX::Rules::Mode::None &&
                    (flags & Orca::UX::MatchBlock::FLAG_SOLO) == 0;
  if (Orca::UX::Rules::SetWanted(mode, flags, room ? Orca::Online::Code() : std::string()))
  {
    DEBUG_LOG_FMT(ROLLBACK, "Online rules: header {} flags {:#04x} wanted from frame {}",
                  static_cast<int>(mode), flags, match.running + 1);
  }
}

// A keyframe nobody took, made under another header than wanted now, is for the wrong match.
void DropStaleKeyframe(Match& match)
{
  if (!match.keyframe || match.session ||
      match.keyframe_generation == Orca::UX::Rules::WantedGeneration())
  {
    return;
  }
  NOTICE_LOG_FMT(ROLLBACK, "Drop-in: keyframe of frame {} dropped: made for another header",
                 match.keyframe->frame);
  if (match.store)
    match.store->Delete(match.keyframe->id);
  match.keyframe.reset();
  if (!match.waiting.empty())
    match.keyframe_wanted = true;
}

// Nothing a friend would replay covers this boundary's frame (HeaderFreeAt's `solo_quiet`).
bool SoloQuiet(const Match& match)
{
  return match.started && !match.finished && match.port && !match.session && !match.joining &&
         !match.download && !match.job && match.plugging.empty() && !match.port->CatchingUp();
}

// Host of a queue room whose game never carried the room's header while the opponent waited: the
// joiner would refuse the keyframe, so report `orca error mismatch` and leave, as a joiner does
// (LeaveOnMismatch). Both players search again.
void FailHeaderWait(Match& match)
{
  WARN_LOG_FMT(ROLLBACK,
               "Online rules: room {}: this game's header isn't the room's after {} boundaries "
               "with the opponent waiting, frame {}: leaving (mismatch)",
               Orca::Online::Code(), HEADER_FAIL_BOUNDARIES, match.running + 1);
  Orca::Online::SetLeaveReason("mismatch");
  Orca::Status::Report("mismatch", "That match couldn't start");
  Leave(match);
}

// Forgets what KeepRoomOpen knew of a room this player left on purpose.
void ForgetRoom(Match& match)
{
  match.room_lost = false;
  match.reopen_backoff = {};
  match.home_code.clear();
  match.reopen_pending.reset();
  match.tell_no_room = false;
}

// Stop sends "leave" then "quit" at once; the delay keeps the quit from finding a ticket request.
constexpr std::chrono::seconds REOPEN_DELAY{1};

// Solo again: on port 1, open a room of its own; elsewhere none ("no-room"): hosts play port 1.
void ReopenRoom(Match& match)
{
  ForgetRoom(match);
  Orca::Online::ShutdownInBackground();
  if (match.local_seat == 0)
    match.reopen_pending = Clock::now() + REOPEN_DELAY;
  else
    match.tell_no_room = true;
}

// A former joiner moves to port 1 from the next frame, like a host; only this game changes.
void Reseat(Match& match)
{
  if (match.local_seat == 0)
    return;
  NOTICE_LOG_FMT(ROLLBACK, "Drop-in: port {} moves to port 1 from frame {}", match.local_seat + 1,
                 match.running + 1);
  match.local_seat = 0;
  // Names came from the old host's roster; this player's own comes from its new room.
  match.port_names.clear();
}

// Host: its friends left. Leave a matchmade room for one of its own, unless a ranked set is still
// being decided: then wait for the room's verdict (up to VERDICT_WAIT).
void AfterFriendsGone(Match& match)
{
  Orca::Online::SetOpponentPlugged(false);
  const std::string queue = Orca::Online::RoomQueue();
  if (queue == "private" || Orca::Online::RoomEnded() || match.linger_until)
    return;
  // Tell the app the opponent left now, since this game won't be in the room to hear it.
  if (!std::exchange(match.friend_left_told, true))
    Orca::Status::State("friend-left left");
  // Set over: stay until the player leaves (SetOverLeave); that leave is not a forfeit.
  if (queue == "ranked" && Orca::Online::SetOver())
  {
    NOTICE_LOG_FMT(ROLLBACK, "Matchmaking: the opponent left the set's room (set over): this "
                             "game stays until its player leaves");
    return;
  }
  if (queue == "ranked" && Orca::Online::MatchLive())
  {
    NOTICE_LOG_FMT(ROLLBACK,
                   "Matchmaking: the opponent went mid-set: waiting for the room's verdict");
    match.linger_until = Clock::now() + VERDICT_WAIT;
    return;
  }
  NOTICE_LOG_FMT(ROLLBACK, "Matchmaking: the {} pairing is over: a room of this game's own", queue);
  ResetDropIn(match);
  ReopenRoom(match);
}

// Host on port 1: its room ended without this player leaving, so friends can't drop in. Report
// once, then reopen after 2 s, backing off to 30 s. While the server holds the seat (ROOM_HOLD),
// reuse the code so pending invites still work; otherwise use a fresh one.
void KeepRoomOpen(Match& match)
{
  if (match.local_seat != 0)
    return;
  // The room ReopenRoom asked for, once its time comes.
  if (match.reopen_pending)
  {
    if (Clock::now() < *match.reopen_pending)
      return;
    match.reopen_pending.reset();
    Orca::Online::StartRoom("", false);
    return;
  }
  if (!Orca::Online::RoomEnded())
  {
    if (Orca::Online::InRoom())
    {
      if (match.room_lost)
      {
        NOTICE_LOG_FMT(ROLLBACK, "Drop-in: hosting room {} again", Orca::Online::Code());
        // Clear the soft error the page keeps showing about the lost room.
        if (Soft())
          Orca::Status::Event("playing");
      }
      match.room_lost = false;
      match.reopen_backoff = {};
      match.home_code = Orca::Online::Code();
    }
    return;
  }
  // A session ends with its room: it fails at its next step and the host goes solo.
  if (match.session)
    return;
  // No error code: this player left the room itself (the app's Leave).
  const std::string code = Orca::Online::RoomErrorCode();
  if (code.empty())
    return;
  // A matchmade room closed after its result: never rejoin that code; open a fresh room.
  if (code == "match-over")
  {
    NOTICE_LOG_FMT(ROLLBACK, "Matchmaking: the room closed after its result; a room of its own");
    Orca::Status::Event("friend-left match-over");
    Orca::UX::Search::End();
    match.linger_until.reset();
    ResetDropIn(match);
    ForgetRoom(match);
    Orca::Online::ShutdownInBackground();
    Orca::Online::StartRoom("", false);
    return;
  }
  const auto now = Clock::now();
  // Someone else holds that room (a friend, or another copy on this account): never rejoin it.
  if (Orca::Online::RoomTaken())
    match.home_code.clear();
  if (!match.room_lost)
  {
    match.room_lost = true;
    match.room_lost_at = now;
    match.reopen_backoff = std::chrono::seconds(2);
    match.reopen_at = now + match.reopen_backoff;
    const std::string why = Orca::Online::StatusLine();
    NOTICE_LOG_FMT(ROLLBACK, "Drop-in: this game's room ended ({}: {}); another opens in {} s",
                   code, why, match.reopen_backoff.count());
    Core::DisplayMessage(why, 6000);
    if (Soft())
    {
      Orca::Status::Report("network", Orca::Online::RoomTaken() ?
                                          why :
                                          fmt::format("{}: your game opens its room again", why));
    }
    // Whoever waited for a keyframe was in that room.
    ResetDropIn(match);
  }
  if (now < match.reopen_at)
    return;
  const bool held = now - match.room_lost_at < ROOM_HOLD;
  const std::string reopen = held ? match.home_code : std::string();
  NOTICE_LOG_FMT(ROLLBACK, "Drop-in: reopening {} (the last ended: {}: {})",
                 reopen.empty() ? std::string("a new room") : "room " + reopen, code,
                 Orca::Online::StatusLine());
  Orca::Online::ShutdownInBackground();
  Orca::Online::StartRoom(reopen, false);
  match.reopen_backoff = std::min(match.reopen_backoff * 2, std::chrono::seconds(30));
  match.reopen_at = now + match.reopen_backoff;
}

// A joining player on its own again (it left, its host did, the session failed, or the join never
// finished): it plays on with every other controller unplugged. `own_boot`: no keyframe was ever
// loaded, so it plays its own game on port 1.
void BecomeSolo(Match& match, const char* why, const char* state, bool own_boot = false,
                bool keep_room = false)
{
  Orca::Events::NoteResync();
  if (match.session)
  {
    GoSolo(match, why, nullptr);
  }
  else
  {
    match.log.clear();
    match.log_base = match.running + 1;
    match.queued_local.clear();
  }
  if (own_boot)
    Reseat(match);
  match.joining = false;
  match.join_in_play = false;
  match.joined = false;
  match.host_here = false;
  match.host_holding = false;
  match.leave_requested = false;
  match.leave_at = -1;
  match.last_percent = -1;
  match.later_names.reset();
  OSD::DiscardTypedMessage(OSD::MessageType::OrcaJoin);
  match.port->SetCatchingUp(false);
  ResetDropIn(match);
  if (!keep_room)
    ReopenRoom(match);
  NOTICE_LOG_FMT(ROLLBACK, "Drop-in: playing solo on port {} from frame {} ({})",
                 match.local_seat + 1, match.running + 1, why);
  if (state)
    Orca::Status::State(state);
}

bool InQueueRoom();

// On the queue's own character select, or searching.
bool QueueOrSearch()
{
  return Orca::UX::Queue::Active() ||
         Orca::UX::Search::Current() != Orca::UX::Search::State::None;
}

// Host: a friend arrived or an invite is on its way while this player is on the queue. The queue
// ends as With Friends ends it (`orca menu cancel`), so the friend's keyframe has no queue header.
void EndQueueForFriend(Match& match, bool arrived, bool invited)
{
  FriendQueueInputs in;
  in.friend_coming = arrived || invited;
  in.queue_or_search = QueueOrSearch();
  in.local_seat = match.local_seat;
  in.joining = match.joining;
  in.friends_room = !Orca::Online::RoomEnded() && Orca::Online::Seat() >= 0 &&
                    Orca::Online::RoomQueue() == "private";
  if (!FriendEndsQueue(in))
    return;
  NOTICE_LOG_FMT(ROLLBACK,
                 "Queue: a friend {} room {} at frame {} while this player is on the {} queue: "
                 "the queue is over (orca menu cancel), so the friend's keyframe carries no queue "
                 "header",
                 arrived ? "arrived in" : "is invited to", Orca::Online::Code(),
                 match.running + 1,
                 (Orca::UX::Queue::Active() ? Orca::UX::Queue::Ranked() :
                                              Orca::UX::Search::Ranked()) ?
                     "ranked" :
                     "casual");
  // Armed again once the friends are gone (RearmFriendsPick).
  if (Orca::UX::Queue::Active())
  {
    match.friends_pick =
        Orca::UX::Queue::Ranked() ? Orca::UX::OnlinePick::Ranked : Orca::UX::OnlinePick::Casual;
    match.friends_pick_played = false;
  }
  Orca::UX::Search::End();
  Orca::UX::Queue::End();
  Orca::Status::Menu("cancel");
}

void HostEvents(Core::System& system, Match& match)
{
  bool arrived = false;
  for (const Orca::Net::PeerEvent& event : Orca::Online::TakePeerEvents())
  {
    if (event.kind == Orca::Net::PeerEvent::Kind::Arrived)
    {
      if (event.seat <= 0 || event.host)
      {
        WARN_LOG_FMT(ROLLBACK, "Drop-in: ignoring a player at seat {}{}", event.seat + 1,
                     event.host ? " that says it hosts" : "");
        continue;
      }
      NOTICE_LOG_FMT(ROLLBACK, "Drop-in: {} arrived for port {}", event.name, event.seat + 1);
      Core::DisplayMessage(fmt::format("{} is joining on port {}", event.name, event.seat + 1), 4000);
      match.waiting[event.seat] = {event.name, event.controls, event.queue};
      match.keyframe_wanted = true;
      arrived = true;
      Orca::Status::State("friend-joining");
    }
    else if (event.kind == Orca::Net::PeerEvent::Kind::Left)
    {
      const bool was_waiting = match.waiting.erase(event.seat) > 0;
      match.plugging.erase(event.seat);
      // Taken in (even if already unplugged and the host went solo): the app still hears it.
      const bool was_playing = match.seated.erase(event.seat) > 0;
      if (match.session)
      {
        NOTICE_LOG_FMT(ROLLBACK, "Drop-in: port {} left the room{}{}", event.seat + 1,
                       event.reason.empty() ? "" : ": ", event.reason);
        match.session->DropSeat(event.seat);
      }
      if (was_playing)
        Orca::Online::SetOpponentPlugged(false);
      // The app hears why a friend left (its bye: "desync", "stalled").
      if (was_waiting || was_playing)
      {
        Orca::Status::State("friend-left " +
                            (event.reason.empty() ? std::string("left") : event.reason));
        match.friend_left_told = true;
      }
    }
  }
  const bool invited = Orca::Online::TakePrepareJoin();
  if (invited)
    match.keyframe_wanted = true;
  // A friend on the way ends this player's queue (before UpdateWanted).
  EndQueueForFriend(match, arrived, invited);
  // Refresh the wanted header: the room's welcome may have come after this boundary's hook.
  UpdateWanted(match);

  // A finished keyframe: offer it to whoever waits, and drop the solo log before it.
  if (match.job && match.job->done)
  {
    std::unique_ptr<KeyframeJob> job = std::move(match.job);
    if (job->thread.joinable())
      job->thread.join();
    if (!job->ok)
    {
      ERROR_LOG_FMT(ROLLBACK, "Drop-in: keyframe of frame {} failed: {}", job->frame, job->error);
      // Whoever waits for it gets another try.
      match.keyframe_wanted = !match.waiting.empty();
    }
    else
    {
      NOTICE_LOG_FMT(ROLLBACK,
                     "Drop-in: keyframe of frame {}: {:.1f} MB raw ({:.1f} MB of NAND named, not "
                     "sent), {:.1f} MB stored; capture {:.1f} ms, pack and compress {:.0f} ms, "
                     "store {:.0f} ms",
                     job->frame, job->raw_size / 1e6, job->referenced / 1e6, job->info.size / 1e6,
                     job->capture_ms,
                     job->pack_ms, job->put_ms);
      // An earlier keyframe nobody took (say, an invite not accepted yet) is replaced.
      if (match.keyframe && match.store)
        match.store->Delete(match.keyframe->id);
      match.keyframe = job->info;
      match.keyframe_generation = job->generation;
      if (!match.session && job->frame >= match.log_base &&
          job->frame <= match.log_base + static_cast<int>(match.log.size()))
      {
        match.log.erase(match.log.begin(), match.log.begin() + (job->frame - match.log_base));
        match.log_base = job->frame;
      }
    }
  }

  // A host in a single-player mode (Classic, Training, Events...) holds waiting friends until its
  // game is back in the menus (UX/OnlineMenu.h SequenceHoldsDropIn).
  const bool hold = Orca::UX::DropInHeld() && !match.waiting.empty();
  if (hold != match.holding)
  {
    match.holding = hold;
    Orca::Online::HoldJoins(hold);
    NOTICE_LOG_FMT(ROLLBACK, "Drop-in: {} at frame {}",
                   hold ? "the game is in a single-player mode, the join waits" :
                          "the join goes on",
                   match.running + 1);
    if (hold)
      Orca::Status::State("friend-holding");
    else if (!match.waiting.empty())
      Orca::Status::State("friend-joining");
  }
  if (hold)
    return;

  // A queue room's opponent must replay a keyframe carrying the room's header, so wait for it. If
  // it never lands, fail the join rather than start the match on the wrong header.
  DropStaleKeyframe(match);
  const u64 generation = Orca::UX::Rules::WantedGeneration();
  const bool in_place = Orca::UX::Rules::HeaderInPlace();
  const HeaderWait header = StepHeaderWait(&match.header_waited, in_place,
                                           !match.waiting.empty(), InQueueRoom());
  if (header == HeaderWait::Fail)
  {
    FailHeaderWait(match);
    return;
  }
  const bool header_ready = header == HeaderWait::Ready;
  if (!in_place && match.keyframe_wanted && match.header_waited % 60 == 1)
  {
    NOTICE_LOG_FMT(ROLLBACK, "Online rules: a keyframe waits for the room's header (frame {}{})",
                   match.running + 1,
                   header_ready ? "; a friends room's, made anyway" : "");
  }

  // Capture a keyframe when one is wanted and the newest is stale, missing or for another header.
  // In a session, only once every earlier frame is final.
  const int next = match.running + 1;
  // Made under the current header. In a session an old one stays: it may be on its way to a friend.
  const bool usable = match.keyframe && match.keyframe_generation == generation;
  const bool fresh = usable && next - match.keyframe->frame <= KEYFRAME_FRESH_FRAMES;
  bool capture_failed = false;
  if (match.keyframe_wanted && !match.job && match.store && header_ready)
  {
    if (fresh)
      match.keyframe_wanted = false;
    else if (next >= FIRST_KEYFRAME_FRAME && (!match.session || match.session->Settled()))
    {
      match.keyframe_wanted = false;
      StartKeyframe(system, match, next);
      if (match.job)
      {
        match.job->generation = generation;
      }
      else
      {
        capture_failed = true;
        match.keyframe_wanted = !match.waiting.empty();
      }
    }
  }
  if (!match.store && !match.waiting.empty())
    WARN_LOG_FMT(ROLLBACK, "Drop-in: no keyframe store: a friend can't join yet");

  // Take in whoever waits once there is a keyframe for them. Not during a solo pause (BeginPause):
  // the session's first step would give up half done. Not solo after a capture failed here: a
  // friend replaying the stale keyframe would replay frames already announced as this player's
  // alone (AloneAt). The capture is retried next boundary.
  if (!match.keyframe || match.waiting.empty() || match.job || s_pausing ||
      (capture_failed && !match.session) || !header_ready || !usable)
  {
    return;
  }
  for (auto it = match.waiting.begin(); it != match.waiting.end();)
  {
    const int seat = it->first;
    const int keyframe_frame = match.keyframe->frame;
    if (!match.session)
    {
      if (keyframe_frame < match.log_base)
      {
        ++it;
        continue;
      }
      Orca::Net::Config config = SessionConfig();
      config.local_seat = 0;
      config.authority_seat = 0;
      config.start_frame = next;
      config.start_pad = match.log.empty() ? Pad{} : match.log.back()[0];
      std::array<Orca::Net::SeatPlan, Orca::Net::MAX_SEATS> plan{};
      plan[0] = {0, Orca::Net::NEVER, 0};
      config.plan = plan;
      // Anything that arrived while solo (a departed friend's last packets) belongs to no one.
      Orca::Online::Transport()->Receive();
      match.session = std::make_unique<Orca::Net::Session>(config, *match.port,
                                                           *Orca::Online::Transport());
      SeedRoundTrips(match);
      match.session->SetLog(match.log_base, std::move(match.log));
      match.log.clear();
      // Solo pads had no delay; now they do, and skipped frames repeat the current pad (start_pad).
      match.queued_local.clear();
      // The host's own controls apply from the next frame (this frame's hook already ran) and never
      // change mid-session: a friend may already have run that frame.
      RefreshOwnValues(match, next + 1);
    }
    match.session->AddPeer(seat, next, keyframe_frame);
    match.seated.insert(seat);
    match.plugging.insert(seat);
    NOTICE_LOG_FMT(ROLLBACK, "Drop-in: port {} is {} from frame {}{}", seat + 1, it->second.name,
                   next, it->second.controls.empty() ? "" : ", with their own controls");
    AddName(match, {seat, next, it->second.name, it->second.controls, it->second.queue});
    // The newcomer plugs in only once every player holds its values.
    match.session->RequireValues(seat, match.names_version);
    NOTICE_LOG_FMT(ROLLBACK, "Drop-in: {}", match.session->Describe());
    Orca::Net::KeyframeInfo offer = *match.keyframe;
    offer.names = AllNames(match);
    offer.names_version = match.names_version;
    // Others acknowledge the newcomer's values in their packets (Session::RequireValues).
    Orca::Online::BroadcastNames(offer.names, offer.names_version);
    Orca::Online::OfferKeyframe(seat, offer);
    NOTICE_LOG_FMT(ROLLBACK,
                   "Drop-in: port {} replays from keyframe frame {}, live from frame {}", seat + 1,
                   keyframe_frame, next);
    it = match.waiting.erase(it);
  }
  // A joiner deletes the keyframe it loaded, so the next arrival gets a fresh one.
  if (match.waiting.empty())
    match.keyframe.reset();
}

// ---- A joining player ----

// Joiner: its keyframe loaded into a friends room, so its own queue ends (`orca menu cancel`) and
// the game kept for after a queue room is dropped (kept, it would block going home). A join that
// fails earlier keeps all of it.
void EndQueueForJoin(Match& match)
{
  const bool queue_or_search = QueueOrSearch();
  if (!JoinEndsQueue(InQueueRoom(), queue_or_search, match.queue_image != nullptr))
    return;
  NOTICE_LOG_FMT(ROLLBACK,
                 "Queue: joined {}'s friends room {}: this player's own queue is over{}{}",
                 match.host_name.empty() ? std::string("a friend") : match.host_name,
                 Orca::Online::Code(), queue_or_search ? " (orca menu cancel)" : "",
                 match.queue_image ? "; the game it kept for after a queue's room is dropped" :
                                     "");
  match.queue_image.reset();
  match.want_queue_image = false;
  match.ranked_fought = false;
  if (!queue_or_search)
    return;
  Orca::UX::Search::End();
  Orca::UX::Queue::End();
  Orca::Status::Menu("cancel");
}

void StartDownload(Match& match, const Orca::Net::KeyframeInfo& info)
{
  auto download = std::make_unique<Download>();
  download->info = info;
  Download* const raw = download.get();
  Orca::Net::KeyframeStore* const store = match.store.get();
  raw->thread = std::thread([raw, store] {
    const auto start = Clock::now();
    std::string error;
    auto blob = store->Get(
        raw->info,
        [raw](u64 done, u64 total) {
          raw->percent = static_cast<int>(done * 100 / std::max<u64>(total, 1));
          return !raw->cancel.load();
        },
        &error);
    raw->transfer_ms = MsSince(start);
    if (!blob)
    {
      raw->error = error;
      raw->done = true;
      return;
    }
    std::vector<u8> plain = std::move(*blob);
    if (Orca::Net::KeyframeHash(plain) != raw->info.hash ||
        !Orca::Net::DecryptKeyframe(raw->info.frame, raw->info.key, &plain))
    {
      raw->error = "the keyframe arrived damaged";
      raw->done = true;
      return;
    }
    const auto unpack_start = Clock::now();
    raw->ok = Orca::Net::UnpackKeyframe(plain, &raw->frame, &raw->image, &raw->nand) &&
              raw->frame == raw->info.frame;
    raw->unpack_ms = MsSince(unpack_start);
    if (!raw->ok)
      raw->error = "the keyframe doesn't unpack";
    raw->done = true;
  });
  match.download = std::move(download);
}

enum class JoinPump
{
  Waiting,
  Ready,
  Failed,
};

// Polls the room for a joiner (host arrival, keyframe offer, download progress) without blocking.
JoinPump PumpJoin(Match& match, std::string* last_status)
{
  {
    for (const Orca::Net::PeerEvent& event : Orca::Online::TakePeerEvents())
    {
      if (event.kind == Orca::Net::PeerEvent::Kind::Arrived && event.host)
      {
        match.host_here = true;
        match.host_seat = event.seat;
        match.host_name = event.name.empty() ? "your friend" : event.name;
        ShowJoinMessage(fmt::format("Joining {}'s game...", match.host_name),
                        static_cast<u32>(std::chrono::milliseconds(JOIN_LIMIT).count()));
      }
      else if (event.kind == Orca::Net::PeerEvent::Kind::Left && event.host)
      {
        // The host never carried the room's header (FailHeaderWait): the match can't start.
        if (event.reason == "mismatch")
        {
          WARN_LOG_FMT(ROLLBACK, "Online rules: the host left room {}: its game wasn't set up for "
                       "this match (mismatch)", Orca::Online::Code());
          match.join_error_code = "mismatch";
          match.join_error = "That match couldn't start";
          return JoinPump::Failed;
        }
        match.join_error_code = "peer_left";
        match.join_error = "Your friend left before you could join";
        return JoinPump::Failed;
      }
      else if (event.kind == Orca::Net::PeerEvent::Kind::Hold && !match.download)
      {
        // The host is in a single-player mode: wait; the time limit restarts after.
        if (event.holding != match.host_holding)
        {
          match.host_holding = event.holding;
          NOTICE_LOG_FMT(ROLLBACK, "Drop-in: the host {}",
                         event.holding ? "is in a single-player mode: the join waits" :
                                         "is back in the menus: the join goes on");
          if (event.holding)
          {
            Orca::Status::State("friend-waiting");
            ShowJoinMessage(
                fmt::format("{} is playing a single-player mode: you'll join when they're back in "
                            "the menus",
                            match.host_name.empty() ? "Your friend" : match.host_name),
                static_cast<u32>(std::chrono::milliseconds(JOIN_LIMIT).count()));
          }
          else
          {
            Orca::Status::State("joining 0");
            match.last_percent = std::max(match.last_percent, 0);
            ShowJoinMessage(fmt::format("Joining {}'s game...", match.host_name.empty() ?
                                                                    "your friend" :
                                                                    match.host_name),
                            static_cast<u32>(std::chrono::milliseconds(JOIN_LIMIT).count()));
          }
        }
      }
      else if (event.kind == Orca::Net::PeerEvent::Kind::Names)
      {
        // The host took another friend in mid-download; acknowledge these values once loaded.
        if (!match.later_names || event.keyframe.names_version > match.later_names->names_version)
          match.later_names = event.keyframe;
      }
      else if (event.kind == Orca::Net::PeerEvent::Kind::Keyframe && !match.download)
      {
        // Offered: any hold the host has now is for another friend's join.
        match.host_holding = false;
        match.offered_time = Clock::now();
        match.host_seat = event.seat;
        NOTICE_LOG_FMT(ROLLBACK, "Drop-in: the host offers keyframe {} (frame {}, {:.1f} MB)",
                       event.keyframe.id, event.keyframe.frame, event.keyframe.size / 1e6);
        StartDownload(match, event.keyframe);
      }
    }
    if (Orca::Online::RoomEnded())
    {
      // The caller reports the room's own code (room_full, room_mismatch, kicked, network).
      match.join_error_code.clear();
      match.join_error = fmt::format("Couldn't join: {}", Orca::Online::StatusLine());
      return JoinPump::Failed;
    }
    if (std::string status = Orca::Online::StatusLine(); status != *last_status)
    {
      NOTICE_LOG_FMT(NETPLAY, "Orca: {}", status);
      *last_status = std::move(status);
    }
    if (match.download)
    {
      // Downloading is the first half of joining; catching up is the second.
      const int percent = match.download->percent / 2;
      if (percent > match.last_percent)
      {
        match.last_percent = percent;
        Orca::Status::State(fmt::format("joining {}", percent));
      }
      if (match.download->done)
        return JoinPump::Ready;
    }
    // A held join has no time limit; the limit counts from when the host lets it go on.
    if (match.host_holding)
      match.boot_time = Clock::now();
    if (Clock::now() - match.boot_time > JOIN_LIMIT)
    {
      match.join_error_code = "network";
      match.join_error = "Your friend's game never sent its state";
      return JoinPump::Failed;
    }
  }
  return JoinPump::Waiting;
}

// A join that failed before this game changed: with "caps join" play on solo, otherwise stop.
void FailJoin(Core::System& system, Match& match)
{
  const std::string sentence =
      match.join_error.empty() ? std::string("Couldn't join your friend's game") : match.join_error;
  // An empty code: the room ended and gives its own reason.
  std::string code = match.join_error_code;
  if (code.empty())
    code = Orca::Online::RoomErrorCode();
  // A matchmade room that closed after its result (the opponent left before this game got in).
  if (code == "match-over")
    Orca::Status::Event("host-left match-over");
  else
    Orca::Status::Report(code.empty() ? "network" : code, sentence);
  match.join_error_code.clear();
  match.join_error.clear();
  if (Soft())
  {
    Core::DisplayMessage(sentence, 6000);
    BecomeSolo(match, "the join failed", "playing", match.loaded_time == Clock::time_point{});
    return;
  }
  StopEmulation(system, sentence);
  End("couldn't join");
}

enum class Load
{
  Done,
  // Nothing in this game changed; FailJoin reports it.
  Failed,
  // Half loaded; the game was stopped.
  Broken,
};

// Loads the host's downloaded keyframe and starts the joiner's session. On Done, `*frame_out` is
// the frame the state now starts at.
Load LoadKeyframe(Core::System& system, Match& match, int* frame_out)
{
  std::unique_ptr<Download> download = std::move(match.download);
  if (download->thread.joinable())
    download->thread.join();
  if (!download->ok)
  {
    ERROR_LOG_FMT(ROLLBACK, "Drop-in: keyframe {}: {}", download->info.id, download->error);
    match.join_error_code = "network";
    match.join_error = "Couldn't load your friend's game";
    return Load::Failed;
  }

  // The host's NAND first, then its machine state. The session NAND is a temporary folder.
  const auto load_start = Clock::now();
  IOS::HLE::FS::HostFileSystem* nand = HostNand(system);
  // Files the keyframe only references were also written by this boot: take them from this boot's
  // kept /tmp copies first, then from the NAND as it is.
  if (match.boot_nand && match.boot_nand->done)
  {
    for (Orca::Net::NandEntry& entry : download->nand)
    {
      if (!entry.reference)
        continue;
      const auto it = match.boot_nand->tmp_files.find(entry.path);
      if (it == match.boot_nand->tmp_files.end() || Orca::Net::NandHash(it->second) != entry.hash)
        continue;
      entry.data = it->second;
      entry.reference = false;
    }
  }
  // Close every NAND file: Windows can't read or delete open ones. The state reopens them.
  if (nand)
    nand->CloseHostFiles();
  std::string missing;
  if (nand && !Orca::Net::ResolveNandReferences(nand->HostRoot(), &download->nand, &missing))
  {
    ERROR_LOG_FMT(ROLLBACK, "Drop-in: this boot's NAND doesn't match the host's: {}", missing);
    // This game plays on solo with its own files.
    nand->ReopenHostFiles();
    match.join_error_code = "room_mismatch";
    match.join_error = "Your friend's game data differs from yours";
    return Load::Failed;
  }
  // Snapshots and NAND journal belong to the old game: new ring before the NAND changes.
  match.port.reset();
  match.port = std::make_unique<RingPort>(system, MAX_ROLLBACK);
  if (!nand || !Core::WiiRootIsTemporary() ||
      !Orca::Net::ReplaceNandTree(nand->HostRoot(), download->nand))
  {
    ERROR_LOG_FMT(ROLLBACK, "Drop-in: replacing the NAND failed (nand {}, temporary {})",
                  nand != nullptr, Core::WiiRootIsTemporary());
    StopEmulation(system, "Couldn't join: the game's save folder couldn't be replaced");
    Orca::Status::Error("internal", "Couldn't load your friend's game");
    End("couldn't join");
    return Load::Broken;
  }
  nand->ReloadFst();
  const int frame = download->frame;
  if (!match.port->LoadImage(std::move(download->image), frame))
  {
    ERROR_LOG_FMT(ROLLBACK, "Drop-in: loading keyframe frame {} into the ring failed", frame);
    StopEmulation(system, "Couldn't join: your friend's game state didn't load");
    Orca::Status::Error("internal", "Couldn't load your friend's game");
    End("couldn't join");
    return Load::Broken;
  }
  match.load_ms = MsSince(load_start);
  match.loaded_time = Clock::now();
  // This machine's game is now the host's: the UI's readers start over from it.
  Orca::Events::NoteResync();
  if (match.store)
    match.store->Delete(download->info.id);
  NOTICE_LOG_FMT(ROLLBACK,
                 "Drop-in: loaded keyframe frame {}: waited {:.0f} ms for it, transfer {:.0f} ms, "
                 "unpack {:.0f} ms, load {:.0f} ms",
                 frame, std::chrono::duration<double, std::milli>(match.offered_time - match.boot_time).count(),
                 download->transfer_ms, download->unpack_ms, match.load_ms);

  // A join during play takes its seat from the friend's room now.
  match.local_seat = Orca::Online::Seat();
  // The host's port values: the newest heard (the offer's, or a later broadcast's).
  if (match.later_names && match.later_names->names_version > download->info.names_version)
    SetNames(match, *match.later_names);
  else
    SetNames(match, download->info);
  match.later_names.reset();
  match.join_in_play = false;
  match.log.clear();
  match.log_base = frame;
  match.queued_local.clear();
  Orca::Net::Config config = SessionConfig();
  config.local_seat = match.local_seat;
  config.authority_seat = match.host_seat;
  config.start_frame = frame;
  std::array<Orca::Net::SeatPlan, Orca::Net::MAX_SEATS> plan{};
  plan[match.host_seat] = {0, Orca::Net::NEVER, 0};
  config.plan = plan;
  match.session =
      std::make_unique<Orca::Net::Session>(config, *match.port, *Orca::Online::Transport());
  // Acknowledge the host's port values from the first packet; the host waits for that to plug in.
  match.session->SetValuesHeld(match.names_version);
  SeedRoundTrips(match);
  match.port->SetCatchingUp(true);
  match.running = frame - 1;
  // Restart the stats line's frame rate: the frames before the keyframe never ran here.
  match.stats_frame = -1;
  // Into a friends room this player's own queue is over (JoinEndsQueue).
  EndQueueForJoin(match);
  // The host's game must carry this room's header (none for friends); checked at the next hook.
  Orca::UX::Rules::ExpectHeader(QueueMode(Orca::Online::RoomQueue()), Orca::Online::Code(),
                                RoomFlags());
  *frame_out = frame;
  return Load::Done;
}

// A launch join (ORCA_JOIN, or an invite the app opened): blocks until the host's keyframe is in.
// Returns the frame the state starts at, or nullopt (join failed, or stopping).
std::optional<int> JoinHost(Core::System& system, Match& match)
{
  JoinPump pump = JoinPump::Waiting;
  while (!Stopping(system))
  {
    // TakeCommands doesn't run during a launch join, so handle "leave" and "join" here.
    if (Orca::Status::Cap("leave") && Orca::Online::TakeLeaveRequest())
    {
      NOTICE_LOG_FMT(ROLLBACK, "Drop-in: left before joining, at the app's request");
      Leave(match);
      return std::nullopt;
    }
    if (Orca::Status::Cap("join"))
    {
      if (const auto code = Orca::Online::TakeJoinRequest(); code && *code != Orca::Online::Code())
      {
        NOTICE_LOG_FMT(ROLLBACK, "Drop-in: joining room {} instead", *code);
        OSD::DiscardTypedMessage(OSD::MessageType::OrcaJoin);
        ResetDropIn(match);
        Orca::Online::ShutdownInBackground();
        Orca::Online::StartRoom(*code, true);
        match.boot_time = Clock::now();
        match.offered_time = match.boot_time;
        match.host_here = false;
        match.host_holding = false;
        match.last_percent = -1;
        match.join_status.clear();
        match.later_names.reset();
        Orca::Status::State("joining 0");
      }
    }
    pump = PumpJoin(match, &match.join_status);
    if (pump != JoinPump::Waiting)
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if (pump == JoinPump::Waiting)
    return std::nullopt;
  if (pump == JoinPump::Failed)
  {
    FailJoin(system, match);
    return std::nullopt;
  }
  int frame = -1;
  switch (LoadKeyframe(system, match, &frame))
  {
  case Load::Done:
    return frame;
  case Load::Failed:
    FailJoin(system, match);
    return std::nullopt;
  case Load::Broken:
    return std::nullopt;
  }
  return std::nullopt;
}

// ---- The app's commands (embed mode, with "caps join") ----

// "leave": a joiner asks its host to unplug it at an agreed frame; a host opens a new room.
void Leave(Match& match)
{
  // Leaving a ranked set forfeits it, and the screen says so (UX/SetEnd.h).
  if (Orca::Online::RoomQueue() == "ranked" && Orca::Online::MatchLive())
    Orca::UX::SetEnd::Current().SelfLeft(false, Orca::UX::SetEnd::NowMs());
  Orca::UX::Search::End();
  match.linger_until.reset();
  if (match.joining && match.session && match.session->CatchingUp())
  {
    // Still catching up, so never plugged in: there's no unplug frame to agree on. Leaving the room
    // is enough; the host drops the seat.
    NOTICE_LOG_FMT(ROLLBACK, "Drop-in: left while catching up, frame {}", match.running);
    BecomeSolo(match, "left while catching up", nullptr);
    Orca::Status::Event("left");
    return;
  }
  if (match.joining && match.session)
  {
    if (!match.leave_requested)
    {
      match.leave_requested = true;
      NOTICE_LOG_FMT(ROLLBACK, "Drop-in: leaving at the app's request, frame {}", match.running);
      match.session->RequestLeave();
    }
    return;
  }
  if (match.joining)
  {
    // Still downloading: give up. A launch joiner never loaded a keyframe: its own boot, port 1.
    BecomeSolo(match, "left before joining", nullptr, match.loaded_time == Clock::time_point{});
  }
  else
  {
    // Leave even with nobody in the room: the page expects "left", then the new room.
    if (match.session)
      GoSolo(match, "left", nullptr);
    ResetDropIn(match);
    ReopenRoom(match);
  }
  Orca::Status::Event("left");
}

// "join <code>": join a friend's game during play, playing solo until its keyframe loads.
void JoinInPlay(Match& match, const std::string& code)
{
  if (code == Orca::Online::Code() && match.joining)
    return;
  if (match.session)
    GoSolo(match, "joining another game", nullptr);
  ResetDropIn(match);
  ForgetRoom(match);
  match.linger_until.reset();
  // A queue match: the join's own messages replace the search's.
  Orca::UX::Search::Matched(false);
  Orca::Online::ShutdownInBackground();
  Orca::Online::StartRoom(code, true);
  NOTICE_LOG_FMT(ROLLBACK, "Drop-in: joining room {} during play, frame {}", code, match.running + 1);
  match.joining = true;
  match.join_in_play = true;
  match.later_names.reset();
  match.boot_time = Clock::now();
  match.offered_time = match.boot_time;
  match.host_here = false;
  match.host_holding = false;
  match.joined = false;
  match.leave_requested = false;
  match.last_percent = -1;
  match.join_status.clear();
  // Too early in this game's boot to have the files the keyframe only references: run the boot to
  // boot_nand_frame first, unthrottled and unseen.
  if (const Orca::Profile* profile = Orca::ActiveProfile();
      profile && profile->boot_nand_frame &&
      match.running + 1 < static_cast<int>(*profile->boot_nand_frame))
  {
    match.join_in_play = false;
    match.boot_ahead_to = static_cast<int>(*profile->boot_nand_frame);
    match.port->SetCatchingUp(true);
  }
  Orca::Status::State("joining 0");
}

// "host <code>": the queue matched this game with an opponent who will drop in. Only while alone:
// leave this room and host the matched one, on port 1 (Reseat).
void HostInPlay(Match& match, const std::string& code)
{
  const bool alone = match.port && !match.session && !match.joining && !match.download &&
                     !match.job && !match.keyframe_wanted && match.waiting.empty() &&
                     match.plugging.empty() && match.seated.empty() && !match.port->CatchingUp() &&
                     !Orca::Online::DropInPending();
  if (!alone)
  {
    WARN_LOG_FMT(ROLLBACK, "Matchmaking: can't host room {}: this game isn't alone", code);
    Orca::Status::Line("unsupported host");
    return;
  }
  Reseat(match);
  ResetDropIn(match);
  ForgetRoom(match);
  match.linger_until.reset();
  Orca::Online::ShutdownInBackground();
  Orca::Online::StartRoom(code, false);
  Orca::UX::Search::Matched(true);
  // The header and rule locks follow at the next alone boundary, once the welcome names the queue.
  NOTICE_LOG_FMT(ROLLBACK, "Matchmaking: hosting room {} from frame {}", code, match.running + 1);
}

bool MaybeRestoreQueueImage(Core::System& system, Match& match);

// Takes the app's commands where the game may change hands: never mid re-run or in a launch join.
void TakeCommands(Core::System& system, Match& match)
{
  // A command sent before the app's "caps" line stays queued in Online until it arrives.
  if (!Orca::Status::Cap("join") && !Orca::Status::Cap("leave") && !Orca::Status::Cap("host"))
    return;
  if (match.session &&
      (match.session->Resimulating() || match.session->CurrentFrame() != match.running + 1))
  {
    return;
  }
  if (match.joining && !match.join_in_play && !match.session)
    return;
  if (Orca::Status::Cap("leave") && Orca::Online::TakeLeaveRequest())
    Leave(match);
  // Restore this player's own game once a queue room is over, so the next pairing doesn't start
  // from the last opponent's character select.
  const bool restored = MaybeRestoreQueueImage(system, match);
  // On the queue's own character select, the current game is the one to come back to.
  const bool queue_css =
      Orca::Status::Cap("queue2") && (restored || Orca::UX::Queue::OnSoloCss());
  if (Orca::Status::Cap("join"))
  {
    if (const auto code = Orca::Online::TakeJoinRequest())
    {
      match.want_queue_image = queue_css && !match.session && !match.joining;
      JoinInPlay(match, *code);
    }
  }
  if (Orca::Status::Cap("host"))
  {
    if (const auto code = Orca::Online::TakeHostRequest())
    {
      match.want_queue_image = queue_css && !match.session && !match.joining;
      HostInPlay(match, *code);
    }
  }
}

// ---- The queue image (UX/Queue.h) ----

// Captures this player's own game and NAND, to restore when the queue's room is over.
void CaptureQueueImage(Core::System& system, Match& match)
{
  match.want_queue_image = false;
  match.ranked_fought = false;
  auto qi = std::make_unique<Match::QueueImage>();
  const auto start = Clock::now();
  if (!SnapshotRing::Capture(system, &qi->image, false))
  {
    ERROR_LOG_FMT(ROLLBACK, "Queue: capturing this player's own game failed");
    return;
  }
  if (IOS::HLE::FS::HostFileSystem* nand = HostNand(system))
  {
    nand->CloseHostFiles();
    const bool ok = Orca::Net::ReadNandTree(nand->HostRoot(), &qi->nand);
    nand->ReopenHostFiles();
    if (!ok)
    {
      ERROR_LOG_FMT(ROLLBACK, "Queue: reading the NAND for this player's own game failed");
      return;
    }
  }
  qi->taken = Clock::now();
  u64 nand_bytes = 0;
  for (const Orca::Net::NandEntry& e : qi->nand)
    nand_bytes += e.data.size();
  NOTICE_LOG_FMT(ROLLBACK, "Queue: this player's own game kept at frame {} ({:.0f} ms, NAND {} "
                 "KB)",
                 match.running + 1, MsSince(start), nand_bytes / 1024);
  match.queue_image = std::move(qi);
}

// Whether this game is in a room the queue made (its welcome said casual or ranked).
bool InQueueRoom()
{
  return !Orca::Online::RoomEnded() && QueueMode(Orca::Online::RoomQueue()) !=
                                           Orca::UX::Rules::Mode::None;
}

// Restores the queue image once the queue's room is over: solo, no drop-in under way, no ranked
// verdict pending. The player is ready again unless their own ready timer ran out.
bool MaybeRestoreQueueImage(Core::System& system, Match& match)
{
  if (!match.queue_image)
    return false;
  Match::QueueImage& qi = *match.queue_image;
  // Armed once the room was actually entered: hosting it, or a session in it.
  if (!qi.armed && InQueueRoom() &&
      ((match.local_seat == 0 && !match.joining) || (match.joining && match.session)))
  {
    qi.armed = true;
    NOTICE_LOG_FMT(ROLLBACK, "Queue: in room {} ({})", Orca::Online::Code(),
                   Orca::Online::RoomQueue());
  }
  // Out of the queue (backed out to the menus): nothing to come back to.
  if (!Orca::UX::Queue::Active())
  {
    match.queue_image.reset();
    return false;
  }
  if (!qi.armed || match.session || match.joining || match.download || match.job ||
      match.linger_until || !match.waiting.empty() || !match.plugging.empty() || InQueueRoom() ||
      !match.port || match.port->CatchingUp())
  {
    return false;
  }
  std::unique_ptr<Match::QueueImage> image = std::move(match.queue_image);
  const auto start = Clock::now();
  IOS::HLE::FS::HostFileSystem* nand = HostNand(system);
  if (!nand || !Core::WiiRootIsTemporary())
  {
    ERROR_LOG_FMT(ROLLBACK, "Queue: no session NAND to put this player's own game back into");
    return false;
  }
  nand->CloseHostFiles();
  // New ring before the NAND changes: the old snapshots belong to the room's game.
  match.port.reset();
  match.port = std::make_unique<RingPort>(system, MAX_ROLLBACK);
  if (!Orca::Net::ReplaceNandTree(nand->HostRoot(), image->nand))
  {
    StopEmulation(system, "Couldn't go back to your own game: its save folder couldn't be "
                          "replaced");
    Orca::Status::Error("internal", "Couldn't go back to your own game");
    End("queue image");
    return false;
  }
  nand->ReloadFst();
  const int frame = match.running + 1;
  if (!match.port->LoadImage(std::move(image->image), frame))
  {
    StopEmulation(system, "Couldn't go back to your own game");
    Orca::Status::Error("internal", "Couldn't go back to your own game");
    End("queue image");
    return false;
  }
  Orca::Events::NoteResync();
  // The header the frame hook read was the room's game's; this game's own is read at the next hook.
  Orca::UX::Rules::MemoryReplaced();
  match.log.clear();
  match.log_base = frame;
  match.queued_local.clear();
  match.keyframe.reset();
  // This player's own game plays port 1, and a room of its own opens (a former joiner had none).
  Reseat(match);
  if (match.tell_no_room)
  {
    match.tell_no_room = false;
    match.reopen_pending = Clock::now() + REOPEN_DELAY;
  }
  const bool mine = Orca::UX::Queue::TakeTimeoutWasMine();
  // After a ranked set (a fight was played in that room) the player presses Start to search again.
  const bool fought = std::exchange(match.ranked_fought, false);
  const char* why = mine ? "the ready timer ran out on us" : fought ? "a ranked set was played" : "";
  Orca::UX::Queue::AfterRestore(!mine && !fought, why);
  NOTICE_LOG_FMT(ROLLBACK, "Queue: back on this player's own character select at frame {} in {:.0f} "
                 "ms ({:.1f} s after the room's game was kept){}",
                 frame, MsSince(start),
                 std::chrono::duration<double>(start - image->taken).count(),
                 *why ? fmt::format("; not ready ({})", why) : std::string());
  return true;
}

// ---- The friends lobby and the queue ----

// A Casual or Ranked pick the frame hook saw at this boundary while this game wasn't alone
// (UX/OnlineMenu.h TakeLobbyPick), taken after the app's commands (DecideLobbyPick). A host whose
// app can search that queue now leaves its room as the app's Leave does: its friends play on, hear
// `host-left`, and come home on the character select the pick opened (ComeHome). It prints
// `orca state left lobby`, then the pick as if made alone. Without the pick's cap it keeps its
// friends and prints the pick unarmed (`orca menu online <queue> kept`), so the app says why and
// cancels; ArmKeptPick arms it once they are gone. Only at a boundary the app's commands may take
// (never mid re-run). Local: the session ends here, so only the room's leave reaches the friends.
void TakeLobbyPickNow(Match& match)
{
  auto pick = Orca::UX::TakeLobbyPick();
  // A fresh pick replaces one that waited.
  if (pick)
    match.lobby_pick.reset();
  else if (match.lobby_pick)
  {
    pick = std::exchange(match.lobby_pick, std::nullopt);
    // A pick that waited is stale once the game has moved on from it (main menu, stage select,
    // fight): leaving the friends then would be for nothing.
    if (!Orca::UX::PickStands(*pick))
    {
      NOTICE_LOG_FMT(ROLLBACK, "Online menu: the {} pick that waited for a boundary is stale by "
                               "frame {} (the game went on from it): dropped",
                     Orca::UX::MenuEventText(*pick, true), match.running + 1);
      return;
    }
  }
  if (!pick)
    return;
  const char* const queue = *pick == Orca::UX::OnlinePick::Ranked ? "ranked" : "casual";
  if (match.session &&
      (match.session->Resimulating() || match.session->CurrentFrame() != match.running + 1))
  {
    // The game already left the menu for the character select: keep the pick, or Start there
    // would do nothing.
    NOTICE_LOG_FMT(ROLLBACK, "Online menu: {} picked mid re-run, frame {}: at the next boundary",
                   queue, match.running + 1);
    match.lobby_pick = pick;
    return;
  }
  LobbyPickInputs in;
  in.joining = match.joining;
  in.local_seat = match.local_seat;
  in.queue_room = InQueueRoom();
  in.host_cap = Orca::Status::Cap("host");
  in.pick_cap = Orca::UX::PickSearchable(*pick);
  in.session = match.session != nullptr;
  in.session_idle = match.session && match.session->Idle();
  in.drop_in_friends =
      !match.waiting.empty() || !match.plugging.empty() || !match.seated.empty();
  in.arrival_pending = Orca::Online::ArrivalPending();
  in.keyframe_kept = match.keyframe || match.job || match.keyframe_wanted;
  const LobbyPickPlan plan = DecideLobbyPick(in);
  // A newer pick replaces any kept or friends pick, whatever it comes to.
  match.kept_pick.reset();
  match.friends_pick.reset();
  if (plan.drop_keyframe)
    ResetDropIn(match);
  switch (plan.step)
  {
  case LobbyPickStep::Ignore:
    NOTICE_LOG_FMT(ROLLBACK, "Online menu: {} picked in a game that isn't this player's alone "
                             "({}): nothing to do here",
                   queue, match.joining ? "its host's" : "with friends");
    return;
  case LobbyPickStep::Announce:
    if (!plan.arm)
    {
      NOTICE_LOG_FMT(ROLLBACK, "Online menu: {} picked with friends in room {}, which the page "
                               "can't search now (no pick-{}): staying with them, frame {}",
                     queue, Orca::Online::Code(), queue, match.running + 1);
      match.kept_pick = pick;
    }
    break;
  case LobbyPickStep::Leave:
    NOTICE_LOG_FMT(ROLLBACK, "Online menu: {} picked with friends in room {}: leaving it for the "
                             "queue, frame {}{}",
                   queue, Orca::Online::Code(), match.running + 1,
                   in.arrival_pending ? " (a friend arriving)" : "");
    match.linger_until.reset();
    if (match.session)
      GoSolo(match, "left for the queue", nullptr);
    ResetDropIn(match);
    ReopenRoom(match);
    // Either player may have pressed it in a shared menu: say what happened, not who did it.
    Core::DisplayMessage(*pick == Orca::UX::OnlinePick::Ranked ? "Left the lobby for Ranked" :
                                                                 "Left the lobby for Casual",
                         4000);
    Orca::Status::Event("left lobby");
    break;
  }
  NOTICE_LOG_FMT(ROLLBACK, "Online menu: orca menu {}{}",
                 Orca::UX::MenuEventText(*pick, Orca::Status::Cap("host")),
                 plan.arm ? "" : " (unarmed)");
  Orca::UX::AnnounceOnlinePick(*pick, plan.arm);
}

// A former joiner on a port other than 1 with no room (MayComeHome), at its first alone boundary
// on the menus: it moves to port 1 from the next frame and opens a room of its own, as after a
// queue room (MaybeRestoreQueueImage). Solo with no session, so nothing re-runs this boundary and
// only this game changes; the next friend's keyframe carries it.
void ComeHome(Match& match)
{
  // Every boundary: the common case (port 1, or not solo) asks the room nothing.
  if (match.local_seat == 0 || match.joining || match.session)
    return;
  HomeInputs in;
  in.local_seat = match.local_seat;
  in.joining = match.joining;
  in.session = match.session != nullptr;
  in.room_up = !Orca::Online::RoomEnded();
  in.solo_idle = SoloIdle(match);
  in.drop_in_pending = Orca::Online::DropInPending();
  in.on_menus = Orca::UX::OnTheMenus();
  in.queue_image = match.queue_image != nullptr;
  in.lingering = match.linger_until.has_value();
  if (!MayComeHome(in))
    return;
  NOTICE_LOG_FMT(ROLLBACK, "Drop-in: back to a game of its own on the menus (frame {}): a room of "
                           "its own opens",
                 match.running + 1);
  Reseat(match);
  // On port 1 now; ReopenRoom opens its own room a moment later.
  ReopenRoom(match);
  // Home on the character select a Casual or Ranked pick opened (its host left for the queue at
  // that pick): announce the same pick, so Start readies it on the queue's own character select.
  // Alone now, the app answers it as any alone pick.
  const std::optional<Orca::UX::OnlinePick> pick = Orca::UX::CssPick();
  if (AnnounceHomePick(pick.has_value(), Orca::Status::Cap("host"),
                       Orca::UX::Queue::Active() ||
                           Orca::UX::Search::Current() != Orca::UX::Search::State::None))
  {
    NOTICE_LOG_FMT(ROLLBACK, "Drop-in: home on the {} character select: orca menu {}",
                   *pick == Orca::UX::OnlinePick::Ranked ? "ranked" : "casual",
                   Orca::UX::MenuEventText(*pick, true));
    Orca::UX::AnnounceOnlinePick(*pick);
  }
}

// A pick this host announced unarmed to keep its friends (TakeLobbyPickNow). Once the friends are
// gone and the game is alone on the character select that pick opened, it is armed as an alone
// pick (`orca menu online <queue>`). Dropped once the game moves on from it or a queue or search is
// under way. First runs only.
void ArmKeptPick(Match& match)
{
  if (!match.kept_pick || (match.session && match.session->Resimulating()))
    return;
  KeptPickInputs in;
  in.stands = Orca::UX::PickStands(*match.kept_pick);
  in.alone = match.local_seat == 0 && !match.joining && match.seated.empty() &&
             AloneAt(match.running + 1, SoloIdle(match), Orca::Online::DropInPending(),
                     match.keyframe ? std::optional(match.keyframe->frame) : std::nullopt);
  in.queue_or_search = Orca::UX::Queue::Active() ||
                       Orca::UX::Search::Current() != Orca::UX::Search::State::None;
  const Orca::UX::OnlinePick pick = *match.kept_pick;
  const char* const queue = pick == Orca::UX::OnlinePick::Ranked ? "ranked" : "casual";
  switch (DecideKeptPick(in))
  {
  case KeptPickStep::Wait:
    return;
  case KeptPickStep::Drop:
    match.kept_pick.reset();
    NOTICE_LOG_FMT(ROLLBACK, "Online menu: the {} pick kept with friends is over by frame {}: "
                             "dropped",
                   queue, match.running + 1);
    return;
  case KeptPickStep::Arm:
    match.kept_pick.reset();
    NOTICE_LOG_FMT(ROLLBACK, "Online menu: the {} pick kept with friends, alone now (frame {}): "
                             "orca menu {}",
                   queue, match.running + 1,
                   Orca::UX::MenuEventText(pick, Orca::Status::Cap("host")));
    Orca::UX::AnnounceOnlinePick(pick);
    return;
  }
}

// A host whose queue ended for a friend: once a friend came in and all of them left, and it is
// alone again on that pick's character select, the pick is armed again so Start searches. First
// runs only.
void RearmFriendsPick(Match& match)
{
  if (!match.friends_pick || (match.session && match.session->Resimulating()))
    return;
  if (!match.seated.empty())
    match.friends_pick_played = true;
  FriendsPickInputs in;
  in.played = match.friends_pick_played;
  in.alone = match.local_seat == 0 && !match.joining && match.seated.empty() &&
             AloneAt(match.running + 1, SoloIdle(match), Orca::Online::DropInPending(),
                     match.keyframe ? std::optional(match.keyframe->frame) : std::nullopt);
  in.on_pick_select = Orca::UX::CssPick() == match.friends_pick;
  // Only a queue the page can search now: unprompted, a refusal would only confuse.
  in.host_cap = Orca::Status::Cap("host") && Orca::UX::PickSearchable(*match.friends_pick);
  in.queue_or_search = QueueOrSearch();
  in.elsewhere = match.joining || match.local_seat != 0;
  const Orca::UX::OnlinePick pick = *match.friends_pick;
  const char* const queue = pick == Orca::UX::OnlinePick::Ranked ? "ranked" : "casual";
  switch (DecideFriendsPick(in))
  {
  case FriendsPickStep::Wait:
    return;
  case FriendsPickStep::Drop:
    match.friends_pick.reset();
    return;
  case FriendsPickStep::Arm:
    match.friends_pick.reset();
    NOTICE_LOG_FMT(ROLLBACK, "Queue: the friends are gone and this game is alone again on the {} "
                             "character select (frame {}): orca menu {}",
                   queue, match.running + 1,
                   Orca::UX::MenuEventText(pick, Orca::Status::Cap("host")));
    Orca::UX::AnnounceOnlinePick(pick);
    return;
  }
}

// A player on a port other than 1 that didn't come home at this boundary (say, in a fight) has no
// room of its own: the app hears "no-room" once, after the old room has finished leaving, so it
// never sees that room again. It still comes home once back on the menus.
void TellNoRoom(Match& match)
{
  if (!match.tell_no_room || Orca::Online::RoomsLeaving())
    return;
  match.tell_no_room = false;
  NOTICE_LOG_FMT(ROLLBACK, "Drop-in: no room of its own on port {}", match.local_seat + 1);
  Orca::Status::State("no-room");
}

// A joiner whose host's game carries another header than this room's leaves before playing it.
void LeaveOnMismatch(Match& match)
{
  if (match.session && match.session->Resimulating())
    return;
  const auto why = Orca::UX::Rules::TakeMismatch();
  if (!why)
    return;
  WARN_LOG_FMT(ROLLBACK, "Online rules: leaving room {}: {}", Orca::Online::Code(), *why);
  Orca::Online::SetLeaveReason(*why);
  Orca::Status::Report(*why, "That match couldn't start");
  Leave(match);
}

// ---- Ports, for the in-game UI ----
//
// Each port's values (player name and own controls) are decided by the host from a frame it picks;
// every machine holds them before running that frame, so the frame hook writes the same bytes
// everywhere (UX/NameTags.h). The host plugs a newcomer in only once every player acknowledges its
// values, and never changes its own mid-session.

// Host: a seat's new values, from their frame. Keeps a few entries per seat.
void AddName(Match& match, Orca::Net::KeyframeInfo::Name name)
{
  auto& history = match.port_names[name.seat];
  if (!history.empty() && history.back().from >= name.from)
    history.pop_back();
  history.push_back(std::move(name));
  if (history.size() > 4)
    history.erase(history.begin());
  ++match.names_version;
}

std::vector<Orca::Net::KeyframeInfo::Name> AllNames(const Match& match)
{
  std::vector<Orca::Net::KeyframeInfo::Name> all;
  for (const auto& [seat, history] : match.port_names)
    all.insert(all.end(), history.begin(), history.end());
  return all;
}

// A joiner takes the host's values and version; its packets acknowledge that version from now on.
void SetNames(Match& match, const Orca::Net::KeyframeInfo& from_host)
{
  match.port_names.clear();
  std::vector<Orca::Net::KeyframeInfo::Name> sorted = from_host.names;
  std::stable_sort(sorted.begin(), sorted.end(),
                   [](const auto& a, const auto& b) { return a.from < b.from; });
  for (auto& name : sorted)
  {
    NOTICE_LOG_FMT(ROLLBACK, "Drop-in: port {} is {} from frame {}{}", name.seat + 1, name.name,
                   name.from, name.controls.empty() ? "" : ", with their own controls");
    match.port_names[name.seat].push_back(std::move(name));
  }
  match.names_version = from_host.names_version;
  if (match.session)
    match.session->SetValuesHeld(match.names_version);
}

// The values a seat had at `frame` (the newest from before it), or none.
const Orca::Net::KeyframeInfo::Name* EntryAt(const Match& match, int seat, int frame)
{
  const auto it = match.port_names.find(seat);
  if (it == match.port_names.end())
    return nullptr;
  // Entries are in frame order.
  for (auto entry = it->second.rbegin(); entry != it->second.rend(); ++entry)
  {
    if (frame >= entry->from)
      return &*entry;
  }
  return nullptr;
}

// Host, as a session starts: record its own name and controls from `frame` if they changed.
void RefreshOwnValues(Match& match, int frame)
{
  const Orca::Net::KeyframeInfo::Name* now = EntryAt(match, match.local_seat, frame);
  std::string own = Orca::Online::Seat() == match.local_seat ?
                        Orca::Online::SeatName(match.local_seat) :
                        std::string();
  if (own.empty() && now)
    own = now->name;
  std::vector<u8> controls = Orca::Events::OwnControls();
  std::vector<u8> queue = Orca::Events::OwnQueue();
  if (now ? now->name == own && now->controls == controls && now->queue == queue :
            own.empty() && controls.empty() && queue.empty())
  {
    return;
  }
  NOTICE_LOG_FMT(ROLLBACK, "Drop-in: port {} is {} from frame {}{}", match.local_seat + 1, own,
                 frame, controls.empty() ? "" : ", with their own controls");
  AddName(match, {match.local_seat, frame, std::move(own), std::move(controls), std::move(queue)});
}

// Controllers plugged in at `frame` (the session's plan, or solo just this player).
std::vector<Orca::Events::PortInfo> PortsAt(Match& match, int frame)
{
  // A host names its own port only while solo: in a session a friend may already have run this
  // frame. A joiner takes all values from its host.
  if (!match.joining && !match.session && !match.port_names.count(match.local_seat) &&
      Orca::Online::Seat() == match.local_seat)
  {
    if (std::string own = Orca::Online::SeatName(match.local_seat); !own.empty())
      AddName(match, {match.local_seat, frame, std::move(own), {}});
  }
  const auto port = [&](int seat) {
    Orca::Events::PortInfo info{seat, "", seat != match.local_seat, {}};
    if (const Orca::Net::KeyframeInfo::Name* entry = EntryAt(match, seat, frame))
    {
      info.name = entry->name;
      info.controls = entry->controls;
      info.queue = entry->queue;
    }
    return info;
  };
  std::vector<Orca::Events::PortInfo> ports;
  if (!match.session)
  {
    ports.push_back(port(match.local_seat));
    return ports;
  }
  for (int seat = 0; seat < Orca::Net::MAX_SEATS; ++seat)
  {
    if (match.session->Plugged(seat, frame))
      ports.push_back(port(seat));
  }
  return ports;
}

// ---- Stats ----

// Publishes link stats every boundary, and the "orca stats" line about once a second.
void PublishStats(Match& match)
{
  if (match.session && match.session->Resimulating())
    return;
  const Orca::Net::Stats stats = match.session ? match.session->GetStats() : Orca::Net::Stats{};
  int peers = 0;
  if (match.session)
  {
    for (int seat = 0; seat < Orca::Net::MAX_SEATS; ++seat)
    {
      if (seat != match.local_seat && match.session->Plugged(seat, match.running))
        ++peers;
    }
  }
  Orca::Events::LinkStats link;
  link.online = peers > 0;
  link.delay = stats.delay;
  link.deepest_rollback = match.stats_rbmax;
  link.stalls = stats.stalls;
  link.hitches = stats.hitches;
  link.peer_hitches = stats.peer_hitches;
  link.round_trip_ms = Orca::Online::RoundTripMs();
  link.confirmed_frame = stats.confirmed_frame;
  const Orca::Net::DirectSummary direct = Orca::Online::Direct();
  link.transport = direct.transport;
  link.link_rtt_ms = direct.rtt_ms;

  const auto now = Clock::now();
  // Paused time is left out of the interval, so fps counts running time.
  match.stats_time += TakePausedTime();
  if (match.stats_frame < 0)
  {
    match.stats_time = now;
    match.stats_frame = match.running;
    match.stats_last = stats;
    match.stats_input = Orca::Events::GetInputAge();
  }
  const double seconds = std::chrono::duration<double>(now - match.stats_time).count();
  // A new session restarts its counters.
  const bool reset = stats.rollbacks < match.stats_last.rollbacks ||
                     stats.stalls < match.stats_last.stalls;
  const Orca::Net::Stats& last = reset ? Orca::Net::Stats{} : match.stats_last;
  if (seconds > 0)
    link.rollbacks_per_second = (stats.rollbacks - last.rollbacks) / seconds;
  Orca::Events::PublishLinkStats(link);
  if (seconds < 1.0)
    return;
  picojson::object o;
  const auto num = [&o](const char* key, double value) { o[key] = picojson::value(value); };
  num("f", match.running);
  num("seat", match.local_seat);
  num("peers", peers);
  num("ping", link.round_trip_ms);
  // Median round trips the delay follows: to the relay, and the slowest friend's (-1 if unknown).
  num("pmed", stats.rtt_median);
  num("fpmed", stats.peer_rtt_median);
  num("delay", stats.delay);
  num("rb", stats.rollbacks - last.rollbacks);
  num("rbf", stats.resimulated_frames - last.resimulated_frames);
  num("rbmax", match.stats_rbmax);
  num("st", stats.stalls - last.stalls);
  num("stms", std::round(match.stats_stall_ms));
  num("ds", s_desyncs);
  num("fps", std::round((match.running - match.stats_frame) / seconds * 10) / 10);
  num("hi", stats.hitches);
  num("phi", stats.peer_hitches);
  num("tx", link.transport);
  num("lrtt", link.link_rtt_ms);
  // Input age (ControllerSource.h): "pa" is the mean age in ms of the newest adapter report at
  // frames whose pad changed, "rpf" adapter reports per frame; -1 without an adapter.
  const Orca::Events::InputAge input = Orca::Events::GetInputAge();
  const Orca::Events::InputAge& input_last =
      input.frames < match.stats_input.frames ? Orca::Events::InputAge{} : match.stats_input;
  const u64 input_changes = input.changes - input_last.changes;
  const u64 input_frames = input.frames - input_last.frames;
  num("pa", input_changes > 0 ?
                std::round((input.age_ms - input_last.age_ms) / input_changes * 10) / 10 :
                -1);
  num("rpf", input_frames > 0 ?
                 std::round(double(input.reports - input_last.reports) / input_frames * 10) / 10 :
                 -1);
  Orca::Status::Stats(picojson::value(o).serialize());
  match.stats_time = now;
  match.stats_frame = match.running;
  match.stats_last = stats;
  match.stats_input = input;
  match.stats_rbmax = 0;
  match.stats_stall_ms = 0;
}

// Waiting in a ranked room for the verdict after the opponent left: once the room closes or
// VERDICT_WAIT passes, open a room of its own (a former joiner moves to port 1 first).
void Linger(Match& match)
{
  if (!match.linger_until)
    return;
  // The verdict came and the room stays open (set over); SetOverLeave takes this game out of it.
  if (Orca::Online::SetOver())
  {
    NOTICE_LOG_FMT(ROLLBACK, "Matchmaking: the verdict came (set over): done waiting");
    match.linger_until.reset();
    return;
  }
  const bool over = Orca::Online::RoomEnded();
  if (!over && Clock::now() < *match.linger_until)
    return;
  match.linger_until.reset();
  // A verdict that came already cleared the countdown; none will come now.
  Orca::UX::SetEnd::Current().NoVerdict(Orca::UX::SetEnd::NowMs());
  if (match.local_seat != 0)
  {
    NOTICE_LOG_FMT(ROLLBACK, "Matchmaking: done waiting in the room ({})",
                   over ? Orca::Online::RoomErrorCode() : "no verdict in time");
    if (over && Orca::Online::RoomErrorCode() == "match-over")
      Orca::Status::Event("host-left match-over");
    Reseat(match);
    ReopenRoom(match);
    return;
  }
  if (!over)
  {
    NOTICE_LOG_FMT(ROLLBACK, "Matchmaking: no verdict in {} s: a room of this game's own",
                   VERDICT_WAIT.count());
    ResetDropIn(match);
    ReopenRoom(match);
  }
}

// Reports games whose frames are now final (UX/Results.h). Every machine reads the same frames, so
// both report the same games. A game counts only once an opponent is plugged in.
void ConfirmResults(Match& match)
{
  if (!match.started || match.finished || !match.port)
    return;
  int confirmed = match.running;
  int plug = INT32_MAX;
  if (match.session)
  {
    if (match.session->Resimulating())
      return;
    confirmed = std::min(confirmed, match.session->ConfirmedFrame() + 1);
    int plugged = 0, latest = -1;
    for (int seat = 0; seat < Orca::Net::MAX_SEATS; ++seat)
    {
      const Orca::Net::SeatPlan& plan = match.session->Plan()[seat];
      if (plan.plug_from == Orca::Net::NEVER || plan.unplug_from != Orca::Net::NEVER)
        continue;
      ++plugged;
      latest = std::max(latest, plan.plug_from);
    }
    if (plugged >= 2)
      plug = latest;
  }
  // The casual ready timer ran out (UX/Queue.h), at the same final frame on both machines.
  Orca::UX::Queue::ConfirmTimeout(confirmed, match.local_seat);
  // Ranked: a player who didn't pick a character in time before the first game leaves the room,
  // which forfeits; the other waits for the verdict.
  if (const auto no_show = Orca::UX::Rules::ConfirmNoShow(confirmed))
  {
    const bool mine = *no_show == match.local_seat;
    NOTICE_LOG_FMT(ROLLBACK, "Online rules: port {} didn't pick a character in time{}",
                   *no_show + 1, mine ? ": leaving the set" : "");
    if (mine && Orca::Online::RoomQueue() == "ranked" && plug != INT32_MAX)
    {
      Orca::Online::SetLeaveReason("no-show");
      Orca::UX::SetEnd::Current().SelfLeft(true, Orca::UX::SetEnd::NowMs());
      Orca::Online::RequestLeave();
    }
    else if (!mine)
    {
      Core::DisplayMessage("Your opponent didn't pick a character in time", 6000);
    }
  }
  for (const Orca::UX::GameResult& r : Orca::UX::Tracker().Confirm(confirmed, plug))
  {
    Orca::Net::GameReport report;
    report.kind = r.kind == Orca::UX::GameResult::Kind::Win  ? Orca::Net::GameReport::Kind::Win :
                  r.kind == Orca::UX::GameResult::Kind::Draw ? Orca::Net::GameReport::Kind::Draw :
                                                               Orca::Net::GameReport::Kind::Void;
    report.winner_seat = r.winner_port;
    report.why = r.why;
    if (report.kind != Orca::Net::GameReport::Kind::Void)
      report.detail = r.DetailJson();
    report.start = r.start;
    report.number = r.number;
    report.set_done = r.set_done;
    report.set_winner_seat = r.set_winner;
    if (r.set_done)
    {
      NOTICE_LOG_FMT(ROLLBACK, "Matchmaking: the set ended with game {}: {}", r.number,
                     r.set_done == 1 ? fmt::format("port {} won", r.set_winner + 1) :
                                       std::string("void"));
    }
    NOTICE_LOG_FMT(ROLLBACK, "Matchmaking: a game ended at frame {}: {}", r.frame,
                   report.kind == Orca::Net::GameReport::Kind::Win ?
                       fmt::format("port {} won, {}", r.winner_port + 1, report.detail) :
                   report.kind == Orca::Net::GameReport::Kind::Draw ?
                       fmt::format("a draw, {}", report.detail) :
                       fmt::format("void ({})", r.why));
    Orca::Online::ReportGame(report);
  }
}

// How long the session has waited on a friend inside StepSession, for the overlay.
void PublishStall(Match& match, double stalled_ms)
{
  Orca::Events::LinkStats link = Orca::Events::GetLinkStats();
  link.stalled_ms = static_cast<int>(stalled_ms);
  link.waiting_seat = -1;
  if (stalled_ms > 0)
  {
    if (match.joining)
      link.waiting_seat = match.host_seat;
    else if (!match.seated.empty())
      link.waiting_seat = *match.seated.begin();
  }
  Orca::Events::PublishLinkStats(link);
}
}  // namespace

namespace
{
// A joiner's room events: the host's port names, and the host leaving (HostGone handles it).
void JoinerEvents(Match& match)
{
  for (const Orca::Net::PeerEvent& event : Orca::Online::TakePeerEvents())
  {
    if (event.kind == Orca::Net::PeerEvent::Kind::Names)
    {
      // A name for frames already run (it arrived after its plug frame): re-run them so the frame
      // hook writes it, as the host did. Ignore broadcasts older than what this player holds.
      if (event.keyframe.names_version < match.names_version)
        continue;
      const std::vector<Orca::Net::KeyframeInfo::Name> before = AllNames(match);
      int earliest = INT32_MAX;
      for (const auto& name : event.keyframe.names)
      {
        if (std::find(before.begin(), before.end(), name) != before.end())
          continue;
        const Orca::Net::SeatPlan& plan = match.session->Plan()[name.seat];
        const int from = std::max(name.from, plan.plug_from);
        if (plan.plug_from == Orca::Net::NEVER || from > match.running || from >= plan.unplug_from)
          continue;
        earliest = std::min(earliest, from);
      }
      SetNames(match, event.keyframe);
      if (earliest <= match.running)
      {
        if (match.session->RerunFrom(earliest))
        {
          NOTICE_LOG_FMT(ROLLBACK, "Drop-in: a name for frame {} arrived at frame {}: re-running",
                         earliest, match.running + 1);
        }
        else
        {
          WARN_LOG_FMT(ROLLBACK,
                       "Drop-in: a name for frame {} arrived at frame {}, too late to re-run",
                       earliest, match.running + 1);
        }
      }
    }
    else if (event.kind == Orca::Net::PeerEvent::Kind::Left && event.host)
      match.host_gone = event.reason;
  }
}

// A joiner's leave is complete: play on solo; the app hears "left" (its own Leave, so no error).
void FinishLeave(Match& match)
{
  LogStats("at leave");
  // A ranked set's leave has its own message on screen (UX/SetEnd.h).
  if (Orca::Online::RoomQueue() != "ranked")
    Core::DisplayMessage(fmt::format("You left {}'s game", match.host_name), 4000);
  BecomeSolo(match, "left", nullptr);
  Orca::Status::Event("left");
}

// A joiner that asked to leave and ran past its unplug frame; it only awaits the host's final ack.
bool LeavingPastUnplug(const Match& match)
{
  if (!match.joining || !match.leave_requested || !match.session)
    return false;
  const int unplug = match.session->Plan()[match.local_seat].unplug_from;
  return unplug != Orca::Net::NEVER && match.running >= unplug;
}

// A joiner whose host left, at a boundary that isn't a re-run: play on solo (with "caps join") or
// stop. Either way the frame gets its pads.
void HostGone(Core::System& system, Match& match, const std::function<Pad(int)>& local_pad)
{
  Orca::Net::PeerEvent event;
  event.reason = *std::exchange(match.host_gone, std::nullopt);
  // This player had asked to leave, so the host leaving completes it, whether or not the host's
  // last acknowledgement arrived first.
  if (Soft() && match.leave_requested && event.reason != "desync")
  {
    NOTICE_LOG_FMT(ROLLBACK, "Drop-in: the host left the room as this player left, frame {}",
                   match.running + 1);
    FinishLeave(match);
    RunSolo(match, local_pad);
    return;
  }
  LogStats("host left");
  if (event.reason == "desync")
    ++s_desyncs;
  if (Soft())
  {
    // The host's game is gone and this one goes on: every other controller unplugs.
    if (event.reason == "desync")
    {
      Orca::Online::ReportDesync();
      Orca::Status::Report("desync", "Your games went out of sync, so the match ended");
    }
    // A queue room's screen already says so (UX/SetEnd.h).
    if (Orca::Online::RoomQueue() == "private")
      Core::DisplayMessage(fmt::format("{} left: you play on", match.host_name), 6000);
    // Ranked set undecided: wait for the verdict (only awarded while someone is in the room).
    const bool linger = Orca::Online::RoomQueue() == "ranked" && Orca::Online::MatchLive() &&
                        !Orca::Online::RoomEnded();
    // Set over: stay on the results screen until this player leaves too; not a forfeit.
    const bool stay = Orca::Online::RoomQueue() == "ranked" && Orca::Online::SetOver();
    if (stay)
    {
      NOTICE_LOG_FMT(ROLLBACK, "Matchmaking: the host left the set's room (set over): this game "
                               "stays until its player leaves");
    }
    BecomeSolo(match, "host left", "host-left", false, linger || stay);
    if (linger)
    {
      NOTICE_LOG_FMT(ROLLBACK, "Matchmaking: the host went mid-set: waiting for the verdict");
      match.linger_until = Clock::now() + VERDICT_WAIT;
    }
    RunSolo(match, local_pad);
    return;
  }
  if (event.reason == "desync")
    Orca::Status::Error("desync", "Your games went out of sync, so the match ended");
  else
    Orca::Status::Error("peer_left", "Your friend's game ended");
  End("host left");
  StopEmulation(system, event.reason == "desync" ?
                            "Your games went out of sync, so the match ended" :
                            "Your friend's game ended");
}

// Host of a queue room: seats the room says left that the session still has in play. Friends rooms
// rely on the silence limit, since friends may come back.
std::vector<int> LeftSeatsInPlay(const Match& match)
{
  std::vector<int> seats;
  if (!match.session || Orca::Online::RoomQueue() == "private")
    return seats;
  for (const int seat : Orca::Online::LeftSeatsPending())
  {
    if (seat >= 0 && seat < Orca::Net::MAX_SEATS && seat != match.local_seat &&
        match.session->Plan()[seat].live_from != Orca::Net::NEVER)
    {
      seats.push_back(seat);
    }
  }
  return seats;
}

std::optional<int> Boundary(Core::System& system,
                            const std::function<Orca::Net::Pad(int local_seat)>& local_pad,
                            const FrameHook& on_frame)
{
  Match& match = TheMatch();
  if (match.stale)
  {
    // A new boot in the same process (the room was left when the last one stopped).
    if (match.store)
      match.store->Cancel();
    match.session.reset();
    match.port.reset();
    match.job.reset();
    match.download.reset();
    match = {};
  }
  if (match.finished)
    return std::nullopt;

  std::optional<int> rewound_to;
  if (!match.started)
  {
    match.started = true;
    // Once emulation has stopped (the CPU thread is gone), leave the room.
    if (!match.on_state_changed)
    {
      match.on_state_changed = Core::AddOnStateChangedCallback([](Core::State state) {
        if (state != Core::State::Uninitialized)
          return;
        End("emulation stopped");
        TheMatch().stale = true;
      });
    }
    match.store = MakeStore();
    if (const std::string leave = Orca::GetEnv("ORCA_TEST_LEAVE_AT"); !leave.empty())
      match.leave_at = std::atoi(leave.c_str());
    Orca::Online::Start();
    // The role (host plays, joiner waits) is known once the room has its ticket.
    std::optional<bool> joining;
    while (!(joining = Orca::Online::Joining()) && !Orca::Online::RoomEnded() && !Stopping(system))
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    match.joining = joining.value_or(false);
    if (match.joining)
    {
      while (Orca::Online::Seat() < 0 && !Orca::Online::RoomEnded() && !Stopping(system))
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      match.local_seat = Orca::Online::Seat();
      if (!match.store)
      {
        StopEmulation(system, "Couldn't join: this Orca has nowhere to get your friend's game from");
        End("no keyframe store");
        return std::nullopt;
      }
      // Until the keyframe is in, run this boot unthrottled and unseen up to boot_nand_frame, so
      // its NAND files needn't travel.
      match.boot_time = Clock::now();
      Orca::Status::State("joining 0");
      match.port = std::make_unique<RingPort>(system, MAX_ROLLBACK);
      match.port->SetCatchingUp(true);
      if (const Orca::Profile* profile = Orca::ActiveProfile(); profile && profile->boot_nand_frame)
        match.boot_ahead_to = static_cast<int>(*profile->boot_nand_frame);
    }
    else
    {
      match.port = std::make_unique<RingPort>(system, MAX_ROLLBACK);
      Orca::Status::State("playing");
      NOTICE_LOG_FMT(ROLLBACK, "Drop-in: playing solo on port 1; room {}", Orca::Online::StatusLine());
    }
  }

  // Once a queue room is over, restore this player's own game first, so this boundary's hook
  // already sees its own character select.
  MaybeRestoreQueueImage(system, match);

  // The header this game's room calls for, written only where HeaderFreeAt allows.
  UpdateWanted(match);
  DropStaleKeyframe(match);
  Orca::UX::Rules::SetHeaderFree(
      HeaderFreeAt(match.running + 1, SoloQuiet(match),
                   match.keyframe ? std::optional(match.keyframe->frame) : std::nullopt));

  // The in-game UI's frame hook, before the snapshot or keyframe so they capture its writes.
  {
    const int frame = match.running + 1;
    const bool resimulating = match.session && match.session->Resimulating();
    const std::vector<Orca::Events::PortInfo> ports = PortsAt(match, frame);
    on_frame(frame, resimulating, ports,
             !resimulating &&
                 AloneAt(frame, SoloIdle(match), Orca::Online::DropInPending(),
                         match.keyframe ? std::optional(match.keyframe->frame) : std::nullopt));
    if (!resimulating && ports != match.told_ports)
    {
      match.told_ports = ports;
      Orca::Events::NotifyPlugIn(frame, ports);
    }
  }

  TakeCommands(system, match);
  LeaveOnMismatch(match);
  if (match.want_queue_image)
    CaptureQueueImage(system, match);
  else
    MaybeRestoreQueueImage(system, match);
  // After the app's commands and the frame hook: a Casual or Ranked pick made with friends, a kept
  // pick to arm, a former joiner's way home, then "no-room" for one that didn't come home.
  TakeLobbyPickNow(match);
  ArmKeptPick(match);
  RearmFriendsPick(match);
  ComeHome(match);
  TellNoRoom(match);

  if (match.joining && !match.session && match.join_in_play)
  {
    // A join during play: this game plays on solo until the friend's keyframe is in.
    int frame = -1;
    switch (PumpJoin(match, &match.join_status))
    {
    case JoinPump::Waiting:
      RunSolo(match, local_pad);
      PublishStats(match);
      return std::nullopt;
    case JoinPump::Failed:
      FailJoin(system, match);
      break;
    case JoinPump::Ready:
      switch (LoadKeyframe(system, match, &frame))
      {
      case Load::Done:
        rewound_to = frame;
        break;
      case Load::Failed:
        FailJoin(system, match);
        break;
      case Load::Broken:
        return std::nullopt;
      }
      break;
    }
    if (match.finished)
      return std::nullopt;
  }
  else if (match.joining && !match.session)
  {
    if (match.running + 1 < match.boot_ahead_to)
    {
      if (PumpJoin(match, &match.join_status) == JoinPump::Failed)
      {
        FailJoin(system, match);
        if (match.finished)
          return std::nullopt;
      }
      else
      {
        Pads pads;
        pads.fill(Orca::Net::UNPLUGGED_PAD);
        pads[0] = Pad{};
        match.port->SetPads(match.running + 1, pads);
        ++match.running;
        return std::nullopt;
      }
    }
    else
    {
      rewound_to = JoinHost(system, match);
      // Failed (with "caps join" this player now plays solo), or stopping.
      if (!rewound_to && (match.joining || match.finished || Stopping(system)))
        return std::nullopt;
    }
  }

  if (!match.joining)
  {
    MaybeHashBootNand(system, match);
    Linger(match);
    if (!Stopping(system))
      KeepRoomOpen(match);
    // While waiting for a ranked room's verdict, nobody new is taken in.
    if (!Orca::Online::RoomEnded() && !match.linger_until)
      HostEvents(system, match);
    if (!match.session)
    {
      RunSolo(match, local_pad);
      PublishStats(match);
      return rewound_to;
    }
  }
  if (!match.session)
    return std::nullopt;

  // Round trips feed the adaptive input delay unless one was chosen; each player's is its own.
  if (const auto round_trip_ms = NewRoundTrip(&match.round_trip_sequence))
    match.session->OnRoundTrip(*round_trip_ms);
  match.session->SetFixedDelay(ChosenDelay());
  // A frame over two frame periods was a local hitch (disc read, shader compile).
  const auto now = Clock::now();
  if (match.last_return && now - *match.last_return > HITCH_TIME && !match.port->CatchingUp())
    match.session->OnLocalHitch();

  // Handle a departed host only outside a re-run, or half-replayed frames would remain.
  if (match.joining)
  {
    JoinerEvents(match);
    if (match.host_gone && !match.session->Resimulating())
    {
      HostGone(system, match, local_pad);
      return rewound_to;
    }
  }

  if (match.joining && match.leave_at >= 0 && match.running >= match.leave_at &&
      !match.leave_requested)
  {
    match.leave_requested = true;
    NOTICE_LOG_FMT(ROLLBACK, "Drop-in: leaving at frame {}", match.running);
    match.session->RequestLeave();
  }

  for (int attempt = 0; attempt < 2; ++attempt)
  {
    bool peer_silent = false;
    // A joiner stalled on its host stops waiting once the host has left the room.
    bool host_left = false;
    // Likewise a queue room's host: opponents that left but are still in play.
    std::vector<int> left_seats;
    // Show a stall while it lasts; StepSession polls the callback during the wait.
    const auto step_start = Clock::now();
    auto stall_published = step_start;
    bool stall_shown = false;
    bool stall_claimed = false;
    const int stalls_before = match.session->GetStats().stalls;
    const int running_before = match.running;
    const Orca::Net::Step step = StepSession(
        *match.session, *match.port, match.running, [&] { return local_pad(match.local_seat); },
        [&] {
          const auto now = Clock::now();
          if (now - stall_published > std::chrono::milliseconds(10))
          {
            stall_published = now;
            stall_shown = true;
            PublishStall(match, MsSince(step_start));
            Orca::Events::NotifyStall(static_cast<int>(MsSince(step_start)));
            // Mid ranked set, a long stall shows as a disconnect with a forfeit countdown.
            Orca::UX::SetEnd::Current().Stall(static_cast<int>(MsSince(step_start)),
                                              Orca::UX::SetEnd::NowMs());
            if (!match.joining)
              left_seats = LeftSeatsInPlay(match);
            if (!stall_claimed && now - step_start >= RANKED_STALL_LIMIT &&
                Orca::Online::RoomQueue() == "ranked" && Orca::Online::MatchLive())
            {
              stall_claimed = true;
              WARN_LOG_FMT(ROLLBACK, "Matchmaking: the opponent's inputs stopped for {} s: "
                                     "claiming the set",
                           RANKED_STALL_LIMIT.count());
              Orca::Online::ReportStall();
              Core::DisplayMessage("Your opponent stopped playing: the set is yours", 6000);
            }
          }
          if (match.joining && Orca::Online::HostLeftPending())
            host_left = true;
          return host_left || !left_seats.empty() || Stopping(system);
        },
        std::chrono::milliseconds{1},
        LeavingPastUnplug(match) ? LEAVE_SILENCE_LIMIT : PEER_SILENCE_LIMIT, &peer_silent);
    match.last_return = Clock::now();
    if (stall_shown)
    {
      PublishStall(match, 0);
      Orca::UX::SetEnd::Current().StallOver(step.kind == Orca::Net::StepKind::Run ||
                                                step.kind == Orca::Net::StepKind::Rollback,
                                            Orca::UX::SetEnd::NowMs());
    }
    if (match.session)
    {
      for (const std::string& note : match.session->TakeDelayNotes())
        NOTICE_LOG_FMT(ROLLBACK, "Online match: {}", note);
    }
    if (match.session && match.session->GetStats().stalls > stalls_before)
      match.stats_stall_ms += MsSince(step_start);
    if (step.kind == Orca::Net::StepKind::Rollback)
    {
      match.stats_rbmax = std::max(match.stats_rbmax, running_before - step.frame + 1);
      // The loaded snapshot holds the hook's writes made with the ports known then, and the
      // rollback may be due to those ports changing. Rerun the hook for the frame landed on.
      on_frame(step.frame, true, PortsAt(match, step.frame), false);
    }
    if (step.kind == Orca::Net::StepKind::Run || step.kind == Orca::Net::StepKind::Rollback)
    {
      match.running = step.frame;
      PublishStats(match);
      // Every 30 s of play with friends, log the session's numbers.
      if (step.kind == Orca::Net::StepKind::Run && !match.session->Resimulating() &&
          step.frame % 1800 == 0 && !match.port->CatchingUp())
      {
        LogStats(fmt::format("at frame {}", step.frame).c_str());
        NOTICE_LOG_FMT(ROLLBACK, "Drop-in: {}", match.session->Describe());
      }
      if (match.joining)
      {
        // Unthrottled and unrendered while far behind the host.
        const int behind = match.session->AuthorityFrame() - step.frame;
        match.port->SetCatchingUp(match.session->CatchingUp() || behind > CATCH_UP_BEHIND);
        if (match.session->CatchingUp())
        {
          const int percent = 50 + std::clamp(50 - behind / 4, 0, 49);
          // The host keeps running, so `behind` can briefly grow; never go backwards.
          if (percent > match.last_percent)
          {
            match.last_percent = percent;
            Orca::Status::State(fmt::format("joining {}", percent));
          }
        }
        else if (!match.joined && match.session->Plugged(match.local_seat, step.frame))
        {
          match.joined = true;
          Orca::Status::State("playing");
          const double catch_up = MsSince(match.loaded_time);
          NOTICE_LOG_FMT(ROLLBACK,
                         "Drop-in: plugged into port {} at frame {}: catch-up {:.0f} ms, {:.0f} ms "
                         "since the keyframe was offered, {:.0f} ms since this Orca's first frame",
                         match.local_seat + 1, step.frame, catch_up, MsSince(match.offered_time),
                         MsSince(match.boot_time));
          ShowJoinMessage(
              fmt::format("Joined {}'s game on port {}", match.host_name, match.local_seat + 1),
              4000);
        }
        if (match.session->LeftDone() && step.kind == Orca::Net::StepKind::Run &&
            !match.session->Resimulating())
        {
          NOTICE_LOG_FMT(ROLLBACK, "Drop-in: unplugged at frame {}",
                         match.session->Plan()[match.local_seat].unplug_from);
          if (Soft())
          {
            FinishLeave(match);
            return rewound_to;
          }
          LogStats("at leave");
          End("left");
          StopEmulation(system, "You left your friend's game");
          return rewound_to;
        }
      }
      else
      {
        // Host: once every friend taken in has plugged in, tell the app they joined.
        if (step.kind == Orca::Net::StepKind::Run && !match.session->Resimulating() &&
            std::erase_if(match.plugging,
                          [&](int seat) { return match.session->Plugged(seat, step.frame); }) > 0 &&
            match.plugging.empty() && match.waiting.empty())
        {
          Orca::Status::State("friend-joined");
          match.friend_left_told = false;
          // A matchmade room's match may begin (YouGameRoom), and the search is over.
          Orca::Online::SetOpponentPlugged(true);
          Orca::UX::Search::End();
        }
        // Nobody else in the session: go solo after IDLE_GRACE_FRAMES. Skip "playing" if the app
        // already heard the friend leave, or it would wipe that line.
        if (match.session->Idle() && match.waiting.empty())
        {
          if (++match.idle_frames >= IDLE_GRACE_FRAMES)
          {
            GoSolo(match, "no friends left", match.friend_left_told ? nullptr : "playing");
            AfterFriendsGone(match);
          }
        }
        else
        {
          match.idle_frames = 0;
        }
      }
      if (step.kind == Orca::Net::StepKind::Rollback)
        return step.frame;
      return rewound_to;
    }
    if (step.kind != Orca::Net::StepKind::Ended)
    {
      // StepSession handles every other step itself; reaching this is a bug.
      ERROR_LOG_FMT(ROLLBACK, "Online: unexpected session step {} at frame {}",
                    static_cast<int>(step.kind), step.frame);
      return rewound_to;
    }
    if (Stopping(system))
      return rewound_to;
    const std::string error = match.session->Error();
    if (host_left && error.empty() && !match.session->Resimulating())
    {
      // The host left: the joined game is gone (or this player's own leave is complete).
      JoinerEvents(match);
      if (!match.host_gone)
        match.host_gone = "";
      HostGone(system, match, local_pad);
      return rewound_to;
    }
    if (!left_seats.empty() && error.empty() && !match.session->Resimulating())
    {
      // The room says they left: unplug them now instead of after the silence limit, and play on
      // while the room's forfeit runs.
      for (const int seat : left_seats)
      {
        NOTICE_LOG_FMT(ROLLBACK, "Drop-in: port {} left the room while this game waited on it: "
                                 "unplugged at once, frame {}",
                       seat + 1, match.running + 1);
        match.session->DropSeat(seat);
      }
      continue;
    }
    if (!match.joining && match.session->CurrentFrame() == match.running + 1 &&
        (attempt == 0 || !error.empty()))
    {
      // The host plays on and unplugs whoever stopped answering or broke the session. If the host's
      // own room went away, KeepRoomOpen reports that instead of "your friend left".
      const bool room_gone = Orca::Online::RoomEnded();
      if (room_gone && Orca::Online::RoomErrorCode() == "match-over" &&
          !Orca::Online::IsDesync(error))
      {
        // Matchmade room closed after its result: KeepRoomOpen tells the app, opens a new room.
        DropFriends(match, "network", false);
        GoSolo(match, "match over", nullptr);
        RunSolo(match, local_pad);
        return rewound_to;
      }
      WARN_LOG_FMT(ROLLBACK, "Drop-in: unplugging every friend at frame {}: {}", match.running + 1,
                   peer_silent ? "no word from them" : error);
      // Mid ranked set the screen already counts their forfeit down; after it they're just gone.
      const bool ranked_stall = error.empty() && Orca::Online::RoomQueue() == "ranked" &&
                                Orca::Online::MatchLive();
      const bool set_over = error.empty() && Orca::Online::SetOver();
      if (set_over)
        Orca::UX::SetEnd::Current().OpponentLeftAfterSet(Orca::UX::SetEnd::NowMs());
      if (!room_gone && !ranked_stall && !set_over)
      {
        Core::DisplayMessage(peer_silent ? std::string(PEER_SILENT_SENTENCE) :
                                           fmt::format("Your friend was disconnected: {}", error),
                             6000);
      }
      if (error.empty())
      {
        // Silent friends: unplug them and step again; a silent ranked opponent loses the set.
        if (ranked_stall)
          Orca::UX::SetEnd::Current().OpponentDropped(Orca::UX::SetEnd::NowMs());
        Orca::Online::ReportStall();
        DropFriends(match, "stalled");
        continue;
      }
      if (Orca::Online::IsDesync(error))
        ++s_desyncs;
      DropFriends(match, Orca::Online::IsDesync(error) ? "desync" : "network",
                  !room_gone && attempt == 0);
      GoSolo(match, "the session failed");
      if (!room_gone)
        AfterFriendsGone(match);
      RunSolo(match, local_pad);
      return rewound_to;
    }
    if (Soft() && match.joining && peer_silent && error.empty() && LeavingPastUnplug(match))
    {
      // Only the host's final acknowledgement was missing: the leave is complete anyway.
      NOTICE_LOG_FMT(ROLLBACK, "Drop-in: no word from the host for {} s after leaving, frame {}",
                     LEAVE_SILENCE_LIMIT.count(), match.running + 1);
      FinishLeave(match);
      RunSolo(match, local_pad);
      return rewound_to;
    }
    WARN_LOG_FMT(ROLLBACK, "Online: session ended at frame {}: {}", match.running + 1,
                 error.empty() ? (peer_silent ? "no word from the host" : "no error") : error);
    NOTICE_LOG_FMT(ROLLBACK, "Drop-in: session state at the end: {}", match.session->Describe());
    if (peer_silent)
    {
      WARN_LOG_FMT(ROLLBACK, "Online: no word from the host for {} s at frame {}",
                   PEER_SILENCE_LIMIT.count(), match.running);
      Orca::Online::ReportPeerStalled(PEER_SILENT_SENTENCE);
    }
    const bool survivable = Orca::Online::ReportSessionEnd(error);
    if (Orca::Online::IsDesync(error))
      ++s_desyncs;
    if (Soft() && survivable && match.joining && Orca::Online::RoomErrorCode() == "match-over")
    {
      // Matchmade room closed after its result: play on as its own game, on port 1, own room.
      NOTICE_LOG_FMT(ROLLBACK, "Matchmaking: the room closed after its result");
      BecomeSolo(match, "match over", "host-left match-over", true);
      RunSolo(match, local_pad);
      return rewound_to;
    }
    if (Soft() && survivable && match.joining && Orca::Online::RoomQueue() == "ranked" &&
        Orca::Online::MatchLive() && !Orca::Online::RoomEnded())
    {
      // Ranked host went quiet: stay (leaving would forfeit) and wait for the verdict.
      NOTICE_LOG_FMT(ROLLBACK, "Matchmaking: the host went quiet mid-set: waiting for the verdict");
      if (peer_silent)
        Orca::Online::ReportStall();
      // The screen counts their forfeit down (UX/SetEnd.h).
      Orca::UX::SetEnd::Current().OpponentDropped(Orca::UX::SetEnd::NowMs());
      BecomeSolo(match, "the session ended", "host-left", false, true);
      match.linger_until = Clock::now() + VERDICT_WAIT;
      RunSolo(match, local_pad);
      return rewound_to;
    }
    if (Soft() && survivable && match.joining && Orca::Online::RoomQueue() == "ranked" &&
        Orca::Online::SetOver())
    {
      // Set over: the host is just gone; stay on the results screen until this player leaves too.
      NOTICE_LOG_FMT(ROLLBACK, "Matchmaking: the host's game ended after the set (set over): "
                               "this game stays until its player leaves");
      Orca::UX::SetEnd::Current().OpponentLeftAfterSet(Orca::UX::SetEnd::NowMs());
      BecomeSolo(match, "the session ended", "host-left", false, true);
      RunSolo(match, local_pad);
      return rewound_to;
    }
    if (Soft() && survivable && match.joining)
    {
      // Unplugged from the host's game; this one goes on.
      Core::DisplayMessage(peer_silent ? std::string(PEER_SILENT_SENTENCE) :
                                         "The connection to your friend's game ended: you play on",
                           6000);
      BecomeSolo(match, "the session ended", "host-left");
      RunSolo(match, local_pad);
      return rewound_to;
    }
    End("session ended");
    StopEmulation(system, peer_silent ? std::string(PEER_SILENT_SENTENCE) :
                                        fmt::format("Online match ended: {}",
                                                    error.empty() ? "stopped" : error));
    return rewound_to;
  }
  return rewound_to;
}

// Notes a fight in the ranked room this queue image is for (MaybeRestoreQueueImage).
void NoteRankedFight(Match& match)
{
  if (match.ranked_fought || !match.queue_image || Orca::Online::RoomEnded() ||
      Orca::Online::RoomQueue() != "ranked")
  {
    return;
  }
  if (Orca::UX::Tracker().LatestScene() == Orca::UX::Reading::Scene::Fight)
  {
    match.ranked_fought = true;
    NOTICE_LOG_FMT(ROLLBACK, "Queue: a ranked fight in room {} (frame {}): back not ready after it",
                   Orca::Online::Code(), match.running + 1);
  }
}

// Set over: after a ranked set's verdict, both players stay on the results screen and can chat on
// the page. This game leaves the room when its results screen is over (read at a final frame), when
// the player holds Z for SET_OVER_Z_HOLD (Z is masked from the game there), or, once the opponent
// is gone, on Start or A. Reads only the local pad and never changes the game.
void SetOverLeave(Match& match)
{
  Orca::UX::SetEnd::Model& set_end = Orca::UX::SetEnd::Current();
  if (!Orca::Online::SetOver() || match.finished)
  {
    if (std::exchange(match.set_over_hint, false))
      set_end.LeaveHint(false, false, 0);
    match.set_over_left = false;
    match.set_over_z_since.reset();
    match.set_over_buttons = 0xFFFF;
    return;
  }
  if (match.set_over_left)
    return;
  using Scene = Orca::UX::Reading::Scene;
  const std::optional<Scene> final_scene = Orca::UX::Tracker().FinalScene();
  const bool on_results = Orca::UX::Tracker().LatestScene() == Scene::Results;
  // The opponent's game is gone: the host's session ended, or the joiner plays solo.
  const bool alone = !match.session;
  const u16 buttons = s_local_pad ? Orca::Net::DecodePad(*s_local_pad).button : u16{0};
  const u16 pressed = static_cast<u16>(buttons & ~match.set_over_buttons);
  match.set_over_buttons = buttons;
  const auto now = Clock::now();
  if (on_results && (buttons & PAD_TRIGGER_Z))
  {
    if (!match.set_over_z_since)
      match.set_over_z_since = now;
  }
  else
  {
    match.set_over_z_since.reset();
  }
  const float held =
      match.set_over_z_since ?
          std::min(1.0f, static_cast<float>(std::chrono::duration<double>(
                                                now - *match.set_over_z_since) /
                                            SET_OVER_Z_HOLD)) :
          0.0f;
  const char* why = nullptr;
  if (final_scene && *final_scene != Scene::Results)
    why = "the game is off the results screen";
  else if (match.set_over_z_since && now - *match.set_over_z_since >= SET_OVER_Z_HOLD)
    why = "Z held";
  else if (alone && on_results && (pressed & (PAD_BUTTON_START | PAD_BUTTON_A)))
    why = "the opponent is gone and the player pressed on";
  match.set_over_hint = on_results && !why;
  set_end.LeaveHint(match.set_over_hint, alone, held);
  if (!why)
    return;
  match.set_over_left = true;
  match.set_over_z_since.reset();
  set_end.LeavingSetRoom(Orca::UX::SetEnd::NowMs());
  NOTICE_LOG_FMT(ROLLBACK, "Matchmaking: set over: leaving room {} ({}), frame {}",
                 Orca::Online::Code(), why, match.running + 1);
  Orca::Online::RequestLeave();
}
}  // namespace

std::optional<int> OnBoundary(Core::System& system,
                              const std::function<Orca::Net::Pad(int local_seat)>& local_pad,
                              const FrameHook& on_frame)
{
  s_solo_idle = false;
  // The local pad as this boundary read it for the game (for SetOverLeave).
  const std::function<Orca::Net::Pad(int)> seen = [&local_pad](int local_seat) {
    const Orca::Net::Pad pad = local_pad(local_seat);
    s_local_pad = pad;
    return pad;
  };
  const std::optional<int> rewound_to = Boundary(system, seen, on_frame);
  ConfirmResults(TheMatch());
  NoteRankedFight(TheMatch());
  SetOverLeave(TheMatch());
  Orca::UX::RankedSet::SetLocalPort(TheMatch().local_seat);
  s_solo_idle = SoloIdle(TheMatch());
  return rewound_to;
}

void LogStats(const char* when)
{
  const Match& match = TheMatch();
  if (!match.session)
    return;
  const Orca::Net::Stats& stats = match.session->GetStats();
  NOTICE_LOG_FMT(
      ROLLBACK,
      "Online match {}: frame {}, {} rollbacks, {} re-run frames, deepest {}, {} stalls ({} "
      "counted), {} waits, {} hitches here, {} there, frame advantage {:.2f}, input delay {}{} "
      "(+{} -{} reverted {}), {} counted stalls a frame more would have spared, {} checksums "
      "matched, confirmed frame {}, round trip {} ms (median {} ms here, {} ms there), link {}, "
      "{} snapshots (every {}, {:.2f} ms each){}{}",
      when, match.session->CurrentFrame(), stats.rollbacks, stats.resimulated_frames,
      stats.deepest_rollback, stats.stalls, stats.counted_stalls, stats.waits, stats.hitches,
      stats.peer_hitches, stats.frame_advantage, stats.delay,
      stats.delay_fixed ? " (chosen)" : "", stats.delay_raises, stats.delay_lowers,
      stats.delay_reverts, stats.spared_stalls, stats.checksums_matched, stats.confirmed_frame,
      Orca::Online::RoundTripMs(), stats.rtt_median, stats.peer_rtt_median,
      Orca::Online::Direct().line, stats.saves,
      stats.snapshot_every, match.port ? match.port->SaveMs() : 0.0,
      match.session->Error().empty() ? "" : "; ", match.session->Error());
}

bool Active()
{
  const Match& match = TheMatch();
  return match.started && !match.finished;
}

void End(const char* reason)
{
  Match& match = TheMatch();
  if (!match.started || match.finished)
    return;
  match.finished = true;
  s_solo_idle = false;
  LogStats(fmt::format("ended ({})", reason).c_str());
  // Transfers in flight give up now; their threads are joined below.
  if (match.store)
    match.store->Cancel();
  match.session.reset();
  match.port.reset();
  match.download.reset();
  match.job.reset();
  if (match.keyframe && match.store)
    match.store->Delete(match.keyframe->id);
  Orca::Online::Shutdown();
}

bool AloneAt(int frame, bool solo_idle, bool drop_in_pending, std::optional<int> stored_keyframe)
{
  return solo_idle && !drop_in_pending &&
         !(stored_keyframe && frame - *stored_keyframe <= KEYFRAME_FRESH_FRAMES);
}

bool HeaderFreeAt(int frame, bool solo_quiet, std::optional<int> stored_keyframe)
{
  return solo_quiet && !(stored_keyframe && frame - *stored_keyframe <= KEYFRAME_FRESH_FRAMES);
}

bool MayComeHome(const HomeInputs& in)
{
  return in.local_seat != 0 && !in.joining && !in.session && !in.room_up && in.solo_idle &&
         !in.drop_in_pending && in.on_menus && !in.queue_image && !in.lingering;
}

LobbyPickPlan DecideLobbyPick(const LobbyPickInputs& in)
{
  LobbyPickPlan plan;
  if (in.joining || in.queue_room)
    return plan;
  const bool friends =
      (in.session && !in.session_idle) || in.drop_in_friends || in.arrival_pending;
  if (!friends)
  {
    plan.step = LobbyPickStep::Announce;
    plan.drop_keyframe = in.keyframe_kept && !in.session;
    return plan;
  }
  if (!in.host_cap || in.local_seat != 0)
    return plan;
  if (in.pick_cap)
  {
    plan.step = LobbyPickStep::Leave;
    return plan;
  }
  plan.step = LobbyPickStep::Announce;
  plan.arm = false;
  return plan;
}

bool AnnounceHomePick(bool css_pick, bool host_cap, bool queue_or_search)
{
  return css_pick && host_cap && !queue_or_search;
}

bool FriendEndsQueue(const FriendQueueInputs& in)
{
  return in.friend_coming && in.queue_or_search && in.local_seat == 0 && !in.joining &&
         in.friends_room;
}

bool JoinEndsQueue(bool queue_room, bool queue_or_search, bool queue_image)
{
  return !queue_room && (queue_or_search || queue_image);
}

FriendsPickStep DecideFriendsPick(const FriendsPickInputs& in)
{
  if (in.queue_or_search || in.elsewhere || !in.host_cap)
    return FriendsPickStep::Drop;
  return in.played && in.alone && in.on_pick_select ? FriendsPickStep::Arm : FriendsPickStep::Wait;
}

KeptPickStep DecideKeptPick(const KeptPickInputs& in)
{
  if (!in.stands || in.queue_or_search)
    return KeptPickStep::Drop;
  return in.alone ? KeptPickStep::Arm : KeptPickStep::Wait;
}

HeaderWait StepHeaderWait(int* waited, bool in_place, bool someone_waiting, bool queue_room)
{
  if (in_place || !someone_waiting)
  {
    *waited = 0;
    return in_place ? HeaderWait::Ready : HeaderWait::Wait;
  }
  ++*waited;
  if (queue_room)
  {
    if (*waited <= HEADER_FAIL_BOUNDARIES)
      return HeaderWait::Wait;
    *waited = 0;
    return HeaderWait::Fail;
  }
  return *waited > HEADER_WAIT_BOUNDARIES ? HeaderWait::Ready : HeaderWait::Wait;
}

bool SoloPauseAllowed()
{
  // A session with no room (tests): nobody else's game is affected.
  if (!Orca::Online::Enabled())
    return true;
  return s_solo_idle && !Orca::Online::DropInPending();
}

bool BeginPause()
{
  const bool was_pausing = s_pausing.exchange(true);
  if (!SoloPauseAllowed())
  {
    if (!was_pausing)
      s_pausing = false;
    return false;
  }
  s64 none = 0;
  s_paused_since_ns.compare_exchange_strong(none, NowNs());
  return true;
}

void EndPause()
{
  s_pausing = false;
  if (const s64 since = s_paused_since_ns.exchange(0); since != 0)
    s_paused_ns += NowNs() - since;
}

bool Pausing()
{
  return s_pausing;
}

std::chrono::nanoseconds TakePausedTime()
{
  return std::chrono::nanoseconds(s_paused_ns.exchange(0));
}
}  // namespace Rollback::OnlineMatch
