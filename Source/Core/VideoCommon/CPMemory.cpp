// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/CPMemory.h"

#include <cstring>
#include <type_traits>
#include <utility>

#include "Common/Logging/Log.h"
#include "Core/System.h"
#include "VideoCommon/CommandProcessor.h"

// CP state
CPState g_main_cp_state;
CPState g_preprocess_cp_state;

void CopyPreprocessCPStateFromMain()
{
  static_assert(std::is_trivially_copyable_v<CPState>);
  std::memcpy(static_cast<void*>(&g_preprocess_cp_state),
              static_cast<const void*>(&g_main_cp_state), sizeof(CPState));
}

CPState::CPState(const u32* memory) : CPState()
{
  matrix_index_a.Hex = memory[MATINDEX_A];
  matrix_index_b.Hex = memory[MATINDEX_B];
  vtx_desc.low.Hex = memory[VCD_LO];
  vtx_desc.high.Hex = memory[VCD_HI];

  for (u32 i = 0; i < CP_NUM_VAT_REG; i++)
  {
    vtx_attr[i].g0.Hex = memory[CP_VAT_REG_A + i];
    vtx_attr[i].g1.Hex = memory[CP_VAT_REG_B + i];
    vtx_attr[i].g2.Hex = memory[CP_VAT_REG_C + i];
  }

  for (u32 i = 0; i < CP_NUM_ARRAYS; i++)
  {
    array_bases[static_cast<CPArray>(i)] = memory[ARRAY_BASE + i];
    array_strides[static_cast<CPArray>(i)] = memory[ARRAY_STRIDE + i];
  }
}

void CPState::LoadCPReg(u8 sub_cmd, u32 value)
{
  switch (sub_cmd & CP_COMMAND_MASK)
  {
  case UNKNOWN_00:
  case UNKNOWN_10:
  case UNKNOWN_20:
    if (!(sub_cmd == UNKNOWN_20 && value == 0))
    {
      // All titles using libogc or the official SDK issue 0x20 with value=0 on startup
      DEBUG_LOG(VIDEO, "Unknown CP command possibly relating to perf queries used: %02x",
                (unsigned)sub_cmd);
    }
    break;

  case MATINDEX_A:
    if (sub_cmd != MATINDEX_A)
    {
      WARN_LOG(VIDEO,
               "CP MATINDEX_A: an exact value of %02x was expected "
               "but instead a value of %02x was seen",
               std::to_underlying(MATINDEX_A), (unsigned)sub_cmd);
    }

    matrix_index_a.Hex = value;
    break;

  case MATINDEX_B:
    if (sub_cmd != MATINDEX_B)
    {
      WARN_LOG(VIDEO,
               "CP MATINDEX_B: an exact value of %02x was expected "
               "but instead a value of %02x was seen",
               std::to_underlying(MATINDEX_B), (unsigned)sub_cmd);
    }

    matrix_index_b.Hex = value;
    break;

  case VCD_LO:
    if (sub_cmd != VCD_LO)  // Stricter than YAGCD
    {
      WARN_LOG(VIDEO,
               "CP VCD_LO: an exact value of %02x was expected "
               "but instead a value of %02x was seen",
               std::to_underlying(VCD_LO), (unsigned)sub_cmd);
    }

    vtx_desc.low.Hex = value;
    break;

  case VCD_HI:
    if (sub_cmd != VCD_HI)  // Stricter than YAGCD
    {
      WARN_LOG(VIDEO,
               "CP VCD_HI: an exact value of %02x was expected "
               "but instead a value of %02x was seen",
               std::to_underlying(VCD_HI), (unsigned)sub_cmd);
    }

    vtx_desc.high.Hex = value;
    break;

  case CP_VAT_REG_A:
    if ((sub_cmd - CP_VAT_REG_A) >= CP_NUM_VAT_REG)
    {
      WARN_LOG(VIDEO, "CP_VAT_REG_A: Invalid VAT %d", sub_cmd - CP_VAT_REG_A);
    }
    vtx_attr[sub_cmd & CP_VAT_MASK].g0.Hex = value;
    break;

  case CP_VAT_REG_B:
    if ((sub_cmd - CP_VAT_REG_B) >= CP_NUM_VAT_REG)
    {
      WARN_LOG(VIDEO, "CP_VAT_REG_B: Invalid VAT %d", sub_cmd - CP_VAT_REG_B);
    }
    vtx_attr[sub_cmd & CP_VAT_MASK].g1.Hex = value;
    break;

  case CP_VAT_REG_C:
    if ((sub_cmd - CP_VAT_REG_C) >= CP_NUM_VAT_REG)
    {
      WARN_LOG(VIDEO, "CP_VAT_REG_C: Invalid VAT %d", sub_cmd - CP_VAT_REG_C);
    }
    vtx_attr[sub_cmd & CP_VAT_MASK].g2.Hex = value;
    break;

  // Pointers to vertex arrays in GC RAM
  case ARRAY_BASE:
    array_bases[static_cast<CPArray>(sub_cmd & CP_ARRAY_MASK)] =
        value & CommandProcessor::GetPhysicalAddressMask(Core::System::GetInstance().IsWii());
    break;

  case ARRAY_STRIDE:
    array_strides[static_cast<CPArray>(sub_cmd & CP_ARRAY_MASK)] = value & 0xFF;
    break;

  default:
    WARN_LOG(VIDEO, "Unknown CP register %02x set to %08x", (unsigned)sub_cmd, value);
  }
}

void CPState::FillCPMemoryArray(u32* memory) const
{
  memory[MATINDEX_A] = matrix_index_a.Hex;
  memory[MATINDEX_B] = matrix_index_b.Hex;
  memory[VCD_LO] = vtx_desc.low.Hex;
  memory[VCD_HI] = vtx_desc.high.Hex;

  for (int i = 0; i < CP_NUM_VAT_REG; ++i)
  {
    memory[CP_VAT_REG_A + i] = vtx_attr[i].g0.Hex;
    memory[CP_VAT_REG_B + i] = vtx_attr[i].g1.Hex;
    memory[CP_VAT_REG_C + i] = vtx_attr[i].g2.Hex;
  }

  for (int i = 0; i < CP_NUM_ARRAYS; ++i)
  {
    memory[ARRAY_BASE + i] = array_bases[static_cast<CPArray>(i)];
    memory[ARRAY_STRIDE + i] = array_strides[static_cast<CPArray>(i)];
  }
}
