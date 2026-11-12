/* Copyright 2026 Dolphin Emulator Project
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef DOLPHIN_AUDIOCOMMON_FIELDAUDIO_H
#define DOLPHIN_AUDIOCOMMON_FIELDAUDIO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The console's audio output, one video field at a time.
 *
 * The AI's output is the DSP's DMA stream at the DMA rate: its samples
 * go straight into the current field's buffer, only byte-swapped. The
 * other sources are added in place on top as they produce samples:
 * disc streaming (converted only when its rate is not the DMA rate, as
 * the AI does when it mixes the two), and, while they play, the remotes'
 * speakers, the Skylanders portal and connected GBAs, each converted
 * from its own rate. Nothing else is resampled: the frontend takes the
 * output at the rate it is reported at.
 *
 * When the field ends the CPU thread seals the buffer, and retro_run
 * hands it whole to the frontend: one consecutive run of samples per
 * field, with no queue and no copy in between.
 *
 * The CPU thread calls everything except the GBA ring (the GBA's own
 * thread) and the field_audio_take pair (retro_run). Rates are divisors
 * of FIELD_AUDIO_RATE_DIVIDEND unless given in Hz. */

#define FIELD_AUDIO_RATE_DIVIDEND 108000000u

/* Stereo frames one field can hold: a 25 Hz field at 48 kHz is 1920. */
#define FIELD_AUDIO_MAX_FRAMES 2048

#define FIELD_AUDIO_GBAS 4

enum field_audio_mix
{
   FIELD_AUDIO_STREAM = 0, /* AI streaming from the disc */
   FIELD_AUDIO_WIIMOTE0,   /* the remotes' speakers */
   FIELD_AUDIO_WIIMOTE1,
   FIELD_AUDIO_WIIMOTE2,
   FIELD_AUDIO_WIIMOTE3,
   FIELD_AUDIO_PORTAL,     /* the Skylanders portal's speaker */
   FIELD_AUDIO_GBA0,       /* the GBAs, fed through their rings */
   FIELD_AUDIO_MIXES = FIELD_AUDIO_GBA0 + FIELD_AUDIO_GBAS
};

/* Starts empty; drops any sealed fields and anything the GBAs queued. */
void field_audio_reset(void);

/* The DMA rate, which is the output rate; 0 until the AI sets it. */
void field_audio_set_dma_rate(uint32_t divisor);
uint32_t field_audio_output_divisor(void);
/* Big-endian right/left pairs, as the AI reads them from memory. */
void field_audio_push_dma(const int16_t *pairs, size_t frames);

void field_audio_mix_set_rate(enum field_audio_mix mix, uint32_t divisor);
/* 0 to 255 per channel, 255 being the sample's own level. */
void field_audio_mix_set_volume(enum field_audio_mix mix, unsigned left, unsigned right);
/* 0 to 256 per channel, 256 being the sample's own level: for a source whose gain is not a
 * register on that scale, so it is rounded once, here, and not again to fit 0 to 255. */
void field_audio_mix_set_gain_q8(enum field_audio_mix mix, unsigned left, unsigned right);
/* Big-endian right/left pairs (disc streaming). */
void field_audio_mix_be_rl(enum field_audio_mix mix, const int16_t *pairs, size_t frames);
/* One host-endian frame, for a source that decodes a sample at a time. */
void field_audio_mix_frame(enum field_audio_mix mix, int16_t left, int16_t right);
/* Little-endian mono bytes, played on both channels. */
void field_audio_mix_mono_le(enum field_audio_mix mix, const uint8_t *bytes, size_t count);

/* The GBA's thread, which mGBA's audio reaches: its stereo frames go
 * straight into a ring at the returned place, at most the returned
 * count; commit how many were written. The CPU thread mixes the ring
 * in when the field ends. */
size_t field_audio_gba_write_begin(unsigned gba, int16_t **frames);
void field_audio_gba_write_end(unsigned gba, size_t frames);
void field_audio_gba_set_rate_hz(unsigned gba, unsigned hz);

/* Ends the field: frames is its length at the output rate, from
 * emulated time. Samples written past it open the next field. */
void field_audio_seal(unsigned frames);

/* retro_run: the oldest sealed field, if any (nonzero). Hand its
 * *frames stereo frames at *samples to the frontend, then release. */
int field_audio_take(const int16_t **samples, unsigned *frames);
void field_audio_release(void);

#ifdef __cplusplus
}
#endif

#endif
