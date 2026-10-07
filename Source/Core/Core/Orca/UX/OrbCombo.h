// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/CommonTypes.h"
#include "InputCommon/GCPadStatus.h"

// Controller shortcut for the YouGame overlay: hold D-pad Up and press Start. A GameCube pad has no
// Home button. Orca prints `orca orb` and the app opens the overlay.
//
// From the Start press until both are released, the game sees neither Start nor the D-pad. The
// session records and sends this masked pad, so every machine sees the same input and it cannot
// desync.
namespace Orca::UX
{
class OrbCombo
{
public:
  static constexpr u16 START = PAD_BUTTON_START;
  static constexpr u16 UP = PAD_BUTTON_UP;
  static constexpr u16 DPAD = PAD_BUTTON_UP | PAD_BUTTON_DOWN | PAD_BUTTON_LEFT | PAD_BUTTON_RIGHT;

  struct Step
  {
    u16 buttons = 0;  // what the game gets
    bool fired = false;
  };
  // Feeds one frame's sample of the local buttons. Pure, no I/O.
  Step Next(u16 buttons);
  bool Held() const { return m_held; }

private:
  bool m_held = false;   // fired; Start and the D-pad stay masked until both are released
  bool m_start = false;  // Start was down in the last sample
};

// Applies the shortcut to the local pad before the session sends it. The combo only advances on a
// first run (rollback re-runs discard their reads) while `live`, because an unfocused pad reads as
// neutral and must not look like a release. CPU thread only.
GCPadStatus FilterLocalPad(GCPadStatus pad, bool first_run, bool live);
}  // namespace Orca::UX
