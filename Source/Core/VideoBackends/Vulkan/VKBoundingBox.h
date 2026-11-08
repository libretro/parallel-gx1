// Copyright 2016 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <string>

#include "Common/CommonTypes.h"
#include "VideoBackends/Vulkan/StagingBuffer.h"
#include "VideoBackends/Vulkan/VulkanLoader.h"

#include "VideoCommon/BoundingBox.h"

namespace Vulkan
{

class VKBoundingBox final : public BoundingBox
{
  /* called by the base class as this backend */
  friend class ::BoundingBox;

public:
  ~VKBoundingBox();

  bool Initialize();

protected:
  void Read(u32 index, u32 length, BBoxType* values);
  void Write(u32 index, const BBoxType* values, u32 length);

private:
  bool CreateGPUBuffer();
  bool CreateReadbackBuffer();

  VkBuffer m_gpu_buffer = VK_NULL_HANDLE;
  vk_heap_alloc_t m_gpu_allocation = {};

  static constexpr size_t BUFFER_SIZE = sizeof(BBoxType) * NUM_BBOX_VALUES;

  StagingBuffer* m_readback_buffer = nullptr;
};

}  // namespace Vulkan
