#include "DolphinLibretro/Audio.h"

#include "AudioCommon/FieldAudio.h"
#include "Core/System.h"
#include "DolphinLibretro/Common/Globals.h"

namespace Libretro
{
namespace Audio
{
static retro_audio_sample_batch_t batch_cb = NULL;
/* The rate the frontend was last told; retro_run only. */
static double s_reported_rate = 0.0;

static double RateOf(unsigned divisor)
{
  return (double)FIELD_AUDIO_RATE_DIVIDEND / (double)divisor;
}

double GetSampleRate()
{
  unsigned divisor = field_audio_output_divisor();
  /* Before the AI sets it: the DMA rate it starts at, 32 kHz on the console being loaded. */
  if (divisor == 0)
    divisor = (Core::System::GetInstance().IsWii() ? 1125 * 2 : 1124 * 2) * 3 / 2;
  s_reported_rate = RateOf(divisor);
  return s_reported_rate;
}

void PushFields()
{
  const int16_t* samples;
  unsigned frames;
  const unsigned divisor = field_audio_output_divisor();

  /* The output is the DSP's DMA stream at its own rate, so a game switching that rate changes
   * what the frontend resamples from. */
  if (divisor && RateOf(divisor) != s_reported_rate)
  {
    retro_system_av_info info;
    retro_get_system_av_info(&info);
    environ_cb(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &info);
  }

  while (field_audio_take(&samples, &frames))
  {
    /* The frontend may take a field in more than one go. */
    while (frames && batch_cb)
    {
      const size_t taken = batch_cb(samples, frames);
      if (taken == 0)
        break;
      samples += taken * 2;
      frames -= (unsigned)taken;
    }
    field_audio_release();
  }
}
}  // namespace Audio
}  // namespace Libretro

extern "C" {
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb)
{
  Libretro::Audio::batch_cb = cb;
}

void retro_set_audio_sample(retro_audio_sample_t cb)
{
  (void)cb;
}
}  // extern "C"
