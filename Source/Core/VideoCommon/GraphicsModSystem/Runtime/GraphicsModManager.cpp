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
  // Takes action, which runs only for an enabled mod
  DecoratedAction(GraphicsModAction* action, bool enabled)
      : m_action_impl(action), m_enabled(enabled)
  {
  }
  ~DecoratedAction() override { delete m_action_impl; }
  DecoratedAction(const DecoratedAction&) = delete;
  DecoratedAction& operator=(const DecoratedAction&) = delete;
  void OnDrawStarted(GraphicsModActionData::DrawStarted* draw_started) override
  {
    if (!m_enabled)
      return;
    m_action_impl->OnDrawStarted(draw_started);
  }
  void BeforeEFB(GraphicsModActionData::PreEFB* efb) override
  {
    if (!m_enabled)
      return;
    m_action_impl->BeforeEFB(efb);
  }
  void OnProjection(GraphicsModActionData::Projection* projection) override
  {
    if (!m_enabled)
      return;
    m_action_impl->OnProjection(projection);
  }
  void OnProjectionAndTexture(GraphicsModActionData::Projection* projection) override
  {
    if (!m_enabled)
      return;
    m_action_impl->OnProjectionAndTexture(projection);
  }
  void OnTextureLoad(GraphicsModActionData::TextureLoad* texture_load) override
  {
    if (!m_enabled)
      return;
    m_action_impl->OnTextureLoad(texture_load);
  }

private:
  GraphicsModAction* m_action_impl;  // owned
  bool m_enabled;
};

bool GraphicsModManager::Initialize()
{
  if (g_ActiveConfig.bGraphicMods)
    LoadGameMods();

  return true;
}

void GraphicsModManager::LoadGameMods()
{
  // The actions copy what they need, so the config goes once they are made.
  GraphicsModGroupConfig config(SConfig::GetInstance().GetGameID());
  config.Load();
  Load(config);
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

namespace
{
// A group's targets by name, as Load gathers them from every mod: a name stays found even when no
// target comes with it.
struct GroupTargets
{
  char* key;
  const GraphicsTargetConfig** targets;
  u32 count;
  u32 capacity;
};

struct GroupTargetsList
{
  GroupTargets* groups;
  u32 count;
  u32 capacity;
};

GroupTargets* FindGroup(GroupTargetsList* list, const char* key)
{
  for (u32 i = 0; i < list->count; i++)
  {
    if (!std::strcmp(list->groups[i].key, key))
      return &list->groups[i];
  }
  return nullptr;
}

GroupTargets* FindOrAddGroup(GroupTargetsList* list, const char* key)
{
  if (GroupTargets* const group = FindGroup(list, key))
    return group;
  if (list->count == list->capacity)
  {
    const u32 capacity = list->capacity ? list->capacity * 2 : 16;
    GroupTargets* const groups =
        static_cast<GroupTargets*>(realloc(list->groups, capacity * sizeof(GroupTargets)));
    if (!groups)
      abort();
    list->groups = groups;
    list->capacity = capacity;
  }
  GroupTargets* const group = &list->groups[list->count++];
  group->key = strdup(key);
  if (!group->key)
    abort();
  group->targets = nullptr;
  group->count = 0;
  group->capacity = 0;
  return group;
}

void AddGroupTarget(GroupTargets* group, const GraphicsTargetConfig* target)
{
  if (group->count == group->capacity)
  {
    const u32 capacity = group->capacity ? group->capacity * 2 : 8;
    const GraphicsTargetConfig** const targets = static_cast<const GraphicsTargetConfig**>(
        realloc(group->targets, capacity * sizeof(*targets)));
    if (!targets)
      abort();
    group->targets = targets;
    group->capacity = capacity;
  }
  group->targets[group->count++] = target;
}

void FreeGroups(GroupTargetsList* list)
{
  for (u32 i = 0; i < list->count; i++)
  {
    free(list->groups[i].key);
    free(list->groups[i].targets);
  }
  free(list->groups);
}

const char* Text(const char* text)
{
  return text ? text : "";
}

// "<title>.<name>", the key of a group within its own mod
char* InternalGroupKey(const char* title, const char* name)
{
  const size_t size = strlen(Text(title)) + 1 + strlen(Text(name)) + 1;
  char* const key = static_cast<char*>(malloc(size));
  if (!key)
    abort();
  snprintf(key, size, "%s.%s", Text(title), Text(name));
  return key;
}
}  // namespace

void GraphicsModManager::Load(const GraphicsModGroupConfig& config)
{
  Reset();

  u32 mod_count;
  const GraphicsModConfig* const mods = config.GetMods(&mod_count);

  GroupTargetsList group_to_targets = {};
  for (u32 m = 0; m < mod_count; m++)
  {
    const GraphicsModConfig& mod = mods[m];
    for (u32 g = 0; g < mod.m_group_count; g++)
    {
      const GraphicsTargetGroupConfig& group = mod.m_groups[g];
      const char* const name = Text(group.m_name);
      if (FindGroup(&group_to_targets, name))
      {
        WARN_LOG(
            VIDEO,
            "Specified graphics mod group '%s' for mod '%s' is already specified by another mod.",
            name, Text(mod.m_title));
      }
      FindOrAddGroup(&group_to_targets, name);

      char* const internal_group = InternalGroupKey(mod.m_title, group.m_name);
      for (u32 t = 0; t < group.m_target_count; t++)
      {
        AddGroupTarget(FindOrAddGroup(&group_to_targets, name), &group.m_targets[t]);
        AddGroupTarget(FindOrAddGroup(&group_to_targets, internal_group), &group.m_targets[t]);
      }
      free(internal_group);
    }
  }

  for (u32 m = 0; m < mod_count; m++)
  {
    const GraphicsModConfig& mod = mods[m];
    for (u32 f = 0; f < mod.m_feature_count; f++)
    {
      const GraphicsModFeatureConfig& feature = mod.m_features[f];
      char* const internal_group = InternalGroupKey(mod.m_title, feature.m_group);

      const auto add_target = [&](const GraphicsTargetConfig& target) {
        GraphicsModAction* const action = m_actions[m_num_actions - 1];
        char key[512];
        switch (target.kind)
        {
        case GraphicsTargetKind::DrawStartedTexture:
          snprintf(key, sizeof(key), "d%s", Text(target.texture_info));
          AddLink(key, action);
          break;
        case GraphicsTargetKind::LoadTexture:
          snprintf(key, sizeof(key), "l%s", Text(target.texture_info));
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
            snprintf(key, sizeof(key), "t%s_%d", Text(target.texture_info),
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

      const auto add_action = [&]() -> bool {
        GraphicsModAction* const created =
            GraphicsModActionFactory::Create(Text(feature.m_action), feature.m_action_xyz);
        if (created == nullptr)
        {
          WARN_LOG(VIDEO, "Failed to create action '%s' for group '%s'.", Text(feature.m_action),
                   Text(feature.m_group));
          return false;
        }
        GraphicsModAction* const action = new DecoratedAction(created, mod.m_enabled);
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
      const GroupTargets* group = FindGroup(&group_to_targets, internal_group);
      if (!group)
        group = FindGroup(&group_to_targets, Text(feature.m_group));
      if (group)
      {
        if (add_action())
        {
          for (u32 t = 0; t < group->count; t++)
            add_target(*group->targets[t]);
        }
      }
      else
      {
        WARN_LOG(VIDEO, "Specified graphics mod group '%s' was not found for mod '%s'",
                 Text(feature.m_group), Text(mod.m_title));
      }
      free(internal_group);
    }
  }

  FreeGroups(&group_to_targets);
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
