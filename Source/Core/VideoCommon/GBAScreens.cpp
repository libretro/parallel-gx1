// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/GBAScreens.h"

#include <cstdlib>
#include <memory>

#include <retro_atomic.h>
#include <rthreads/retro_eventcount.h>

#include "Common/MathUtil.h"
#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/AbstractStagingTexture.h"
#include "VideoCommon/AbstractTexture.h"
#include "VideoCommon/PostProcessing.h"
#include "VideoCommon/TextureConfig.h"

enum
{
  SCREEN_BUFFERS = 3,
  /* the exchange word: the buffer in the middle, whether the GPU thread
   * has not taken it yet, and the size of its frame */
  EXCHANGE_INDEX = 3,
  EXCHANGE_FRESH = 4,
  EXCHANGE_WIDTH_SHIFT = 3,
  EXCHANGE_HEIGHT_SHIFT = 12,
  EXCHANGE_SIZE_MASK = 0x1FF,

  SCREEN_STOPPED = 0,
  SCREEN_RUNNING = 1,
  /* the buffers are being taken back: the GBA waits to stop */
  SCREEN_REVOKING = 2,

  INIT_NONE = 0,
  INIT_BUSY = 1,
  INIT_DONE = 2
};

/* Between a GBA's thread (the writer) and the GPU thread (the reader).
 * The buffers, their stride and writer_index are set by the GPU thread
 * before it sets lent; the writer reads them, and from then on owns
 * writer_index and attached, until it was told to let go. */
struct GBAScreen
{
  u32* buffers[SCREEN_BUFFERS];
  size_t stride;
  void (*detach)(void* context);
  void* detach_context;
  retro_eventcount_t event;
  retro_atomic_int_t lent;
  retro_atomic_int_t exchange;
  retro_atomic_int_t state;
  retro_atomic_int_t detached;
  int writer_index;
  int attached;
};

/* The GPU thread's side of a screen. */
struct GBAScreenView
{
  std::unique_ptr<AbstractTexture> texture;
  std::unique_ptr<AbstractStagingTexture> staging[SCREEN_BUFFERS];
  /* without upload memory that stays mapped: the three buffers */
  u32* memory;
  /* the buffer the texture was last filled from, given back to the GBA
   * once that is done */
  int copying;
  /* width << 16 | height of what the texture holds; 0 for nothing */
  u32 shown_size;
  /* the GPU had nothing to lend: not asked again until the backend is */
  bool lend_failed;
};

static GBAScreen s_screens[GBA_SCREENS];
static GBAScreenView s_views[GBA_SCREENS];
static retro_atomic_int_t s_shown;
static retro_atomic_int_t s_init;

static void ScreensInit()
{
  unsigned i;

  if (retro_atomic_load_acquire_int(&s_init) == INIT_DONE)
    return;
  if (!retro_atomic_cas_int(&s_init, INIT_NONE, INIT_BUSY))
  {
    while (retro_atomic_load_acquire_int(&s_init) != INIT_DONE)
    {
    }
    return;
  }
  for (i = 0; i < GBA_SCREENS; i++)
  {
    retro_eventcount_init(&s_screens[i].event);
    retro_atomic_int_init(&s_screens[i].lent, 0);
    retro_atomic_int_init(&s_screens[i].exchange, 0);
    retro_atomic_int_init(&s_screens[i].state, SCREEN_STOPPED);
    retro_atomic_int_init(&s_screens[i].detached, 0);
  }
  retro_atomic_store_release_int(&s_init, INIT_DONE);
}

/* Waits on the screen's event until test(screen) holds. */
static void ScreenWait(GBAScreen* screen, bool (*test)(GBAScreen* screen))
{
  while (!test(screen))
  {
    const int key = retro_eventcount_prepare_wait(&screen->event);
    if (test(screen))
    {
      retro_eventcount_cancel_wait(&screen->event);
      break;
    }
    retro_eventcount_commit_wait(&screen->event, key);
  }
}

static bool ScreenNotRevoking(GBAScreen* screen)
{
  return retro_atomic_load_acquire_int(&screen->state) != SCREEN_REVOKING;
}

static bool ScreenDetached(GBAScreen* screen)
{
  return retro_atomic_load_acquire_int(&screen->detached) != 0;
}

void GBAScreensSetShown(bool shown)
{
  retro_atomic_store_relaxed_int(&s_shown, shown ? 1 : 0);
}

void GBAScreenStart(unsigned gba, void (*detach)(void* context), void* context)
{
  GBAScreen* const screen = &s_screens[gba];

  ScreensInit();
  screen->detach = detach;
  screen->detach_context = context;
  screen->attached = 0;
  retro_atomic_store_release_int(&screen->state, SCREEN_RUNNING);
}

