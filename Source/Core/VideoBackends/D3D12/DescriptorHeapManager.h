// Copyright 2019 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "VideoBackends/D3D12/Common.h"
#include "VideoCommon/RenderState.h"

namespace DX12
{
// This class provides an abstraction for D3D12 descriptor heaps.
struct DescriptorHandle final
{
  D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle;
  D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle;
  u32 index;

  operator bool() const { return cpu_handle.ptr != 0; }
};

class DescriptorHeapManager final
{
public:
  DescriptorHeapManager();
  ~DescriptorHeapManager();

  ID3D12DescriptorHeap* GetDescriptorHeap() const { return m_descriptor_heap.Get(); }
  u32 GetDescriptorIncrementSize() const { return m_descriptor_increment_size; }

  bool Create(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, u32 num_descriptors);

  bool Allocate(DescriptorHandle* handle);
  void Free(const DescriptorHandle& handle);
  void Free(u32 index);

private:
  ComPtr<ID3D12DescriptorHeap> m_descriptor_heap;
  u32 m_num_descriptors = 0;
  u32 m_descriptor_increment_size = 0;

  D3D12_CPU_DESCRIPTOR_HANDLE m_heap_base_cpu = {};

  // One bit per descriptor, set while it is free
  u64* m_free_slots = nullptr;
  u32 m_free_slot_words = 0;
};

class SamplerHeapManager final
{
public:
  SamplerHeapManager();
  ~SamplerHeapManager();

  ID3D12DescriptorHeap* GetDescriptorHeap() const { return m_descriptor_heap.Get(); }

  bool Create(ID3D12Device* device, u32 num_descriptors);
  bool Lookup(const SamplerState& ss, D3D12_CPU_DESCRIPTOR_HANDLE* handle);
  void Clear();

private:
  ComPtr<ID3D12DescriptorHeap> m_descriptor_heap;
  u32 m_num_descriptors = 0;
  u32 m_descriptor_increment_size = 0;
  u32 m_current_offset = 0;

  D3D12_CPU_DESCRIPTOR_HANDLE m_heap_base_cpu{};

  // The samplers made, sorted by their state's bits; one per descriptor at most
  struct CachedSampler
  {
    u64 key;
    D3D12_CPU_DESCRIPTOR_HANDLE handle;
  };
  CachedSampler* m_samplers = nullptr;
  u32 m_sampler_count = 0;
};
}  // namespace DX12
