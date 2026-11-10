// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <stddef.h>

#include "Common/CommonTypes.h"

/* A run of texture bytes. Reads through TexDecoder_DecodeTexel() past its
 * end give zeros. */
struct TexBytes
{
  const u8* data;
  size_t size;
};

/* The bytes from offset on; none past the end. */
inline TexBytes TexBytesSkip(TexBytes bytes, size_t offset)
{
  if (offset > bytes.size)
    offset = bytes.size;
  bytes.data += offset;
  bytes.size -= offset;
  return bytes;
}
