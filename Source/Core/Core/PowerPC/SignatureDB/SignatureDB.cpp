// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/SignatureDB/SignatureDB.h"

#include "Common/CommonTypes.h"
#include "Core/PowerPC/MMU.h"

namespace SignatureDB
{
u32 ComputeCodeChecksum(const Core::CPUThreadGuard& guard, u32 offsetStart, u32 offsetEnd)
{
  u32 sum = 0;
  for (u32 offset = offsetStart; offset <= offsetEnd; offset += 4)
  {
    u32 opcode = PowerPC::MMU::HostRead_Instruction(guard, offset);
    u32 op = opcode & 0xFC000000;
    u32 op2 = 0;
    u32 op3 = 0;
    u32 auxop = op >> 26;
    switch (auxop)
    {
    case 4:  // PS instructions
      op2 = opcode & 0x0000003F;
      switch (op2)
      {
      case 0:
      case 8:
      case 16:
      case 21:
      case 22:
        op3 = opcode & 0x000007C0;
      }
      break;

    case 7:  // addi muli etc
    case 8:
    case 10:
    case 11:
    case 12:
    case 13:
    case 14:
    case 15:
      op2 = opcode & 0x03FF0000;
      break;

    case 19:  // MCRF??
    case 31:  // integer
    case 63:  // fpu
      op2 = opcode & 0x000007FF;
      break;
    case 59:  // fpu
      op2 = opcode & 0x0000003F;
      if (op2 < 16)
        op3 = opcode & 0x000007C0;
      break;
    default:
      if (auxop >= 32 && auxop < 56)
        op2 = opcode & 0x03FF0000;
      break;
    }
    // Checksum only uses opcode, not opcode data, because opcode data changes
    // in all compilations, but opcodes don't!
    sum = (((sum << 17) & 0xFFFE0000) | ((sum >> 15) & 0x0001FFFF));
    sum = sum ^ (op | op2 | op3);
  }
  return sum;
}
}  // namespace SignatureDB
