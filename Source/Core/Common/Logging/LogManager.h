// Copyright 2009 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstdarg>
#include <memory>
#include <string>
#include <vector>

#include "Common/BitSet.h"
#include "Common/Config/Config.h"
#include "Common/EnumMap.h"
#include "Common/Logging/Log.h"

#include <retro_atomic.h>

namespace Common::Log
{
// This variable should only be read to update the effective log level, and its base layer should
// only be set when the user selects a new verbosity. Everything else should use the effective log
// level instead. When running a release build this prevents overwriting the LDEBUG config value
// with the clamped LINFO level.
extern const Config::Info<LogLevel> LOGGER_VERBOSITY;

/* Where log messages go: a function, and what it is called with. Any thread may call it. Its
 * owner keeps the context alive while the listener is registered. */
struct LogListener
{
  void (*log)(void* context, LogLevel level, const char* msg);
  void* context;

  enum LISTENER
  {
    FILE_LISTENER = 0,
    CONSOLE_LISTENER,
    LOG_WINDOW_LISTENER,
    CUSTOM_LISTENER,
    NUMBER_OF_LISTENERS  // Must be last
  };
};

class LogManager final
{
public:
  struct LogContainer
  {
    const char* m_short_name;
    const char* m_full_name;
    bool m_enable = false;
  };

  ~LogManager();

  static LogManager* GetInstance();
  static void Init();
  static void Shutdown();

  void Log(LogLevel level, LogType type, const char* file, int line, const char* message);
  void LogWithFullPath(LogLevel level, LogType type, const char* file, int line,
                       const char* message);

  // Use this function instead of LOGGER_VERBOSITY to determine which logs should be printed.
  LogLevel GetEffectiveLogLevel() const;
  void SetConfigLogLevel(LogLevel level);

  void SetEnable(LogType type, bool enable);
  bool IsEnabled(LogType type, LogLevel level = LogLevel::LNOTICE) const;

  const char* GetShortName(LogType type) const;
  const char* GetFullName(LogType type) const;

  /* a listener with a null function takes the one there away */
  void RegisterListener(LogListener::LISTENER id, LogListener listener);
  void EnableListener(LogListener::LISTENER id, bool enable);
  bool IsListenerEnabled(LogListener::LISTENER id) const;

  void SaveSettings();

private:
  LogManager();

  LogManager(const LogManager&) = delete;
  LogManager& operator=(const LogManager&) = delete;
  LogManager(LogManager&&) = delete;
  LogManager& operator=(LogManager&&) = delete;

  void SetEffectiveLogLevel();

  retro_atomic_int_t m_effective_level{}; /* a LogLevel */
  Config::ConfigChangedCallbackID m_config_changed_callback_id;
  EnumMap<LogContainer, LAST_LOG_TYPE> m_log{};
  LogListener m_listeners[LogListener::NUMBER_OF_LISTENERS]{};
  BitSet32 m_listener_ids;
  size_t m_path_cutoff_point = 0;
};
}  // namespace Common::Log
