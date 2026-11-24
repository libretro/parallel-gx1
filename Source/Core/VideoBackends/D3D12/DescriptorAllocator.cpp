// Copyright 2019 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/D3D12/DescriptorAllocator.h"

#include "Common/Assert.h"

#include "VideoBackends/D3D12/DX12Context.h"

#include <cstring>

namespace DX12
{
DescriptorAllocator::DescriptorAllocator() = default;
DescriptorAllocator::~DescriptorAllocator() = default;

bool DescriptorAllocator::Create(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type,
                                 u32 num_descriptors)
{
  const D3D12_DESCRIPTOR_HEAP_DESC desc = {type, static_cast<UINT>(num_descriptors),
                                           D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
  HRESULT hr = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_descriptor_heap));
  ASSERT_MSG(VIDEO, SUCCEEDED(hr), "Creating descriptor heap for linear allocator failed: %s",
             DX12::HRText(hr).text);
  if (FAILED(hr))
    return false;

  m_num_descriptors = num_descriptors;
  m_descriptor_increment_size = device->GetDescriptorHandleIncrementSize(type);
  m_heap_base_cpu = m_descriptor_heap->GetCPUDescriptorHandleForHeapStart();
  m_heap_base_gpu = m_descriptor_heap->GetGPUDescriptorHandleForHeapStart();
  return true;
}

bool DescriptorAllocator::Allocate(u32 num_handles, DescriptorHandle* out_base_handle)
{
  if ((m_current_offset + num_handles) > m_num_descriptors)
    return false;

  out_base_handle->index = m_current_offset;
  out_base_handle->cpu_handle.ptr =
      m_heap_base_cpu.ptr + m_current_offset * m_descriptor_increment_size;
  out_base_handle->gpu_handle.ptr =
      m_heap_base_gpu.ptr + m_current_offset * m_descriptor_increment_size;
  m_current_offset += num_handles;
  return true;
}

void DescriptorAllocator::Reset()
{
  m_current_offset = 0;
}

bool operator==(const SamplerStateSet& lhs, const SamplerStateSet& rhs)
{
  // There shouldn't be any padding here, so this will be safe.
  return std::memcmp(lhs.states, rhs.states, sizeof(lhs.states)) == 0;
}

bool operator!=(const SamplerStateSet& lhs, const SamplerStateSet& rhs)
{
  return std::memcmp(lhs.states, rhs.states, sizeof(lhs.states)) != 0;
}

bool operator<(const SamplerStateSet& lhs, const SamplerStateSet& rhs)
{
  return std::memcmp(lhs.states, rhs.states, sizeof(lhs.states)) < 0;
}

SamplerAllocator::SamplerAllocator() = default;
SamplerAllocator::~SamplerAllocator() = default;

bool SamplerAllocator::Create(ID3D12Device* device)
{
  return DescriptorAllocator::Create(device, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
                                     D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE);
}

bool SamplerAllocator::GetGroupHandle(const SamplerStateSet& sss,
                                      D3D12_GPU_DESCRIPTOR_HANDLE* handle)
{
  u32 low = 0;
  u32 high = m_group_count;
  while (low < high)
  {
    const u32 mid = low + (high - low) / 2;
    if (m_groups[mid].states < sss)
      low = mid + 1;
    else
      high = mid;
  }
  if (low < m_group_count && m_groups[low].states == sss)
  {
    *handle = m_groups[low].handle;
    return true;
  }
  if (m_group_count == MAX_GROUPS)
    return false;

  // Allocate a group of descriptors.
  DescriptorHandle allocation;
  if (!Allocate(VideoCommon::MAX_PIXEL_SHADER_SAMPLERS, &allocation))
    return false;

  // Lookup sampler handles from global cache.
  D3D12_CPU_DESCRIPTOR_HANDLE source_handles[VideoCommon::MAX_PIXEL_SHADER_SAMPLERS];
  for (u32 i = 0; i < VideoCommon::MAX_PIXEL_SHADER_SAMPLERS; i++)
  {
    if (!g_dx_context->GetSamplerHeapManager().Lookup(sss.states[i], &source_handles[i]))
      return false;
  }

  // Copy samplers from the sampler heap.
  static constexpr UINT source_sizes[VideoCommon::MAX_PIXEL_SHADER_SAMPLERS] = {
      1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
  g_dx_context->GetDevice()->CopyDescriptors(1, &allocation.cpu_handle,
                                             &VideoCommon::MAX_PIXEL_SHADER_SAMPLERS,
                                             VideoCommon::MAX_PIXEL_SHADER_SAMPLERS, source_handles,
                                             source_sizes, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
  *handle = allocation.gpu_handle;
  for (u32 i = m_group_count; i > low; i--)
    m_groups[i] = m_groups[i - 1];
  m_groups[low].states = sss;
  m_groups[low].handle = allocation.gpu_handle;
  m_group_count++;
  return true;
}

bool SamplerAllocator::ShouldReset() const
{
  // We only reset the sampler heap if more than half of the descriptors are used.
  // This saves descriptor copying when there isn't a large number of sampler configs per frame.
  return m_group_count >= (D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE / 2);
}

void SamplerAllocator::Reset()
{
  DescriptorAllocator::Reset();
  m_group_count = 0;
}
}  // namespace DX12
