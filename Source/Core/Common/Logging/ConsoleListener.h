// Copyright 2010 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/Logging/Log.h"

/* The host's console, debugger output or system log: the listener LogManager starts with */
void ConsoleListenerInit();
void ConsoleListenerShutdown();
void ConsoleListenerLog(void* context, Common::Log::LogLevel level, const char* text);
