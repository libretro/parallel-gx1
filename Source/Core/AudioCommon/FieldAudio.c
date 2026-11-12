/* Copyright 2026 Dolphin Emulator Project
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "AudioCommon/FieldAudio.h"

#include <string.h>

#include <retro_atomic.h>
#include <retro_spsc.h>

/* Sealed fields wait here for retro_run; normally one does. */
#define FIELD_RING 4
/* Room past the field for a source that ran a little ahead of it. */
#define FIELD_SLACK 256
#define FIELD_CAPACITY (FIELD_AUDIO_MAX_FRAMES + FIELD_SLACK)

/* A GBA's ring holds a few fields of its 65 kHz output. */
#define GBA_RING_BYTES (16384 * 4)

#define PHASE_ONE ((uint64_t)1 << 32)

typedef struct field_mix
{
   uint64_t step;   /* source frames per output frame, 32.32; 0 when not set */
   uint64_t phase;  /* where the next output frame falls past prev, 32.32 */
   uint32_t divisor;
   int32_t prev_l, prev_r; /* scaled by the volume: 8 fraction bits */
   int32_t  vol_l, vol_r; /* 0 to 256 */
   unsigned cursor; /* next output frame in the open field */
} field_mix_t;

static int16_t  s_fields[FIELD_RING][FIELD_AUDIO_MAX_FRAMES * 2];
static unsigned s_lengths[FIELD_RING];
/* Fields sealed and released, counted forever: the CPU thread writes
 * the first, retro_run the second. */
static retro_atomic_int_t s_sealed;
static retro_atomic_int_t s_released;

/* CPU thread only from here on. */
static unsigned    s_open;        /* the field being written */
/* What every source added to the open field, summed in 32 bits: clamped
 * to 16 once, when the field is sealed, so a loud sum does not depend on
 * the order the sources came in. Zero past the furthest cursor. */
static int32_t     s_sums[FIELD_CAPACITY * 2];
static unsigned    s_dma_cursor;  /* next DMA frame in it */
static uint32_t    s_dma_divisor; /* the output rate */
static field_mix_t s_mixes[FIELD_AUDIO_MIXES];

/* A GBA's samples cross from its thread in a ring; its rate is set from
 * there too, and taken up by the CPU thread when it mixes. */
static retro_spsc_t       s_gba_rings[FIELD_AUDIO_GBAS];
static int                s_gba_rings_ready;
static retro_atomic_int_t s_gba_hz[FIELD_AUDIO_GBAS];
static int                s_gba_hz_used[FIELD_AUDIO_GBAS];

static int16_t clamp16(int32_t v)
{
   if (v > 32767)
      return 32767;
   if (v < -32768)
      return -32768;
   return (int16_t)v;
}

static int16_t from_be(int16_t v)
{
   const uint16_t u = (uint16_t)v;
   return (int16_t)(uint16_t)((u >> 8) | (u << 8));
}

/* Source frames per output frame for a rate in Hz: the rate times the
 * output divisor over the dividend. */
static uint64_t step_for_hz(unsigned hz)
{
   if (!hz || !s_dma_divisor)
      return 0;
   return (((uint64_t)hz * s_dma_divisor) << 32) / FIELD_AUDIO_RATE_DIVIDEND;
}

static void mix_update_step(field_mix_t *mix)
{
   mix->step = (s_dma_divisor && mix->divisor)
      ? ((uint64_t)s_dma_divisor << 32) / mix->divisor : 0;
   mix->phase = PHASE_ONE;
}

void field_audio_reset(void)
{
   unsigned i;

   if (!s_gba_rings_ready)
   {
      for (i = 0; i < FIELD_AUDIO_GBAS; i++)
         retro_spsc_init(&s_gba_rings[i], GBA_RING_BYTES);
      s_gba_rings_ready = 1;
   }
   for (i = 0; i < FIELD_AUDIO_GBAS; i++)
   {
      const void *unused;
      size_t queued;
      while ((queued = retro_spsc_read_begin(&s_gba_rings[i], &unused)) != 0)
         retro_spsc_read_end(&s_gba_rings[i], queued);
      s_gba_hz_used[i] = -1;
   }

   memset(s_fields, 0, sizeof(s_fields));
   memset(s_sums, 0, sizeof(s_sums));
   memset(s_mixes, 0, sizeof(s_mixes));
   for (i = 0; i < FIELD_AUDIO_MIXES; i++)
   {
      s_mixes[i].vol_l = 256;
      s_mixes[i].vol_r = 256;
      s_mixes[i].phase = PHASE_ONE;
   }
   /* The portal's speaker always plays at 8 kHz. */
   s_mixes[FIELD_AUDIO_PORTAL].divisor = FIELD_AUDIO_RATE_DIVIDEND / 8000;
   mix_update_step(&s_mixes[FIELD_AUDIO_PORTAL]);

   s_open       = 0;
   s_dma_cursor = 0;
   retro_atomic_store_release_int(&s_sealed, 0);
   retro_atomic_store_release_int(&s_released, 0);
}

