// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/Orca/Session/Events.h"

namespace Core
{
class CPUThreadGuard;
}

// Puts each player's YouGame username into Brawl's own name tags, so the game shows it over the
// fighter and on the results. Each tag also carries that player's own controls, so they keep their
// layout in someone else's game.
//
// Memory layout (Brawl rev 2; Project+ runs the same executable):
//   - Save tags: g_GameGlobal (0x805A00E0) +0x28 -> records (0x90172D40), tag i at
//     +0xE0 + i * 0x124. Name first, up to 5 UTF-16BE characters and a 0. Empty name = unused.
//   - Tag controls: +0x0C rumble (1 on), +0x14 layout, 0x2D bytes, defaults at 0x80406938.
//     GameCube 12 bytes (L, R, Z, D-pad up, side, down, A, B, C-stick, Y, X, then tap jump in bit
//     0x80), Wii Remote 8, with Nunchuk 12 (last: tap jump 0x40, shake smash 0x80), Classic 13
//     (last: tap jump 0x80). Other bytes are actions (0 attack, 1 special, 2 jump, 3 shield,
//     4 grab, 5 smash, 9 taunt, 0xA-0xC taunts, 0xE none).
//   - On scMelee's first frame the game copies each port's tag layout into the per-port table
//     (0x805B7480, pointed to by 0x805A00C8), which the match reads. So whatever a port's tag holds
//     when the match starts is what that port plays with.
//   - Character select: scene manager (0x805A0060) +4 -> scene ("scSelctCharacter"), +0x400 ->
//     muSelCharTask, +0x44 + 4 * i -> player area i: +0x1B0 port i, +0x1B4 kind (1: human joined),
//     +0x1C8 tag (-1: none). Joining clears the tag to -1. Leaving the screen copies each tag into
//     gmPlayerInitData (+0x0C name, +0x18 tag index).
//
// On every character-select frame, a joined port with a named player and no tag gets one: the tag
// already holding that name, else the highest unused one, built as the game builds one. A duplicate
// name gets its port number as the last character (SAND2). The player's controls are written into
// it. A port with controls but no showable name gets "P<port>". A port with neither gets nothing.
//
// Determinism: writes depend only on emulated memory and the session's synced port list, and are
// idempotent (a re-run frame writes the same bytes; a port that has a tag is left alone). Names and
// controls come from the host (OnlineMatch.cpp, "Ports") and every machine has them before the
// frame they apply from.
//
// TODO: the character select's name plate keeps "PLAYER n" until the game redraws it (its setter,
// 0x8069B1CC, only runs when the player picks a tag). Needs a game-side code.
namespace Orca::UX
{
constexpr int TAG_CHARS = 5;
constexpr int TAG_SLOTS = 120;

// A YouGame username as a Brawl tag: letters uppercased, digits and '-', '.', '!', '?' kept, '_'
// and spaces become '-', anything else dropped, at most 5 characters. Locale-independent, so
// identical on every machine. Empty when nothing usable is left.
std::u16string BrawlTag(std::string_view username);

// Emulated memory access (tests substitute a map).
class GuestMemory
{
public:
  virtual ~GuestMemory() = default;
  virtual bool Valid(u32 address) const = 0;  // a mapped, readable RAM address
  virtual u32 Read32(u32 address) const = 0;
  virtual u16 Read16(u32 address) const = 0;
  virtual u8 Read8(u32 address) const = 0;
  virtual void Write8(u32 address, u8 value) = 0;
  virtual void Write16(u32 address, u16 value) = 0;
  virtual void Write32(u32 address, u32 value) = 0;
};

// The CPU thread's emulated memory, through the guard.
class GuardMemory final : public GuestMemory
{
public:
  explicit GuardMemory(const Core::CPUThreadGuard& guard) : m_guard(guard) {}
  bool Valid(u32 address) const override;
  u32 Read32(u32 address) const override;
  u16 Read16(u32 address) const override;
  u8 Read8(u32 address) const override;
  void Write8(u32 address, u8 value) override;
  void Write16(u32 address, u16 value) override;
  void Write32(u32 address, u32 value) override;

private:
  const Core::CPUThreadGuard& m_guard;
};

// A controls profile (PortInfo::controls): a tag's rumble byte, then its layout. Any other size is
// no profile.
constexpr u32 CONTROLS_LAYOUT_SIZE = 0x2D;
constexpr u32 CONTROLS_PROFILE_SIZE = 1 + CONTROLS_LAYOUT_SIZE;

// One frame's writes for these ports. Returns how many ports got a tag. Profile bytes are clamped
// to values the game's own menus could set (rumble 0 or 1, tap-jump and shake-smash bits only,
// actions up to 0xE), else the game's default, since profiles come from other machines.
int ApplyNameTags(GuestMemory& memory, const std::vector<Events::PortInfo>& ports);

// Runs from Events' frame callback on every frame (first runs and re-runs), before the session
// saves a snapshot or keyframe, so those include what it wrote.
void WriteNameTags(const Core::CPUThreadGuard& guard, const std::vector<Events::PortInfo>& ports);

// This player's own controls, read from their own save, to carry into an online game. Uses the tag
// `port` wears on the character select, else the tag named by `last_worn` (kept by the caller),
// else the tag matching `own_name`. Empty means the game's defaults.
std::vector<u8> ReadOwnControls(const GuestMemory& memory, int port, std::string_view own_name,
                                std::u16string* last_worn);

// Frame hook: on first runs while playing alone, publishes this player's controls to the session
// (Events::SetOwnControls). Brawl rev 2 and Project+ only.
void ReadOwnControlsFrame(const Core::CPUThreadGuard& guard,
                          const std::vector<Events::PortInfo>& ports);
}  // namespace Orca::UX
