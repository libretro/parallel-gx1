// Copyright 2009 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

#include "Common/Logging/Log.h"

namespace Common
{
// Message alerts
enum class MsgType
{
  Information,
  Question,
  Warning,
  Critical
};

using MsgAlertHandler = bool (*)(const char* caption, const char* text, bool yes_no, MsgType style);

void RegisterMsgAlertHandler(MsgAlertHandler handler);

[[nodiscard]] std::string GetStringT(const char* string);

/* An alert in printf's terms, formatted into a buffer on the stack (a
 * longer one is cut). */
bool MsgAlert(bool yes_no, MsgType style, Common::Log::LogType log_type, const char* file, int line,
              const char* format, ...) DOLPHIN_PRINTF_ATTR(6, 7);

void SetEnableAlert(bool enable);
void SetAbortOnPanicAlert(bool should_abort);

}  // namespace Common

#define GenericAlert(yes_no, style, log_type, ...)                                                 \
  Common::MsgAlert(yes_no, style, Common::Log::LogType::log_type, __FILE__, __LINE__, __VA_ARGS__)
#define SuccessAlert(...) GenericAlert(false, Common::MsgType::Information, MASTER_LOG, __VA_ARGS__)
#define PanicAlert(...) GenericAlert(false, Common::MsgType::Warning, MASTER_LOG, __VA_ARGS__)
#define PanicYesNo(...) GenericAlert(true, Common::MsgType::Warning, MASTER_LOG, __VA_ARGS__)
#define AskYesNo(...) GenericAlert(true, Common::MsgType::Question, MASTER_LOG, __VA_ARGS__)
#define CriticalAlert(...) GenericAlert(false, Common::MsgType::Critical, MASTER_LOG, __VA_ARGS__)
#define PanicYesNoAssert(log_type, ...)                                                            \
  GenericAlert(true, Common::MsgType::Warning, log_type, __VA_ARGS__)
