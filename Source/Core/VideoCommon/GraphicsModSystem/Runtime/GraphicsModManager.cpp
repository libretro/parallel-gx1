// Copyright 2022 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/GraphicsModSystem/Runtime/GraphicsModManager.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"

#include <file/file_path.h>

#include "Core/ConfigManager.h"

#include "VideoCommon/GraphicsModSystem/Config/GraphicsMod.h"
#include "VideoCommon/GraphicsModSystem/Config/GraphicsModGroup.h"
#include "VideoCommon/GraphicsModSystem/Constants.h"
#include "VideoCommon/GraphicsModSystem/Runtime/GraphicsModActionFactory.h"
#include "VideoCommon/TextureInfo.h"
#include "VideoCommon/VideoConfig.h"

GraphicsModManager* g_graphics_mod_manager;

class GraphicsModManager::DecoratedAction final : public GraphicsModAction
{
public:
  // Takes action
  DecoratedAction(GraphicsModAction* action, GraphicsModConfig mod)
      : m_action_impl(action), m_mod(std::move(mod))
  {
  }
  ~DecoratedAction() override { delete m_action_impl; }
  DecoratedAction(const DecoratedAction&) = delete;
  DecoratedAction& operator=(const DecoratedAction&) = delete;
  void OnDrawStarted(GraphicsModActionData::DrawStarted* draw_started) override
  {
    if (!m_mod.m_enabled)
      return;
    m_action_impl->OnDrawStarted(draw_started);
  }
  void BeforeEFB(GraphicsModActionData::PreEFB* efb) override
  {
    if (!m_mod.m_enabled)
      return;
    m_action_impl->BeforeEFB(efb);
  }
  void OnProjection(GraphicsModActionData::Projection* projection) override
  {
    if (!m_mod.m_enabled)
      return;
    m_action_impl->OnProjection(projection);
  }
  void OnProjectionAndTexture(GraphicsModActionData::Projection* projection) override
  {
    if (!m_mod.m_enabled)
      return;
    m_action_impl->OnProjectionAndTexture(projection);
  }
  void OnTextureLoad(GraphicsModActionData::TextureLoad* texture_load) override
  {
    if (!m_mod.m_enabled)
      return;
    m_action_impl->OnTextureLoad(texture_load);
  }

private:
  GraphicsModAction* m_action_impl;  // owned
  GraphicsModConfig m_mod;
};

bool GraphicsModManager::Initialize()
{
  if (g_ActiveConfig.bGraphicMods)
  {
    // If a config change occurred in a previous session,
    // remember the old change count value.  By setting
    // our current change count to the old value, we
    // avoid loading the stale data when we
    // check for config changes.
    const u32 old_game_mod_changes = g_ActiveConfig.graphics_mod_config ?
                                         g_ActiveConfig.graphics_mod_config->GetChangeCount() :
                                         0;
    g_ActiveConfig.graphics_mod_config = GraphicsModGroupConfig(SConfig::GetInstance().GetGameID());
    g_ActiveConfig.graphics_mod_config->Load();
    g_ActiveConfig.graphics_mod_config->SetChangeCount(old_game_mod_changes);
    g_graphics_mod_manager->Load(*g_ActiveConfig.graphics_mod_config);
  }

  return true;
}

GraphicsModManager::~GraphicsModManager()
{
  Reset();
}

static constexpr u32 NO_TARGET = 0xFFFFFFFFu;

static u32 HashKey(const char* key)
{
  // FNV-1a
  u32 hash = 2166136261u;
  for (; *key; key++)
    hash = (hash ^ static_cast<u8>(*key)) * 16777619u;
  return hash;
}

GraphicsModActionList GraphicsModManager::Find(const char* key) const
{
  if (!m_num_targets)
    return {nullptr, 0};
  const u32 mask = m_index_capacity - 1;
  for (u32 i = HashKey(key) & mask; m_index[i] != NO_TARGET; i = (i + 1) & mask)
  {
    const Target& target = m_targets[m_index[i]];
    if (!strcmp(target.key, key))
      return {m_linked + target.first, target.count};
  }
  return {nullptr, 0};
}

GraphicsModActionList GraphicsModManager::GetProjectionActions(ProjectionType projection_type) const
{
  if (!m_num_targets)
    return {nullptr, 0};
  char key[16];
  snprintf(key, sizeof(key), "p%d", static_cast<int>(projection_type));
  return Find(key);
}

