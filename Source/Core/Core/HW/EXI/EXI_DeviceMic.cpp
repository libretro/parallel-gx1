// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HW/EXI/EXI_DeviceMic.h"

#include <algorithm>
#include <cstring>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"

#include "Core/CoreTiming.h"
#include "Core/HW/EXI/EXI.h"
#include "Core/HW/GCPad.h"
#include "Core/HW/SystemTimers.h"
#include "Core/System.h"

#include <libretro.h>
#include "DolphinLibretro/Common/Globals.h"

namespace ExpansionInterface
{
// The most the emulated microphone can buffer: 64 samples a read, 500 reads deep.
constexpr size_t MIC_STREAM_BYTES = 64 * 500 * sizeof(s16);

void CEXIMic::StreamInit()
{
  retro_spsc_init(&m_stream, MIC_STREAM_BYTES);
}

void CEXIMic::StreamTerminate()
{
  StreamStop();
  retro_spsc_free(&m_stream);
}

void CEXIMic::StreamStart()
{
  if (!Libretro::Input::g_has_microphone_support)
    return;

  if (retro_atomic_load_acquire_ptr(&m_retro_mic))
    StreamStop();

  // Start from an empty stream; the consumer side may drop what is there.
  retro_spsc_skip(&m_stream, retro_spsc_read_avail(&m_stream));

  retro_microphone_params_t params{};
  params.rate = sample_rate;
  retro_microphone_t* mic = Libretro::Input::g_microphone_interface.open_mic(&params);

  if (!mic)
  {
    WARN_LOG_FMT(EXPANSIONINTERFACE, "CEXIMic: failed to open GC mic at {}Hz", sample_rate);
    return;
  }

  Libretro::Input::g_microphone_interface.set_mic_state(mic, true);
  retro_atomic_store_release_ptr(&m_retro_mic, mic);
}

void CEXIMic::StreamStop()
{
  auto* mic = static_cast<retro_microphone_t*>(retro_atomic_exchange_ptr(&m_retro_mic, nullptr));
  if (mic)
  {
    // A read the frontend thread already started finishes before the mic goes.
    call_gate_close(&m_mic_gate);
    Libretro::Input::g_microphone_interface.close_mic(mic);
    call_gate_reopen(&m_mic_gate);
  }

  retro_spsc_skip(&m_stream, retro_spsc_read_avail(&m_stream));
}

void CEXIMic::StreamReadOne()
{
  if (retro_atomic_exchange_int(&m_overflowed, 0))
    status.buff_ovrflw = 1;

  if (retro_spsc_read_avail(&m_stream) >= static_cast<size_t>(buff_size))
    retro_spsc_read(&m_stream, ring_buffer, buff_size);
}

// EXI Mic Device
// This works by opening and starting an input stream when the is_active
// bit is set. The interrupt is scheduled in the future based on sample rate and
// buffer size settings. When the console handles the interrupt, it will send
// cmdGetBuffer, which is when we actually read data from a buffer filled
// in the background.

u8 const CEXIMic::exi_id[] = {0, 0x0a, 0, 0, 0};

CEXIMic::CEXIMic(Core::System& system, int index)
    : IEXIDevice(system), slot(index)
{
  m_position = 0;
  command = 0;
  status.U16 = 0;

  sample_rate = rate_base;
  buff_size = ring_base;
  buff_size_samples = buff_size / sample_size;

  ring_pos = 0;
  std::memset(ring_buffer, 0, sizeof(ring_buffer));

  next_int_ticks = 0;

  StreamInit();
}

CEXIMic::~CEXIMic()
{
  StreamTerminate();
}

bool CEXIMic::IsPresent() const
{
  return true;
}

void CEXIMic::SetCS(int cs)
{
  if (cs)  // not-selected to selected
    m_position = 0;
  // Doesn't appear to do anything we care about
  // else if (command == cmdReset)
}

void CEXIMic::UpdateNextInterruptTicks()
{
  int diff = (m_system.GetSystemTimers().GetTicksPerSecond() / sample_rate) * buff_size_samples;
  next_int_ticks = m_system.GetCoreTiming().GetTicks() + diff;
  m_system.GetExpansionInterface().ScheduleUpdateInterrupts(CoreTiming::FromThread::CPU, diff);
}

bool CEXIMic::IsInterruptSet()
{
  if (next_int_ticks && m_system.GetCoreTiming().GetTicks() >= next_int_ticks)
  {
    if (status.is_active)
      UpdateNextInterruptTicks();
    else
      next_int_ticks = 0;

    return true;
  }
  else
  {
    return false;
  }
}

void CEXIMic::TransferByte(u8& byte)
{
  if (m_position == 0)
  {
    command = byte;  // first byte is command
    byte = 0xFF;     // would be tristate, but we don't care.
    m_position++;
    return;
  }

  int pos = m_position - 1;

  switch (command)
  {
  case cmdID:
    byte = exi_id[pos];
    break;

  case cmdGetStatus:
    if (pos == 0)
      status.button = Pad::GetMicButton(slot);

    byte = status.U8[pos ^ 1];

    if (pos == 1)
      status.buff_ovrflw = 0;
    break;

  case cmdSetStatus:
  {
    bool wasactive = status.is_active;
    status.U8[pos ^ 1] = byte;

    // safe to do since these can only be entered if both bytes of status have been written
    if (!wasactive && status.is_active)
    {
      sample_rate = rate_base << status.sample_rate;
      buff_size = ring_base << status.buff_size;
      buff_size_samples = buff_size / sample_size;

      UpdateNextInterruptTicks();

      StreamStart();
    }
    else if (wasactive && !status.is_active)
    {
      StreamStop();
    }
  }
  break;

  case cmdGetBuffer:
  {
    if (ring_pos == 0)
      StreamReadOne();

    byte = ring_buffer[ring_pos ^ 1];
    ring_pos = (ring_pos + 1) % buff_size;
  }
  break;

  default:
    ERROR_LOG_FMT(EXPANSIONINTERFACE, "EXI MIC: unknown command byte {:02x}", command);
    break;
  }

  m_position++;
}

void CEXIMic::PollLibretroMic()
{
  // Frontend thread.
  call_gate_frame_t frame;
  if (!call_gate_enter(&m_mic_gate, &frame))
    return;

  if (auto* mic = static_cast<retro_microphone_t*>(retro_atomic_load_acquire_ptr(&m_retro_mic)))
  {
    s16 buf[64];  // max buff_size_samples (ring_base << 2 = 128 bytes / 2 = 64 samples)

    const int frames = Libretro::Input::g_microphone_interface.read_mic(mic, buf, std::size(buf));
    if (frames > 0)
    {
      const size_t bytes = frames * sizeof(s16);
      if (retro_spsc_write(&m_stream, buf, bytes) != bytes)
        retro_atomic_store_release_int(&m_overflowed, 1);
    }
  }

  call_gate_exit(&m_mic_gate, &frame);
}

}  // namespace ExpansionInterface
