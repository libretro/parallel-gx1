// Copyright 2019 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/D3D12/DescriptorHeapManager.h"

#include <bit>
#include <cstdlib>
#include <cstring>

#include "Common/Assert.h"

#include "VideoBackends/D3D12/DX12Context.h"
#include "VideoCommon/VideoConfig.h"

namespace DX12
{
DescriptorHeapManager::DescriptorHeapManager() = default;
DescriptorHeapManager::~DescriptorHeapManager()
{
  std::free(m_free_slots);
}

bool DescriptorHeapManager::Create(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type,
                                   u32 num_descriptors)
{
  D3D12_DESCRIPTOR_HEAP_DESC desc = {type, static_cast<UINT>(num_descriptors),
                                     D3D12_DESCRIPTOR_HEAP_FLAG_NONE};

  HRESULT hr = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_descriptor_heap));
  ASSERT_MSG(VIDEO, SUCCEEDED(hr), "Failed to create descriptor heap: %s", DX12::HRText(hr).text);
  if (FAILED(hr))
    return false;

  m_heap_base_cpu = m_descriptor_heap->GetCPUDescriptorHandleForHeapStart();
  m_num_descriptors = num_descriptors;
  m_descriptor_increment_size = device->GetDescriptorHandleIncrementSize(type);

  // Set every descriptor's slot to unallocated (1)
  std::free(m_free_slots);
  m_free_slot_words = (num_descriptors + 63) / 64;
  m_free_slots = static_cast<u64*>(std::malloc(m_free_slot_words * sizeof(u64)));
  if (!m_free_slots)
  {
    m_free_slot_words = 0;
    return false;
  }
  for (u32 word = 0; word < m_free_slot_words; word++)
    m_free_slots[word] = ~u64{0};
  if (num_descriptors % 64)
    m_free_slots[m_free_slot_words - 1] = (u64{1} << (num_descriptors % 64)) - 1;

  return true;
}

bool DescriptorHeapManager::Allocate(DescriptorHandle* handle)
{
  // The lowest free descriptor
  for (u32 word = 0; word < m_free_slot_words; word++)
  {
    const u64 free_bits = m_free_slots[word];
    if (!free_bits)
      continue;

    const u32 bit = static_cast<u32>(std::countr_zero(free_bits));
    const u32 index = word * 64 + bit;
    m_free_slots[word] = free_bits & (free_bits - 1);

    handle->index = index;
    handle->cpu_handle.ptr = m_heap_base_cpu.ptr + index * m_descriptor_increment_size;
    handle->gpu_handle.ptr = 0;
    return true;
  }

  PanicAlert("Out of fixed descriptors");
  return false;
}

void DescriptorHeapManager::Free(u32 index)
{
  ASSERT(index < m_num_descriptors);

  m_free_slots[index / 64] |= u64{1} << (index % 64);
}

void DescriptorHeapManager::Free(const DescriptorHandle& handle)
{
  Free(handle.index);
}

SamplerHeapManager::SamplerHeapManager() = default;
SamplerHeapManager::~SamplerHeapManager()
{
  std::free(m_samplers);
}

