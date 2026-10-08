// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/CrashNote.h"

#include <cstring>

#ifdef _WIN32
#include <algorithm>
#include <array>
#include <atomic>
#include <exception>
#include <functional>
#include <mutex>

#include <windows.h>

#include <fmt/format.h>
#endif

namespace Orca::CrashNote
{
bool WriteLine(std::FILE* out, std::string_view text) noexcept
{
  if (!out)
    return false;
  // One call (one lock), so another thread's line can't land between the text and its newline.
  const bool wrote =
      std::fprintf(out, "%.*s\n", static_cast<int>(text.size()), text.data()) >= 0;
  return std::fflush(out) == 0 && wrote;
}

std::string TypeName(std::string_view decorated)
{
  std::string_view s = decorated;
  // Class (V) or struct (U), then "name@scope@...@@"; anything else is left as it is.
  if (!s.starts_with(".?AV") && !s.starts_with(".?AU"))
    return std::string(decorated);
  s.remove_prefix(4);
  if (s.size() < 3 || !s.ends_with("@@") || s.find_first_of("?$") != std::string_view::npos)
    return std::string(decorated);
  s.remove_suffix(2);
  std::string name;
  while (!s.empty())
  {
    const size_t at = s.rfind('@');
    const std::string_view part = at == std::string_view::npos ? s : s.substr(at + 1);
    if (part.empty())
      return std::string(decorated);
    if (!name.empty())
      name += "::";
    name += part;
    s = at == std::string_view::npos ? std::string_view{} : s.substr(0, at);
  }
  return name;
}

namespace
{
// MSVC's throw info, x64 and ARM64 (ehdata.h): every pointer is an offset from the image base.
//   _ThrowInfo         { u32 attributes; s32 unwind, forward_compat, catchable_type_array; }
//   _CatchableTypeArray { s32 count; s32 types[count]; }
//   _CatchableType     { u32 properties; s32 type; s32 mdisp, pdisp, vdisp; s32 size; s32 copy; }
//   TypeDescriptor     { void* vftable; void* spare; char name[]; }
constexpr size_t THROW_INFO_TYPES = 12;
constexpr size_t CATCHABLE_TYPE_TYPE = 4;
constexpr size_t CATCHABLE_TYPE_MDISP = 8;
constexpr size_t CATCHABLE_TYPE_PDISP = 12;
constexpr size_t TYPE_DESCRIPTOR_NAME = 2 * sizeof(void*);
constexpr s32 MAX_TYPES = 16;
constexpr size_t MAX_NAME = 255;

s32 ReadS32(const u8* at)
{
  s32 value;
  std::memcpy(&value, at, sizeof(value));
  return value;
}
}  // namespace

ThrownTypes ReadThrowInfo(const u8* image_base, const u8* throw_info)
{
  ThrownTypes out;
  if (!image_base || !throw_info)
    return out;
  const s32 array_offset = ReadS32(throw_info + THROW_INFO_TYPES);
  if (array_offset <= 0)
    return out;
  const u8* array = image_base + array_offset;
  const s32 count = ReadS32(array);
  for (s32 i = 0; i < count && i < MAX_TYPES; ++i)
  {
    const s32 type_offset = ReadS32(array + 4 + 4 * i);
    if (type_offset <= 0)
      break;
    const u8* catchable = image_base + type_offset;
    const s32 descriptor_offset = ReadS32(catchable + CATCHABLE_TYPE_TYPE);
    if (descriptor_offset <= 0)
      break;
    const char* name =
        reinterpret_cast<const char*>(image_base + descriptor_offset + TYPE_DESCRIPTOR_NAME);
    std::string decorated(name, strnlen(name, MAX_NAME));
    // A non-virtual base (pdisp -1) sits at mdisp; std::exception is never a virtual base.
    if (decorated == ".?AVexception@std@@" && ReadS32(catchable + CATCHABLE_TYPE_PDISP) == -1)
      out.std_exception_offset = ReadS32(catchable + CATCHABLE_TYPE_MDISP);
    out.decorated.push_back(std::move(decorated));
  }
  return out;
}

#ifdef _WIN32
namespace
{
constexpr DWORD MSVC_CXX_EXCEPTION = 0xE06D7363;
// The record's first parameter for a C++ throw (ehdata.h EH_MAGIC_NUMBER1 and its later forms).
constexpr ULONG_PTR EH_MAGIC_NUMBERS[] = {0x19930520, 0x19930521, 0x19930522};
constexpr int MAX_REPORTS = 16;
constexpr size_t MAX_WHAT = 300;
constexpr int MAX_FRAMES = 8;

std::mutex s_mutex;
std::array<size_t, MAX_REPORTS> s_reported{};
int s_reports = 0;
thread_local bool t_reporting = false;

// [base, base + size) of a loaded module, from its PE header; size 0 if it can't be read.
std::pair<uintptr_t, size_t> ModuleRange(HMODULE module)
{
  if (!module)
    return {0, 0};
  const auto base = reinterpret_cast<uintptr_t>(module);
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE)
    return {base, 0};
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE)
    return {base, 0};
  return {base, nt->OptionalHeader.SizeOfImage};
}

