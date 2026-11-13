// Copyright 2015 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/Logging/ConsoleListener.h"

#include <windows.h>

void ConsoleListenerInit()
{
}

void ConsoleListenerShutdown()
{
}

void ConsoleListenerLog(void*, Common::Log::LogLevel, const char* text)
{
  /* a message is at most what LogManager formats it into */
  wchar_t wide[1280];
  if (MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, sizeof(wide) / sizeof(wide[0])) > 0)
    ::OutputDebugStringW(wide);
}
