// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/UX/NameTags.h"

#include <algorithm>
#include <cstdlib>
#include <optional>
#include <utility>

#include "Core/Core.h"
#include "Core/Orca/Profile.h"
#include "Core/PowerPC/MMU.h"

namespace Orca::UX
{
namespace
{
constexpr u32 SCENE_MANAGER = 0x805A0060;
constexpr u32 GAME_GLOBAL = 0x805A00E0;
constexpr u32 GAME_GLOBAL_NAME_RECORDS = 0x28;
constexpr u32 RECORDS_TAGS = 0xE0;
constexpr u32 TAG_SIZE = 0x124;
constexpr u32 TAG_RUMBLE = 0x0C;
constexpr u32 TAG_CONTROLS = 0x14;
constexpr u32 CONTROLS_TEMPLATE = 0x80406938;
constexpr u32 CONTROLS_TEMPLATE_SIZE = CONTROLS_LAYOUT_SIZE;
// Flag byte offsets in the layout: GameCube tap jump, Nunchuk tap jump (0x40) and shake smash
// (0x80), Classic tap jump. Every other byte is an action.
constexpr u32 FLAGS_GAMECUBE = 11, FLAGS_NUNCHUK = 0x1F, FLAGS_CLASSIC = 0x2C;
// The highest action the menus can set. 0xE is "none".
constexpr u8 MAX_ACTION = 0x0E;
constexpr u32 SCENE_SELCHAR_TASK = 0x400;
constexpr u32 TASK_AREAS = 0x44;
constexpr u32 AREA_INDEX = 0x1B0;
constexpr u32 AREA_PLAYER_KIND = 0x1B4;
constexpr u32 AREA_NAME_ID = 0x1C8;
constexpr u32 PLAYER_KIND_HUMAN = 1;
constexpr u32 NO_TAG = 0xFFFFFFFF;
constexpr char CHARACTER_SELECT[] = "scSelctCharacter";

bool Pointer(const GuestMemory& m, u32 p)
{
  return p % 4 == 0 && m.Valid(p);
}

bool SceneIs(const GuestMemory& m, std::string_view want)
{
  if (!Pointer(m, SCENE_MANAGER))
    return false;
  const u32 manager = m.Read32(SCENE_MANAGER);
  if (!Pointer(m, manager + 4))
    return false;
  const u32 scene = m.Read32(manager + 4);
  if (!Pointer(m, scene))
    return false;
  const u32 name = m.Read32(scene);
  for (size_t i = 0; i <= want.size(); ++i)
  {
    if (!m.Valid(name + static_cast<u32>(i)))
      return false;
    const char c = static_cast<char>(m.Read8(name + static_cast<u32>(i)));
    if (c != (i < want.size() ? want[i] : '\0'))
      return false;
  }
  return true;
}

std::u16string TagAt(const GuestMemory& m, u32 tag)
{
  std::u16string name;
  for (u32 i = 0; i < TAG_CHARS; ++i)
  {
    const u16 c = m.Read16(tag + 2 * i);
    if (c == 0)
      break;
    name.push_back(static_cast<char16_t>(c));
  }
  return name;
}

// The tag holding this name; -1 when none does.
int FindTag(const GuestMemory& m, u32 tags, const std::u16string& name)
{
  for (int i = 0; i < TAG_SLOTS; ++i)
  {
    if (TagAt(m, tags + static_cast<u32>(i) * TAG_SIZE) == name)
      return i;
  }
  return -1;
}

// Creates a tag in the highest unused slot the way the game does: record cleared, name, rumble on,
// default controls from CONTROLS_TEMPLATE. -1 when every tag is taken.
int MakeTag(GuestMemory& m, u32 tags, const std::u16string& name)
{
  int unused = -1;
  for (int i = 0; i < TAG_SLOTS; ++i)
  {
    if (TagAt(m, tags + static_cast<u32>(i) * TAG_SIZE).empty())
      unused = i;
  }
  if (unused < 0 || !m.Valid(CONTROLS_TEMPLATE) ||
      !m.Valid(CONTROLS_TEMPLATE + CONTROLS_TEMPLATE_SIZE - 1))
  {
    return -1;
  }
  const u32 tag = tags + static_cast<u32>(unused) * TAG_SIZE;
  for (u32 i = 0; i < TAG_SIZE; i += 2)
    m.Write16(tag + i, 0);
  for (u32 i = 0; i < name.size() && i < TAG_CHARS; ++i)
    m.Write16(tag + 2 * i, static_cast<u16>(name[i]));
  m.Write8(tag + TAG_RUMBLE, 1);
  for (u32 i = 0; i < CONTROLS_TEMPLATE_SIZE; ++i)
    m.Write8(tag + TAG_CONTROLS + i, m.Read8(CONTROLS_TEMPLATE + i));
  return unused;
}

// Whether another port already wears tag `slot`. The caller has checked the areas.
bool WornByAnotherPort(const GuestMemory& m, u32 task, int port, int slot)
{
  for (int other = 0; other < 4; ++other)
  {
    if (other == port)
      continue;
    const u32 at = task + TASK_AREAS + 4 * static_cast<u32>(other);
    const u32 area = m.Read32(at);
    if (Pointer(m, area) && Pointer(m, area + AREA_NAME_ID) &&
        m.Read32(area + AREA_NAME_ID) == static_cast<u32>(slot))
    {
      return true;
    }
  }
  return false;
}

// Two usernames can share their first five letters (sandbox-ada, sandbox-bo). Brawl never lets two
// players wear one tag, so the second port's tag ends in its port number instead (SAND2).
std::u16string Variant(std::u16string name, int port)
{
  const char16_t digit = static_cast<char16_t>(u'1' + port);
  if (name.size() < static_cast<size_t>(TAG_CHARS))
    name.push_back(digit);
  else
    name.back() = digit;
  return name;
}

// Clamps a profile's layout byte to what the game's menus could set, else the default.
u8 LayoutByte(u32 i, u8 value, u8 default_value)
{
  switch (i)
  {
  case FLAGS_GAMECUBE:
  case FLAGS_CLASSIC:
    return value & 0x80;
  case FLAGS_NUNCHUK:
    return value & 0xC0;
  default:
    return value <= MAX_ACTION ? value : default_value;
  }
}

// Writes rumble and layout into the tag, only bytes that differ. The caller checked that the
// defaults are readable.
void WriteControls(GuestMemory& m, u32 tag, const std::vector<u8>& profile)
{
  const u8 rumble = profile[0] != 0 ? 1 : 0;
  if (m.Read8(tag + TAG_RUMBLE) != rumble)
    m.Write8(tag + TAG_RUMBLE, rumble);
  for (u32 i = 0; i < CONTROLS_LAYOUT_SIZE; ++i)
  {
    const u8 value = LayoutByte(i, profile[1 + i], m.Read8(CONTROLS_TEMPLATE + i));
    if (m.Read8(tag + TAG_CONTROLS + i) != value)
      m.Write8(tag + TAG_CONTROLS + i, value);
  }
}

bool TemplateReadable(const GuestMemory& m)
{
  return m.Valid(CONTROLS_TEMPLATE) && m.Valid(CONTROLS_TEMPLATE + CONTROLS_TEMPLATE_SIZE - 1);
}

// The character select's task, when that scene is running.
bool CharacterSelectTask(const GuestMemory& m, u32* task)
{
  if (!SceneIs(m, CHARACTER_SELECT))
    return false;
  const u32 scene = m.Read32(m.Read32(SCENE_MANAGER) + 4);
  if (!Pointer(m, scene + SCENE_SELCHAR_TASK))
    return false;
  *task = m.Read32(scene + SCENE_SELCHAR_TASK);
  return Pointer(m, *task + TASK_AREAS);
}

// The save's first tag, when the whole table is mapped.
bool TagTable(const GuestMemory& m, u32* tags)
{
  if (!Pointer(m, GAME_GLOBAL))
    return false;
  const u32 global = m.Read32(GAME_GLOBAL);
  if (!Pointer(m, global + GAME_GLOBAL_NAME_RECORDS))
    return false;
  const u32 records = m.Read32(global + GAME_GLOBAL_NAME_RECORDS);
  *tags = records + RECORDS_TAGS;
  return Pointer(m, records) && m.Valid(*tags + TAG_SLOTS * TAG_SIZE - 1);
}

// Port `port`'s player area on the character select, if a human joined it.
std::optional<u32> JoinedArea(const GuestMemory& m, u32 task, int port)
{
  const u32 at = task + TASK_AREAS + 4 * static_cast<u32>(port);
  if (!Pointer(m, at))
    return std::nullopt;
  const u32 area = m.Read32(at);
  if (!Pointer(m, area) || !Pointer(m, area + AREA_NAME_ID) ||
      m.Read32(area + AREA_INDEX) != static_cast<u32>(port) ||
      m.Read32(area + AREA_PLAYER_KIND) != PLAYER_KIND_HUMAN)
  {
    return std::nullopt;
  }
  return area;
}

// "P2" etc., for a port with controls but no showable name. Looks like the game's own label.
std::u16string PortLabel(int port)
{
  return {u'P', static_cast<char16_t>(u'1' + port)};
}
}  // namespace

bool GuardMemory::Valid(u32 address) const
{
  return PowerPC::MMU::HostIsRAMAddress(m_guard, address);
}
u32 GuardMemory::Read32(u32 address) const
{
  return PowerPC::MMU::HostRead<u32>(m_guard, address);
}
u16 GuardMemory::Read16(u32 address) const
{
  return PowerPC::MMU::HostRead<u16>(m_guard, address);
}
u8 GuardMemory::Read8(u32 address) const
{
  return PowerPC::MMU::HostRead<u8>(m_guard, address);
}
void GuardMemory::Write8(u32 address, u8 value)
{
  PowerPC::MMU::HostWrite<u8>(m_guard, value, address);
}
void GuardMemory::Write16(u32 address, u16 value)
{
  PowerPC::MMU::HostWrite<u16>(m_guard, value, address);
}
void GuardMemory::Write32(u32 address, u32 value)
{
  PowerPC::MMU::HostWrite<u32>(m_guard, value, address);
}

std::u16string BrawlTag(std::string_view username)
{
  std::u16string tag;
  for (const char c : username)
  {
    if (tag.size() >= TAG_CHARS)
      break;
    if (c >= 'a' && c <= 'z')
      tag.push_back(static_cast<char16_t>(c - 'a' + 'A'));
    else if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
             c == '!' || c == '?')
      tag.push_back(static_cast<char16_t>(c));
    else if (c == '_' || c == ' ')
      tag.push_back(u'-');
    // Anything else, including every byte of non-ASCII characters, is dropped.
  }
  return tag;
}

