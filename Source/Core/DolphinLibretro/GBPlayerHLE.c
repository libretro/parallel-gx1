/* Copyright 2026 Dolphin Emulator Project
 * SPDX-License-Identifier: GPL-2.0-or-later */

/* See GBPlayerHLE.h. */

#include <stdarg.h>
#include <string.h>

#include <mgba-util/audio-buffer.h>
#include <mgba-util/image.h>
#include <mgba-util/vfs.h>
#include <mgba/core/cheats.h>
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
/* mGBA's ring, emptied every frame, holds this many stereo frames: a GBA
 * frame makes about 550, a Game Boy one, at its 131072 Hz, about 2200 */
#define GBP_HLE_AUDIO_FRAMES 4096
#define GBP_HLE_KEYS 10
/* more than either handheld has RAM blocks */
#define GBP_HLE_MAX_MAPS 16
/* the longest code line the frontend hands over is copied here to split */
#define GBP_HLE_MAX_CHEAT 1024

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
/* the frontend's copy when it lends no framebuffer, blue in the low byte
 * where mGBA has red */
static uint32_t gbp_frame[GBP_HLE_MAX_WIDTH * GBP_HLE_MAX_HEIGHT];

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

static const struct retro_input_descriptor gbp_input_descriptors[] = {
   { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "A" },
   { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "B" },
   { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select" },
   { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "Start" },
   { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "Right" },
   { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "Left" },
   { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "Up" },
   { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "Down" },
   { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,      "R" },
   { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,      "L" },
   { 0, 0, 0, 0, NULL }
};

/* The handheld's RAM where it sits in its own address space, for
 * the frontend's achievements and cheat search */
static struct retro_memory_descriptor gbp_maps[GBP_HLE_MAX_MAPS];
/* the working RAM: the frontend's system RAM */
static void *gbp_wram;
static size_t gbp_wram_size;
/* the frontend's codes, one set; made on the first code */
static struct mCheatSet *gbp_cheats;

static void gbp_audio_restart(void);

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

/* Every block of RAM mGBA lists, lent where it lies */
static void gbp_set_memory_maps(void)
{
   const struct mCoreMemoryBlock *blocks;
   struct retro_memory_map map;
   size_t count = gbp_core->listMemoryBlocks(gbp_core, &blocks);
   size_t i;
   unsigned n = 0;

   gbp_wram      = NULL;
   gbp_wram_size = 0;
   for (i = 0; i < count && n < GBP_HLE_MAX_MAPS; i++)
   {
      const struct mCoreMemoryBlock *b = &blocks[i];
      size_t size                      = 0;
      void *ptr;

      if ((b->flags & mCORE_MEMORY_VIRTUAL) || !(b->flags & mCORE_MEMORY_WRITE))
         continue;
      if (!(ptr = gbp_core->getMemoryBlock(gbp_core, b->id, &size)) || !size)
         continue;
      /* the working RAM whole, banks and all */
      if (!strcmp(b->internalName, "wram"))
      {
         gbp_wram      = ptr;
         gbp_wram_size = size;
      }
      /* a banked block shows its first banks, as after a reset */
      if (b->maxSegment && size > b->end - b->start)
         size = b->end - b->start;
      memset(&gbp_maps[n], 0, sizeof(gbp_maps[n]));
      gbp_maps[n].flags = RETRO_MEMDESC_SYSTEM_RAM;
      gbp_maps[n].ptr   = ptr;
      gbp_maps[n].start = b->start;
      gbp_maps[n].len   = size;
      n++;
   }
   map.descriptors     = gbp_maps;
   map.num_descriptors = n;
   gbp_environ_cb(RETRO_ENVIRONMENT_SET_MEMORY_MAPS, &map);
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
   gbp_audio_restart();
   gbp_set_memory_maps();
   gbp_environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS,
         (void*)gbp_input_descriptors);
   return 1;
}

