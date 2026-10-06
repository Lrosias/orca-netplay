// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <string_view>

// Player-visible names say Orca; upstream attribution and licence text are unchanged.
namespace Orca
{
// The window title in a session: "Orca — <game> — <room status>".
std::string WindowTitle();

// Replaces "Dolphin" with "Orca" in player-visible text during a session; otherwise unchanged.
std::string BrandText(std::string_view text);
}  // namespace Orca
