// Copyright 2022 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "VideoCommon/GraphicsModSystem/Config/GraphicsModAsset.h"
#include "VideoCommon/GraphicsModSystem/Config/GraphicsModFeature.h"
#include "VideoCommon/GraphicsModSystem/Config/GraphicsTargetGroup.h"

struct GraphicsModConfig
{
  std::string m_title;
  std::string m_author;
  std::string m_description;
  bool m_enabled = false;
  u16 m_weight = 0;
  std::string m_relative_path;

  enum class Source
  {
    User,
    System
  };
  Source m_source = Source::User;

  std::vector<GraphicsTargetGroupConfig> m_groups;
  std::vector<GraphicsModFeatureConfig> m_features;
  std::vector<GraphicsModAssetConfig> m_assets;

  static std::optional<GraphicsModConfig> Create(const std::string& file, Source source);

  std::string GetAbsolutePath() const;
};

/* The mods a game's profile lists, with its enabled and weight applied; nullopt where
 * the profile cannot be read. */
std::optional<std::vector<GraphicsModConfig>> LoadGraphicsModProfile(const std::string& file);
