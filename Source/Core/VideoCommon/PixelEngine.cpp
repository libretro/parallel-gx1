// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/PixelEngine.h"

#include "Common/BitField.h"
#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"

#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HW/MMIO.h"
#include "Core/HW/ProcessorInterface.h"
#include "Core/System.h"

#include "VideoCommon/BoundingBox.h"
#include "VideoCommon/Fifo.h"
#include "VideoCommon/PerfQueryBase.h"
#include "VideoCommon/VideoBackendBase.h"

namespace PixelEngine
{
enum
{
  INT_CAUSE_PE_TOKEN = 0x200,   // GP Token
  INT_CAUSE_PE_FINISH = 0x400,  // GP Finished
};

PixelEngineManager::PixelEngineManager(Core::System& system) : m_system{system}
{
}

void PixelEngineManager::DoState(PointerWrap& p)
{
  p.Do(m_z_conf);
  p.Do(m_alpha_conf);
  p.Do(m_dst_alpha_conf);
  p.Do(m_alpha_mode_conf);
  p.Do(m_alpha_read);
  p.Do(m_control);

  p.Do(m_token);
  // Saved as the separate fields it once was, so states stay compatible.
  const int pending = retro_atomic_load_acquire_int(&m_pending);
  u16 token_pending = static_cast<u16>(pending);
  bool token_interrupt_pending = (pending & PENDING_TOKEN_INT) != 0;
  bool finish_interrupt_pending = (pending & PENDING_FINISH_INT) != 0;
  bool event_raised = (pending & PENDING_EVENT_RAISED) != 0;
  p.Do(token_pending);
  p.Do(token_interrupt_pending);
  p.Do(finish_interrupt_pending);
  p.Do(event_raised);
  if (p.IsReadMode())
  {
    retro_atomic_store_release_int(
        &m_pending, token_pending | (token_interrupt_pending ? PENDING_TOKEN_INT : 0) |
                        (finish_interrupt_pending ? PENDING_FINISH_INT : 0) |
                        (event_raised ? PENDING_EVENT_RAISED : 0));
  }

  p.Do(m_signal_token_interrupt);
  p.Do(m_signal_finish_interrupt);
}

void PixelEngineManager::Init()
{
  m_control.hex = 0;
  m_z_conf.hex = 0;
  m_alpha_conf.hex = 0;
  m_dst_alpha_conf.hex = 0;
  m_alpha_mode_conf.hex = 0;
  m_alpha_read.hex = 0;

  m_token = 0;
  retro_atomic_store_release_int(&m_pending, 0);

  m_signal_token_interrupt = false;
  m_signal_finish_interrupt = false;

  m_event_type_set_token_finish =
      m_system.GetCoreTiming().RegisterEvent("SetTokenFinish", SetTokenFinish_OnMainThread_Static);
}

void PixelEngineManager::RegisterMMIO(MMIO::Mapping* mmio, u32 base)
{
  // Directly mapped registers.
  struct
  {
    u32 addr;
    u16* ptr;
  } directly_mapped_vars[] = {
      {PE_ZCONF, &m_z_conf.hex},
      {PE_ALPHACONF, &m_alpha_conf.hex},
      {PE_DSTALPHACONF, &m_dst_alpha_conf.hex},
      {PE_ALPHAMODE, &m_alpha_mode_conf.hex},
      {PE_ALPHAREAD, &m_alpha_read.hex},
  };
  for (auto& mapped_var : directly_mapped_vars)
  {
    mmio->Register(base | mapped_var.addr, MMIO::DirectRead<u16>(mapped_var.ptr),
                   MMIO::DirectWrite<u16>(mapped_var.ptr));
  }

  // Performance queries registers: read only, need to call the video backend
  // to get the results.
  struct
  {
    u32 addr;
    PerfQueryType pqtype;
  } pq_regs[] = {
      {PE_PERF_ZCOMP_INPUT_ZCOMPLOC_L, PQ_ZCOMP_INPUT_ZCOMPLOC},
      {PE_PERF_ZCOMP_OUTPUT_ZCOMPLOC_L, PQ_ZCOMP_OUTPUT_ZCOMPLOC},
      {PE_PERF_ZCOMP_INPUT_L, PQ_ZCOMP_INPUT},
      {PE_PERF_ZCOMP_OUTPUT_L, PQ_ZCOMP_OUTPUT},
      {PE_PERF_BLEND_INPUT_L, PQ_BLEND_INPUT},
      {PE_PERF_EFB_COPY_CLOCKS_L, PQ_EFB_COPY_CLOCKS},
  };
  for (auto& pq_reg : pq_regs)
  {
    mmio->Register(base | pq_reg.addr, MMIO::ComplexRead<u16>([pq_reg](Core::System&, u32) {
                     return g_video_backend->Video_GetQueryResult(pq_reg.pqtype) & 0xFFFF;
                   }),
                   MMIO::InvalidWrite<u16>());
    mmio->Register(base | (pq_reg.addr + 2), MMIO::ComplexRead<u16>([pq_reg](Core::System&, u32) {
                     return g_video_backend->Video_GetQueryResult(pq_reg.pqtype) >> 16;
                   }),
                   MMIO::InvalidWrite<u16>());
  }

  // Control register
  mmio->Register(base | PE_CTRL_REGISTER, MMIO::DirectRead<u16>(&m_control.hex),
                 MMIO::ComplexWrite<u16>([](Core::System& system, u32, u16 val) {
                   auto& pe = system.GetPixelEngine();

                   UPECtrlReg tmpCtrl{.hex = val};

                   if (tmpCtrl.pe_token)
                     pe.m_signal_token_interrupt = false;

                   if (tmpCtrl.pe_finish)
                     pe.m_signal_finish_interrupt = false;

                   pe.m_control.pe_token_enable = tmpCtrl.pe_token_enable.Value();
                   pe.m_control.pe_finish_enable = tmpCtrl.pe_finish_enable.Value();
                   pe.m_control.pe_token = false;   // this flag is write only
                   pe.m_control.pe_finish = false;  // this flag is write only

                   DEBUG_LOG_FMT(PIXELENGINE, "(w16) CTRL_REGISTER: {:#06x}", val);
                   pe.UpdateInterrupts();
                 }));

  // Token register, readonly.
  mmio->Register(base | PE_TOKEN_REG, MMIO::DirectRead<u16>(&m_token), MMIO::InvalidWrite<u16>());

  // BBOX registers, readonly and need to update a flag.
  for (int i = 0; i < 4; ++i)
  {
    mmio->Register(base | (PE_BBOX_LEFT + 2 * i),
                   MMIO::ComplexRead<u16>([i](Core::System& system, u32) {
                     g_bounding_box->Disable(system.GetPixelShaderManager());
                     return g_video_backend->Video_GetBoundingBox(i);
                   }),
                   MMIO::InvalidWrite<u16>());
  }
}

void PixelEngineManager::UpdateInterrupts()
{
  auto& processor_interface = m_system.GetProcessorInterface();

  // check if there is a token-interrupt
  processor_interface.SetInterrupt(INT_CAUSE_PE_TOKEN,
                                   m_signal_token_interrupt && m_control.pe_token_enable);

  // check if there is a finish-interrupt
  processor_interface.SetInterrupt(INT_CAUSE_PE_FINISH,
                                   m_signal_finish_interrupt && m_control.pe_finish_enable);
}

void PixelEngineManager::SetTokenFinish_OnMainThread_Static(Core::System& system, u64 userdata,
                                                            s64 cycles_late)
{
  system.GetPixelEngine().SetTokenFinish_OnMainThread(userdata, cycles_late);
}

void PixelEngineManager::SetTokenFinish_OnMainThread(u64 userdata, s64 cycles_late)
{
  // Take the flags, leave the token: the next event reads it again.
  int pending = retro_atomic_load_acquire_int(&m_pending);
  while (!retro_atomic_cas_int(&m_pending, pending, pending & 0xFFFF))
    pending = retro_atomic_load_acquire_int(&m_pending);

  m_token = static_cast<u16>(pending);

  if (pending & PENDING_TOKEN_INT)
  {
    m_signal_token_interrupt = true;
    UpdateInterrupts();
  }

  if (pending & PENDING_FINISH_INT)
  {
    m_signal_finish_interrupt = true;
    UpdateInterrupts();
  }
}

// Raise the event handler above on the CPU thread.
// THIS IS EXECUTED FROM VIDEO THREAD
void PixelEngineManager::RaiseEvent(int set, int keep, int cycles_into_future)
{
  int pending = retro_atomic_load_acquire_int(&m_pending);
  while (!retro_atomic_cas_int(&m_pending, pending, (pending & keep) | set | PENDING_EVENT_RAISED))
    pending = retro_atomic_load_acquire_int(&m_pending);
  if (pending & PENDING_EVENT_RAISED)
    return;

  CoreTiming::FromThread from = CoreTiming::FromThread::NON_CPU;
  s64 cycles = 0;  // we don't care about timings for dual core mode.
  if (!m_system.IsDualCoreMode() || m_system.GetFifo().UseDeterministicGPUThread())
  {
    from = CoreTiming::FromThread::CPU;

    // Hack: Dolphin's single-core gpu timings are way too fast. Enforce a minimum delay to give
    //       games time to setup any interrupt state
    cycles = std::max(500, cycles_into_future);
  }
  m_system.GetCoreTiming().ScheduleEvent(cycles, m_event_type_set_token_finish, 0, from);
}

// SetToken
// THIS IS EXECUTED FROM VIDEO THREAD
void PixelEngineManager::SetToken(const u16 token, const bool interrupt, int cycles_into_future)
{
  DEBUG_LOG_FMT(PIXELENGINE, "VIDEO Backend raises INT_CAUSE_PE_TOKEN (btw, token: {:04x})", token);

  RaiseEvent(token | (interrupt ? PENDING_TOKEN_INT : 0), ~0xFFFF, cycles_into_future);
}

// SetFinish
// THIS IS EXECUTED FROM VIDEO THREAD (BPStructs.cpp) when a new frame has been drawn
void PixelEngineManager::SetFinish(int cycles_into_future)
{
  DEBUG_LOG_FMT(PIXELENGINE, "VIDEO Set Finish");

  RaiseEvent(PENDING_FINISH_INT, ~0, cycles_into_future);
}

}  // namespace PixelEngine