GraphicsModActionList
GraphicsModManager::GetProjectionTextureActions(ProjectionType projection_type,
                                                const char* texture_name) const
{
  if (!m_num_targets)
    return {nullptr, 0};
  char key[TextureInfo::NAME_SIZE + 16];
  snprintf(key, sizeof(key), "t%s_%d", texture_name, static_cast<int>(projection_type));
  return Find(key);
}

GraphicsModActionList GraphicsModManager::GetDrawStartedActions(const char* texture_name) const
{
  if (!m_num_targets)
    return {nullptr, 0};
  char key[TextureInfo::NAME_SIZE + 16];
  snprintf(key, sizeof(key), "d%s", texture_name);
  return Find(key);
}

GraphicsModActionList GraphicsModManager::GetTextureLoadActions(const char* texture_name) const
{
  if (!m_num_targets)
    return {nullptr, 0};
  char key[TextureInfo::NAME_SIZE + 16];
  snprintf(key, sizeof(key), "l%s", texture_name);
  return Find(key);
}

static void MakeEFBKey(char* key, size_t size, u32 width, u32 height, TextureFormat format)
{
  snprintf(key, size, "e%ux%u_%d", width, height, static_cast<int>(format));
}

GraphicsModActionList GraphicsModManager::GetEFBActions(const FBInfo& efb) const
{
  if (!m_num_targets)
    return {nullptr, 0};
  char key[48];
  MakeEFBKey(key, sizeof(key), efb.m_width, efb.m_height, efb.m_texture_format);
  return Find(key);
}

void GraphicsModManager::AddLink(const char* key, GraphicsModAction* action)
{
  if (m_num_links == m_links_capacity)
  {
    const u32 capacity = m_links_capacity ? m_links_capacity * 2 : 32;
    Link* const links = static_cast<Link*>(realloc(m_links, capacity * sizeof(Link)));
    if (!links)
      abort();
    m_links = links;
    m_links_capacity = capacity;
  }
  Link& link = m_links[m_num_links];
  link.key = strdup(key);
  if (!link.key)
    abort();
  link.action = action;
  link.order = m_num_links++;
}

int GraphicsModManager::CompareLinks(const void* a, const void* b)
{
  const Link* const la = static_cast<const Link*>(a);
  const Link* const lb = static_cast<const Link*>(b);
  const int by_key = strcmp(la->key, lb->key);
  if (by_key)
    return by_key;
  return la->order < lb->order ? -1 : la->order > lb->order;
}

void GraphicsModManager::BuildTargets()
{
  if (!m_num_links)
    return;
  qsort(m_links, m_num_links, sizeof(Link), CompareLinks);

  m_linked = static_cast<GraphicsModAction**>(malloc(m_num_links * sizeof(GraphicsModAction*)));
  m_targets = static_cast<Target*>(malloc(m_num_links * sizeof(Target)));
  u32 capacity = 16;
  while (capacity < m_num_links * 2)
    capacity *= 2;
  m_index = static_cast<u32*>(malloc(capacity * sizeof(u32)));
  if (!m_linked || !m_targets || !m_index)
    abort();
  memset(m_index, 0xFF, capacity * sizeof(u32));
  m_index_capacity = capacity;

  for (u32 i = 0; i < m_num_links; i++)
  {
    m_linked[i] = m_links[i].action;
    if (m_num_targets && !strcmp(m_targets[m_num_targets - 1].key, m_links[i].key))
    {
      m_targets[m_num_targets - 1].count++;
      continue;
    }
    Target& target = m_targets[m_num_targets];
    target.key = m_links[i].key;
    target.first = i;
    target.count = 1;
    u32 slot = HashKey(target.key) & (capacity - 1);
    while (m_index[slot] != NO_TARGET)
      slot = (slot + 1) & (capacity - 1);
    m_index[slot] = m_num_targets++;
  }
}

