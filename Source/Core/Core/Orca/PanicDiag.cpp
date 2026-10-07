// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/PanicDiag.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <tuple>
#include <vector>

#include <fmt/format.h>

#include "Common/Swap.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/Gekko.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

namespace Orca::PanicDiag
{
const std::vector<u32> WORDS = {0x100, 0x300, 0x500, 0x504, 0x900, 0xC00,
                                0xD8,  0xDC,  0xE0,  0xE4,  0x30,  0x34};

std::string DescribeWord(u32 physical_address, const std::vector<View>& views)
{
  std::string line = fmt::format("orca diag word {:08x}:", physical_address);
  bool any = false;
  bool differ = false;
  u32 first = 0;
  for (const View& view : views)
  {
    if (!view.base || physical_address < view.physical_address ||
        physical_address - view.physical_address + 4 > view.size)
    {
      continue;
    }
    u32 raw;
    std::memcpy(&raw, view.base + (physical_address - view.physical_address), sizeof(raw));
    const u32 word = Common::swap32(raw);
    if (!any)
      first = word;
    else if (word != first)
      differ = true;
    any = true;
    line += fmt::format(" {}={:08x}", view.name, word);
  }
  if (!any)
    line += " no view";
  if (differ)
    line += " differ";
  return line;
}

namespace
{
std::string ViewName(Memory::MemoryManager& memory, const u8* base)
{
  if (base == memory.GetRAM())
    return "ram";
  if (base == memory.GetEXRAM())
    return "exram";
  constexpr u64 SPAN = 0x100000000ULL;
  if (const u8* physical = memory.GetPhysicalBase();
      physical && base >= physical && static_cast<u64>(base - physical) < SPAN)
  {
    return fmt::format("phys:{:08x}", static_cast<u32>(base - physical));
  }
  if (const u8* logical = memory.GetLogicalBase();
      logical && base >= logical && static_cast<u64>(base - logical) < SPAN)
  {
    return fmt::format("logical:{:08x}", static_cast<u32>(base - logical));
  }
  return "view";
}
}  // namespace

void DumpOnce(Core::System& system, u32 effective_address, bool write)
{
  static std::atomic<bool> s_done{false};
  if (s_done.exchange(true))
    return;

  const PowerPC::PowerPCState& ppc = system.GetPPCState();
  Memory::MemoryManager& memory = system.GetMemory();
  std::vector<std::string> lines;
  lines.push_back(fmt::format("orca diag first invalid {} {:08x} at pc {:08x}",
                              write ? "write to" : "read from", effective_address, ppc.pc));
  lines.push_back(fmt::format(
      "orca diag regs msr {:08x} srr0 {:08x} srr1 {:08x} lr {:08x} ctr {:08x} r1 {:08x} "
      "dec {:08x} hid4 {:08x}",
      ppc.msr.Hex, ppc.spr[SPR_SRR0], ppc.spr[SPR_SRR1], ppc.spr[SPR_LR], ppc.spr[SPR_CTR],
      ppc.gpr[1], ppc.spr[SPR_DEC], ppc.spr[SPR_HID4]));
  for (const auto& [label, first, second] :
       {std::tuple{"ibat", SPR_IBAT0U, SPR_IBAT4U}, std::tuple{"dbat", SPR_DBAT0U, SPR_DBAT4U}})
  {
    std::string line = fmt::format("orca diag {}", label);
    for (int i = 0; i < 8; ++i)
    {
      const int upper = (i < 4 ? first : second) + 2 * (i % 4);
      line += fmt::format(" {}:{:08x}/{:08x}", i, ppc.spr[upper], ppc.spr[upper + 1]);
    }
    lines.push_back(std::move(line));
  }

  std::vector<View> views;
  for (const Memory::GuestRamView& view : memory.GetGuestRamViews())
  {
    views.push_back(View{ViewName(memory, view.base), view.base, view.physical_address, view.size});
  }
  std::string list =
      fmt::format("orca diag views fastmem {}:", memory.GetPhysicalBase() ? "on" : "off");
  for (const View& view : views)
    list += fmt::format(" {}@{:08x}+{:x}", view.name, view.physical_address, view.size);
  lines.push_back(std::move(list));
  for (const u32 address : WORDS)
    lines.push_back(DescribeWord(address, views));

  for (const std::string& line : lines)
    std::fprintf(stderr, "%s\n", line.c_str());
  std::fflush(stderr);
}
}  // namespace Orca::PanicDiag
