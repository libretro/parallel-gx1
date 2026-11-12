// Copyright 2010 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <vector>
#include <wrl/client.h>

#include "Common/CommonTypes.h"
#include "Common/HRWrap.h"

namespace DX11
{
using Microsoft::WRL::ComPtr;

namespace D3D
{
extern ComPtr<ID3D11Device> device;
extern ComPtr<ID3D11Device1> device1;
extern ComPtr<ID3D11DeviceContext> context;
extern D3D_FEATURE_LEVEL feature_level;

void Destroy();

// The AA modes the frontend's device supports.
std::vector<u32> GetAAModes();

// Checks for support of the given texture format.
bool SupportsTextureFormat(DXGI_FORMAT format);

// Logic op support on the frontend's device.
bool SupportsLogicOp();

}  // namespace D3D

// Common::HRText, with the device's removal reason after a device removed error.
Common::HResultText HRText(HRESULT hr);

}  // namespace DX11
