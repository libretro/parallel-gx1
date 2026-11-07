// Copyright 2019 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/D3DCommon/D3DCommon.h"

#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include "Common/Assert.h"
#include "Common/MsgHandler.h"
#include "Common/StringUtil.h"

#include "VideoCommon/TextureConfig.h"
#include "VideoCommon/VideoConfig.h"

namespace D3DCommon
{
DXGI_FORMAT GetDXGIFormatForAbstractFormat(AbstractTextureFormat format, bool typeless)
{
  switch (format)
  {
  case AbstractTextureFormat::DXT1:
    return DXGI_FORMAT_BC1_UNORM;
  case AbstractTextureFormat::DXT3:
    return DXGI_FORMAT_BC2_UNORM;
  case AbstractTextureFormat::DXT5:
    return DXGI_FORMAT_BC3_UNORM;
  case AbstractTextureFormat::BPTC:
    return DXGI_FORMAT_BC7_UNORM;
  case AbstractTextureFormat::RGBA8:
    return typeless ? DXGI_FORMAT_R8G8B8A8_TYPELESS : DXGI_FORMAT_R8G8B8A8_UNORM;
  case AbstractTextureFormat::BGRA8:
    return typeless ? DXGI_FORMAT_B8G8R8A8_TYPELESS : DXGI_FORMAT_B8G8R8A8_UNORM;
  case AbstractTextureFormat::RGB10_A2:
    return typeless ? DXGI_FORMAT_R10G10B10A2_TYPELESS : DXGI_FORMAT_R10G10B10A2_UNORM;
  case AbstractTextureFormat::RGBA16F:
    return typeless ? DXGI_FORMAT_R16G16B16A16_TYPELESS : DXGI_FORMAT_R16G16B16A16_FLOAT;
  case AbstractTextureFormat::R16:
    return typeless ? DXGI_FORMAT_R16_TYPELESS : DXGI_FORMAT_R16_UNORM;
  case AbstractTextureFormat::R32F:
    return typeless ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_R32_FLOAT;
  case AbstractTextureFormat::D16:
    return DXGI_FORMAT_R16_TYPELESS;
  case AbstractTextureFormat::D24_S8:
    return DXGI_FORMAT_R24G8_TYPELESS;
  case AbstractTextureFormat::D32F:
    return DXGI_FORMAT_R32_TYPELESS;
  case AbstractTextureFormat::D32F_S8:
    return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
  default:
    PanicAlertFmt("Unhandled texture format.");
    return DXGI_FORMAT_R8G8B8A8_UNORM;
  }
}
DXGI_FORMAT GetSRVFormatForAbstractFormat(AbstractTextureFormat format)
{
  switch (format)
  {
  case AbstractTextureFormat::DXT1:
    return DXGI_FORMAT_BC1_UNORM;
  case AbstractTextureFormat::DXT3:
    return DXGI_FORMAT_BC2_UNORM;
  case AbstractTextureFormat::DXT5:
    return DXGI_FORMAT_BC3_UNORM;
  case AbstractTextureFormat::BPTC:
    return DXGI_FORMAT_BC7_UNORM;
  case AbstractTextureFormat::RGBA8:
    return DXGI_FORMAT_R8G8B8A8_UNORM;
  case AbstractTextureFormat::BGRA8:
    return DXGI_FORMAT_B8G8R8A8_UNORM;
  case AbstractTextureFormat::RGB10_A2:
    return DXGI_FORMAT_R10G10B10A2_UNORM;
  case AbstractTextureFormat::RGBA16F:
    return DXGI_FORMAT_R16G16B16A16_FLOAT;
  case AbstractTextureFormat::R16:
    return DXGI_FORMAT_R16_UNORM;
  case AbstractTextureFormat::R32F:
    return DXGI_FORMAT_R32_FLOAT;
  case AbstractTextureFormat::D16:
    return DXGI_FORMAT_R16_UNORM;
  case AbstractTextureFormat::D24_S8:
    return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
  case AbstractTextureFormat::D32F:
    return DXGI_FORMAT_R32_FLOAT;
  case AbstractTextureFormat::D32F_S8:
    return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
  default:
    PanicAlertFmt("Unhandled SRV format");
    return DXGI_FORMAT_UNKNOWN;
  }
}

DXGI_FORMAT GetRTVFormatForAbstractFormat(AbstractTextureFormat format, bool integer)
{
  switch (format)
  {
  case AbstractTextureFormat::RGBA8:
    return integer ? DXGI_FORMAT_R8G8B8A8_UINT : DXGI_FORMAT_R8G8B8A8_UNORM;
  case AbstractTextureFormat::BGRA8:
    return DXGI_FORMAT_B8G8R8A8_UNORM;
  case AbstractTextureFormat::RGB10_A2:
    return DXGI_FORMAT_R10G10B10A2_UNORM;
  case AbstractTextureFormat::RGBA16F:
    return DXGI_FORMAT_R16G16B16A16_FLOAT;
  case AbstractTextureFormat::R16:
    return integer ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R16_UNORM;
  case AbstractTextureFormat::R32F:
    return DXGI_FORMAT_R32_FLOAT;
  default:
    PanicAlertFmt("Unhandled RTV format");
    return DXGI_FORMAT_UNKNOWN;
  }
}
DXGI_FORMAT GetDSVFormatForAbstractFormat(AbstractTextureFormat format)
{
  switch (format)
  {
  case AbstractTextureFormat::D16:
    return DXGI_FORMAT_D16_UNORM;
  case AbstractTextureFormat::D24_S8:
    return DXGI_FORMAT_D24_UNORM_S8_UINT;
  case AbstractTextureFormat::D32F:
    return DXGI_FORMAT_D32_FLOAT;
  case AbstractTextureFormat::D32F_S8:
    return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
  default:
    PanicAlertFmt("Unhandled DSV format");
    return DXGI_FORMAT_UNKNOWN;
  }
}

AbstractTextureFormat GetAbstractFormatForDXGIFormat(DXGI_FORMAT format)
{
  switch (format)
  {
  case DXGI_FORMAT_R8G8B8A8_UINT:
  case DXGI_FORMAT_R8G8B8A8_UNORM:
  case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    return AbstractTextureFormat::RGBA8;

  case DXGI_FORMAT_B8G8R8A8_UNORM:
  case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    return AbstractTextureFormat::BGRA8;

  case DXGI_FORMAT_R10G10B10A2_UNORM:
  case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    return AbstractTextureFormat::RGB10_A2;

  case DXGI_FORMAT_R16G16B16A16_FLOAT:
  case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    return AbstractTextureFormat::RGBA16F;

  case DXGI_FORMAT_R16_UINT:
  case DXGI_FORMAT_R16_UNORM:
  case DXGI_FORMAT_R16_TYPELESS:
    return AbstractTextureFormat::R16;

  case DXGI_FORMAT_R32_FLOAT:
  case DXGI_FORMAT_R32_TYPELESS:
    return AbstractTextureFormat::R32F;

  case DXGI_FORMAT_D16_UNORM:
    return AbstractTextureFormat::D16;

  case DXGI_FORMAT_D24_UNORM_S8_UINT:
    return AbstractTextureFormat::D24_S8;

  case DXGI_FORMAT_D32_FLOAT:
    return AbstractTextureFormat::D32F;

  case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    return AbstractTextureFormat::D32F_S8;

  case DXGI_FORMAT_BC1_UNORM:
    return AbstractTextureFormat::DXT1;
  case DXGI_FORMAT_BC2_UNORM:
    return AbstractTextureFormat::DXT3;
  case DXGI_FORMAT_BC3_UNORM:
    return AbstractTextureFormat::DXT5;
  case DXGI_FORMAT_BC7_UNORM:
    return AbstractTextureFormat::BPTC;

  default:
    return AbstractTextureFormat::Undefined;
  }
}

void SetDebugObjectName(IUnknown* resource, std::string_view name)
{
  if (!g_ActiveConfig.bEnableValidationLayer)
    return;

  Microsoft::WRL::ComPtr<ID3D11DeviceChild> child11;
  Microsoft::WRL::ComPtr<ID3D12DeviceChild> child12;
  if (SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(&child11))))
  {
    child11->SetPrivateData(WKPDID_D3DDebugObjectName, static_cast<UINT>(name.length()),
                            name.data());
  }
  else if (SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(&child12))))
  {
    child12->SetPrivateData(WKPDID_D3DDebugObjectName, static_cast<UINT>(name.length()),
                            name.data());
  }
}
}  // namespace D3DCommon