int ApplyNameTags(GuestMemory& m, const std::vector<Events::PortInfo>& ports)
{
  u32 task = 0, tags = 0;
  if (ports.empty() || !CharacterSelectTask(m, &task) || !TagTable(m, &tags))
    return 0;
  int given = 0;
  for (const Events::PortInfo& p : ports)
  {
    if (p.port < 0 || p.port > 3)
      continue;
    const bool carries = p.controls.size() == CONTROLS_PROFILE_SIZE;
    std::u16string name = BrawlTag(p.name);
    if (name.empty() && carries)
      name = PortLabel(p.port);
    if (name.empty())
      continue;
    const std::optional<u32> area = JoinedArea(m, task, p.port);
    if (!area || m.Read32(*area + AREA_NAME_ID) != NO_TAG)
      continue;
    std::u16string tag = name;
    int slot = FindTag(m, tags, tag);
    if (slot >= 0 && WornByAnotherPort(m, task, p.port, slot))
    {
      tag = Variant(name, p.port);
      slot = FindTag(m, tags, tag);
      if (slot >= 0 && WornByAnotherPort(m, task, p.port, slot))
        continue;
    }
    if (carries && !TemplateReadable(m))
      continue;
    if (slot < 0)
      slot = MakeTag(m, tags, tag);
    if (slot < 0)
      continue;
    if (carries)
      WriteControls(m, tags + static_cast<u32>(slot) * TAG_SIZE, p.controls);
    m.Write32(*area + AREA_NAME_ID, static_cast<u32>(slot));
    ++given;
  }
  return given;
}

