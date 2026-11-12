// Copyright 2014 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/JitRegister.h"

#include <cinttypes>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "Common/CommonTypes.h"
#include "Common/IOFile.h"

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

static File::IOFile s_perf_map_file;

namespace Common::JitRegister
{
static bool s_is_enabled = false;

void Init(const std::string& perf_dir)
{
  if (!perf_dir.empty() || getenv("PERF_BUILDID_DIR"))
  {
    char filename[1024];
    snprintf(filename, sizeof(filename), "%s/perf-%d.map",
             perf_dir.empty() ? "/tmp" : perf_dir.c_str(), static_cast<int>(getpid()));
    if (s_perf_map_file.Open(filename, "w"))
      s_is_enabled = true;
  }
}

void Shutdown()
{
  if (s_perf_map_file.IsOpen())
    s_perf_map_file.Close();

  s_is_enabled = false;
}

bool IsEnabled()
{
  return s_is_enabled;
}

static void RegisterV(const void* base_address, u32 code_size, const char* format, va_list args)
{
  // Linux perf /tmp/perf-$pid.map:
  if (!s_perf_map_file)
    return;

  char entry[512];
  int len = snprintf(entry, sizeof(entry), "%" PRIxPTR " %x ",
                     reinterpret_cast<uintptr_t>(base_address), code_size);
  if (len < 0 || len >= static_cast<int>(sizeof(entry)) - 1)
    return;
  const int name_len = vsnprintf(entry + len, sizeof(entry) - len - 1, format, args);
  if (name_len < 0)
    return;
  len += name_len;
  if (len > static_cast<int>(sizeof(entry)) - 2)
    len = static_cast<int>(sizeof(entry)) - 2;
  entry[len++] = '\n';
  s_perf_map_file.WriteBytes(entry, len);
}

void Register(const void* base_address, u32 code_size, const char* format, ...)
{
  if (!s_is_enabled)
    return;
  va_list args;
  va_start(args, format);
  RegisterV(base_address, code_size, format, args);
  va_end(args);
}

void Register(const void* start, const void* end, const char* format, ...)
{
  if (!s_is_enabled)
    return;
  va_list args;
  va_start(args, format);
  RegisterV(start,
            static_cast<u32>(static_cast<const char*>(end) - static_cast<const char*>(start)),
            format, args);
  va_end(args);
}
}  // namespace Common::JitRegister
