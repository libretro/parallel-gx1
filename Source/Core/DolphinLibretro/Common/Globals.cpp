#include "Globals.h"

namespace Libretro
{
retro_environment_t environ_cb = nullptr;
bool g_emuthread_launched = false;
Gecko::GeckoCodeList g_gecko_codes;
ActionReplay::ARCodeList g_ar_codes;
static retro_atomic_int_t s_shutdown_requested;

void RequestShutdown()
{
  retro_atomic_store_release_int(&s_shutdown_requested, 1);
}

bool TakeShutdownRequest()
{
  return retro_atomic_load_acquire_int(&s_shutdown_requested) &&
         retro_atomic_exchange_int(&s_shutdown_requested, 0);
}

namespace Input
{
retro_microphone_interface g_microphone_interface{};
bool g_has_microphone_support = false;
bool g_gc_mic_button[4];
retro_atomic_ptr_t g_active_microphones[MAX_ACTIVE_MICROPHONES];
call_gate_t g_microphone_poll_gate;
} // namespace Input

namespace Video
{
retro_video_refresh_t video_cb = nullptr;
struct retro_hw_render_callback hw_render{};
}  // namespace Video
}  // namespace Libretro
