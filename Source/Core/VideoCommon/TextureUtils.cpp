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

  if (m_dumped_textures.empty())
  {
    if (!File::IsDirectory(dump_dir))
      File::CreateDir(dump_dir);

    for (auto& filename : Common::DoFileSearch(dump_dir, ".png", true))
    {
      std::string name;
      SplitPath(filename, nullptr, &name, nullptr);
      m_dumped_textures.insert(name);
    }

    NOTICE_LOG(VIDEO, "Found %" PRIu64 " dumped textures that will not be re-dumped.",
               (u64)m_dumped_textures.size());
  }

  const std::string name = BuildDumpTextureFilename(std::move(basename), level, is_arbitrary);
  const bool file_existed = !m_dumped_textures.insert(name).second;
  if (file_existed)
    return;

  texture.Save(dump_dir + "/" + name + ".png", level);
}
}  // namespace VideoCommon::TextureUtils