void GBAScreenStop(unsigned gba)
{
  GBAScreen* const screen = &s_screens[gba];

  ScreensInit();
  for (;;)
  {
    if (retro_atomic_cas_int(&screen->state, SCREEN_RUNNING, SCREEN_STOPPED))
      break;
    if (retro_atomic_load_acquire_int(&screen->state) == SCREEN_STOPPED)
      break;
    /* the buffers are being taken back: the GBA's thread, which would
     * have let go, is gone, so nothing renders into them any more */
    GBAScreenDetached(gba);
    ScreenWait(screen, ScreenNotRevoking);
  }
}

u32* GBAScreenFrameEnd(unsigned gba, u32 width, u32 height, size_t* stride)
{
  GBAScreen* const screen = &s_screens[gba];

  if (!retro_atomic_load_acquire_int(&screen->lent))
  {
    screen->attached = 0;
    return nullptr;
  }
  if (!screen->attached)
  {
    /* the frame that ended went into the GBA's own buffer */
    screen->attached = 1;
  }
  else
  {
    if (width > GBA_SCREEN_MAX_WIDTH)
      width = GBA_SCREEN_MAX_WIDTH;
    if (height > GBA_SCREEN_MAX_HEIGHT)
      height = GBA_SCREEN_MAX_HEIGHT;
    screen->writer_index =
        retro_atomic_exchange_int(&screen->exchange,
                                  screen->writer_index | EXCHANGE_FRESH |
                                      (int)(width << EXCHANGE_WIDTH_SHIFT) |
                                      (int)(height << EXCHANGE_HEIGHT_SHIFT)) &
        EXCHANGE_INDEX;
  }
  *stride = screen->stride;
  return screen->buffers[screen->writer_index];
}

u32* GBAScreenBuffer(unsigned gba, size_t* stride)
{
  GBAScreen* const screen = &s_screens[gba];

  if (!screen->attached || !retro_atomic_load_acquire_int(&screen->lent))
    return nullptr;
  *stride = screen->stride;
  return screen->buffers[screen->writer_index];
}

void GBAScreenDetached(unsigned gba)
{
  GBAScreen* const screen = &s_screens[gba];

  screen->attached = 0;
  retro_atomic_store_release_int(&screen->detached, 1);
  retro_eventcount_notify(&screen->event);
}

static void ViewFree(GBAScreenView* view)
{
  int i;

  for (i = 0; i < SCREEN_BUFFERS; i++)
    view->staging[i].reset();
  view->texture.reset();
  std::free(view->memory);
  view->memory = nullptr;
  view->shown_size = 0;
  view->lend_failed = false;
}

/* Lends the GBA its buffers: upload memory that stays mapped, or plain
 * memory uploaded from. */
static bool ScreenLend(unsigned gba)
{
  GBAScreen* const screen = &s_screens[gba];
  GBAScreenView* const view = &s_views[gba];
  const TextureConfig config(GBA_SCREEN_MAX_WIDTH, GBA_SCREEN_MAX_HEIGHT, 1, 1, 1,
                             AbstractTextureFormat::RGBA8, 0, AbstractTextureType::Texture_2DArray);
  int i;

  if (view->lend_failed)
    return false;
  view->texture = g_gfx->CreateTexture(config, "GBA screen");
  if (!view->texture)
  {
    view->lend_failed = true;
    return false;
  }

  for (i = 0; i < SCREEN_BUFFERS; i++)
  {
    view->staging[i] = g_gfx->CreateStagingTexture(StagingTextureType::Upload, config);
    if (!view->staging[i] || !view->staging[i]->m_persistent_map || !view->staging[i]->Map() ||
        (view->staging[i]->GetMappedStride() & 3) != 0)
    {
      break;
    }
  }
  if (i == SCREEN_BUFFERS)
  {
    for (i = 0; i < SCREEN_BUFFERS; i++)
      screen->buffers[i] = reinterpret_cast<u32*>(view->staging[i]->GetMappedPointer());
    screen->stride = view->staging[0]->GetMappedStride() / sizeof(u32);
  }
  else
  {
    const size_t plane = (size_t)GBA_SCREEN_MAX_WIDTH * GBA_SCREEN_MAX_HEIGHT;

    for (i = 0; i < SCREEN_BUFFERS; i++)
      view->staging[i].reset();
    view->memory = static_cast<u32*>(std::calloc(plane * SCREEN_BUFFERS, sizeof(u32)));
    if (!view->memory)
    {
      ViewFree(view);
      view->lend_failed = true;
      return false;
    }
    for (i = 0; i < SCREEN_BUFFERS; i++)
      screen->buffers[i] = view->memory + plane * i;
    screen->stride = GBA_SCREEN_MAX_WIDTH;
  }

  /* the GBA writes 0 and the GPU copies from 2; 1 is in the middle */
  screen->writer_index = 0;
  retro_atomic_store_relaxed_int(&screen->exchange, 1);
  view->copying = 2;
  view->shown_size = 0;
  retro_atomic_store_release_int(&screen->lent, 1);
  return true;
}

