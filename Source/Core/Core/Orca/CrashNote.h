// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"

// What Orca says on stderr as it fails, so the app's crash report (desktop orca-crash.ts, which
// sends the run's stderr) says why. Nothing here may throw: it runs where a C++ exception can't
// unwind. On Windows that is anything the CPU thread calls from JIT code (an invalid access's
// panic alert, a CoreTiming event): the JIT registers no unwind tables, so the exception can't be
// dispatched and Windows ends Orca with 0xE06D7363 at once, without std::terminate and without a
// word on stderr (bug report b5ef88b5, Orca 0.3.29, 2026-10-07).
namespace Orca::CrashNote
{
// Writes `text` and a newline to `out` and flushes it. Unlike fmt::print, which throws
// fmt::system_error when the stream takes fewer bytes than it was given, it never throws; returns
// false when the write failed.
bool WriteLine(std::FILE* out, std::string_view text) noexcept;

// A thrown type as written in MSVC's throw info (".?AVsystem_error@std@@"), readable
// ("std::system_error"). Names it can't read (templates, other forms) come back unchanged.
std::string TypeName(std::string_view decorated);

// What MSVC's throw info (x64 and ARM64: image-relative offsets) says about a thrown object: every
// type a catch could take it as, most derived first, as decorated names, and where its
// std::exception base sits in the object when it has one.
struct ThrownTypes
{
  std::vector<std::string> decorated;
  std::optional<s32> std_exception_offset;
};
// `image_base` is the throwing module's base (the exception record's fourth parameter) and
// `throw_info` its _ThrowInfo (the third). Reads at most 16 types and 255 characters of each name.
ThrownTypes ReadThrowInfo(const u8* image_base, const u8* throw_info);

// Windows: from here on, each C++ exception Orca's own code throws prints one line on stderr as it
// is thrown, before any catch:
//   Orca: C++ exception <type> on thread <id>: <what> (at Orca.exe+<offset> ...)
// Up to 16 different ones per run, each once. A caught one costs a line; one that kills Orca is
// the last line of its log. Elsewhere it does nothing: Mac and Linux builds have no exceptions
// (-fno-exceptions), so a failure that would throw aborts with its own message.
void InstallThrowReporter();
}  // namespace Orca::CrashNote
