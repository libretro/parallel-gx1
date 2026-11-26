// Copyright 2016 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

class BootSessionData;

namespace Core
{
enum class RestoreReason
{
  EmulationEnd,
  CrashRecovery,
};

void InitializeWiiRoot();
void ShutdownWiiRoot();

bool WiiRootIsInitialized();

void BackupWiiSettings();
void RestoreWiiSettings(RestoreReason reason);

// Initialize the filesystem contents.
void InitializeWiiFileSystemContents(const BootSessionData& boot_session_data);

}  // namespace Core