void gbp_hle_unload(void)
{
   if (!gbp_core)
      return;
   gbp_set_rumble(&gbp_rumble, 0.0f);
   gbp_cheats    = NULL;
   gbp_wram      = NULL;
   gbp_wram_size = 0;
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
   struct retro_framebuffer fb;
   struct mAudioBuffer *audio;
   unsigned char *out;
   size_t pitch;
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
    * one pass that swaps them writes the frontend's own framebuffer when it
    * lends one, the core's copy when not. mGBA keeps drawing into its own:
    * a line it does not redraw keeps what it drew, which a buffer the
    * frontend may hand out anew each frame would not. */
   gbp_core->currentVideoSize(gbp_core, &width, &height);
   memset(&fb, 0, sizeof(fb));
   fb.width        = width;
   fb.height       = height;
   fb.access_flags = RETRO_MEMORY_ACCESS_WRITE;
   if (gbp_environ_cb(RETRO_ENVIRONMENT_GET_CURRENT_SOFTWARE_FRAMEBUFFER, &fb)
         && fb.data && fb.format == RETRO_PIXEL_FORMAT_XRGB8888
         && fb.width == width && fb.height == height
         && fb.pitch >= width * sizeof(uint32_t))
   {
      out   = (unsigned char*)fb.data;
      pitch = fb.pitch;
   }
   else
   {
      out   = (unsigned char*)gbp_frame;
      pitch = GBP_HLE_MAX_WIDTH * sizeof(gbp_frame[0]);
   }
   for (y = 0; y < height; y++)
   {
      const mColor *src = gbp_video + (size_t)y * GBP_HLE_MAX_WIDTH;
      uint32_t *dst     = (uint32_t*)(out + (size_t)y * pitch);
      unsigned x;
      for (x = 0; x < width; x++)
      {
         const uint32_t c = src[x];
         dst[x] = (c & 0xff00u) | ((c & 0xffu) << 16) | ((c >> 16) & 0xffu);
      }
   }
   if (gbp_video_cb)
      gbp_video_cb(out, width, height, pitch);

   /* The frame's samples, where mGBA wrote them: the ring is emptied after
    * each frame, so they start at its beginning, in one run */
   audio = gbp_core->getAudioBuffer(gbp_core);
   if (audio->data.readPtr == audio->data.data && gbp_audio_batch_cb)
   {
      size_t frames = mAudioBufferAvailable(audio);
      if (frames)
         gbp_audio_batch_cb((const int16_t*)audio->data.data, frames);
   }
   mAudioBufferClear(audio);
}

/* mGBA's ring emptied, the next frame's samples written from its start */
static void gbp_audio_restart(void)
{
   mAudioBufferClear(gbp_core->getAudioBuffer(gbp_core));
}

void gbp_hle_reset(void)
{
   if (!gbp_core)
      return;
   gbp_core->reset(gbp_core);
   gbp_audio_restart();
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
   if (!gbp_core || size < gbp_core->stateSize(gbp_core)
         || !gbp_core->loadState(gbp_core, data))
      return 0;
   gbp_audio_restart();
   return 1;
}

void *gbp_hle_memory_data(unsigned id)
{
   return id == RETRO_MEMORY_SYSTEM_RAM ? gbp_wram : NULL;
}

size_t gbp_hle_memory_size(unsigned id)
{
   return id == RETRO_MEMORY_SYSTEM_RAM ? gbp_wram_size : 0;
}

void gbp_hle_cheat_reset(void)
{
   struct mCheatDevice *device;
   if (!gbp_core || !(device = gbp_core->cheatDevice(gbp_core)))
      return;
   mCheatDeviceClear(device);
   gbp_cheats = NULL;
}

/* code: one cheat's lines, GameShark, Action Replay or CodeBreaker, split
 * on '+' as the frontend joins them */
void gbp_hle_cheat_set(unsigned index, int enabled, const char *code)
{
   char line[GBP_HLE_MAX_CHEAT];
   struct mCheatDevice *device;
   char *cur;
   char *next;

   (void)index;
   if (!enabled || !code || !gbp_core
         || !(device = gbp_core->cheatDevice(gbp_core)))
      return;
   if (!gbp_cheats)
   {
      if (!(gbp_cheats = device->createSet(device, NULL)))
         return;
      mCheatAddSet(device, gbp_cheats);
   }
   strlcpy(line, code, sizeof(line));
   for (cur = line; cur; cur = next)
   {
      if ((next = strchr(cur, '+')))
         *next++ = '\0';
      if (*cur)
         mCheatAddLine(gbp_cheats, cur, 0);
   }
}