void field_audio_set_dma_rate(uint32_t divisor)
{
   unsigned i;
   s_dma_divisor = divisor;
   for (i = 0; i < FIELD_AUDIO_MIXES; i++)
      mix_update_step(&s_mixes[i]);
   for (i = 0; i < FIELD_AUDIO_GBAS; i++)
      s_gba_hz_used[i] = -1;
}

uint32_t field_audio_output_divisor(void)
{
   return s_dma_divisor;
}

void field_audio_push_dma(const int16_t *pairs, size_t frames)
{
   int32_t *out = s_sums + s_dma_cursor * 2;
   size_t i;
   if (s_dma_cursor + frames > FIELD_CAPACITY)
      frames = FIELD_CAPACITY - s_dma_cursor;
   for (i = 0; i < frames; i++)
   {
      /* Added, as other sources may already be there. */
      out[i * 2]     += from_be(pairs[i * 2 + 1]);
      out[i * 2 + 1] += from_be(pairs[i * 2]);
   }
   s_dma_cursor += (unsigned)frames;
}

void field_audio_mix_set_rate(enum field_audio_mix id, uint32_t divisor)
{
   field_mix_t *mix = &s_mixes[id];
   if (mix->divisor == divisor)
      return;
   mix->divisor = divisor;
   mix_update_step(mix);
}

void field_audio_mix_set_volume(enum field_audio_mix id, unsigned left, unsigned right)
{
   /* 255 means the sample's own level. */
   s_mixes[id].vol_l = (int32_t)(left + (left >> 7));
   s_mixes[id].vol_r = (int32_t)(right + (right >> 7));
}

void field_audio_mix_set_gain_q8(enum field_audio_mix id, unsigned left, unsigned right)
{
   s_mixes[id].vol_l = (int32_t)(left > 256 ? 256 : left);
   s_mixes[id].vol_r = (int32_t)(right > 256 ? 256 : right);
}

static void mix_add(field_mix_t *mix, int32_t l, int32_t r)
{
   if (mix->cursor < FIELD_CAPACITY)
   {
      int32_t *out = s_sums + mix->cursor * 2;
      out[0] += l;
      out[1] += r;
      mix->cursor++;
   }
}

/* A sample with 8 fraction bits, rounded to the nearest one: shifting
 * them off would floor it: a DC offset of half a step, and distortion,
 * on every source that is scaled or resampled. */
static int32_t round_q8(int32_t v)
{
  return (v + 128) >> 8;
}

/* One source frame: added as is when the source runs at the output
 * rate, otherwise the output frames that fall between it and the last
 * one, interpolated between the two. */
static void mix_frame(field_mix_t *mix, int32_t l, int32_t r)
{
   if (!mix->step)
      return;
   /* Kept with the volume's 8 fraction bits through the interpolation,
    * so the frame is rounded once, at the end. */
   l *= mix->vol_l;
   r *= mix->vol_r;
   if (mix->step == PHASE_ONE)
   {
     mix_add(mix, round_q8(l), round_q8(r));
     return;
   }
   while (mix->phase < PHASE_ONE)
   {
      const int32_t frac = (int32_t)(mix->phase >> 16);
      mix_add(mix, round_q8(mix->prev_l + (int32_t)(((int64_t)(l - mix->prev_l) * frac) >> 16)),
              round_q8(mix->prev_r + (int32_t)(((int64_t)(r - mix->prev_r) * frac) >> 16)));
      mix->phase += mix->step;
   }
   mix->phase -= PHASE_ONE;
   mix->prev_l = l;
   mix->prev_r = r;
}

void field_audio_mix_be_rl(enum field_audio_mix id, const int16_t *pairs, size_t frames)
{
   field_mix_t *mix = &s_mixes[id];
   size_t i;
   for (i = 0; i < frames; i++)
      mix_frame(mix, from_be(pairs[i * 2 + 1]), from_be(pairs[i * 2]));
}

void field_audio_mix_frame(enum field_audio_mix id, int16_t left, int16_t right)
{
   mix_frame(&s_mixes[id], left, right);
}

