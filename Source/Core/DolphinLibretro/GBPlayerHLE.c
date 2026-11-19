/* Copyright 2026 Dolphin Emulator Project
 * SPDX-License-Identifier: GPL-2.0-or-later */

/* See GBPlayerHLE.h. */

#include <stdarg.h>
#include <string.h>

#include <mgba-util/audio-buffer.h>
#include <mgba-util/image.h>
#include <mgba-util/vfs.h>
#include <mgba/core/core.h>
#include <mgba/core/interface.h>
#include <mgba/core/log.h>

#include <compat/strl.h>
#include <file/file_path.h>
#include <retro_miscellaneous.h>

#include "Core/HW/GBAVFile.h"
#include "DolphinLibretro/GBPlayerHLE.h"

/* Big enough for every picture mGBA makes: the Super Game Boy's border is
 * the largest */
#define GBP_HLE_MAX_WIDTH  256
#define GBP_HLE_MAX_HEIGHT 224
/* mGBA's ring holds this many stereo frames: a GBA frame makes about 550,
 * a Game Boy one, at its 131072 Hz, about 2200 */
#define GBP_HLE_AUDIO_FRAMES 4096
#define GBP_HLE_KEYS 10

static retro_environment_t        gbp_environ_cb;
static retro_video_refresh_t      gbp_video_cb;
static retro_audio_sample_batch_t gbp_audio_batch_cb;
static retro_input_poll_t         gbp_input_poll_cb;
static retro_input_state_t        gbp_input_state_cb;

static struct retro_rumble_interface gbp_rumble_cb;

static struct mCore *gbp_core;
/* The game's rumble, as much of each frame as the motor ran */
static struct mRumbleIntegrator gbp_rumble;
/* mGBA draws here; a line it does not redraw keeps what it drew */
static mColor gbp_video[GBP_HLE_MAX_WIDTH * GBP_HLE_MAX_HEIGHT];
/* the frontend's copy, blue in the low byte where mGBA has red */
static uint32_t gbp_frame[GBP_HLE_MAX_WIDTH * GBP_HLE_MAX_HEIGHT];
static int16_t gbp_audio[GBP_HLE_AUDIO_FRAMES * 2];

/* The joypad's buttons in the handheld's key order: A, B, Select, Start,
 * Right, Left, Up, Down, R, L */
static const unsigned gbp_keys[GBP_HLE_KEYS] = {
   RETRO_DEVICE_ID_JOYPAD_A,
   RETRO_DEVICE_ID_JOYPAD_B,
   RETRO_DEVICE_ID_JOYPAD_SELECT,
   RETRO_DEVICE_ID_JOYPAD_START,
   RETRO_DEVICE_ID_JOYPAD_RIGHT,
   RETRO_DEVICE_ID_JOYPAD_LEFT,
   RETRO_DEVICE_ID_JOYPAD_UP,
   RETRO_DEVICE_ID_JOYPAD_DOWN,
   RETRO_DEVICE_ID_JOYPAD_R,
   RETRO_DEVICE_ID_JOYPAD_L
};

/* mGBA's log has nowhere to go */
static void gbp_log(struct mLogger *logger, int category,
      enum mLogLevel level, const char *format, va_list args)
{
   (void)logger;
   (void)category;
   (void)level;
   (void)format;
   (void)args;
}

static struct mLogger gbp_logger = { gbp_log, NULL };

static void gbp_set_rumble(struct mRumbleIntegrator *rumble, float value)
{
   (void)rumble;
   if (gbp_rumble_cb.set_rumble_state)
      gbp_rumble_cb.set_rumble_state(0, RETRO_RUMBLE_STRONG,
            (uint16_t)(value * 0xffff));
}

void gbp_hle_set_environment(retro_environment_t cb)
{
   gbp_environ_cb = cb;
}

void gbp_hle_set_video_refresh(retro_video_refresh_t cb)
{
   gbp_video_cb = cb;
}

void gbp_hle_set_audio_sample_batch(retro_audio_sample_batch_t cb)
{
   gbp_audio_batch_cb = cb;
}

void gbp_hle_set_input_poll(retro_input_poll_t cb)
{
   gbp_input_poll_cb = cb;
}

void gbp_hle_set_input_state(retro_input_state_t cb)
{
   gbp_input_state_cb = cb;
}

int gbp_hle_active(void)
{
   return gbp_core != NULL;
}

/* save_dir/<the ROM's name>.srm, as the frontend's mGBA core names it;
 * beside the ROM with no save directory */
static void gbp_save_path(char *out, size_t size,
      const char *rom_path, const char *save_dir)
{
   char name[PATH_MAX_LENGTH];

   fill_pathname_base(name, rom_path, sizeof(name));
   path_remove_extension(name);
   strlcat(name, ".srm", sizeof(name));

   if (save_dir && *save_dir)
      fill_pathname_join_special(out, save_dir, name, size);
   else
   {
      fill_pathname_basedir(out, rom_path, size);
      strlcat(out, name, size);
   }
}

