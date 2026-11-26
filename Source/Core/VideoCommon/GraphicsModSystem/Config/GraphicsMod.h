// Copyright 2022 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

#include "Common/CommonTypes.h"
#include "VideoCommon/GraphicsModSystem/Config/GraphicsModFeature.h"
#include "VideoCommon/GraphicsModSystem/Config/GraphicsTargetGroup.h"

// Zeroed is empty, and FreeGraphicsModConfig frees what it owns; it can be moved as bytes.
struct GraphicsModConfig
{
  char* m_title;  // owned, null when the mod gives none
  bool m_enabled;
  u16 m_weight;
  char* m_relative_path;  // owned

  enum class Source
  {
    User,
    System
  };
  Source m_source;

  // Each grown by doubling
  GraphicsTargetGroupConfig* m_groups;
  u32 m_group_count;
  u32 m_group_capacity;
  GraphicsModFeatureConfig* m_features;
  u32 m_feature_count;
  u32 m_feature_capacity;

  // False, with the problem logged, for a file that is not a graphics mod; config is zeroed or
  // freed before, and is to be freed after either way.
  static bool Create(const std::string& file, Source source, GraphicsModConfig* config);

  std::string GetAbsolutePath() const;
};

void FreeGraphicsModConfig(GraphicsModConfig* config);

// Mods, grown by doubling; zeroed is empty, freed with FreeGraphicsModList
struct GraphicsModList
{
  GraphicsModConfig* mods;
  u32 count;
  u32 capacity;
};

// A new zeroed mod at the end, or nullptr when out of memory
GraphicsModConfig* AddGraphicsMod(GraphicsModList* list);
void FreeGraphicsModList(GraphicsModList* list);

/* The mods a game's profile lists, with its enabled and weight applied, added to mods; false where
 * the profile cannot be read. */
bool LoadGraphicsModProfile(const std::string& file, GraphicsModList* mods);
