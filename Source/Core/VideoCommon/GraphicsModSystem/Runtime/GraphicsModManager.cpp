// Copyright 2022 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/GraphicsModSystem/Runtime/GraphicsModManager.h"

#include <cstdio>
#include <string>
#include <string_view>
#include <variant>

#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "Common/VariantUtil.h"

#include <file/file_path.h>

#include "Core/ConfigManager.h"

#include "VideoCommon/Assets/DirectFilesystemAssetLibrary.h"
#include "VideoCommon/GraphicsModSystem/Config/GraphicsMod.h"
#include "VideoCommon/GraphicsModSystem/Config/GraphicsModAsset.h"
#include "VideoCommon/GraphicsModSystem/Config/GraphicsModGroup.h"
#include "VideoCommon/GraphicsModSystem/Constants.h"
#include "VideoCommon/GraphicsModSystem/Runtime/GraphicsModActionFactory.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/VideoEvents.h"

std::unique_ptr<GraphicsModManager> g_graphics_mod_manager;

class GraphicsModManager::DecoratedAction final : public GraphicsModAction
{
public:
  DecoratedAction(std::unique_ptr<GraphicsModAction> action, GraphicsModConfig mod)
      : m_action_impl(std::move(action)), m_mod(std::move(mod))
  {
  }
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
  void OnTextureCreate(GraphicsModActionData::TextureCreate* texture_create) override
  {
    if (!m_mod.m_enabled)
      return;
    m_action_impl->OnTextureCreate(texture_create);
  }
  void OnFrameEnd() override
  {
    if (!m_mod.m_enabled)
      return;
    m_action_impl->OnFrameEnd();
  }

private:
  std::unique_ptr<GraphicsModAction> m_action_impl;
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

    m_end_of_frame_event =
        GetVideoEvents().after_frame_event.Register([this](Core::System&) { EndOfFrame(); });
  }

  return true;
}

const std::vector<GraphicsModAction*>&
GraphicsModManager::GetProjectionActions(ProjectionType projection_type) const
{
  if (const auto it = m_projection_target_to_actions.find(projection_type);
      it != m_projection_target_to_actions.end())
  {
    return it->second;
  }

  return m_default;
}

const std::vector<GraphicsModAction*>&
GraphicsModManager::GetProjectionTextureActions(ProjectionType projection_type,
                                                const std::string& texture_name) const
{
  if (m_projection_texture_target_to_actions.empty())
    return m_default;
  char key[256];
  const int made =
      snprintf(key, sizeof(key), "%s_%d", texture_name.c_str(), static_cast<int>(projection_type));
  const size_t made_size = made < 0 ? 0 : static_cast<size_t>(made);
  // texture names fit; a longer one is made on the heap
  const std::string long_key =
      made_size >= sizeof(key) ?
          StringFromFormat("%s_%d", texture_name.c_str(), static_cast<int>(projection_type)) :
          std::string();
  const std::string_view lookup =
      made_size >= sizeof(key) ? std::string_view(long_key) : std::string_view(key, made_size);
  if (const auto it = m_projection_texture_target_to_actions.find(lookup);
      it != m_projection_texture_target_to_actions.end())
  {
    return it->second;
  }

  return m_default;
}

const std::vector<GraphicsModAction*>&
GraphicsModManager::GetDrawStartedActions(const std::string& texture_name) const
{
  if (const auto it = m_draw_started_target_to_actions.find(texture_name);
      it != m_draw_started_target_to_actions.end())
  {
    return it->second;
  }

  return m_default;
}

const std::vector<GraphicsModAction*>&
GraphicsModManager::GetTextureLoadActions(const std::string& texture_name) const
{
  if (const auto it = m_load_texture_target_to_actions.find(texture_name);
      it != m_load_texture_target_to_actions.end())
  {
    return it->second;
  }

  return m_default;
}

const std::vector<GraphicsModAction*>&
GraphicsModManager::GetTextureCreateActions(const std::string& texture_name) const
{
  if (const auto it = m_create_texture_target_to_actions.find(texture_name);
      it != m_create_texture_target_to_actions.end())
  {
    return it->second;
  }

  return m_default;
}

const std::vector<GraphicsModAction*>& GraphicsModManager::GetEFBActions(const FBInfo& efb) const
{
  if (const auto it = m_efb_target_to_actions.find(efb); it != m_efb_target_to_actions.end())
  {
    return it->second;
  }

  return m_default;
}

const std::vector<GraphicsModAction*>& GraphicsModManager::GetXFBActions(const FBInfo& xfb) const
{
  if (const auto it = m_efb_target_to_actions.find(xfb); it != m_efb_target_to_actions.end())
  {
    return it->second;
  }

  return m_default;
}

