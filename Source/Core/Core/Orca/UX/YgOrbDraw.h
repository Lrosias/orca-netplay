// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <span>
#include <string_view>

#include <imgui.h>

#include "Core/Orca/UX/Kit.h"
#include "Core/Orca/UX/YgOrb.h"

// Draws the YouGame button and its lines (YgOrb.h) with ImGui. Only Overlay.cpp includes this,
// since the platform layer has no ImGui. Video thread only, inside the overlay's ImGui frame.
namespace Orca::UX::YgOrb
{
// The button's bounds including its dot and badge; empty (min == max) when off.
Kit::Box ButtonBox(const Model::View& view);
// Draws the button with its blink ring, held-seat dot and unread badge.
void DrawButton(ImDrawList* dl, const Model::View& view);
// One row of the pill beside the button: a chat line (name over up to two lines of text), an Orca
// toast (text beside the logo), or a notice ("<name> <text>" on one line, with an optional hint).
struct PillRow
{
  enum class Type
  {
    Chat,
    Toast,
    Notice,
  };
  Type type = Type::Chat;
  std::string_view name;
  std::string_view text;
  std::string_view key;
  std::string_view verb;
  Kind kind = Kind::Join;  // notices only
  float alpha = 1;
  double at = 0;  // arrival time on the overlay's clock; rows are sorted by it
};
inline constexpr std::size_t PILL_ROWS = 3;

// Draws the pill that extends right from under the button, showing the newest PILL_ROWS of `rows`
// and Current()'s notices, oldest first. Does not allocate per frame. The pill ends before
// `right_limit`. Returns its box, or an empty box when nothing is drawn.
Kit::Box DrawPill(ImDrawList* dl, const Model::View& view, std::span<const PillRow> rows,
                  double now_ms, float right_limit);

// Length of the longest prefix of `text` that fits in `width`, cut at a UTF-8 character boundary.
// Prefers to cut at a space in the second half, so words split only when one alone is too wide.
template <typename Measure>
std::size_t FitPrefix(std::string_view text, float width, Measure&& measure)
{
  if (measure(text) <= width)
    return text.size();
  const auto continuation = [&](std::size_t i) {
    return i < text.size() && (static_cast<unsigned char>(text[i]) & 0xC0) == 0x80;
  };
  // Invariant: a prefix of `lo` bytes fits, `hi` does not; both are character boundaries.
  std::size_t lo = 0, hi = text.size();
  while (true)
  {
    std::size_t mid = lo + (hi - lo) / 2;
    while (mid > lo && continuation(mid))
      --mid;
    if (mid == lo)
    {
      mid = lo + 1;
      while (mid < hi && continuation(mid))
        ++mid;
    }
    if (mid >= hi)
      break;
    if (measure(text.substr(0, mid)) <= width)
      lo = mid;
    else
      hi = mid;
  }
  // The word ends exactly at the limit, so keep it whole.
  if (lo < text.size() && text[lo] == ' ')
    return lo;
  const std::size_t space = text.substr(0, lo).rfind(' ');
  if (space != std::string_view::npos && space > lo / 2)
    return space;
  return lo;
}
}  // namespace Orca::UX::YgOrb
