// Copyright 2009 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/Logging/LogManager.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/Logging/ConsoleListener.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"

namespace Common::Log
{
const Config::Info<bool> LOGGER_WRITE_TO_FILE{{Config::System::Logger, "Options", "WriteToFile"},
                                              false};
const Config::Info<bool> LOGGER_WRITE_TO_CONSOLE{
    {Config::System::Logger, "Options", "WriteToConsole"}, true};
const Config::Info<bool> LOGGER_WRITE_TO_WINDOW{
    {Config::System::Logger, "Options", "WriteToWindow"}, true};
const Config::Info<LogLevel> LOGGER_VERBOSITY{{Config::System::Logger, "Options", "Verbosity"},
                                              LogLevel::LNOTICE};

// Any thread may log. The file is opened for appending, where the OS makes each write land
// whole at the end, so a message is one write and nothing is shared in user space.
class FileLogListener : public LogListener
{
public:
  FileLogListener(const std::string& filename)
  {
    File::CreateFullPath(filename);
#ifdef _WIN32
    m_file = CreateFileW(UTF8ToWString(filename).c_str(), FILE_APPEND_DATA,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
#else
    m_file = open(filename.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
#endif
    SetEnable(true);
  }

  ~FileLogListener() override
  {
#ifdef _WIN32
    if (m_file != INVALID_HANDLE_VALUE)
      CloseHandle(m_file);
#else
    if (m_file >= 0)
      close(m_file);
#endif
  }

  void Log(LogLevel, const char* msg) override
  {
    if (!IsEnabled() || !IsValid())
      return;

    const size_t length = std::strlen(msg);
#ifdef _WIN32
    DWORD written;
    WriteFile(m_file, msg, static_cast<DWORD>(length), &written, nullptr);
#else
    [[maybe_unused]] const ssize_t written = write(m_file, msg, length);
#endif
  }

#ifdef _WIN32
  bool IsValid() const { return m_file != INVALID_HANDLE_VALUE; }
#else
  bool IsValid() const { return m_file >= 0; }
#endif
  bool IsEnabled() const { return retro_atomic_load_relaxed_int(&m_enable) != 0; }
  void SetEnable(bool enable) { retro_atomic_store_relaxed_int(&m_enable, enable ? 1 : 0); }

private:
#ifdef _WIN32
  HANDLE m_file = INVALID_HANDLE_VALUE;
#else
  int m_file = -1;
#endif
  retro_atomic_int_t m_enable{};
};

void GenericLog(LogLevel level, LogType type, const char* file, int line, const char* format, ...)
{
  char message[1024];
  va_list ap;
  auto* instance = LogManager::GetInstance();
  if (instance == nullptr || !instance->IsEnabled(type, level))
    return;
  va_start(ap, format);
  vsnprintf(message, sizeof(message), format, ap);
  va_end(ap);
  instance->Log(level, type, file, line, message);
}

static size_t DeterminePathCutOffPoint()
{
  constexpr const char* pattern = "/source/core/";
#ifdef _WIN32
  constexpr const char* pattern2 = "\\source\\core\\";
#endif
  std::string path = __FILE__;
  Common::ToLower(&path);
  size_t pos = path.find(pattern);
#ifdef _WIN32
  if (pos == std::string::npos)
    pos = path.find(pattern2);
#endif
  if (pos != std::string::npos)
    return pos + strlen(pattern);
  return 0;
}

LogManager::LogManager()
{
  // create log containers
  m_log[LogType::ACHIEVEMENTS] = {"RetroAchievements", "Achievements"};
  m_log[LogType::ACTIONREPLAY] = {"ActionReplay", "Action Replay"};
  m_log[LogType::AUDIO] = {"Audio", "Audio Emulator"};
  m_log[LogType::AUDIO_INTERFACE] = {"AI", "Audio Interface"};
  m_log[LogType::BOOT] = {"BOOT", "Boot"};
  m_log[LogType::COMMANDPROCESSOR] = {"CP", "Command Processor"};
  m_log[LogType::COMMON] = {"COMMON", "Common"};
  m_log[LogType::CONSOLE] = {"CONSOLE", "Dolphin Console"};
  m_log[LogType::CONTROLLERINTERFACE] = {"CI", "Controller Interface"};
  m_log[LogType::CORE] = {"CORE", "Core"};
  m_log[LogType::DISCIO] = {"DIO", "Disc IO"};
  m_log[LogType::DSPHLE] = {"DSPHLE", "DSP HLE"};
  m_log[LogType::DSPLLE] = {"DSPLLE", "DSP LLE"};
  m_log[LogType::DSP_MAIL] = {"DSPMails", "DSP Mails"};
  m_log[LogType::DSPINTERFACE] = {"DSP", "DSP Interface"};
  m_log[LogType::DVDINTERFACE] = {"DVD", "DVD Interface"};
  m_log[LogType::AMMEDIABOARD] = {"AMMB", "AMMB Interface"};
  m_log[LogType::AMMEDIABOARD_NET] = {"AMMB_NET", "AMMB - Network"};
  m_log[LogType::DYNA_REC] = {"JIT", "JIT Dynamic Recompiler"};
  m_log[LogType::EXPANSIONINTERFACE] = {"EXI", "Expansion Interface"};
  m_log[LogType::FILEMON] = {"FileMon", "File Monitor"};
  m_log[LogType::FRAMEDUMP] = {"FRAMEDUMP", "FrameDump"};
  m_log[LogType::GPFIFO] = {"GP", "GatherPipe FIFO"};
  m_log[LogType::HOST_GPU] = {"Host GPU", "Host GPU"};
  m_log[LogType::HSP] = {"HSP", "High-Speed Port"};
  m_log[LogType::IOS] = {"IOS", "IOS"};
  m_log[LogType::IOS_DI] = {"IOS_DI", "IOS - Drive Interface"};
  m_log[LogType::IOS_ES] = {"IOS_ES", "IOS - ETicket Services"};
  m_log[LogType::IOS_FS] = {"IOS_FS", "IOS - Filesystem Services"};
  m_log[LogType::IOS_SD] = {"IOS_SD", "IOS - SDIO"};
  m_log[LogType::IOS_SSL] = {"IOS_SSL", "IOS - SSL"};
  m_log[LogType::IOS_STM] = {"IOS_STM", "IOS - State Transition Manager"};
  m_log[LogType::IOS_NET] = {"IOS_NET", "IOS - Network"};
  m_log[LogType::IOS_USB] = {"IOS_USB", "IOS - USB"};
  m_log[LogType::IOS_WC24] = {"IOS_WC24", "IOS - WiiConnect24"};
  m_log[LogType::IOS_WFS] = {"IOS_WFS", "IOS - WFS"};
  m_log[LogType::IOS_WIIMOTE] = {"IOS_WIIMOTE", "IOS - Wii Remote"};
  m_log[LogType::MASTER_LOG] = {"MASTER", "Master Log"};
  m_log[LogType::MEMCARD_MANAGER] = {"MemCard Manager", "Memory Card Manager"};
  m_log[LogType::MEMMAP] = {"MI", "Memory Interface & Memory Map"};
  m_log[LogType::NETPLAY] = {"NETPLAY", "Netplay"};
  m_log[LogType::OSHLE] = {"HLE", "OSHLE"};
  m_log[LogType::OSREPORT] = {"OSREPORT", "OSReport EXI"};
  m_log[LogType::OSREPORT_HLE] = {"OSREPORT_HLE", "OSReport HLE"};
  m_log[LogType::PIXELENGINE] = {"PE", "Pixel Engine"};
  m_log[LogType::PROCESSORINTERFACE] = {"PI", "Processor Interface"};
  m_log[LogType::POWERPC] = {"PowerPC", "PowerPC IBM CPU"};
  m_log[LogType::SERIALINTERFACE] = {"SI", "Serial Interface"};
  m_log[LogType::SERIALINTERFACE_AMBB] = {"SI_AMBB", "AMBB Interface"};
  m_log[LogType::SERIALINTERFACE_CARD] = {"SI_CARD", "CARD Interface"};
  m_log[LogType::SERIALINTERFACE_JVSIO] = {"SI_JVS", "JVS-I/O"};
  m_log[LogType::SP1] = {"SP1", "Serial Port 1"};
  m_log[LogType::SYMBOLS] = {"SYMBOLS", "Symbols"};
  m_log[LogType::VIDEO] = {"Video", "Video Backend"};
  m_log[LogType::VIDEOINTERFACE] = {"VI", "Video Interface"};
  m_log[LogType::WIIMOTE] = {"Wiimote", "Wii Remote"};
  m_log[LogType::WII_IPC] = {"WII_IPC", "WII IPC"};

  RegisterListener(LogListener::FILE_LISTENER,
                   std::make_unique<FileLogListener>(File::GetUserPath(F_MAINLOG_IDX)));
  RegisterListener(LogListener::CONSOLE_LISTENER, std::make_unique<ConsoleListener>());

  // Set up log listeners
  SetEffectiveLogLevel();
  EnableListener(LogListener::FILE_LISTENER, Config::Get(LOGGER_WRITE_TO_FILE));
  EnableListener(LogListener::CONSOLE_LISTENER, Config::Get(LOGGER_WRITE_TO_CONSOLE));
  EnableListener(LogListener::LOG_WINDOW_LISTENER, Config::Get(LOGGER_WRITE_TO_WINDOW));

  for (auto& container : m_log)
  {
    container.m_enable = Config::Get(
        Config::Info<bool>{{Config::System::Logger, "Logs", container.m_short_name}, false});
  }

  m_path_cutoff_point = DeterminePathCutOffPoint();

  m_config_changed_callback_id =
      Config::AddConfigChangedCallback([this]() { SetEffectiveLogLevel(); });
}

LogManager::~LogManager()
{
  Config::RemoveConfigChangedCallback(m_config_changed_callback_id);
}

void LogManager::SaveSettings()
{
  Config::ConfigChangeCallbackGuard config_guard;

  Config::SetBaseOrCurrent(LOGGER_WRITE_TO_FILE, IsListenerEnabled(LogListener::FILE_LISTENER));
  Config::SetBaseOrCurrent(LOGGER_WRITE_TO_CONSOLE,
                           IsListenerEnabled(LogListener::CONSOLE_LISTENER));
  Config::SetBaseOrCurrent(LOGGER_WRITE_TO_WINDOW,
                           IsListenerEnabled(LogListener::LOG_WINDOW_LISTENER));

  for (const auto& container : m_log)
  {
    const Config::Info<bool> info{{Config::System::Logger, "Logs", container.m_short_name}, false};
    Config::SetBaseOrCurrent(info, container.m_enable);
  }

  Config::Save();
}

void LogManager::Log(LogLevel level, LogType type, const char* file, int line, const char* message)
{
  if (!IsEnabled(type, level) || !static_cast<bool>(m_listener_ids))
    return;

  LogWithFullPath(level, type, file + m_path_cutoff_point, line, message);
}

void LogManager::LogWithFullPath(LogLevel level, LogType type, const char* file, int line,
                                 const char* message)
{
  /* minutes, seconds and milliseconds of the hour, UTC */
  const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
  char msg[1280];
  snprintf(msg, sizeof(msg), "%02d:%02d:%03d %s:%d %c[%s]: %s\n",
           static_cast<int>((ms / 60000) % 60), static_cast<int>((ms / 1000) % 60),
           static_cast<int>(ms % 1000), file, line, LOG_LEVEL_TO_CHAR[static_cast<int>(level)],
           GetShortName(type), message);

  for (const auto listener_id : m_listener_ids)
  {
    if (m_listeners[listener_id])
      m_listeners[listener_id]->Log(level, msg);
  }
}

LogLevel LogManager::GetEffectiveLogLevel() const
{
  return static_cast<LogLevel>(retro_atomic_load_relaxed_int(&m_effective_level));
}

void LogManager::SetConfigLogLevel(const LogLevel level)
{
  Config::SetBaseOrCurrent(LOGGER_VERBOSITY, level);
  SetEffectiveLogLevel();
}

void LogManager::SetEffectiveLogLevel()
{
  const LogLevel level = Config::Get(LOGGER_VERBOSITY);
  const LogLevel clamped_level = level < LogLevel::LNOTICE         ? LogLevel::LNOTICE :
                                 level > MAX_EFFECTIVE_LOGLEVEL ? MAX_EFFECTIVE_LOGLEVEL :
                                                                  level;
  retro_atomic_store_relaxed_int(&m_effective_level, static_cast<int>(clamped_level));
}

void LogManager::SetEnable(LogType type, bool enable)
{
  m_log[type].m_enable = enable;
}

bool LogManager::IsEnabled(LogType type, LogLevel level) const
{
  return m_log[type].m_enable && GetEffectiveLogLevel() >= level;
}

std::vector<LogManager::LogContainer> LogManager::GetLogTypes()
{
  std::vector<LogContainer> log_types;
  log_types.reserve(m_log.size());

  for (const auto& container : m_log)
    log_types.emplace_back(container);

  return log_types;
}

const char* LogManager::GetShortName(LogType type) const
{
  return m_log[type].m_short_name;
}

const char* LogManager::GetFullName(LogType type) const
{
  return m_log[type].m_full_name;
}

void LogManager::RegisterListener(LogListener::LISTENER id, std::unique_ptr<LogListener> listener)
{
  m_listeners[id] = std::move(listener);
}

void LogManager::EnableListener(LogListener::LISTENER id, bool enable)
{
  m_listener_ids[id] = enable;
}

bool LogManager::IsListenerEnabled(LogListener::LISTENER id) const
{
  return m_listener_ids[id];
}

// Singleton. Ugh.
static std::unique_ptr<LogManager> s_log_manager;

LogManager* LogManager::GetInstance()
{
  return s_log_manager.get();
}

void LogManager::Init()
{
  s_log_manager = std::unique_ptr<LogManager>(new LogManager());
}

void LogManager::Shutdown()
{
  if (s_log_manager)
    s_log_manager->SaveSettings();
  s_log_manager.reset();
}
}  // namespace Common::Log
