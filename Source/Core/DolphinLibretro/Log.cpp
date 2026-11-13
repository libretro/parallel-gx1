
#include <stdio.h>
#include <string.h>

#include "Common/Logging/Log.h"
#include "Common/Logging/LogManager.h"
#include "DolphinLibretro/Log.h"
#include "DolphinLibretro/Common/Options.h"
#if defined(ANDROID)
#include <android/log.h>
#endif

namespace Libretro
{
extern retro_environment_t environ_cb;
unsigned msg_interface_version = 0;
namespace Log
{
/* The frontend's log, where the core's messages go when it has one */
static retro_log_printf_t s_frontend_log;

static void LogToFrontend(void*, Common::Log::LogLevel level, const char* text);
static void ApplyLogOptions();

/* With a log from the frontend, messages go there instead of the console, as the core's options
 * filter them; it stays until the log manager goes with the game. */
void Init()
{
  struct retro_log_callback log = {};
  Common::Log::LogManager* const mgr = Common::Log::LogManager::GetInstance();
  if (!environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log) || !log.log)
    return;

  s_frontend_log = log.log;
  ApplyLogOptions();
  mgr->RegisterListener(Common::Log::LogListener::CUSTOM_LISTENER,
                        Common::Log::LogListener{LogToFrontend, nullptr});
  mgr->EnableListener(Common::Log::LogListener::CUSTOM_LISTENER, true);
  mgr->EnableListener(Common::Log::LogListener::LISTENER::CONSOLE_LISTENER, false);
}

static void ApplyLogOptions()
{
  Common::Log::LogManager::GetInstance()->SetConfigLogLevel(
    static_cast<Common::Log::LogLevel>(
        Libretro::Options::GetCached<int>(
            Libretro::Options::main_interface::LOG_LEVEL, static_cast<int>(Common::Log::LogLevel::LINFO))));

  Common::Log::LogManager::GetInstance()->SetEnable(
    Common::Log::LogType::BOOT,
    Libretro::Options::GetCached<bool>(Libretro::Options::main_interface::LOG_BOOT, true));

  Common::Log::LogManager::GetInstance()->SetEnable(
    Common::Log::LogType::CORE,
    Libretro::Options::GetCached<bool>(Libretro::Options::main_interface::LOG_CORE, true));

  Common::Log::LogManager::GetInstance()->SetEnable(
    Common::Log::LogType::VIDEO,
    Libretro::Options::GetCached<bool>(Libretro::Options::main_interface::LOG_VIDEO, true));

  Common::Log::LogManager::GetInstance()->SetEnable(
    Common::Log::LogType::COMMON,
    Libretro::Options::GetCached<bool>(Libretro::Options::main_interface::LOG_COMMON, true));

  Common::Log::LogManager::GetInstance()->SetEnable(
    Common::Log::LogType::HOST_GPU,
    Libretro::Options::GetCached<bool>(Libretro::Options::main_interface::LOG_HOST_GPU, false));

  Common::Log::LogManager::GetInstance()->SetEnable(
    Common::Log::LogType::MEMMAP,
    Libretro::Options::GetCached<bool>(Libretro::Options::main_interface::LOG_MEMMAP, false));

  Common::Log::LogManager::GetInstance()->SetEnable(
    Common::Log::LogType::DSPINTERFACE,
    Libretro::Options::GetCached<bool>(Libretro::Options::main_interface::LOG_DSPINTERFACE, false));

  Common::Log::LogManager::GetInstance()->SetEnable(
    Common::Log::LogType::DSPHLE,
    Libretro::Options::GetCached<bool>(Libretro::Options::main_interface::LOG_DSPHLE, false));

  Common::Log::LogManager::GetInstance()->SetEnable(
    Common::Log::LogType::DSPLLE,
    Libretro::Options::GetCached<bool>(Libretro::Options::main_interface::LOG_DSPLLE, false));

  Common::Log::LogManager::GetInstance()->SetEnable(
    Common::Log::LogType::DSP_MAIL,
    Libretro::Options::GetCached<bool>(Libretro::Options::main_interface::LOG_DSP_MAIL, false));
}

static void LogToFrontend(void*, Common::Log::LogLevel level, const char* text)
{
  switch (level)
  {
  case Common::Log::LogLevel::LDEBUG:
    s_frontend_log(RETRO_LOG_DEBUG, text);
    break;
  case Common::Log::LogLevel::LWARNING:
    s_frontend_log(RETRO_LOG_WARN, text);
    break;
  case Common::Log::LogLevel::LERROR:
    s_frontend_log(RETRO_LOG_ERROR, text);
    break;
  case Common::Log::LogLevel::LNOTICE:
  case Common::Log::LogLevel::LINFO:
  default:
    s_frontend_log(RETRO_LOG_INFO, text);
    break;
  }
#if defined(ANDROID) && defined(_DEBUG)
  __android_log_print(ANDROID_LOG_INFO, "DolphinEmuLibretro", "%s", text);
#endif
}

retro_log_level GetRetroLogLevel(Common::Log::LogLevel level)
{
  switch(level)
  {
    case Common::Log::LogLevel::LNOTICE:
      return retro_log_level::RETRO_LOG_INFO;
    case Common::Log::LogLevel::LERROR:
      return retro_log_level::RETRO_LOG_ERROR;
    case Common::Log::LogLevel::LWARNING:
      return retro_log_level::RETRO_LOG_WARN;
    case Common::Log::LogLevel::LINFO:
      return retro_log_level::RETRO_LOG_INFO;
    case Common::Log::LogLevel::LDEBUG:
      return retro_log_level::RETRO_LOG_DEBUG;
  }

  return retro_log_level::RETRO_LOG_INFO;
}

retro_log_level GetRetroLogLevelForMsgType(Common::MsgType level)
{
  switch(level)
  {
    case Common::MsgType::Information:
      return retro_log_level::RETRO_LOG_INFO;
    case Common::MsgType::Question:
      return retro_log_level::RETRO_LOG_INFO;
    case Common::MsgType::Warning:
      return retro_log_level::RETRO_LOG_WARN;
    case Common::MsgType::Critical:
      return retro_log_level::RETRO_LOG_ERROR;
  }

  return retro_log_level::RETRO_LOG_INFO;
}

void DoLogFrontEnd(retro_log_level level, const char* text, unsigned int duration)
{
  if (msg_interface_version >= 1)
  {
    struct retro_message_ext message = {
        text,
        duration,
        3, // priority
        level,
        RETRO_MESSAGE_TARGET_ALL,
        RETRO_MESSAGE_TYPE_NOTIFICATION,
        -1 // progress
    };

    environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE_EXT, &message);
  }
  else
  {
    struct retro_message message = {
      text,
      180 // frames
    };

    environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &message);
  }
}

void LogFrontEnd(Common::Log::LogLevel level, const char* text, unsigned int duration)
{
  DoLogFrontEnd(GetRetroLogLevel(level), text, duration);
}

void LogFrontEnd(Common::MsgType style, const char* caption, const char* text, unsigned int duration)
{
  char message[1024];

  if (text && !strcmp(text, "Failed to create shared context for shader compiling."))
    return; // ignore

  snprintf(message, sizeof(message), "%s - %s",
         caption ? caption : "",
         text ? text : "");

  DoLogFrontEnd(GetRetroLogLevelForMsgType(style), message, duration);
}

}  // namespace Log
}  // namespace Libretro
