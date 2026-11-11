// Copyright 2022 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/GraphicsModSystem/Runtime/Actions/PrintAction.h"

#include "Common/Logging/Log.h"

void PrintAction::OnDrawStarted(GraphicsModActionData::DrawStarted*)
{
  INFO_LOG(VIDEO, "OnDrawStarted Called");
}

void PrintAction::BeforeEFB(GraphicsModActionData::PreEFB* efb)
{
  if (!efb) [[unlikely]]
    return;

  if (!efb->scaled_width) [[unlikely]]
    return;

  if (!efb->scaled_height) [[unlikely]]
    return;

  INFO_LOG(VIDEO, "OnEFB Called. Original [%u, %u], Scaled [%u, %u]", efb->texture_width,
           efb->texture_height, *efb->scaled_width, *efb->scaled_height);
}

void PrintAction::OnProjection(GraphicsModActionData::Projection*)
{
  INFO_LOG(VIDEO, "OnProjection Called");
}

void PrintAction::OnProjectionAndTexture(GraphicsModActionData::Projection*)
{
  INFO_LOG(VIDEO, "OnProjectionAndTexture Called");
}

void PrintAction::OnTextureLoad(GraphicsModActionData::TextureLoad* texture_load)
{
  if (!texture_load) [[unlikely]]
    return;

  INFO_LOG(VIDEO, "OnTextureLoad Called.  Texture: %.*s",
           static_cast<int>(texture_load->texture_name.size()), texture_load->texture_name.data());
}
