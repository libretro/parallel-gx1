// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HLE/HLE_VarArgs.h"

#include "Common/Logging/Log.h"

#include "Core/Core.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

HLE::SystemVABI::VAListStruct::VAListStruct(const Core::CPUThreadGuard& guard, u32 address)
    : VAList(guard, 0)
{
  // svr4_va_list: u8 gpr, u8 fpr, u32 overflow_arg_area, u32 reg_save_area
  m_from_struct = true;
  m_address = address;
  m_has_fpr_area = guard.GetSystem().GetPPCState().cr.GetBit(6) == 1;
  m_gpr += PowerPC::MMU::HostRead<u8>(guard, address);
  m_fpr += PowerPC::MMU::HostRead<u8>(guard, address + 1);
  m_stack = PowerPC::MMU::HostRead<u32>(guard, address + 4);
  m_reg_save_area = PowerPC::MMU::HostRead<u32>(guard, address + 8);
}

u32 HLE::SystemVABI::VAList::GetGPR(u32 gpr) const
{
  if (!m_from_struct)
    return m_guard.GetSystem().GetPPCState().gpr[gpr];
  if (gpr < 3 || gpr > 10)
  {
    ERROR_LOG(OSHLE, "VAListStruct at %08x doesn't have GPR%u!", m_address, gpr);
    return 0;
  }
  const u32 gpr_address = Common::AlignUp(m_reg_save_area + 4 * (gpr - 3), 4);
  return PowerPC::MMU::HostRead<u32>(m_guard, gpr_address);
}

double HLE::SystemVABI::VAList::GetFPR(u32 fpr) const
{
  if (!m_from_struct)
    return m_guard.GetSystem().GetPPCState().ps[fpr].PS0AsDouble();
  if (!m_has_fpr_area || fpr < 1 || fpr > 8)
  {
    ERROR_LOG(OSHLE, "VAListStruct at %08x doesn't have FPR%u!", m_address, fpr);
    return 0.0;
  }
  // the FPRs follow the eight saved GPRs
  const u32 fpr_address = Common::AlignUp(m_reg_save_area + 4 * 8 + 8 * (fpr - 1), 8);
  return PowerPC::MMU::HostRead<double>(m_guard, fpr_address);
}
