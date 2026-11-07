// Copyright 2019 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <d3dcommon.h>
#include <dxgiformat.h>
#include <string>
#include <vector>
#include <wrl/client.h>

#include "Common/CommonTypes.h"

enum class AbstractTextureFormat : u32;

namespace D3DCommon
{
// Helpers for texture format conversion.
DXGI_FORMAT GetDXGIFormatForAbstractFormat(AbstractTextureFormat format, bool typeless);
DXGI_FORMAT GetSRVFormatForAbstractFormat(AbstractTextureFormat format);
DXGI_FORMAT GetRTVFormatForAbstractFormat(AbstractTextureFormat format, bool integer);
DXGI_FORMAT GetDSVFormatForAbstractFormat(AbstractTextureFormat format);
AbstractTextureFormat GetAbstractFormatForDXGIFormat(DXGI_FORMAT format);

// This function will assign a name to the given resource.
// The DirectX debug layer will make it easier to identify resources that way,
// e.g. when listing up all resources who have unreleased references.
void SetDebugObjectName(IUnknown* resource, std::string_view name);
}  // namespace D3DCommon