void field_audio_mix_mono_le(enum field_audio_mix id, const uint8_t *bytes, size_t count)
{
   field_mix_t *mix = &s_mixes[id];
   size_t i;
   for (i = 0; i < count; i++)
   {
      const int16_t s = (int16_t)(uint16_t)(bytes[i * 2] | (bytes[i * 2 + 1] << 8));
      mix_frame(mix, s, s);
   }
}

size_t field_audio_gba_write_begin(unsigned gba, int16_t **frames)
{
   void *space;
   size_t bytes;
   if (gba >= FIELD_AUDIO_GBAS || !s_gba_rings_ready)
      return 0;
   bytes   = retro_spsc_write_begin(&s_gba_rings[gba], &space);
   *frames = (int16_t*)space;
   return bytes / (2 * sizeof(int16_t));
}

void field_audio_gba_write_end(unsigned gba, size_t frames)
{
   retro_spsc_write_end(&s_gba_rings[gba], frames * 2 * sizeof(int16_t));
}

void field_audio_gba_set_rate_hz(unsigned gba, unsigned hz)
{
   if (gba < FIELD_AUDIO_GBAS)
      retro_atomic_store_release_int(&s_gba_hz[gba], (int)hz);
}

/* CPU thread: what each GBA queued is mixed in, read in place from its
 * ring. */
static void mix_gbas(void)
{
   unsigned i;
   if (!s_gba_rings_ready)
      return;
   for (i = 0; i < FIELD_AUDIO_GBAS; i++)
   {
      field_mix_t *mix = &s_mixes[FIELD_AUDIO_GBA0 + i];
      const int hz     = retro_atomic_load_acquire_int(&s_gba_hz[i]);
      const void *data;
      size_t bytes;

      if (hz != s_gba_hz_used[i])
      {
         s_gba_hz_used[i] = hz;
         mix->step  = step_for_hz((unsigned)hz);
         mix->phase = PHASE_ONE;
      }
      while ((bytes = retro_spsc_read_begin(&s_gba_rings[i], &data)) >= 2 * sizeof(int16_t))
      {
         const int16_t *frames = (const int16_t*)data;
         const size_t count    = bytes / (2 * sizeof(int16_t));
         size_t j;
         for (j = 0; j < count; j++)
            mix_frame(mix, frames[j * 2], frames[j * 2 + 1]);
         retro_spsc_read_end(&s_gba_rings[i], count * 2 * sizeof(int16_t));
      }
   }
}

void field_audio_seal(unsigned frames)
{
   unsigned i, ahead = 0;
   int16_t *field;

   mix_gbas();
   field = s_fields[s_open];

   if (frames > FIELD_AUDIO_MAX_FRAMES)
      frames = FIELD_AUDIO_MAX_FRAMES;

   if (s_dma_cursor > frames)
      ahead = s_dma_cursor - frames;
   s_dma_cursor = s_dma_cursor > frames ? s_dma_cursor - frames : 0;
   for (i = 0; i < FIELD_AUDIO_MIXES; i++)
   {
      field_mix_t *mix = &s_mixes[i];
      if (mix->cursor > frames)
      {
         if (mix->cursor - frames > ahead)
            ahead = mix->cursor - frames;
         mix->cursor -= frames;
      }
      else
         mix->cursor = 0;
   }

   /* With every other field still unread there is nowhere to go: the
    * open field starts over, and its samples are lost. */
   if (retro_atomic_load_acquire_int(&s_sealed) - retro_atomic_load_acquire_int(&s_released)
         >= FIELD_RING - 1)
   {
      memset(s_sums, 0, sizeof(s_sums));
      s_dma_cursor = 0;
      for (i = 0; i < FIELD_AUDIO_MIXES; i++)
         s_mixes[i].cursor = 0;
      return;
   }

   for (i = 0; i < frames * 2; i++)
      field[i] = clamp16(s_sums[i]);
   /* Samples written past the field's end open the next one; what was
    * summed behind them goes back to zero. */
   if (ahead)
      memmove(s_sums, s_sums + frames * 2, ahead * 2 * sizeof(int32_t));
   memset(s_sums + ahead * 2, 0, frames * 2 * sizeof(int32_t));

   s_lengths[s_open] = frames;
   s_open            = (s_open + 1) % FIELD_RING;
   retro_atomic_fetch_add_int(&s_sealed, 1);
}

int field_audio_take(const int16_t **samples, unsigned *frames)
{
   const int released = retro_atomic_load_acquire_int(&s_released);
   unsigned index;
   if (retro_atomic_load_acquire_int(&s_sealed) == released)
      return 0;
   index    = (unsigned)released % FIELD_RING;
   *samples = s_fields[index];
   *frames  = s_lengths[index];
   return 1;
}

void field_audio_release(void)
{
   retro_atomic_fetch_add_int(&s_released, 1);
}
