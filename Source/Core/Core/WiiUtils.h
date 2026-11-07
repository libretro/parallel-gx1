// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

#include "Common/CommonTypes.h"
#include "Core/IOS/ES/Formats.h"
#include "Core/IOS/USB/Bluetooth/BTReal.h"

// Small utility functions for common Wii related tasks.

namespace DiscIO
{
class VolumeWAD;
}

namespace IOS::HLE
{
class BluetoothEmuDevice;
class ESCore;
class Kernel;
}  // namespace IOS::HLE

namespace IOS::HLE::FS
{
class FileSystem;
}

namespace WiiUtils
{
enum class InstallType
{
  Permanent,
  Temporary,
};

bool InstallWAD(IOS::HLE::Kernel& ios, const DiscIO::VolumeWAD& wad, InstallType type);
// Same as the above, but constructs a temporary IOS and VolumeWAD instance for importing
// and does a permanent install.
bool EnsureTMDIsImported(IOS::HLE::FS::FileSystem& fs, IOS::HLE::ESCore& es, u64 title_id);

std::shared_ptr<IOS::HLE::BluetoothEmuDevice> GetBluetoothEmuDevice();
// Same as GetBluetoothEmuDevice, but for Bluetooth passthrough.
std::shared_ptr<IOS::HLE::BluetoothRealDevice> GetBluetoothRealDevice();
}  // namespace WiiUtils
