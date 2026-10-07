// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <vector>

#include "Common/CommonTypes.h"

namespace Core
{
class System;
}

// A one-time dump at the first invalid guest access without MMU emulation (the "Invalid write"
// panic). The Mac boot crash of 2026-10-06 (M1/M2 on macOS 15: Brawl's OSSleepThread writes
// through a NULL current thread, then the external-interrupt vector at 0x500 reads zero) needs
// to know whether low MEM1 was never written or whether the host views of RAM disagree. The
// lines go to stderr only, which the desktop app keeps in the run's log and sends with its crash
// report. Nothing in emulation changes.
namespace Orca::PanicDiag
{
// One host mapping of guest RAM, as the dump reads it.
struct View
{
  std::string name;  // "ram", "phys", "logical 80000000", ...
  const u8* base;
  u32 physical_address;
  u32 size;
};

// The physical words the dump reads through every view: the exception vectors (system reset,
// DSI, external interrupt, decrementer, syscall) and the OS globals around the current thread.
extern const std::vector<u32> WORDS;

// One `orca diag word` line: the big-endian word at `physical_address` through every view that
// covers it, and "differ" when they don't all agree.
std::string DescribeWord(u32 physical_address, const std::vector<View>& views);

// Once per process: registers, BATs, RAM views and WORDS, as `orca diag` lines on stderr.
void DumpOnce(Core::System& system, u32 effective_address, bool write);
}  // namespace Orca::PanicDiag
