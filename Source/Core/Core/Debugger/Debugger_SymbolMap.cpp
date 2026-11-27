// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Debugger/Debugger_SymbolMap.h"

#include <string>

#include <cstdio>
#include <cstring>

#include "Common/CommonTypes.h"
#include "Common/StringUtil.h"

#include "Core/Core.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PPCSymbolDB.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

namespace Dolphin_Debugger
{
// Returns true if the address is not a valid RAM address or NULL.
static bool IsStackBottom(u32 addr)
{
  return !addr || !PowerPC::MMU::HostIsRAMAddress(addr);
}

// The name of the symbol holding addr, or " --- "
static const char* GetDescription(const PPCSymbolDB& ppc_symbol_db, u32 addr)
{
  const Common::Symbol* const symbol = ppc_symbol_db.GetSymbolFromAddr(addr);
  return symbol ? symbol->name.c_str() : " --- ";
}

void PrintCallstack(Common::Log::LogType type, Common::Log::LogLevel level)
{
  auto& power_pc = Core::System::GetInstance().GetPowerPC();
  const auto& ppc_state = power_pc.GetPPCState();
  auto& ppc_symbol_db = power_pc.GetSymbolDB();

  GENERIC_LOG(type, level, "== STACK TRACE - SP = %08x ==", ppc_state.gpr[1]);

  if (LR(ppc_state) == 0)
  {
    GENERIC_LOG(type, level, " LR = 0 - this is bad");
  }

  if (const char* const lr_desc = GetDescription(ppc_symbol_db, LR(ppc_state));
      std::strcmp(lr_desc, GetDescription(ppc_symbol_db, ppc_state.pc)) != 0)
  {
    GENERIC_LOG(type, level, " * %s  [ LR = %08x ]", lr_desc, LR(ppc_state));
  }

  if (IsStackBottom(ppc_state.gpr[1]))
    return;

  u32 addr = PowerPC::MMU::HostRead<u32>(ppc_state.gpr[1]);  // SP

  // Walk the stack chain
  for (int count = 0; !IsStackBottom(addr + 4) && (count < 20); ++count)
  {
    const u32 func_addr = PowerPC::MMU::HostRead<u32>(addr + 4);
    const char* func_desc = GetDescription(ppc_symbol_db, func_addr);
    if (!*func_desc || !std::strcmp(func_desc, "Invalid"))
      func_desc = "(unknown)";
    GENERIC_LOG(type, level, " * %s [ addr = %08x ]", func_desc, func_addr);

    if (IsStackBottom(addr))
      break;

    addr = PowerPC::MMU::HostRead<u32>(addr);
  }
}

void PrintDataBuffer(const Core::System& system, Common::Log::LogType type, u32 address, u32 size,
                     std::string_view title)
{
  const u8* data = system.GetMemory().GetPointerForRange(address, size);

  GENERIC_LOG(type, Common::Log::LogLevel::LDEBUG, "%.*s", static_cast<int>(title.size()),
              title.data());
  for (u32 j = 0; j < size;)
  {
    std::string hex_line;
    for (int i = 0; i < 16; i++)
    {
      hex_line += StringFromFormat("%02x ", (unsigned)data[j++]);

      if (j >= size)
        break;
    }
    GENERIC_LOG(type, Common::Log::LogLevel::LDEBUG, "   Data: %s", hex_line.c_str());
  }
}

}  // namespace Dolphin_Debugger