void GraphicsModManager::Load(const GraphicsModGroupConfig& config)
{
  Reset();

  const auto& mods = config.GetMods();

  auto filesystem_library = std::make_shared<VideoCommon::DirectFilesystemAssetLibrary>();

  std::map<std::string, std::vector<GraphicsTargetConfig>> group_to_targets;
  for (const auto& mod : mods)
  {
    for (const GraphicsTargetGroupConfig& group : mod.m_groups)
    {
      if (const bool inserted = m_groups.insert(group.m_name).second; !inserted)
      {
        WARN_LOG(
            VIDEO,
            "Specified graphics mod group '%s' for mod '%s' is already specified by another mod.",
            group.m_name.c_str(), mod.m_title.c_str());
      }

      const std::string internal_group = mod.m_title + "." + group.m_name;
      for (const GraphicsTargetConfig& target : group.m_targets)
      {
        group_to_targets[group.m_name].push_back(target);
        group_to_targets[internal_group].push_back(target);
      }
    }

    std::string base_path;
    SplitPath(mod.GetAbsolutePath(), &base_path, nullptr, nullptr);
    for (const GraphicsModAssetConfig& asset : mod.m_assets)
    {
      auto asset_map = asset.m_map;
      for (auto& [k, v] : asset_map)
      {
        if (path_is_absolute(v.c_str()))
        {
          WARN_LOG(VIDEO,
                   "Specified graphics mod asset '%s' for mod '%s' has an absolute path, you "
                   "shouldn't release this to users.",
                   asset.m_asset_id.c_str(), mod.m_title.c_str());
        }
        else
        {
          v = base_path + v;
        }
      }

      filesystem_library->SetAssetIDMapData(asset.m_asset_id, std::move(asset_map));
    }
  }

  const auto create_action =
      [filesystem_library = std::move(filesystem_library)](
          const std::string_view& action_name, const Common::Vec3& xyz,
          GraphicsModConfig mod_config) -> std::unique_ptr<GraphicsModAction> {
    auto action = GraphicsModActionFactory::Create(action_name, xyz, filesystem_library);
    if (action == nullptr)
    {
      return nullptr;
    }
    return std::make_unique<DecoratedAction>(std::move(action), std::move(mod_config));
  };

  for (const auto& mod : mods)
  {
    for (const GraphicsModFeatureConfig& feature : mod.m_features)
    {
      const std::string internal_group = mod.m_title + "." + feature.m_group;

      const auto add_target = [&](const GraphicsTargetConfig& target) {
        std::visit(
            overloaded{
                [&](const DrawStartedTextureTarget& the_target) {
                  m_draw_started_target_to_actions[the_target.m_texture_info_string].push_back(
                      m_actions.back().get());
                },
                [&](const LoadTextureTarget& the_target) {
                  m_load_texture_target_to_actions[the_target.m_texture_info_string].push_back(
                      m_actions.back().get());
                },
                [&](const CreateTextureTarget& the_target) {
                  m_create_texture_target_to_actions[the_target.m_texture_info_string].push_back(
                      m_actions.back().get());
                },
                [&](const EFBTarget& the_target) {
                  FBInfo info;
                  info.m_height = the_target.m_height;
                  info.m_width = the_target.m_width;
                  info.m_texture_format = the_target.m_texture_format;
                  m_efb_target_to_actions[info].push_back(m_actions.back().get());
                },
                [&](const XFBTarget& the_target) {
                  FBInfo info;
                  info.m_height = the_target.m_height;
                  info.m_width = the_target.m_width;
                  info.m_texture_format = the_target.m_texture_format;
                  m_xfb_target_to_actions[info].push_back(m_actions.back().get());
                },
                [&](const ProjectionTarget& the_target) {
                  if (the_target.m_texture_info_string)
                  {
                    const std::string lookup =
                        StringFromFormat("%s_%d", the_target.m_texture_info_string->c_str(),
                                         static_cast<int>(the_target.m_projection_type));
                    m_projection_texture_target_to_actions[lookup].push_back(
                        m_actions.back().get());
                  }
                  else
                  {
                    m_projection_target_to_actions[the_target.m_projection_type].push_back(
                        m_actions.back().get());
                  }
                },
            },
            target);
      };

      const auto add_action = [&](GraphicsModConfig mod_config) -> bool {
        auto action = create_action(feature.m_action, feature.m_action_xyz, std::move(mod_config));
        if (action == nullptr)
        {
          WARN_LOG(VIDEO, "Failed to create action '%s' for group '%s'.", feature.m_action.c_str(),
                   feature.m_group.c_str());
          return false;
        }
        m_actions.push_back(std::move(action));
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
}

void GraphicsModManager::EndOfFrame()
{
  for (auto&& action : m_actions)
  {
    action->OnFrameEnd();
  }
}

void GraphicsModManager::Reset()
{
  m_actions.clear();
  m_groups.clear();
  m_projection_target_to_actions.clear();
  m_projection_texture_target_to_actions.clear();
  m_draw_started_target_to_actions.clear();
  m_load_texture_target_to_actions.clear();
  m_create_texture_target_to_actions.clear();
  m_efb_target_to_actions.clear();
  m_xfb_target_to_actions.clear();
}