static void GetD3DSamplerDesc(D3D12_SAMPLER_DESC* desc, const SamplerState& state)
{
  if (state.tm0.mipmap_filter == FilterMode::Linear)
  {
    if (state.tm0.min_filter == FilterMode::Linear)
    {
      desc->Filter = (state.tm0.mag_filter == FilterMode::Linear) ?
                         D3D12_FILTER_MIN_MAG_MIP_LINEAR :
                         D3D12_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR;
    }
    else
    {
      desc->Filter = (state.tm0.mag_filter == FilterMode::Linear) ?
                         D3D12_FILTER_MIN_POINT_MAG_MIP_LINEAR :
                         D3D12_FILTER_MIN_MAG_POINT_MIP_LINEAR;
    }
  }
  else
  {
    if (state.tm0.min_filter == FilterMode::Linear)
    {
      desc->Filter = (state.tm0.mag_filter == FilterMode::Linear) ?
                         D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT :
                         D3D12_FILTER_MIN_LINEAR_MAG_MIP_POINT;
    }
    else
    {
      desc->Filter = (state.tm0.mag_filter == FilterMode::Linear) ?
                         D3D12_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT :
                         D3D12_FILTER_MIN_MAG_MIP_POINT;
    }
  }

  static constexpr D3D12_TEXTURE_ADDRESS_MODE address_modes[3] = {
      D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_WRAP,
      D3D12_TEXTURE_ADDRESS_MODE_MIRROR};
  desc->AddressU = address_modes[static_cast<u32>(state.tm0.wrap_u.Value())];
  desc->AddressV = address_modes[static_cast<u32>(state.tm0.wrap_v.Value())];
  desc->AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  desc->MaxLOD = state.tm1.max_lod / 16.f;
  desc->MinLOD = state.tm1.min_lod / 16.f;
  desc->MipLODBias = static_cast<s32>(state.tm0.lod_bias) / 256.f;
  desc->ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;

  if (state.tm0.anisotropic_filtering != 0)
  {
    desc->Filter = D3D12_FILTER_ANISOTROPIC;
    desc->MaxAnisotropy = 1u << state.tm0.anisotropic_filtering;
  }
}

bool SamplerHeapManager::Lookup(const SamplerState& ss, D3D12_CPU_DESCRIPTOR_HANDLE* handle)
{
  const u64 key = ss.Hex();
  u32 low = 0;
  u32 high = m_sampler_count;
  while (low < high)
  {
    const u32 mid = low + (high - low) / 2;
    if (m_samplers[mid].key < key)
      low = mid + 1;
    else
      high = mid;
  }
  if (low < m_sampler_count && m_samplers[low].key == key)
  {
    *handle = m_samplers[low].handle;
    return true;
  }

  if (m_current_offset == m_num_descriptors)
  {
    // We can clear at any time because the descriptors are copied prior to execution.
    // It's still not free, since we have to recreate all our samplers again.
    WARN_LOG(VIDEO, "Out of samplers, resetting CPU heap");
    Clear();
    low = 0;
  }

  D3D12_SAMPLER_DESC desc = {};
  GetD3DSamplerDesc(&desc, ss);

  const D3D12_CPU_DESCRIPTOR_HANDLE new_handle = {m_heap_base_cpu.ptr +
                                                  m_current_offset * m_descriptor_increment_size};
  g_dx_context->GetDevice()->CreateSampler(&desc, new_handle);

  std::memmove(m_samplers + low + 1, m_samplers + low,
               (m_sampler_count - low) * sizeof(CachedSampler));
  m_samplers[low].key = key;
  m_samplers[low].handle = new_handle;
  m_sampler_count++;
  m_current_offset++;
  *handle = new_handle;
  return true;
}

void SamplerHeapManager::Clear()
{
  m_sampler_count = 0;
  m_current_offset = 0;
}

bool SamplerHeapManager::Create(ID3D12Device* device, u32 num_descriptors)
{
  const D3D12_DESCRIPTOR_HEAP_DESC desc = {D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, num_descriptors};
  HRESULT hr = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_descriptor_heap));
  ASSERT_MSG(VIDEO, SUCCEEDED(hr), "Failed to create sampler descriptor heap: %s",
             DX12::HRText(hr).text);
  if (FAILED(hr))
    return false;

  m_num_descriptors = num_descriptors;
  std::free(m_samplers);
  m_samplers = static_cast<CachedSampler*>(std::malloc(num_descriptors * sizeof(CachedSampler)));
  m_sampler_count = 0;
  if (!m_samplers)
    return false;
  m_descriptor_increment_size =
      device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
  m_heap_base_cpu = m_descriptor_heap->GetCPUDescriptorHandleForHeapStart();
  return true;
}
}  // namespace DX12
