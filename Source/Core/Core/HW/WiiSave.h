// Copyright 2010 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <functional>
#include <string>

#include "Common/CommonTypes.h"

namespace IOS::HLE
{
namespace FS
{
class FileSystem;
}
class IOSC;
}  // namespace IOS::HLE

namespace WiiSave
{
class Storage;
// Each freed with FreeStorage
Storage* MakeNandStorage(IOS::HLE::FS::FileSystem* fs, u64 tid);
Storage* MakeDataBinStorage(IOS::HLE::IOSC* iosc, const std::string& path, const char* mode);
void FreeStorage(Storage* storage);

enum class CopyResult
{
  Success,
  Error,
  Cancelled,
  CorruptedSource,
  TitleMissing,
  NumberOfEntries
};

CopyResult Copy(Storage* source, Storage* destination);
}  // namespace WiiSave
