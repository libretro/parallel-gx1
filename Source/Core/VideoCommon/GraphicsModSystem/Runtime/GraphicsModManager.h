// Copyright 2022 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "VideoCommon/GraphicsModSystem/Runtime/FBInfo.h"
#include "VideoCommon/GraphicsModSystem/Runtime/GraphicsModAction.h"
#include "VideoCommon/XFMemory.h"

class GraphicsModGroupConfig;

// The actions a target runs, in the order the mods name them; lent until the next Load.
struct GraphicsModActionList
{
  GraphicsModAction* const* actions;
  u32 count;
};

class GraphicsModManager
{
public:
  GraphicsModManager() = default;
  ~GraphicsModManager();
  GraphicsModManager(const GraphicsModManager&) = delete;
  GraphicsModManager& operator=(const GraphicsModManager&) = delete;

  bool Initialize();

  GraphicsModActionList GetProjectionActions(ProjectionType projection_type) const;
  GraphicsModActionList GetProjectionTextureActions(ProjectionType projection_type,
                                                    const char* texture_name) const;
  GraphicsModActionList GetDrawStartedActions(const char* texture_name) const;
  GraphicsModActionList GetTextureLoadActions(const char* texture_name) const;
  GraphicsModActionList GetEFBActions(const FBInfo& efb) const;

  void Load(const GraphicsModGroupConfig& config);

private:
  void Reset();

  class DecoratedAction;

  // A target and an action it runs, as Load finds them; keyed by kind and name, "d<texture>"
  // for a draw say.
  struct Link
  {
    char* key;
    GraphicsModAction* action;
    u32 order;
  };
  void AddLink(const char* key, GraphicsModAction* action);
  static int CompareLinks(const void* a, const void* b);
  // Groups the links by key into m_targets, their actions into m_linked.
  void BuildTargets();
  GraphicsModActionList Find(const char* key) const;

  // Owned
  GraphicsModAction** m_actions = nullptr;
  u32 m_num_actions = 0;
  u32 m_actions_capacity = 0;

  Link* m_links = nullptr;
  u32 m_num_links = 0;
  u32 m_links_capacity = 0;

  // Each target's actions, a run of m_linked
  struct Target
  {
    const char* key;  // a link's
    u32 first;
    u32 count;
  };
  Target* m_targets = nullptr;
  u32 m_num_targets = 0;
  GraphicsModAction** m_linked = nullptr;
  // Open-addressed: key hash to target, NO_TARGET where free; a power of two in size.
  u32* m_index = nullptr;
  u32 m_index_capacity = 0;
};

extern GraphicsModManager* g_graphics_mod_manager;
