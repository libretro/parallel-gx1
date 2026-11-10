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

void InitializeWiiRoot(bool use_temporary);
void ShutdownWiiRoot();

bool WiiRootIsInitialized();
bool WiiRootIsTemporary();

void BackupWiiSettings();
void RestoreWiiSettings(RestoreReason reason);

// Initialize or clean up the filesystem contents.
void InitializeWiiFileSystemContents(const BootSessionData& boot_session_data);
void CleanUpWiiFileSystemContents(const BootSessionData& boot_session_data);

}  // namespace Core
