// Copyright 2010 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/D3D/D3DBase.h"

#include <algorithm>
#include <array>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/ConfigManager.h"
#include "VideoBackends/D3D/D3DState.h"
#include "VideoBackends/D3D/DXTexture.h"
#include "VideoBackends/D3DCommon/D3DCommon.h"
#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/VideoConfig.h"

namespace DX11
{
namespace D3D
{
ComPtr<ID3D11Device> device;
ComPtr<ID3D11Device1> device1;
ComPtr<ID3D11DeviceContext> context;
D3D_FEATURE_LEVEL feature_level;

void Destroy()
{
  stateman.reset();

  if (context)
  {
    context->ClearState();
    context->Flush();
  }

  context.Reset();
  device1.Reset();

  auto remaining_references = device.Reset();
  if (remaining_references)
    ERROR_LOG_FMT(VIDEO, "Unreleased references: {}.", remaining_references);
  else
    NOTICE_LOG_FMT(VIDEO, "Successfully released all device references!");

}

std::vector<u32> GetAAModes()
{
  // NOTE: D3D 10.0 doesn't support multisampled resources which are bound as depth buffers AND
  // shader resources. Thus, we can't have MSAA with 10.0 level hardware.
  if (feature_level == D3D_FEATURE_LEVEL_10_0)
    return {};

  const DXGI_FORMAT target_format =
      D3DCommon::GetDXGIFormatForAbstractFormat(FramebufferManager::GetEFBColorFormat(), false);
  std::vector<u32> aa_modes;
  for (u32 samples = 1; samples <= D3D11_MAX_MULTISAMPLE_SAMPLE_COUNT; ++samples)
  {
    UINT quality_levels = 0;
    if (SUCCEEDED(
            device->CheckMultisampleQualityLevels(target_format, samples, &quality_levels)) &&
        quality_levels > 0)
    {
      aa_modes.push_back(samples);
    }
  }

  return aa_modes;
}

bool SupportsTextureFormat(DXGI_FORMAT format)
{
  UINT support;
  if (FAILED(device->CheckFormatSupport(format, &support)))
    return false;

  return (support & D3D11_FORMAT_SUPPORT_TEXTURE2D) != 0;
}

bool SupportsLogicOp()
{
  if (!device1)
    return false;

  D3D11_FEATURE_DATA_D3D11_OPTIONS options{};
  if (FAILED(device1->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &options,
                                          sizeof(options))))
  {
    return false;
  }

  return options.OutputMergerLogicOp != FALSE;
}

}  // namespace D3D

}  // namespace DX11
