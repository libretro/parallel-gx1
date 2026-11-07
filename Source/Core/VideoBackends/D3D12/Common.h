// Copyright 2019 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <wrl/client.h>

// MinGW-w64 headers before 12.0 do not define these.
#ifndef D3D12_MAX_MULTISAMPLE_SAMPLE_COUNT
#define D3D12_MAX_MULTISAMPLE_SAMPLE_COUNT 32
#endif
#ifndef D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE
#define D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE 2048
#endif

#include "Common/MsgHandler.h"
#include "VideoBackends/D3DCommon/D3DCommon.h"

namespace DX12
{
using Microsoft::WRL::ComPtr;

static inline void ResourceBarrier(ID3D12GraphicsCommandList* cmdlist, ID3D12Resource* resource,
                                   D3D12_RESOURCE_STATES from_state,
                                   D3D12_RESOURCE_STATES to_state)
{
  const D3D12_RESOURCE_BARRIER barrier = {
      D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
      D3D12_RESOURCE_BARRIER_FLAG_NONE,
      {{resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from_state, to_state}}};
  cmdlist->ResourceBarrier(1, &barrier);
}
}  // namespace DX12
