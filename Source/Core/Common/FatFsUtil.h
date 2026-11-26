// Copyright 2022 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/Common.h"
#include "Common/CommonTypes.h"

namespace Common
{
static constexpr auto SD_PACK_TEXT = _trans("Pack SD Card Now");
static constexpr auto SD_UNPACK_TEXT = _trans("Unpack SD Card Now");

bool SyncSDFolderToSDImage();
bool SyncSDImageToSDFolder();

class FatFsCallbacks
{
public:
  FatFsCallbacks();
  virtual ~FatFsCallbacks();

  virtual u8 DiskInitialize(u8 pdrv);
  virtual u8 DiskStatus(u8 pdrv);
  virtual int DiskRead(u8 pdrv, u8* buff, u32 sector, unsigned int count) = 0;
  virtual int DiskWrite(u8 pdrv, const u8* buff, u32 sector, unsigned int count) = 0;
  virtual int DiskIOCtl(u8 pdrv, u8 cmd, void* buff) = 0;
  virtual u32 GetCurrentTimeFAT();
};

// Runs function with FatFs reading and writing through callbacks; false, without running it, when
// FatFs is busy on another thread
bool RunInFatFsContext(FatFsCallbacks& callbacks, void (*function)(void* user), void* user);
}  // namespace Common
