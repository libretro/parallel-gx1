// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string>

#include "Common/CommonTypes.h"

namespace HLE::SystemVABI
{
class VAList;
}

namespace HLE_OS
{
// The arguments of a guest printf, read off its va_list
class HLEPrintArgs final
{
public:
  HLEPrintArgs(HLE::SystemVABI::VAList* list) : m_va_list(list) {}

  u32 GetU32();
  u64 GetU64();
  double GetF64();
  std::string GetString(std::optional<u32> max_length);
  std::u16string GetU16String(std::optional<u32> max_length);

private:
  HLE::SystemVABI::VAList* m_va_list;
};

std::string GetStringVA(HLEPrintArgs* args, std::string_view string);

void HLE_GeneralDebugPrint();
void HLE_GeneralDebugVPrint();
void HLE_write_console();
void HLE_OSPanic();
void HLE_LogDPrint();
void HLE_LogVDPrint();
void HLE_LogFPrint();
void HLE_LogVFPrint();
}  // namespace HLE_OS
