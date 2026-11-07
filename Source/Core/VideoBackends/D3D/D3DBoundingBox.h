// Copyright 2014 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/CommonTypes.h"
#include "VideoBackends/D3D/D3DBase.h"

#include "VideoCommon/BoundingBox.h"

namespace DX11
{
class D3DBoundingBox final : public BoundingBox
{
  /* called by the base class as this backend */
  friend class ::BoundingBox;

public:
  ~D3DBoundingBox();

  bool Initialize();

protected:
  void Read(u32 index, u32 length, BBoxType* values);
  void Write(u32 index, const BBoxType* values, u32 length);

private:
  ComPtr<ID3D11Buffer> m_buffer;
  ComPtr<ID3D11Buffer> m_staging_buffer;
  ComPtr<ID3D11UnorderedAccessView> m_uav;
};

}  // namespace DX11
