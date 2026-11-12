// Copyright 2009 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>

#include "Common/CommonTypes.h"

namespace Common::Log
{
enum class LogType : int
{
  ACHIEVEMENTS,
  ACTIONREPLAY,
  AUDIO,
  AUDIO_INTERFACE,
  BOOT,
  COMMANDPROCESSOR,
  COMMON,
  CONSOLE,
  CONTROLLERINTERFACE,
  CORE,
  DISCIO,
  DSPHLE,
  DSPLLE,
  DSP_MAIL,
  DSPINTERFACE,
  DVDINTERFACE,
  AMMEDIABOARD,
  AMMEDIABOARD_NET,
  DYNA_REC,
  EXPANSIONINTERFACE,
  FILEMON,
  FRAMEDUMP,
  GPFIFO,
  HOST_GPU,
  HSP,
  IOS,
  IOS_DI,
  IOS_ES,
  IOS_FS,
  IOS_NET,
  IOS_SD,
  IOS_SSL,
  IOS_STM,
  IOS_USB,
  IOS_WC24,
  IOS_WFS,
  IOS_WIIMOTE,
  MASTER_LOG,
  MEMMAP,
  MEMCARD_MANAGER,
  NETPLAY,
  OSHLE,
  OSREPORT,
  OSREPORT_HLE,
  PIXELENGINE,
  PROCESSORINTERFACE,
  POWERPC,
  SERIALINTERFACE,
  SERIALINTERFACE_AMBB,
  SERIALINTERFACE_CARD,
  SERIALINTERFACE_JVSIO,
  SP1,
  SYMBOLS,
  VIDEO,
  VIDEOINTERFACE,
  WII_IPC,
  WIIMOTE,

  NUMBER_OF_LOGS  // Must be last
};

constexpr LogType LAST_LOG_TYPE =
    static_cast<LogType>(static_cast<int>(LogType::NUMBER_OF_LOGS) - 1);

enum class LogLevel : int
{
  LNOTICE = 1,   // VERY important information that is NOT errors. Like startup and OSReports.
  LERROR = 2,    // Critical errors
  LWARNING = 3,  // Something is suspicious.
  LINFO = 4,     // General information.
  LDEBUG = 5,    // Detailed debugging - might make things slow.
};

#if defined(_DEBUG) || defined(DEBUGFAST)
constexpr auto MAX_EFFECTIVE_LOGLEVEL = Common::Log::LogLevel::LDEBUG;
#else
constexpr auto MAX_EFFECTIVE_LOGLEVEL = Common::Log::LogLevel::LINFO;
#endif  // logging

static const char LOG_LEVEL_TO_CHAR[7] = "-NEWID";

/* A message in printf's terms, formatted into a buffer on the stack (a
 * longer one is cut), only when its type and level are enabled. */
void GenericLog(LogLevel level, LogType type, const char* file, int line, const char* format, ...)
    DOLPHIN_PRINTF_ATTR(5, 6);

}  // namespace Common::Log

// printf capable API

#define GENERIC_LOG(t, v, ...)                                                                     \
  do                                                                                               \
  {                                                                                                \
    if (v <= Common::Log::MAX_EFFECTIVE_LOGLEVEL)                                                  \
      Common::Log::GenericLog(v, t, __FILE__, __LINE__, __VA_ARGS__);                              \
  } while (0)

#define ERROR_LOG(t, ...)                                                                          \
  GENERIC_LOG(Common::Log::LogType::t, Common::Log::LogLevel::LERROR, __VA_ARGS__)
#define WARN_LOG(t, ...)                                                                           \
  GENERIC_LOG(Common::Log::LogType::t, Common::Log::LogLevel::LWARNING, __VA_ARGS__)
#define NOTICE_LOG(t, ...)                                                                         \
  GENERIC_LOG(Common::Log::LogType::t, Common::Log::LogLevel::LNOTICE, __VA_ARGS__)
#define INFO_LOG(t, ...)                                                                           \
  GENERIC_LOG(Common::Log::LogType::t, Common::Log::LogLevel::LINFO, __VA_ARGS__)
#define DEBUG_LOG(t, ...)                                                                          \
  GENERIC_LOG(Common::Log::LogType::t, Common::Log::LogLevel::LDEBUG, __VA_ARGS__)
