// Copyright 2010 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <functional>
#include <memory>
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
struct StorageDeleter
{
  void operator()(Storage* p) const;
};

using StoragePointer = std::unique_ptr<Storage, StorageDeleter>;
StoragePointer MakeNandStorage(IOS::HLE::FS::FileSystem* fs, u64 tid);
StoragePointer MakeDataBinStorage(IOS::HLE::IOSC* iosc, const std::string& path, const char* mode);

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
