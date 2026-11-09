// Copyright 2014 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/CPUDetect.h"
#include "Common/Hash.h"

CPUInfo cpu_info;

extern "C" int common_cpu_has_crc32(void)
{
  return cpu_info.bCRC32;
}

CPUInfo::CPUInfo()
{
}

std::string CPUInfo::Summarize()
{
  return "Generic";
}
