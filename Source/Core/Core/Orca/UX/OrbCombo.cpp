// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/UX/OrbCombo.h"

#include "Common/Logging/Log.h"
#include "Core/Orca/Status.h"

namespace Orca::UX
{
OrbCombo::Step OrbCombo::Next(u16 buttons)
{
  const bool start = (buttons & START) != 0;
  Step step;
  // Start pressed (not held from before) while Up is down.
  if (!m_held && start && !m_start && (buttons & UP) != 0)
  {
    m_held = true;
    step.fired = true;
  }
  m_start = start;
  if (m_held)
  {
    // Unmask once both Start and the D-pad are released.
    if ((buttons & (START | DPAD)) == 0)
      m_held = false;
    buttons &= static_cast<u16>(~(START | DPAD));
  }
  step.buttons = buttons;
  return step;
}

GCPadStatus FilterLocalPad(GCPadStatus pad, bool first_run, bool live)
{
  static OrbCombo s_combo;
  if (first_run && live)
  {
    const OrbCombo::Step step = s_combo.Next(pad.button);
    pad.button = step.buttons;
    if (step.fired)
    {
      INFO_LOG_FMT(CORE, "Orca: the YouGame shortcut (Up + Start)");
      Status::Line("orca orb");
    }
  }
  else if (s_combo.Held())
  {
    pad.button &= static_cast<u16>(~(OrbCombo::START | OrbCombo::DPAD));
  }
  return pad;
}
}  // namespace Orca::UX
