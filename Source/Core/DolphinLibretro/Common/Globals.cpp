#include "Globals.h"

namespace Libretro
{
retro_environment_t environ_cb = nullptr;
bool g_emuthread_launched = false;
std::vector<Gecko::GeckoCode> g_gecko_codes;
std::vector<ActionReplay::ARCode> g_ar_codes;

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
