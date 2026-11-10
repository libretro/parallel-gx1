// Copyright 2016 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/Image.h"

#include <cstdlib>
#include <string>

#include <formats/image.h>
#include <formats/rpng.h>
#include <libretro.h>
#include <streams/interface_stream.h>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"

namespace Common
{
bool LoadPNG(const u8* input, size_t input_size, Common::UniqueBuffer<u8>* data_out,
             u32* width_out, u32* height_out)
{
  struct rpng_info info;
  rpng_t* rpng = rpng_alloc();
  void* out = nullptr;
  unsigned width = 0, height = 0;
  int ret = IMAGE_PROCESS_ERROR;

  if (!rpng)
    return false;
  /* rpng only reads the buffer it is lent */
  if (rpng_set_buf_ptr(rpng, const_cast<u8*>(input), input_size) && rpng_start(rpng))
  {
    while (rpng_iterate_image(rpng))
    {
    }
    if (rpng_is_valid(rpng) && rpng_get_info(rpng, &info))
    {
      /* decoded straight into the caller's buffer: R, G, B, A bytes */
      data_out->reset(static_cast<size_t>(info.width) * info.height * 4);
      if (rpng_set_output(rpng, reinterpret_cast<uint32_t*>(data_out->data()), info.width))
      {
        do
          ret = rpng_process_image(rpng, &out, input_size, &width, &height, true);
        while (ret == IMAGE_PROCESS_NEXT);
      }
    }
  }
  rpng_free(rpng);
  if (ret != IMAGE_PROCESS_END)
    return false;
  *width_out = width;
  *height_out = height;
  return true;
}

bool SavePNG(const std::string& path, const u8* input, u32 width, u32 height, u32 stride)
{
  intfstream_t* stream = intfstream_open_file(path.c_str(), RETRO_VFS_FILE_ACCESS_WRITE,
                                              RETRO_VFS_FILE_ACCESS_HINT_NONE);
  bool ok;

  if (!stream)
    return false;
  ok = rpng_save_image_stream_fmt(input, stream, width, height, static_cast<signed>(stride),
                                  RPNG_PIXFMT_RGBA32, nullptr);
  intfstream_close(stream);
  free(stream);
  if (!ok)
    ERROR_LOG_FMT(FRAMEDUMP, "Failed to save {} by {} image to {}", width, height, path);
  return ok;
}
}  // namespace Common