void GraphicsModManager::Load(const GraphicsModGroupConfig& config)
{
  Reset();

  const auto& mods = config.GetMods();

  std::map<std::string, std::vector<GraphicsTargetConfig>> group_to_targets;
  for (const auto& mod : mods)
  {
    for (const GraphicsTargetGroupConfig& group : mod.m_groups)
    {
      if (group_to_targets.find(group.m_name) != group_to_targets.end())
      {
        WARN_LOG(
            VIDEO,
            "Specified graphics mod group '%s' for mod '%s' is already specified by another mod.",
            group.m_name.c_str(), mod.m_title.c_str());
      }
      group_to_targets[group.m_name];

      const std::string internal_group = mod.m_title + "." + group.m_name;
      for (const GraphicsTargetConfig& target : group.m_targets)
      {
        group_to_targets[group.m_name].push_back(target);
        group_to_targets[internal_group].push_back(target);
      }
    }
  }

  const auto create_action = [](const std::string_view& action_name, const Common::Vec3& xyz,
                                GraphicsModConfig mod_config) -> GraphicsModAction* {
    GraphicsModAction* const action = GraphicsModActionFactory::Create(action_name, xyz);
    if (action == nullptr)
    {
      return nullptr;
    }
    return new DecoratedAction(action, std::move(mod_config));
  };

  for (const auto& mod : mods)
  {
    for (const GraphicsModFeatureConfig& feature : mod.m_features)
    {
      const std::string internal_group = mod.m_title + "." + feature.m_group;

      const auto add_target = [&](const GraphicsTargetConfig& target) {
        GraphicsModAction* const action = m_actions[m_num_actions - 1];
        char key[512];
        switch (target.kind)
        {
        case GraphicsTargetKind::DrawStartedTexture:
          snprintf(key, sizeof(key), "d%s", target.texture_info.c_str());
          AddLink(key, action);
          break;
        case GraphicsTargetKind::LoadTexture:
          snprintf(key, sizeof(key), "l%s", target.texture_info.c_str());
          AddLink(key, action);
          break;
        // no action runs on texture creation or on XFB copies
        case GraphicsTargetKind::CreateTexture:
        case GraphicsTargetKind::XFB:
          break;
        case GraphicsTargetKind::EFB:
          MakeEFBKey(key, sizeof(key), target.width, target.height, target.texture_format);
          AddLink(key, action);
          break;
        case GraphicsTargetKind::Projection:
          if (target.has_texture_info)
          {
            snprintf(key, sizeof(key), "t%s_%d", target.texture_info.c_str(),
                     static_cast<int>(target.projection_type));
          }
          else
          {
            snprintf(key, sizeof(key), "p%d", static_cast<int>(target.projection_type));
          }
          AddLink(key, action);
          break;
        }
      };

      const auto add_action = [&](GraphicsModConfig mod_config) -> bool {
        GraphicsModAction* const action =
            create_action(feature.m_action, feature.m_action_xyz, std::move(mod_config));
        if (action == nullptr)
        {
          WARN_LOG(VIDEO, "Failed to create action '%s' for group '%s'.", feature.m_action.c_str(),
                   feature.m_group.c_str());
          return false;
        }
        if (m_num_actions == m_actions_capacity)
        {
          const u32 capacity = m_actions_capacity ? m_actions_capacity * 2 : 16;
          GraphicsModAction** const actions = static_cast<GraphicsModAction**>(
              realloc(m_actions, capacity * sizeof(GraphicsModAction*)));
          if (!actions)
            abort();
          m_actions = actions;
          m_actions_capacity = capacity;
        }
        m_actions[m_num_actions++] = action;
        return true;
      };

      // Prefer groups in the pack over groups from another pack
      if (const auto local_it = group_to_targets.find(internal_group);
          local_it != group_to_targets.end())
      {
        if (add_action(mod))
        {
          for (const GraphicsTargetConfig& target : local_it->second)
          {
            add_target(target);
          }
        }
      }
      else if (const auto global_it = group_to_targets.find(feature.m_group);
               global_it != group_to_targets.end())
      {
        if (add_action(mod))
        {
          for (const GraphicsTargetConfig& target : global_it->second)
          {
            add_target(target);
          }
        }
      }
      else
      {
        WARN_LOG(VIDEO, "Specified graphics mod group '%s' was not found for mod '%s'",
                 feature.m_group.c_str(), mod.m_title.c_str());
      }
    }
  }

  BuildTargets();
}

void GraphicsModManager::Reset()
{
  for (u32 i = 0; i < m_num_actions; i++)
    delete m_actions[i];
  for (u32 i = 0; i < m_num_links; i++)
    free(m_links[i].key);
  free(m_actions);
  free(m_links);
  free(m_targets);
  free(m_linked);
  free(m_index);
  m_actions = nullptr;
  m_links = nullptr;
  m_targets = nullptr;
  m_linked = nullptr;
  m_index = nullptr;
  m_num_actions = m_actions_capacity = 0;
  m_num_links = m_links_capacity = 0;
  m_num_targets = 0;
  m_index_capacity = 0;
}
