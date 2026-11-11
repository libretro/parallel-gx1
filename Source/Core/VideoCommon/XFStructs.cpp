// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later


#include <bit>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/Swap.h"

#include "Core/HW/Memmap.h"
#include "Core/System.h"

#include "VideoCommon/CPMemory.h"
#include "VideoCommon/Fifo.h"
#include "VideoCommon/GeometryShaderManager.h"
#include "VideoCommon/PixelShaderManager.h"
#include "VideoCommon/VertexLoaderManager.h"
#include "VideoCommon/VertexManagerBase.h"
#include "VideoCommon/XFMemory.h"
#include "VideoCommon/XFStateManager.h"

static void XFMemWritten(XFStateManager& xf_state_manager, u32 transferSize, u32 baseAddress)
{
  g_vertex_manager->Flush();
  xf_state_manager.InvalidateXFRange(baseAddress, baseAddress + transferSize);
}

static void XFRegWritten(Core::System& system, XFStateManager& xf_state_manager, u32 address,
                         u32 value)
{
  if (address >= XFMEM_REGISTERS_START && address < XFMEM_REGISTERS_END)
  {
    switch (address)
    {
    case XFMEM_ERROR:
    case XFMEM_DIAG:
    case XFMEM_STATE0:  // internal state 0
    case XFMEM_STATE1:  // internal state 1
    case XFMEM_CLOCK:
    case XFMEM_SETGPMETRIC:
      // Not implemented
      break;

    case XFMEM_CLIPDISABLE:
      // Clipping hints: nothing to do with them
      break;

    case XFMEM_VTXSPECS:  //__GXXfVtxSpecs, wrote 0004
      VertexLoaderManager::g_needs_cp_xf_consistency_check = true;
      break;

    case XFMEM_SETNUMCHAN:
      if (xfmem.numChan.numColorChans != (value & 3))
        g_vertex_manager->Flush();
      xf_state_manager.SetLightingConfigChanged();
      break;

    case XFMEM_SETCHAN0_AMBCOLOR:  // Channel Ambient Color
    case XFMEM_SETCHAN1_AMBCOLOR:
    {
      u8 chan = address - XFMEM_SETCHAN0_AMBCOLOR;
      if (xfmem.ambColor[chan] != value)
      {
        g_vertex_manager->Flush();
        xf_state_manager.SetMaterialColorChanged(chan);
      }
      break;
    }

    case XFMEM_SETCHAN0_MATCOLOR:  // Channel Material Color
    case XFMEM_SETCHAN1_MATCOLOR:
    {
      u8 chan = address - XFMEM_SETCHAN0_MATCOLOR;
      if (xfmem.matColor[chan] != value)
      {
        g_vertex_manager->Flush();
        xf_state_manager.SetMaterialColorChanged(chan + 2);
      }
      break;
    }

    case XFMEM_SETCHAN0_COLOR:  // Channel Color
    case XFMEM_SETCHAN1_COLOR:
    case XFMEM_SETCHAN0_ALPHA:  // Channel Alpha
    case XFMEM_SETCHAN1_ALPHA:
      if (((u32*)&xfmem)[address] != (value & 0x7fff))
        g_vertex_manager->Flush();
      xf_state_manager.SetLightingConfigChanged();
      break;

    case XFMEM_DUALTEX:
      if (xfmem.dualTexTrans.enabled != bool(value & 1))
        g_vertex_manager->Flush();
      xf_state_manager.SetTexMatrixInfoChanged(-1);
      break;

    case XFMEM_SETMATRIXINDA:
      xf_state_manager.SetTexMatrixChangedA(value);
      VertexLoaderManager::g_needs_cp_xf_consistency_check = true;
      break;
    case XFMEM_SETMATRIXINDB:
      xf_state_manager.SetTexMatrixChangedB(value);
      VertexLoaderManager::g_needs_cp_xf_consistency_check = true;
      break;

    case XFMEM_SETVIEWPORT:
    case XFMEM_SETVIEWPORT + 1:
    case XFMEM_SETVIEWPORT + 2:
    case XFMEM_SETVIEWPORT + 3:
    case XFMEM_SETVIEWPORT + 4:
    case XFMEM_SETVIEWPORT + 5:
      g_vertex_manager->Flush();
      xf_state_manager.SetViewportChanged();
      system.GetPixelShaderManager().SetViewportChanged();
      system.GetGeometryShaderManager().SetViewportChanged();
      break;

    case XFMEM_SETPROJECTION:
    case XFMEM_SETPROJECTION + 1:
    case XFMEM_SETPROJECTION + 2:
    case XFMEM_SETPROJECTION + 3:
    case XFMEM_SETPROJECTION + 4:
    case XFMEM_SETPROJECTION + 5:
    case XFMEM_SETPROJECTION + 6:
      g_vertex_manager->Flush();
      xf_state_manager.SetProjectionChanged();
      system.GetGeometryShaderManager().SetProjectionChanged();
      break;

    case XFMEM_SETNUMTEXGENS:  // GXSetNumTexGens
      if (xfmem.numTexGen.numTexGens != (value & 15))
        g_vertex_manager->Flush();
      break;

    case XFMEM_SETTEXMTXINFO:
    case XFMEM_SETTEXMTXINFO + 1:
    case XFMEM_SETTEXMTXINFO + 2:
    case XFMEM_SETTEXMTXINFO + 3:
    case XFMEM_SETTEXMTXINFO + 4:
    case XFMEM_SETTEXMTXINFO + 5:
    case XFMEM_SETTEXMTXINFO + 6:
    case XFMEM_SETTEXMTXINFO + 7:
      g_vertex_manager->Flush();
      xf_state_manager.SetTexMatrixInfoChanged(address - XFMEM_SETTEXMTXINFO);
      break;

    case XFMEM_SETPOSTMTXINFO:
    case XFMEM_SETPOSTMTXINFO + 1:
    case XFMEM_SETPOSTMTXINFO + 2:
    case XFMEM_SETPOSTMTXINFO + 3:
    case XFMEM_SETPOSTMTXINFO + 4:
    case XFMEM_SETPOSTMTXINFO + 5:
    case XFMEM_SETPOSTMTXINFO + 6:
    case XFMEM_SETPOSTMTXINFO + 7:
      g_vertex_manager->Flush();
      xf_state_manager.SetTexMatrixInfoChanged(address - XFMEM_SETPOSTMTXINFO);
      break;

    // --------------
    // Unknown Regs
    // --------------

    // Maybe these are for Normals?
    case 0x1048:  // xfmem.texcoords[0].nrmmtxinfo.hex = data; break; ??
    case 0x1049:
    case 0x104a:
    case 0x104b:
    case 0x104c:
    case 0x104d:
    case 0x104e:
    case 0x104f:
      DEBUG_LOG(VIDEO, "Possible Normal Mtx XF reg?: %x=%x", address, value);
      break;

    case 0x1013:
    case 0x1014:
    case 0x1015:
    case 0x1016:
    case 0x1017:

    default:
      WARN_LOG(VIDEO, "Unknown XF Reg: %x=%x", address, value);
      break;
    }
  }
}