int gbp_hle_load(const char *rom_path, const char *system_dir,
      const char *save_dir)
{
   enum retro_pixel_format format = RETRO_PIXEL_FORMAT_XRGB8888;
   char path[PATH_MAX_LENGTH];
   struct VFile *rom;
   struct VFile *vf;

   if (!gbp_environ_cb
         || !gbp_environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &format))
      return 0;

   mLogSetDefaultLogger(&gbp_logger);

   if (!(rom = gba_vfile_open(rom_path, 0)))
      return 0;
   if (!(gbp_core = mCoreFindVF(rom)))
   {
      rom->close(rom);
      return 0;
   }
   gbp_core->init(gbp_core);

   mCoreInitConfig(gbp_core, "dolphin");
   /* a GBA game sees the Game Boy Player it was made for, and rumbles */
   mCoreConfigSetIntValue(&gbp_core->config, "gba.forceGbp", 1);
   mCoreConfigSetValue(&gbp_core->config, "gb.model", "DMG");
   mCoreConfigSetValue(&gbp_core->config, "cgb.model", "CGB");

   memset(&gbp_rumble_cb, 0, sizeof(gbp_rumble_cb));
   gbp_environ_cb(RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE, &gbp_rumble_cb);
   mRumbleIntegratorInit(&gbp_rumble);
   gbp_rumble.setRumble = gbp_set_rumble;
   gbp_core->setPeripheral(gbp_core, mPERIPH_RUMBLE, &gbp_rumble.d);

   gbp_core->setVideoBuffer(gbp_core, gbp_video, GBP_HLE_MAX_WIDTH);
   gbp_core->setAudioBufferSize(gbp_core, GBP_HLE_AUDIO_FRAMES);

   if (!gbp_core->loadROM(gbp_core, rom))
   {
      rom->close(rom);
      gbp_hle_unload();
      return 0;
   }

   /* the BIOS where the frontend's GBA cores look; mGBA's own stands in */
   if (system_dir && *system_dir)
   {
      fill_pathname_join_special(path, system_dir, "gba_bios.bin",
            sizeof(path));
      if ((vf = gba_vfile_open(path, 0)) && !gbp_core->loadBIOS(gbp_core, vf, 0))
         vf->close(vf);
   }

   gbp_save_path(path, sizeof(path), rom_path, save_dir);
   if ((vf = gba_vfile_open(path, 1)) && !gbp_core->loadSave(gbp_core, vf))
      vf->close(vf);

   gbp_core->reset(gbp_core);
   return 1;
}

void gbp_hle_unload(void)
{
   if (!gbp_core)
      return;
   gbp_set_rumble(&gbp_rumble, 0.0f);
   mCoreConfigDeinit(&gbp_core->config);
   gbp_core->deinit(gbp_core);
   gbp_core = NULL;
}

void gbp_hle_get_av_info(struct retro_system_av_info *info)
{
   unsigned width  = 240;
   unsigned height = 160;

   memset(info, 0, sizeof(*info));
   if (gbp_core)
   {
      gbp_core->currentVideoSize(gbp_core, &width, &height);
      info->timing.fps         = (double)gbp_core->frequency(gbp_core)
         / (double)gbp_core->frameCycles(gbp_core);
      info->timing.sample_rate = (double)gbp_core->audioSampleRate(gbp_core);
   }
   info->geometry.base_width   = width;
   info->geometry.base_height  = height;
   info->geometry.max_width    = GBP_HLE_MAX_WIDTH;
   info->geometry.max_height   = GBP_HLE_MAX_HEIGHT;
   info->geometry.aspect_ratio = (float)width / (float)height;
}

void gbp_hle_run(void)
{
   struct mAudioBuffer *audio;
   unsigned width;
   unsigned height;
   unsigned keys = 0;
   unsigned i;
   unsigned y;

   if (!gbp_core)
      return;

   if (gbp_input_poll_cb)
      gbp_input_poll_cb();
   if (gbp_input_state_cb)
   {
      for (i = 0; i < GBP_HLE_KEYS; i++)
      {
         if (gbp_input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, gbp_keys[i]))
            keys |= 1u << i;
      }
   }
   gbp_core->setKeys(gbp_core, keys);

   gbp_core->runFrame(gbp_core);

   /* mGBA draws red in the low byte, the frontend wants blue there: the
    * one pass that swaps them is the frontend's copy */
   gbp_core->currentVideoSize(gbp_core, &width, &height);
   for (y = 0; y < height; y++)
   {
      const mColor *src = gbp_video + (size_t)y * GBP_HLE_MAX_WIDTH;
      uint32_t *dst     = gbp_frame + (size_t)y * GBP_HLE_MAX_WIDTH;
      unsigned x;
      for (x = 0; x < width; x++)
      {
         const uint32_t c = src[x];
         dst[x] = (c & 0xff00u) | ((c & 0xffu) << 16) | ((c >> 16) & 0xffu);
      }
   }
   if (gbp_video_cb)
      gbp_video_cb(gbp_frame, width, height,
            GBP_HLE_MAX_WIDTH * sizeof(gbp_frame[0]));

   audio = gbp_core->getAudioBuffer(gbp_core);
   for (;;)
   {
      size_t frames = mAudioBufferRead(audio, gbp_audio, GBP_HLE_AUDIO_FRAMES);
      if (frames == 0)
         break;
      if (gbp_audio_batch_cb)
         gbp_audio_batch_cb(gbp_audio, frames);
      if (frames < GBP_HLE_AUDIO_FRAMES)
         break;
   }
}

void gbp_hle_reset(void)
{
   if (gbp_core)
      gbp_core->reset(gbp_core);
}

size_t gbp_hle_serialize_size(void)
{
   return gbp_core ? gbp_core->stateSize(gbp_core) : 0;
}

int gbp_hle_serialize(void *data, size_t size)
{
   if (!gbp_core || size < gbp_core->stateSize(gbp_core))
      return 0;
   return gbp_core->saveState(gbp_core, data) ? 1 : 0;
}

int gbp_hle_unserialize(const void *data, size_t size)
{
   if (!gbp_core || size < gbp_core->stateSize(gbp_core))
      return 0;
   return gbp_core->loadState(gbp_core, data) ? 1 : 0;
}
