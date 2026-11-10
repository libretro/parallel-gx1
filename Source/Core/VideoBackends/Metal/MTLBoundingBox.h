// Copyright 2022 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/BoundingBox.h"

#include <Metal/Metal.h>

#include "VideoBackends/Metal/MRCHelpers.h"

namespace Metal
{
class BoundingBox final : public ::BoundingBox
{
  /* called by the base class as this backend */
  friend class ::BoundingBox;

public:
  ~BoundingBox();

  bool Initialize();

protected:
  void Read(u32 index, u32 length, BBoxType* values);
  void Write(u32 index, const BBoxType* values, u32 length);

private:
  BBoxType* m_cpu_buffer_ptr;
  MRCOwned<id<MTLFence>> m_download_fence;
  MRCOwned<id<MTLFence>> m_upload_fence;
  MRCOwned<id<MTLBuffer>> m_cpu_buffer;
  MRCOwned<id<MTLBuffer>> m_gpu_buffer;
};
}  // namespace Metal
