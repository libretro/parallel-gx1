// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "Common/CommonTypes.h"

/* A shader's code for the GPU (SPIR-V or DXBC): the block it lives in,
 * owned by the shader, and the code within it. rslc's result block is
 * kept whole, the code pointing into it, and given back with release
 * (shader_arena_blob_release); code read back from the disk cache gets a
 * block of its own, freed with free() (release NULL). */
struct ShaderBinary
{
  void* block;
  const u8* code;
  size_t size;
  void (*release)(void* block);
};

static inline void ShaderBinaryInit(ShaderBinary* binary)
{
  binary->block = NULL;
  binary->code = NULL;
  binary->size = 0;
  binary->release = NULL;
}

static inline void ShaderBinaryFree(ShaderBinary* binary)
{
  if (binary->release)
    binary->release(binary->block);
  else
    free(binary->block);
  ShaderBinaryInit(binary);
}

/* Takes the block a binary owns, leaving it empty. */
static inline ShaderBinary ShaderBinaryTake(ShaderBinary* binary)
{
  ShaderBinary taken = *binary;
  ShaderBinaryInit(binary);
  return taken;
}

/* Code from elsewhere, copied into a block of its own. False if it could
 * not be allocated. */
static inline bool ShaderBinaryCopy(ShaderBinary* binary, const void* data, size_t size)
{
  ShaderBinaryInit(binary);
  binary->block = malloc(size ? size : 1);
  if (!binary->block)
    return false;
  if (size)
    memcpy(binary->block, data, size);
  binary->code = static_cast<const u8*>(binary->block);
  binary->size = size;
  return true;
}
