// Copyright 2014 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "UICommon/UICommon.h"

#include <algorithm>
#include <clocale>
#include <cmath>
#include <locale>
#include <memory>

#include <fmt/format.h>

#include "Common/Common.h"
#include "Common/CommonPaths.h"
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/Logging/LogManager.h"
#include "Common/MathUtil.h"
#include "Common/MsgHandler.h"

#include "Core/Config/MainSettings.h"
#include "Core/ConfigLoaders/BaseConfigLoader.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/HW/GBAPad.h"
#include "Core/HW/GCKeyboard.h"
#include "Core/HW/GCPad.h"
#include "Core/HW/ProcessorInterface.h"
#include "Core/HW/Wiimote.h"
#include "Core/IOS/IOS.h"
#include "Core/IOS/STM/STM.h"
#include "Core/System.h"
#include "Core/WiiRoot.h"

#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/GCAdapter.h"


#include "VideoCommon/VideoBackendBase.h"
#include "VideoCommon/VideoConfig.h"

namespace UICommon
{
static Config::ConfigChangedCallbackID s_config_changed_callback_id;
static Common::HookableEvent<> s_flush_unsaved_data_event_hook;

static void CreateDumpPath(std::string path)
{
  if (!path.empty())
    File::SetUserPath(D_DUMP_IDX, std::move(path));
  File::CreateFullPath(File::GetUserPath(D_DUMPAUDIO_IDX));
  File::CreateFullPath(File::GetUserPath(D_DUMPDSP_IDX));
  File::CreateFullPath(File::GetUserPath(D_DUMPSSL_IDX));
  File::CreateFullPath(File::GetUserPath(D_DUMPOBJECTS_IDX));
  File::CreateFullPath(File::GetUserPath(D_DUMPTEXTURES_IDX));
}

static void CreateLoadPath(std::string path)
{
  if (!path.empty())
    File::SetUserPath(D_LOAD_IDX, std::move(path));
  File::CreateFullPath(File::GetUserPath(D_HIRESTEXTURES_IDX));
  File::CreateFullPath(File::GetUserPath(D_GRAPHICSMOD_IDX));
  File::CreateFullPath(File::GetUserPath(D_DYNAMICINPUT_IDX));
}

static void CreateWFSPath(const std::string& path)
{
  if (!path.empty())
    File::SetUserPath(D_WFSROOT_IDX, path + '/');
}

static void InitCustomPaths()
{
  File::SetUserPath(D_WIIROOT_IDX, Config::Get(Config::MAIN_FS_PATH));
  CreateLoadPath(Config::Get(Config::MAIN_LOAD_PATH));
  CreateDumpPath(Config::Get(Config::MAIN_DUMP_PATH));
  CreateWFSPath(Config::Get(Config::MAIN_WFS_PATH));
  File::SetUserPath(F_WIISDCARDIMAGE_IDX, Config::Get(Config::MAIN_WII_SD_CARD_IMAGE_PATH));
  File::SetUserPath(D_WIISDCARDSYNCFOLDER_IDX,
                    Config::Get(Config::MAIN_WII_SD_CARD_SYNC_FOLDER_PATH));
  File::CreateFullPath(File::GetUserPath(D_WIISDCARDSYNCFOLDER_IDX));
#ifdef HAS_LIBMGBA
  File::SetUserPath(F_GBABIOS_IDX, Config::Get(Config::MAIN_GBA_BIOS_PATH));
  File::SetUserPath(D_GBASAVES_IDX, Config::Get(Config::MAIN_GBA_SAVES_PATH));
  File::CreateFullPath(File::GetUserPath(D_GBASAVES_IDX));
#endif
}

static void RefreshConfig()
{
  Common::SetEnableAlert(Config::Get(Config::MAIN_USE_PANIC_HANDLERS));
  Common::SetAbortOnPanicAlert(Config::Get(Config::MAIN_ABORT_ON_PANIC_ALERT));
}

void Init()
{
  Core::RestoreWiiSettings(Core::RestoreReason::CrashRecovery);

  Config::Init();
  const auto config_changed_callback = [] {
    InitCustomPaths();
    RefreshConfig();
  };
  s_config_changed_callback_id = Config::AddConfigChangedCallback(config_changed_callback);
  Config::AddLayer(ConfigLoaders::GenerateBaseConfigLoader());
  SConfig::Init();
  g_Config.Init();
  Common::Log::LogManager::Init();
  VideoBackendBase::ActivateBackend(Config::Get(Config::MAIN_GFX_BACKEND));

  RefreshConfig();
}

void Shutdown()
{
  Config::RemoveConfigChangedCallback(s_config_changed_callback_id);

  GCAdapter::Shutdown();
  WiimoteReal::Shutdown();
  Common::Log::LogManager::Shutdown();
  g_Config.Shutdown();
  SConfig::Shutdown();
  Config::Shutdown();
}

[[nodiscard]] Common::EventHook AddFlushUnsavedDataCallback(std::function<void()> callback)
{
  return s_flush_unsaved_data_event_hook.Register(std::move(callback));
}

void FlushUnsavedData()
{
  INFO_LOG_FMT(CORE, "Flushing unsaved data...");
  s_flush_unsaved_data_event_hook.Trigger();
}

void ShutdownControllers()
{
  g_controller_interface.Shutdown();
}

void CreateDirectories()
{
  File::CreateFullPath(File::GetUserPath(D_USER_IDX));
  File::CreateFullPath(File::GetUserPath(D_CACHE_IDX));
  File::CreateFullPath(File::GetUserPath(D_COVERCACHE_IDX));
  File::CreateFullPath(File::GetUserPath(D_CONFIG_IDX));
  File::CreateFullPath(File::GetUserPath(D_CONFIG_IDX) + GRAPHICSMOD_CONFIG_DIR DIR_SEP);
  File::CreateFullPath(File::GetUserPath(D_DUMPDSP_IDX));
  File::CreateFullPath(File::GetUserPath(D_DUMPSSL_IDX));
  File::CreateFullPath(File::GetUserPath(D_DUMPTEXTURES_IDX));
  File::CreateFullPath(File::GetUserPath(D_GAMESETTINGS_IDX));
  File::CreateFullPath(File::GetUserPath(D_GCUSER_IDX));
  File::CreateFullPath(File::GetUserPath(D_GCUSER_IDX) + USA_DIR DIR_SEP);
  File::CreateFullPath(File::GetUserPath(D_GCUSER_IDX) + EUR_DIR DIR_SEP);
  File::CreateFullPath(File::GetUserPath(D_GCUSER_IDX) + JAP_DIR DIR_SEP);
  File::CreateFullPath(File::GetUserPath(D_TRIUSER_IDX));
  File::CreateFullPath(File::GetUserPath(D_HIRESTEXTURES_IDX));
  File::CreateFullPath(File::GetUserPath(D_GRAPHICSMOD_IDX));
  File::CreateFullPath(File::GetUserPath(D_LOGS_IDX));
  File::CreateFullPath(File::GetUserPath(D_MAPS_IDX));
  File::CreateFullPath(File::GetUserPath(D_RETROACHIEVEMENTSCACHE_IDX));
  File::CreateFullPath(File::GetUserPath(D_ASM_ROOT_IDX));
  File::CreateFullPath(File::GetUserPath(D_WFSROOT_IDX));
#ifndef ANDROID
  File::CreateFullPath(File::GetUserPath(D_THEMES_IDX));
  File::CreateFullPath(File::GetUserPath(D_STYLES_IDX));
#else
  // Disable media scanning in directories with a .nomedia file
  File::CreateEmptyFile(File::GetUserPath(D_COVERCACHE_IDX) + DIR_SEP NOMEDIA_FILE);
#endif
}

void SetUserDirectory(std::string custom_path)
{
  File::CreateFullPath(custom_path + DIR_SEP);
  File::SetUserPath(D_USER_IDX, std::move(custom_path));
}

bool TriggerSTMPowerEvent()
{
  const auto ios = Core::System::GetInstance().GetIOS();
  if (!ios)
    return false;

  const auto stm = ios->GetDeviceByName("/dev/stm/eventhook");
  if (!stm || !std::static_pointer_cast<IOS::HLE::STMEventHookDevice>(stm)->HasHookInstalled())
    return false;

  Core::DisplayMessage("Shutting down", 30000);
  ios->GetSystem().GetProcessorInterface().PowerButton_Tap();

  return true;
}

std::string FormatSize(u64 bytes, int decimals)
{
  // i18n: The symbol for the unit "bytes"
  const char* const unit_symbols[] = {_trans("B"),   _trans("KiB"), _trans("MiB"), _trans("GiB"),
                                      _trans("TiB"), _trans("PiB"), _trans("EiB")};

  // Find largest power of 2 less than size.
  // div 10 to get largest named unit less than size
  // 10 == log2(1024) (number of B in a KiB, KiB in a MiB, etc)
  // Max value is 63 / 10 = 6
  const int unit = MathUtil::IntLog2(std::max<u64>(bytes, 1)) / 10;

  // Don't need exact values, only 5 most significant digits
  const double unit_size = std::pow(2, unit * 10);
  return fmt::format("{:.{}Lf} {}", bytes / unit_size, decimals,
                     Common::GetStringT(unit_symbols[unit]));
}
}  // namespace UICommon