/* Whether the GPU is done with the buffer the texture was filled from. */
static bool ViewCopyDone(GBAScreenView* view)
{
  return !view->staging[view->copying] || StagingTransferDone(view->staging[view->copying].get());
}

static void ViewUpload(GBAScreenView* view, int index, u32 width, u32 height)
{
  if (view->staging[index])
  {
    const MathUtil::Rectangle<int> rect(0, 0, (int)width, (int)height);
    view->staging[index]->CopyToTexture(rect, view->texture.get(), rect, 0, 0);
  }
  else
  {
    const size_t plane = (size_t)GBA_SCREEN_MAX_WIDTH * GBA_SCREEN_MAX_HEIGHT;
    view->texture->Load(0, GBA_SCREEN_MAX_WIDTH, GBA_SCREEN_MAX_HEIGHT, GBA_SCREEN_MAX_WIDTH,
                        reinterpret_cast<const u8*>(view->memory + plane * index),
                        plane * sizeof(u32));
  }
}

bool GBAScreensUpdate()
{
  bool any = false;
  unsigned i;

  if (!retro_atomic_load_relaxed_int(&s_shown) ||
      retro_atomic_load_acquire_int(&s_init) != INIT_DONE)
  {
    return false;
  }

  for (i = 0; i < GBA_SCREENS; i++)
  {
    GBAScreen* const screen = &s_screens[i];
    GBAScreenView* const view = &s_views[i];
    int word;

    if (retro_atomic_load_relaxed_int(&screen->state) == SCREEN_STOPPED)
    {
      view->shown_size = 0;
      continue;
    }
    if (!retro_atomic_load_relaxed_int(&screen->lent) && !ScreenLend(i))
      continue;

    word = retro_atomic_load_acquire_int(&screen->exchange);
    if ((word & EXCHANGE_FRESH) && ViewCopyDone(view))
    {
      /* the GBA only ever leaves it fresh: what is swapped out is */
      word = retro_atomic_exchange_int(&screen->exchange, view->copying);
      {
        const u32 width = ((u32)word >> EXCHANGE_WIDTH_SHIFT) & EXCHANGE_SIZE_MASK;
        const u32 height = ((u32)word >> EXCHANGE_HEIGHT_SHIFT) & EXCHANGE_SIZE_MASK;
        view->copying = word & EXCHANGE_INDEX;
        ViewUpload(view, view->copying, width, height);
        view->shown_size = width << 16 | height;
      }
    }
    if (view->shown_size)
      any = true;
  }
  return any;
}

void GBAScreensDraw(VideoCommon::PostProcessing* post_processor, int width, int height)
{
  u32 sizes[GBA_SCREENS];
  const AbstractTexture* textures[GBA_SCREENS];
  u32 total_width = 0;
  u32 count = 0;
  u32 scale;
  u32 i;
  int right = width;

  for (i = 0; i < GBA_SCREENS; i++)
  {
    if (!s_views[i].shown_size)
      continue;
    sizes[count] = s_views[i].shown_size;
    textures[count] = s_views[i].texture.get();
    total_width += sizes[count] >> 16;
    count++;
  }
  if (!count || width <= 0 || height <= 0)
    return;

  /* whole pixels, about a third of the picture's height */
  scale = (u32)height / 480;
  if (scale < 1)
    scale = 1;

  for (i = count; i-- > 0;)
  {
    const int src_width = (int)(sizes[i] >> 16);
    const int src_height = (int)(sizes[i] & 0xFFFF);
    int dst_width = src_width * (int)scale;
    int dst_height = src_height * (int)scale;

    /* side by side across the picture when wider than it */
    if (total_width * scale > (u32)width)
    {
      dst_width = (int)((s64)src_width * width / total_width);
      dst_height = (int)((s64)src_height * width / total_width);
    }
    post_processor->BlitScreen(
        MathUtil::Rectangle<int>(right - dst_width, height - dst_height, right, height),
        MathUtil::Rectangle<int>(0, 0, src_width, src_height), textures[i]);
    right -= dst_width;
  }
}

void GBAScreensRevoke()
{
  unsigned i;

  if (retro_atomic_load_acquire_int(&s_init) != INIT_DONE)
    return;

  for (i = 0; i < GBA_SCREENS; i++)
  {
    GBAScreen* const screen = &s_screens[i];

    if (retro_atomic_load_relaxed_int(&screen->lent))
    {
      retro_atomic_store_release_int(&screen->lent, 0);
      /* a running GBA lets go on its own thread first */
      if (retro_atomic_cas_int(&screen->state, SCREEN_RUNNING, SCREEN_REVOKING))
      {
        retro_atomic_store_release_int(&screen->detached, 0);
        screen->detach(screen->detach_context);
        ScreenWait(screen, ScreenDetached);
        retro_atomic_store_release_int(&screen->state, SCREEN_RUNNING);
        retro_eventcount_notify(&screen->event);
      }
    }
    ViewFree(&s_views[i]);
  }
}
