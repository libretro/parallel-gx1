// Copyright 2023 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/TextureUtils.h"

#include <cinttypes>
#include <cstdio>

#include "Common/FileSearch.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"

#include "Core/ConfigManager.h"

#include "VideoCommon/AbstractTexture.h"

namespace
{
std::string BuildDumpTextureFilename(std::string basename, u32 level, bool is_arbitrary)
{
  if (is_arbitrary)
    basename += "_arb";

  if (level > 0)
  {
    char mip[24];
    snprintf(mip, sizeof(mip), "_mip%u", level);
    basename += mip;
  }

  return basename;
}
}  // namespace
namespace VideoCommon::TextureUtils
{
void DumpTexture(const ::AbstractTexture& texture, std::string basename, u32 level,
                 bool is_arbitrary)
{
  const std::string dump_dir =
      File::GetUserPath(D_DUMPTEXTURES_IDX) + SConfig::GetInstance().GetGameID();

  if (!File::IsDirectory(dump_dir))
    File::CreateDir(dump_dir);

  const std::string name = BuildDumpTextureFilename(std::move(basename), level, is_arbitrary);
  const std::string filename = dump_dir + "/" + name + ".png";

  if (File::Exists(filename))
    return;

  texture.Save(filename, level);
}

void TextureDumper::DumpTexture(const ::AbstractTexture& texture, std::string basename, u32 level,
                                bool is_arbitrary)
{
  const std::string dump_dir =
      File::GetUserPath(D_DUMPTEXTURES_IDX) + SConfig::GetInstance().GetGameID();

  if (!m_dumped_textures.count)
  {
    if (!File::IsDirectory(dump_dir))
      File::CreateDir(dump_dir);

    path_list_t filenames;
    path_list_init(&filenames);
    Common::DoFileSearch(dump_dir.c_str(), ".png", true, &filenames);
    for (unsigned i = 0; i < filenames.count; i++)
    {
      std::string name;
      SplitPath(path_list_get(&filenames, i), nullptr, &name, nullptr);
      string_set_add(&m_dumped_textures, name.c_str(), name.size());
    }
    path_list_free(&filenames);

    NOTICE_LOG(VIDEO, "Found %" PRIu64 " dumped textures that will not be re-dumped.",
               (u64)m_dumped_textures.count);
  }

  const std::string name = BuildDumpTextureFilename(std::move(basename), level, is_arbitrary);
  // a name the set has, or cannot take, is not dumped
  if (!string_set_add(&m_dumped_textures, name.c_str(), name.size()))
    return;

  texture.Save(dump_dir + "/" + name + ".png", level);
}
}  // namespace VideoCommon::TextureUtils
