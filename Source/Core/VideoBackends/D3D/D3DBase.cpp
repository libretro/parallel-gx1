// Copyright 2010 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/D3D/D3DBase.h"

#include <algorithm>
#include <array>

#include "Common/CommonTypes.h"
#include "Common/DynamicLibrary.h"
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
static Common::DynamicLibrary s_d3d11_library;
namespace D3D
{
ComPtr<IDXGIFactory> dxgi_factory;
ComPtr<ID3D11Device> device;
ComPtr<ID3D11Device1> device1;
ComPtr<ID3D11DeviceContext> context;
D3D_FEATURE_LEVEL feature_level;

constexpr std::array<D3D_FEATURE_LEVEL, 3> s_supported_feature_levels{
    D3D_FEATURE_LEVEL_11_0,
    D3D_FEATURE_LEVEL_10_1,
    D3D_FEATURE_LEVEL_10_0,
};

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

  dxgi_factory.Reset();
  D3DCommon::UnloadLibraries();
  s_d3d11_library.Close();
}

std::vector<u32> GetAAModes(u32 adapter_index)
{
  // Use temporary device if we don't have one already.
  Common::DynamicLibrary temp_lib;
  ComPtr<ID3D11Device> temp_device = device;
  D3D_FEATURE_LEVEL temp_feature_level = feature_level;
  if (!temp_device)
  {
    ComPtr<IDXGIFactory> temp_dxgi_factory = D3DCommon::CreateDXGIFactory(false);
    if (!temp_dxgi_factory)
      return {};

    ComPtr<IDXGIAdapter> adapter;
    temp_dxgi_factory->EnumAdapters(adapter_index, &adapter);

    PFN_D3D11_CREATE_DEVICE d3d11_create_device;
    if (!temp_lib.Open("d3d11.dll") ||
        !temp_lib.GetSymbol("D3D11CreateDevice", &d3d11_create_device))
    {
      return {};
    }

    HRESULT hr = d3d11_create_device(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
                                     s_supported_feature_levels.data(),
                                     static_cast<UINT>(s_supported_feature_levels.size()),
                                     D3D11_SDK_VERSION, &temp_device, &temp_feature_level, nullptr);
    if (FAILED(hr))
      return {};
  }

  // NOTE: D3D 10.0 doesn't support multisampled resources which are bound as depth buffers AND
  // shader resources. Thus, we can't have MSAA with 10.0 level hardware.
  if (temp_feature_level == D3D_FEATURE_LEVEL_10_0)
    return {};

  const DXGI_FORMAT target_format =
      D3DCommon::GetDXGIFormatForAbstractFormat(FramebufferManager::GetEFBColorFormat(), false);
  std::vector<u32> aa_modes;
  for (u32 samples = 1; samples <= D3D11_MAX_MULTISAMPLE_SAMPLE_COUNT; ++samples)
  {
    UINT quality_levels = 0;
    if (SUCCEEDED(
            temp_device->CheckMultisampleQualityLevels(target_format, samples, &quality_levels)) &&
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

bool SupportsLogicOp(u32 adapter_index)
{
  // Use temporary device if we don't have one already.
  Common::DynamicLibrary temp_lib;
  ComPtr<ID3D11Device1> temp_device1 = device1;
  if (!device)
  {
    ComPtr<ID3D11Device> temp_device;

    ComPtr<IDXGIFactory> temp_dxgi_factory = D3DCommon::CreateDXGIFactory(false);
    if (!temp_dxgi_factory)
      return false;

    ComPtr<IDXGIAdapter> adapter;
    temp_dxgi_factory->EnumAdapters(adapter_index, &adapter);

    PFN_D3D11_CREATE_DEVICE d3d11_create_device;
    if (!temp_lib.Open("d3d11.dll") ||
        !temp_lib.GetSymbol("D3D11CreateDevice", &d3d11_create_device))
    {
      return false;
    }

    HRESULT hr = d3d11_create_device(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
                                     s_supported_feature_levels.data(),
                                     static_cast<UINT>(s_supported_feature_levels.size()),
                                     D3D11_SDK_VERSION, &temp_device, nullptr, nullptr);
    if (FAILED(hr))
      return false;

    if (FAILED(temp_device.As(&temp_device1)))
      return false;
  }

  if (!temp_device1)
    return false;

  D3D11_FEATURE_DATA_D3D11_OPTIONS options{};
  if (FAILED(temp_device1->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &options,
                                               sizeof(options))))
  {
    return false;
  }

  return options.OutputMergerLogicOp != FALSE;
}

}  // namespace D3D

}  // namespace DX11
