/* Copyright 2026 Dolphin Emulator Project
 * SPDX-License-Identifier: GPL-2.0-or-later */

/* The Game Boy Player without its startup disc: a Game Boy, Game Boy Color or
 * Game Boy Advance ROM runs on mGBA alone, its picture, sound and buttons
 * going straight to and from the frontend. No GameCube runs; each retro_run
 * is one frame of the handheld. The save is the one the Game Boy Player and
 * the frontend's mGBA core keep: <save directory>/<ROM name>.srm. */

#ifndef GBPLAYER_HLE_H
#define GBPLAYER_HLE_H

#include <stddef.h>

#include <libretro.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The frontend's callbacks, as it sets them */
void gbp_hle_set_environment(retro_environment_t cb);
void gbp_hle_set_video_refresh(retro_video_refresh_t cb);
void gbp_hle_set_audio_sample_batch(retro_audio_sample_batch_t cb);
void gbp_hle_set_input_poll(retro_input_poll_t cb);
void gbp_hle_set_input_state(retro_input_state_t cb);

/* Starts the ROM at rom_path; the BIOS is system_dir's gba_bios.bin when it
 * is there, the save in save_dir. Returns 0 when the ROM cannot run. */
int gbp_hle_load(const char *rom_path, const char *system_dir,
      const char *save_dir);
/* The save written back, everything let go */
void gbp_hle_unload(void);
/* Whether a ROM runs here rather than on the GameCube */
int gbp_hle_active(void);

void gbp_hle_get_av_info(struct retro_system_av_info *info);
void gbp_hle_run(void);
void gbp_hle_reset(void);

size_t gbp_hle_serialize_size(void);
int gbp_hle_serialize(void *data, size_t size);
int gbp_hle_unserialize(const void *data, size_t size);

/* The working RAM as the frontend's system RAM; no save RAM, the save file
 * being the core's own */
void *gbp_hle_memory_data(unsigned id);
size_t gbp_hle_memory_size(unsigned id);

void gbp_hle_cheat_reset(void);
void gbp_hle_cheat_set(unsigned index, int enabled, const char *code);

#ifdef __cplusplus
}
#endif

#endif
