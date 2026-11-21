// Copyright 2009 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HW/DVD/FileMonitor.h"

#include <cinttypes>
#include <string>

#include <cstdio>
#include <cstring>

#include <string/stdstring.h>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/Logging/LogManager.h"

#include "DiscIO/Filesystem.h"
#include "DiscIO/Volume.h"

namespace FileMonitor
{
// Filtered files
static bool IsSoundFile(const char* filename)
{
  // the extension is from the last dot of the file name
  const char* name = filename;
  for (const char* c = filename; *c; c++)
  {
#ifdef _WIN32
    if (*c == '/' || *c == ':')
#else
    if (*c == '/')
#endif
      name = c + 1;
  }
  const char* const extension = std::strrchr(name, '.');
  if (!extension)
    return false;

  static const char* const extensions[] = {
      ".adp",    // 1080 Avalanche, Crash Bandicoot, etc.
      ".adx",    // Sonic Adventure 2 Battle, etc.
      ".afc",    // Zelda WW
      ".ast",    // Zelda TP, Mario Kart
      ".brstm",  // Wii Sports, Wario Land, etc.
      ".dsp",    // Metroid Prime
      ".hps",    // SSB Melee
      ".ogg",    // Tony Hawk's Underground 2
      ".sad",    // Disaster
      ".snd",    // Tales of Symphonia
      ".song",   // Tales of Symphonia
      ".ssm",    // Custom Robo, Kirby Air Ride, etc.
      ".str",    // Harry Potter & the Sorcerer's Stone
  };
  for (size_t i = 0; i < sizeof(extensions) / sizeof(extensions[0]); i++)
  {
    if (string_is_equal_case_insensitive(extension, extensions[i]))
      return true;
  }
  return false;
}

FileLogger::FileLogger() = default;

FileLogger::~FileLogger() = default;

void FileLogger::Log(const DiscIO::Volume& volume, const DiscIO::Partition& partition, u64 offset)
{
  // Do nothing if the log isn't selected
  if (!Common::Log::LogManager::GetInstance()->IsEnabled(Common::Log::LogType::FILEMON,
                                                         Common::Log::LogLevel::LWARNING))
  {
    return;
  }

  const DiscIO::FileSystem* file_system = volume.GetFileSystem(partition);

  // Do nothing if there is no valid file system
  if (!file_system)
    return;

  DiscIO::FileInfo file_info;

  // Do nothing if no file was found at that offset
  if (!file_system->FindFileInfo(offset, &file_info))
    return;

  const u64 file_offset = file_info.GetOffset();

  // Do nothing if we found the same file again
  if (m_previous_partition == partition && m_previous_file_offset == file_offset)
    return;

  const u64 size_kb = file_info.GetSize() / 1000;
  const std::string path = file_info.GetPath();
  if (IsSoundFile(path.c_str()))
    INFO_LOG(FILEMON, "%7" PRIu64 " kB %s", size_kb, path.c_str());
  else
    WARN_LOG(FILEMON, "%7" PRIu64 " kB %s", size_kb, path.c_str());

  // Update the last accessed file
  m_previous_partition = partition;
  m_previous_file_offset = file_offset;
}

}  // namespace FileMonitor
