// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <string_view>

namespace Orca
{
// The one game Orca.app plays for now, and how the disc picker asks for it.
inline constexpr std::string_view APP_GAME_ID = "RSBE01";
inline constexpr std::string_view APP_DISC_PROMPT =
    "Choose your own Super Smash Bros. Brawl disc image (USA, Rev 2).";

struct DiscCheck
{
  bool ok = false;
  // For a refusal: the status code (Orca::Status) and a sentence for the player.
  std::string code;
  std::string sentence;
};

// Checks before boot that the disc is this game, at the revision its Orca profile expects.
DiscCheck CheckDisc(const std::string& path, std::string_view game_id);

// Checks the boot file (-e) before anything else: "disc_missing" if nothing is at the path,
// "disc_unreadable" if it won't open; the sentence is the path. Dolphin's own check raises a panic
// alert, which is invisible inside the app's window on Windows and would hang the run.
DiscCheck CheckBootFile(const std::string& path);
}  // namespace Orca
