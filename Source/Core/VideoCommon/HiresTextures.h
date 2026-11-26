// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "Common/PathList.h"
#include "VideoCommon/TextureInfo.h"

namespace VideoCommon
{
class TextureDataResource;
}

enum class TextureFormat;

// Adds the directories under root_directory for game_id to result, which is then sorted and its
// repeats dropped.
void GetTextureDirectoriesWithGameId(const char* root_directory, const char* game_id,
                                     path_list_t* result);

// Tries each id in priority order, leaving in result the directories for the first id that
// matches anything. Leaves result empty if none of the ids match.
void GetTextureDirectoriesForFirstMatchingGameId(const char* root_directory,
                                                 const std::vector<std::string>& game_ids,
                                                 path_list_t* result);

class HiresTexture
{
public:
  static void Update();
  static void Clear();
  static void Shutdown();
  // The pack's texture for texture_info, null when the pack has none; kept until Clear.
  static HiresTexture* Search(const TextureInfo& texture_info);

  HiresTexture(bool has_arbitrary_mipmaps, std::string id);

  bool HasArbitraryMipmaps() const { return m_has_arbitrary_mipmaps; }
  VideoCommon::TextureDataResource* LoadTexture() const;
  const std::string& GetId() const { return m_id; }

private:
  bool m_has_arbitrary_mipmaps = false;
  std::string m_id;
};