void LoadXFReg(u16 base_address, u8 transfer_size, const u8* data)
{
  if (base_address > XFMEM_REGISTERS_END)
  {
    WARN_LOG(VIDEO, "XF load base address past end of address space: %x %u bytes",
             (unsigned)base_address, (unsigned)transfer_size);
    return;
  }

  u32 end_address = base_address + transfer_size;  // exclusive

  // do not allow writes past registers
  if (end_address > XFMEM_REGISTERS_END)
  {
    WARN_LOG(VIDEO, "XF load ends past end of address space: %x %u bytes", (unsigned)base_address,
             (unsigned)transfer_size);
    end_address = XFMEM_REGISTERS_END;
  }

  auto& system = Core::System::GetInstance();
  auto& xf_state_manager = system.GetXFStateManager();

  // write to XF mem
  if (base_address < XFMEM_REGISTERS_START)
  {
    const u32 xf_mem_base = base_address;
    u32 xf_mem_transfer_size = transfer_size;

    if (end_address > XFMEM_REGISTERS_START)
    {
      xf_mem_transfer_size = XFMEM_REGISTERS_START - base_address;
      base_address = XFMEM_REGISTERS_START;
    }

    XFMemWritten(xf_state_manager, xf_mem_transfer_size, xf_mem_base);
    for (u32 i = 0; i < xf_mem_transfer_size; i++)
    {
      ((u32*)&xfmem)[xf_mem_base + i] = Common::swap32(data);
      data += 4;
    }
  }

  // write to XF regs
  if (base_address >= XFMEM_REGISTERS_START)
  {
    for (u32 address = base_address; address < end_address; address++)
    {
      const u32 value = Common::swap32(data);

      XFRegWritten(system, xf_state_manager, address, value);
      ((u32*)&xfmem)[address] = value;

      data += 4;
    }
  }
}

// TODO - verify that it is correct. Seems to work, though.
void LoadIndexedXF(CPArray array, u32 index, u16 address, u8 size)
{
  // load stuff from array to address in xf mem

  const u32 buf_size = size * sizeof(u32);
  u32* currData = reinterpret_cast<u32*>(&xfmem) + address;
  u32* newData;
  auto& system = Core::System::GetInstance();
  auto& fifo = system.GetFifo();
  if (fifo.UseDeterministicGPUThread())
  {
    newData = static_cast<u32*>(fifo.PopFifoAuxBuffer(buf_size));
  }
  else
  {
    auto& memory = system.GetMemory();
    newData = reinterpret_cast<u32*>(memory.GetPointerForRange(
        g_main_cp_state.array_bases[array] + g_main_cp_state.array_strides[array] * index,
        buf_size));
  }

  auto& xf_state_manager = system.GetXFStateManager();
  bool changed = false;
  for (u32 i = 0; i < size; ++i)
  {
    if (currData[i] != Common::swap32(newData[i]))
    {
      changed = true;
      XFMemWritten(xf_state_manager, size, address);
      break;
    }
  }
  if (changed)
  {
    for (u32 i = 0; i < size; ++i)
      currData[i] = Common::swap32(newData[i]);
  }
}

void PreprocessIndexedXF(CPArray array, u32 index, u16 address, u8 size)
{
  const size_t buf_size = size * sizeof(u32);

  auto& system = Core::System::GetInstance();
  auto& memory = system.GetMemory();
  const u8* new_data = memory.GetPointerForRange(
      g_preprocess_cp_state.array_bases[array] + g_preprocess_cp_state.array_strides[array] * index,
      buf_size);

  system.GetFifo().PushFifoAuxBuffer(new_data, buf_size);
}
