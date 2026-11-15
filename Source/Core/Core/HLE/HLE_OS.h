// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string>

#include "Common/CommonTypes.h"

namespace Core
{
class CPUThreadGuard;
}
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
  HLEPrintArgs(const Core::CPUThreadGuard& guard, HLE::SystemVABI::VAList* list)
      : m_guard(guard), m_va_list(list)
  {
  }

  u32 GetU32();
  u64 GetU64();
  double GetF64();
  std::string GetString(std::optional<u32> max_length);
  std::u16string GetU16String(std::optional<u32> max_length);

private:
  const Core::CPUThreadGuard& m_guard;
  HLE::SystemVABI::VAList* m_va_list;
};

std::string GetStringVA(HLEPrintArgs* args, std::string_view string);

void HLE_GeneralDebugPrint(const Core::CPUThreadGuard& guard);
void HLE_GeneralDebugVPrint(const Core::CPUThreadGuard& guard);
void HLE_write_console(const Core::CPUThreadGuard& guard);
void HLE_OSPanic(const Core::CPUThreadGuard& guard);
void HLE_LogDPrint(const Core::CPUThreadGuard& guard);
void HLE_LogVDPrint(const Core::CPUThreadGuard& guard);
void HLE_LogFPrint(const Core::CPUThreadGuard& guard);
void HLE_LogVFPrint(const Core::CPUThreadGuard& guard);
}  // namespace HLE_OS
