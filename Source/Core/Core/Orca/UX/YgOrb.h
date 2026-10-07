// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The YouGame button and its notice lines, drawn by Orca over a full-screen game in place of the
// app's own button, which Orca's window covers. The app drives it over stdin:
//   orb <x> <y> <size> <lit> <away> <badge>
//       Show the button: top-left (x, y) and size in the view's device pixels (see ScaledPlace),
//       lit 0|1 (full or half strength), away 0|1 (amber held-seat dot), badge 0..99 unread.
//   orb blink    A white ring grows out of the button three times.
//   orb off      Hide the button and its lines.
//   notice <kind> <name> <text> [<key> <verb>]
//       A line beside the button. kind is join|leave|away|back|voice|call|dm|invite; fields are
//       URI-encoded as in `chat` (Chat.h); key and verb are an optional action hint.
// On macOS clicks pass through to the app's button. On Windows a press on the disc prints
// `orca orb` instead of `orca click`. Host-only: nothing here touches emulated state. This header
// has no ImGui so DolphinNoGUI can include it; YgOrbDraw.h draws it.
namespace Orca::UX::YgOrb
{
enum class Kind
{
  Join,
  Leave,
  Away,
  Back,
  Voice,
  Call,
  Dm,
  Invite,
};

struct Place
{
  float x = 0;
  float y = 0;
  float size = 0;
  bool lit = false;
  bool away = false;
  int badge = 0;
};

struct Command
{
  enum class Type
  {
    Off,
    Blink,
    Place,
  };
  Type type = Type::Off;
  Place place;
};
// Parses a whole "orb ..." stdin line; nullopt when malformed.
std::optional<Command> ParseOrb(std::string_view line);

struct Notice
{
  Kind kind = Kind::Join;
  std::string name;
  std::string text;
  std::string key;   // empty: no hint
  std::string verb;
};
// Parses a whole "notice ..." stdin line; nullopt when malformed. The fields are player text, so
// never print or log them.
std::optional<Notice> ParseNotice(std::string_view line);
// Maximum code points in each decoded hint field.
inline constexpr std::size_t HINT_POINTS = 24;

// Converts `place` from the view's device pixels to backbuffer pixels (these differ on Retina
// Macs). Unscaled until the app has sent a `rect`.
Place ScaledPlace(const Place& place, float view_width, float display_width);

// How long a notice stays, in ms, matching the app's own button.
double DwellMs(const Notice& notice);

// A line's opacity at `now_ms`: 1, fading out over its last FADE_MS, then 0.
float LineAlpha(const Notice& notice, double at_ms, double now_ms);

// The button's state, driven by an explicit clock so tests can control it. At() never allocates;
// Lines() copies the lines out only when they change. Thread-safe.
class Model
{
public:
  static constexpr double FADE_MS = 400;
  static constexpr std::size_t KEEP = 3;
  // A blink is RINGS rings of RING_MS each.
  static constexpr double RING_MS = 700;
  static constexpr int RINGS = 3;

  struct Entry
  {
    double at = 0;  // arrival time, ms
    Notice notice;
  };
  struct View
  {
    bool on = false;
    Place place;
    // Blink ring progress 0..1, or negative for none.
    float ring = -1;
    // Bumped whenever the lines change.
    std::uint64_t generation = 0;
    bool showing = false;
    // View width in device pixels from the last `rect`, or 0.
    float view_width = 0;
  };

  void Apply(const Command& command, double now_ms);
  void Add(Notice notice, double now_ms);
  // The app's `rect`: the view's size in device pixels.
  void SetView(int width, int height);
  View At(double now_ms) const;
  // Copies the newest KEEP lines, oldest first, into `out` if `generation` is stale. Returns the
  // current generation.
  std::uint64_t Lines(std::uint64_t generation, std::vector<Entry>* out) const;
  // Whether (x, y), in view pixels, is on the button's disc.
  bool Hit(float x, float y) const;

private:
  mutable std::mutex m_lock;
  bool m_on = false;
  Place m_place;
  double m_blink_at = -1;
  std::deque<Entry> m_lines;
  std::uint64_t m_generation = 0;
  int m_view_width = 0;
};

// The process-wide button. The stdin thread updates it and the video thread draws it. NowMs() is
// steady time in ms.
Model& Current();
double NowMs();
// Apply the app's `orb`, `notice` and `rect` lines. Any thread.
void Apply(const Command& command);
void Show(Notice notice);
void SetViewSize(int width, int height);
}  // namespace Orca::UX::YgOrb