// Orca's own code: Orca.exe, and the C++ library DLL whose helpers throw for it (std::vector::at,
// std::stoi). Drivers and system DLLs throw and catch their own; those are left out.
bool OwnThrow(ULONG_PTR image_base)
{
  if (image_base == reinterpret_cast<ULONG_PTR>(GetModuleHandleW(nullptr)))
    return true;
  wchar_t path[MAX_PATH];
  const DWORD length =
      GetModuleFileNameW(reinterpret_cast<HMODULE>(image_base), path, static_cast<DWORD>(MAX_PATH));
  if (length == 0 || length >= MAX_PATH)
    return false;
  const wchar_t* file = path;
  for (const wchar_t* c = path; *c; ++c)
  {
    if (*c == L'\\' || *c == L'/')
      file = c + 1;
  }
  return _wcsnicmp(file, L"msvcp", 5) == 0;
}

std::string Report(const EXCEPTION_RECORD& record)
{
  const auto* image_base = reinterpret_cast<const u8*>(record.ExceptionInformation[3]);
  const auto* throw_info = reinterpret_cast<const u8*>(record.ExceptionInformation[2]);
  const ThrownTypes types = ReadThrowInfo(image_base, throw_info);
  std::string line = fmt::format("Orca: C++ exception {} on thread {}",
                                 types.decorated.empty() ? "of an unknown type" :
                                                           TypeName(types.decorated.front()),
                                 GetCurrentThreadId());
  if (types.std_exception_offset && record.ExceptionInformation[1])
  {
    const auto* object = reinterpret_cast<const std::exception*>(
        reinterpret_cast<const u8*>(record.ExceptionInformation[1]) + *types.std_exception_offset);
    if (const char* what = object->what())
    {
      std::string text(what, strnlen(what, MAX_WHAT));
      for (char& c : text)
      {
        if (c == '\n' || c == '\r')
          c = ' ';
      }
      line += ": " + text;
    }
  }
  // Where it was thrown: Orca.exe's frames on this stack past the exception dispatch (this
  // handler's own frames come first, then ntdll's and the throw's; JIT frames end the walk).
  void* frames[48];
  const USHORT count = RtlCaptureStackBackTrace(0, 48, frames, nullptr);
  const auto [base, size] = ModuleRange(GetModuleHandleW(nullptr));
  bool past_dispatch = false;
  int shown = 0;
  std::string where;
  for (USHORT i = 0; i < count && shown < MAX_FRAMES; ++i)
  {
    const auto at = reinterpret_cast<uintptr_t>(frames[i]);
    const bool own = size && at >= base && at - base < size;
    if (!own)
    {
      past_dispatch = true;
      continue;
    }
    if (!past_dispatch)
      continue;
    where += fmt::format("{}Orca.exe+{:#x}", where.empty() ? "" : " ", at - base);
    ++shown;
  }
  if (!where.empty())
    line += " (at " + where + ")";
  return line;
}

LONG CALLBACK OnException(EXCEPTION_POINTERS* pointers)
{
  const EXCEPTION_RECORD* record = pointers ? pointers->ExceptionRecord : nullptr;
  if (!record || record->ExceptionCode != MSVC_CXX_EXCEPTION || record->NumberParameters < 4 ||
      std::find(std::begin(EH_MAGIC_NUMBERS), std::end(EH_MAGIC_NUMBERS),
                record->ExceptionInformation[0]) == std::end(EH_MAGIC_NUMBERS) ||
      t_reporting || !OwnThrow(record->ExceptionInformation[3]))
  {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  t_reporting = true;
  try
  {
    std::lock_guard lock(s_mutex);
    if (s_reports < MAX_REPORTS)
    {
      std::string line = Report(*record);
      // Once per type, message and place, whichever thread threw it.
      std::string key = line;
      if (const size_t thread = key.find(" on thread "); thread != std::string::npos)
        key.erase(thread, key.find_first_of(":(", thread) - thread);
      const size_t hash = std::hash<std::string>{}(key);
      if (std::find(s_reported.begin(), s_reported.begin() + s_reports, hash) ==
          s_reported.begin() + s_reports)
      {
        s_reported[s_reports++] = hash;
        line += '\n';
        // Straight to the handle: the C runtime's stderr may be the stream that failed.
        DWORD written;
        WriteFile(GetStdHandle(STD_ERROR_HANDLE), line.data(), static_cast<DWORD>(line.size()),
                  &written, nullptr);
      }
    }
  }
  catch (...)
  {
  }
  t_reporting = false;
  return EXCEPTION_CONTINUE_SEARCH;
}
}  // namespace

void InstallThrowReporter()
{
  static std::atomic<bool> s_installed{false};
  if (s_installed.exchange(true))
    return;
  // Called last, after the fastmem handler (MemTools.cpp), which never takes a C++ exception.
  AddVectoredExceptionHandler(FALSE, OnException);
}
#else
void InstallThrowReporter()
{
}
#endif
}  // namespace Orca::CrashNote
