// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/MsgHandler.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

#include "Common/Logging/Log.h"
#ifdef _WIN32
#include "Common/StringUtil.h"
#endif

namespace Common
{
namespace
{
// Default non library dependent panic alert
bool DefaultMsgHandler(const char* caption, const char* text, bool yes_no, MsgType style)
{
#ifdef _WIN32
  int window_style = MB_ICONINFORMATION;
  if (style == MsgType::Question)
    window_style = MB_ICONQUESTION;
  if (style == MsgType::Warning)
    window_style = MB_ICONWARNING;

  return IDYES == MessageBox(0, UTF8ToTStr(text).c_str(), UTF8ToTStr(caption).c_str(),
                             window_style | (yes_no ? MB_YESNO : MB_OK));
#else
  fprintf(stderr, "%s\n", text);

  // Return no to any question (which will in general crash the emulator)
  return false;
#endif
}

MsgAlertHandler s_msg_handler = DefaultMsgHandler;
bool s_alert_enabled = true;
bool s_abort_on_panic_alert = false;

const char* GetCaption(MsgType style)
{
  static const std::string info_caption = GetStringT("Information");
  static const std::string ques_caption = GetStringT("Question");
  static const std::string warn_caption = GetStringT("Warning");
  static const std::string crit_caption = GetStringT("Critical");

  switch (style)
  {
  case MsgType::Information:
    return info_caption.c_str();
  case MsgType::Question:
    return ques_caption.c_str();
  case MsgType::Warning:
    return warn_caption.c_str();
  case MsgType::Critical:
    return crit_caption.c_str();
  default:
    return "Unhandled caption";
  }
}

Log::LogLevel GetMessageAlertLogLevel(const MsgType style)
{
  switch (style)
  {
  case MsgType::Critical:
    return Log::LogLevel::LERROR;
  case MsgType::Warning:
    return Log::LogLevel::LWARNING;
  default:
    return Log::LogLevel::LINFO;
  }
}
}  // Anonymous namespace

// Select the function that is used for message boxes.
void RegisterMsgAlertHandler(MsgAlertHandler handler)
{
  s_msg_handler = handler;
}

// enable/disable the alert handler
void SetEnableAlert(bool enable)
{
  s_alert_enabled = enable;
}

void SetAbortOnPanicAlert(bool should_abort)
{
  s_abort_on_panic_alert = should_abort;
}

std::string GetStringT(const char* string)
{
  return string;
}

static bool ShowMessageAlert(std::string_view text, bool yes_no, Common::Log::LogType log_type,
                             MsgType style, const char* file, int line)
{
  const char* caption = GetCaption(style);
  const Log::LogLevel level = GetMessageAlertLogLevel(style);
  // logged at the caller's file and line
  Common::Log::GenericLog(level, log_type, file, line, "%s: %.*s", caption,
                          static_cast<int>(text.size()), text.data());

  // Panic alerts.
  if (style == MsgType::Warning && s_abort_on_panic_alert)
  {
    std::abort();
  }

  // Don't ignore questions, especially AskYesNo, PanicYesNo could be ignored
  if (s_msg_handler != nullptr &&
      (s_alert_enabled || style == MsgType::Question || style == MsgType::Critical))
  {
    return s_msg_handler(caption, text.data(), yes_no, style);
  }

  return true;
}

bool MsgAlert(bool yes_no, MsgType style, Common::Log::LogType log_type, const char* file, int line,
              const char* format, ...)
{
  char message[1024];
  va_list ap;
  va_start(ap, format);
  vsnprintf(message, sizeof(message), format, ap);
  va_end(ap);
  return ShowMessageAlert(message, yes_no, log_type, style, file, line);
}
}  // namespace Common