namespace
{
// The addresses are Brawl rev 2's (RSBE01), also booted by Project+'s launcher.
bool BrawlRev2()
{
  const Orca::Profile* profile = Orca::ActiveProfile();
  return profile && profile->revision == 2 &&
         (profile->IsLauncher() ? profile->disc == "RSBE01" : profile->game_id == "RSBE01");
}
}  // namespace

void WriteNameTags(const Core::CPUThreadGuard& guard, const std::vector<Events::PortInfo>& ports)
{
  if (ports.empty() || !BrawlRev2())
    return;
  GuardMemory memory(guard);
  ApplyNameTags(memory, ports);
}

std::vector<u8> ReadOwnControls(const GuestMemory& m, int port, std::string_view own_name,
                                std::u16string* last_worn)
{
  u32 tags = 0;
  if (!TagTable(m, &tags))
    return {};
  // On the character select, the tag the port wears is the one they play with.
  u32 task = 0;
  if (port >= 0 && port <= 3 && CharacterSelectTask(m, &task))
  {
    if (const std::optional<u32> area = JoinedArea(m, task, port))
    {
      const u32 id = m.Read32(*area + AREA_NAME_ID);
      if (id < static_cast<u32>(TAG_SLOTS))
      {
        if (std::u16string worn = TagAt(m, tags + id * TAG_SIZE); !worn.empty())
          *last_worn = std::move(worn);
      }
    }
  }
  int slot = last_worn->empty() ? -1 : FindTag(m, tags, *last_worn);
  if (const std::u16string own = BrawlTag(own_name); slot < 0 && !own.empty())
    slot = FindTag(m, tags, own);
  if (slot < 0)
    return {};
  const u32 tag = tags + static_cast<u32>(slot) * TAG_SIZE;
  std::vector<u8> profile;
  profile.reserve(CONTROLS_PROFILE_SIZE);
  profile.push_back(m.Read8(tag + TAG_RUMBLE));
  for (u32 i = 0; i < CONTROLS_LAYOUT_SIZE; ++i)
    profile.push_back(m.Read8(tag + TAG_CONTROLS + i));
  return profile;
}

void ReadOwnControlsFrame(const Core::CPUThreadGuard& guard,
                          const std::vector<Events::PortInfo>& ports)
{
  if (!BrawlRev2())
    return;
  // PortInfo.remote differs per machine, so it only picks what to read here, never what to write.
  const auto own =
      std::find_if(ports.begin(), ports.end(), [](const Events::PortInfo& p) { return !p.remote; });
  if (own == ports.end())
    return;
  static std::u16string s_last_worn;
  static std::vector<u8> s_published;
  // After a resync (keyframe load or back to solo), a tag with the remembered name may belong to
  // someone else's save, so forget it.
  static u64 s_resyncs = 0;
  if (const u64 resyncs = Events::Resyncs(); resyncs != s_resyncs)
  {
    s_resyncs = resyncs;
    s_last_worn.clear();
  }
  GuardMemory memory(guard);
  std::vector<u8> profile = ReadOwnControls(memory, own->port, own->name, &s_last_worn);
  if (profile != s_published)
  {
    s_published = profile;
    Events::SetOwnControls(std::move(profile));
  }
}

}  // namespace Orca::UX
