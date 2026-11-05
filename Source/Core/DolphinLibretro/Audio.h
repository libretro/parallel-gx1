#pragma once

#include <libretro.h>

namespace Libretro
{
extern double g_core_refresh_rate;
namespace Audio
{
// The output rate the frontend is told, in Hz.
double GetSampleRate();
// From retro_run, once the frame has run: every field that ended goes to the frontend whole,
// straight from the buffer the emulated hardware wrote it into.
void PushFields();
}  // namespace Audio
}  // namespace Libretro
