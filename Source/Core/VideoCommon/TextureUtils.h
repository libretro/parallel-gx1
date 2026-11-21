// Copyright 2023 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

#include "Common/CommonTypes.h"
#include "Common/StringSet.h"

class AbstractTexture;

namespace VideoCommon::TextureUtils
{
class TextureDumper
{
public:
  TextureDumper() : m_dumped_textures{} {}
  ~TextureDumper() { string_set_free(&m_dumped_textures); }
  TextureDumper(const TextureDumper&) = delete;
  TextureDumper& operator=(const TextureDumper&) = delete;

  // Only dumps if texture did not already exist anywhere within the dump-textures path.
  void DumpTexture(const ::AbstractTexture& texture, std::string basename, u32 level,
                   bool is_arbitrary);

private:
  // The names of the textures dumped, or found dumped before
  string_set_t m_dumped_textures;
};

void DumpTexture(const ::AbstractTexture& texture, std::string basename, u32 level,
                 bool is_arbitrary);

}  // namespace VideoCommon::TextureUtils
