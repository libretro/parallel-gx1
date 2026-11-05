#pragma once

#include <libretro.h>
#include <vector>

#include <retro_atomic.h>

#include "Common/CallGate.h"

#include "Core/ActionReplay.h"
#include "Core/GeckoCode.h"
#include "Core/IOS/USB/Emulated/Microphone.h"

namespace Libretro
{
extern retro_environment_t environ_cb;
extern bool g_emuthread_launched;
extern std::vector<Gecko::GeckoCode> g_gecko_codes;
extern std::vector<ActionReplay::ARCode> g_ar_codes;
inline constexpr unsigned g_gbplayer_subsystem_id = 0x101;

namespace Input
{
extern retro_microphone_interface g_microphone_interface;
extern bool g_has_microphone_support;
extern bool g_gc_mic_button[4];
// Open emulated USB microphones, polled by the frontend thread inside
// g_microphone_poll_gate; the emulation thread adds and removes them.
constexpr int MAX_ACTIVE_MICROPHONES = 4;
extern retro_atomic_ptr_t g_active_microphones[MAX_ACTIVE_MICROPHONES];
extern call_gate_t g_microphone_poll_gate;
} // namespace Input

namespace Video
{
extern retro_video_refresh_t video_cb;
extern struct retro_hw_render_callback hw_render;
}  // namespace Video
}  // namespace Libretro
