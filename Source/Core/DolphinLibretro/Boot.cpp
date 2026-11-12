#include <cctype>
#include <cstdio>
#include <string.h>
#include <functional>
#include <iterator>
#include <libretro.h>
#include <string>

#include <streams/file_stream.h>
#include <vfs/vfs_hybrid.h>
#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/StringUtil.h"
#include "Common/Thread.h"
#include "Common/Version.h"
#include "Core/Boot/Boot.h"
#include "Core/BootManager.h"
#include "Core/CommonTitles.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/SYSCONFSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/GeckoCodeConfig.h"
#include "Core/HW/DVD/DVDInterface.h"
#include "Core/HW/EXI/EXI_Device.h"
#include "Core/HW/HSP/HSP_Device.h"
#include "Core/HW/VideoInterface.h"
#include "Core/HW/WiimoteReal/WiimoteReal.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"
#include "DolphinLibretro/Audio.h"
#include "DolphinLibretro/Common/Globals.h"
#include "DolphinLibretro/Common/Options.h"
#include "DolphinLibretro/Input.h"
#include "DolphinLibretro/Log.h"
#include "DolphinLibretro/VR.h"
#include "DolphinLibretro/Video.h"
#include "DolphinLibretro/VideoContexts/ContextStatus.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/ControllerInterface/DualShockUDPClient/DualShockUDPClient.h"
#include "UICommon/UICommon.h"
#include "VideoCommon/AsyncRequests.h"
#include "VideoCommon/Fifo.h"
#include "VideoCommon/GBAScreens.h"
#include "VideoCommon/Resources/CustomResourceManager.h"
#include "VideoCommon/VertexLoaderManager.h"
#include "VideoCommon/VideoBackendBase.h"
#include "VideoCommon/VideoConfig.h"

namespace Libretro
{
extern retro_environment_t environ_cb;

// Disk swapping
static void InitDiskControlInterface();
static unsigned disk_index = 0;
static bool eject_state;
static std::vector<std::string> disk_paths;

static void ResetDiskControlState()
{
  disk_index = 0;
  eject_state = false;
  disk_paths.clear();
}

// GBPlayer
static std::string GBPlayer_rom_path;
static bool GBPlayer_active;

// silence forward declaration warnings
void reload_cheats_from_ini();
void generate_cht_from_ini(std::string fileName);

void reload_cheats_from_ini()
{
  // Load Dolphin INI files
  const auto& sconfig = SConfig::GetInstance();
  Common::IniFile globalIni = sconfig.LoadDefaultGameIni();
  Common::IniFile localIni = sconfig.LoadLocalGameIni();

  // Load ActionReplay codes
  Libretro::g_ar_codes = ActionReplay::LoadCodes(globalIni, localIni);

  // Load Gecko codes
  Libretro::g_gecko_codes = Gecko::LoadCodes(globalIni, localIni);
}

void generate_cht_from_ini(std::string fileName)
{
  const char* sysdir = nullptr;

  if (!Libretro::environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &sysdir) || !sysdir)
    return;

  std::string base = sysdir;
  size_t pos = base.find_last_of("/\\");
  if (pos != std::string::npos)
      base = base.substr(0, pos);

  // Write RetroArch .cht
  std::string cheatDir = base + "/cheats/dolphin-emu/";
  std::string cheatFile = cheatDir + fileName + ".cht";

  if (File::Exists(cheatFile))
  {
    INFO_LOG(BOOT, "Cheats: pre-existing cht file found: %s", cheatFile.c_str());
    return;
  }

  size_t total = Libretro::g_ar_codes.size() + Libretro::g_gecko_codes.size();
  if (total == 0)
  {
    INFO_LOG(BOOT, "Cheats: not generating cht file as no ActionReply or Gecko codes found");
    return;
  }

  if (!File::CreateFullPath(cheatFile))
    return;

  RFILE* const out = filestream_open(cheatFile.c_str(), RETRO_VFS_FILE_ACCESS_WRITE,
                                     RETRO_VFS_FILE_ACCESS_HINT_NONE);
  unsigned cheat_index = 0;
  bool written = out != nullptr;

  if (written)
  {
    filestream_printf(out, "cheats = %u\n\n", static_cast<unsigned>(total));

    for (const auto& c : Libretro::g_ar_codes)
    {
      bool first = true;
      filestream_printf(out, "cheat%u_desc = \"%s\"\ncheat%u_code = \"", cheat_index,
                        c.name.c_str(), cheat_index);
      for (const auto& op : c.ops)
      {
        filestream_printf(out, "%s%08X %08X", first ? "" : "+", op.cmd_addr, op.value);
        first = false;
      }
      filestream_printf(out, "\"\ncheat%u_enable = %s\n\n", cheat_index++,
                        c.enabled ? "true" : "false");
    }

    for (const auto& c : Libretro::g_gecko_codes)
    {
      bool first = true;
      filestream_printf(out, "cheat%u_desc = \"%s\"\ncheat%u_code = \"", cheat_index,
                        c.name.c_str(), cheat_index);
      for (const auto& line : c.codes)
      {
        filestream_printf(out, "%s%s", first ? "" : "+", line.original_line.c_str());
        first = false;
      }
      filestream_printf(out, "\"\ncheat%u_enable = %s\n\n", cheat_index++,
                        c.enabled ? "true" : "false");
    }

    written = !filestream_error(out);
    written = filestream_close(out) == 0 && written;
  }

  if (!written)
  {
    ERROR_LOG(BOOT, "Cheats: unable to create a new cht file at: %s", cheatFile.c_str());
    return;
  }

  INFO_LOG(BOOT, "Cheats: cht file created successfully at: %s", cheatFile.c_str());
}

// A region-free title runs in the fallback region, which follows the system locale and is PAL
// when there is none, as for Kodi on LibreELEC. A Japanese title can hang on a PAL console, so a
// region-free title runs in the region its game ID names.
static void SetRegionFreeFallback(const BootParameters& boot_params)
{
  const DiscIO::Volume* volume = nullptr;
  if (const auto* disc = std::get_if<BootParameters::Disc>(&boot_params.parameters))
    volume = disc->volume.get();
  else if (const auto* wad = std::get_if<DiscIO::VolumeWAD>(&boot_params.parameters))
    volume = wad;

  if (!volume || volume->GetRegion() != DiscIO::Region::Unknown)
    return;

  const std::string game_id = volume->GetGameID();
  if (game_id.size() < 4)
    return;

  const DiscIO::Region region = DiscIO::CountryCodeToRegion(
      game_id[3], volume->GetVolumeType(), Config::Get(Config::MAIN_FALLBACK_REGION));
  if (region == DiscIO::Region::Unknown)
    return;

  NOTICE_LOG(BOOT, "Region-free title %s: running it as %s", game_id.c_str(),
             DiscIO::GetName(region, false).c_str());
  Config::SetCurrent(Config::MAIN_FALLBACK_REGION, region);
}

} // namespace Libretro

/* The path without its last component, and its last component, either
 * separator taken and trailing ones ignored. */
static std::string PathParent(const std::string& path)
{
  size_t end = path.find_last_not_of("/\\");
  size_t slash;
  if (end == std::string::npos)
    return std::string();
  slash = path.find_last_of("/\\", end);
  return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

static std::string PathLast(const std::string& path)
{
  size_t end = path.find_last_not_of("/\\");
  size_t slash;
  if (end == std::string::npos)
    return std::string();
  slash = path.find_last_of("/\\", end);
  return path.substr(slash == std::string::npos ? 0 : slash + 1,
                     end - (slash == std::string::npos ? 0 : slash + 1) + 1);
}

/* A load that fails returns to a frontend that calls retro_deinit next,
 * never retro_unload_game: what the load brought up comes down here, the
 * Wii Remote scanner's threads above all, which would otherwise outlive
 * the core's controllers. */
static void UndoFailedLoad(bool input_up)
{
  if (input_up)
    Libretro::Input::Shutdown();
  Libretro::Log::Shutdown();
  UICommon::ShutdownControllers();
  UICommon::Shutdown();
  Libretro::ResetDiskControlState();
}

bool retro_load_game(const struct retro_game_info* game)
{
  Libretro::ResetDiskControlState();

  const char* save_dir = NULL;
  const char* gba_save_dir = NULL;
  const char* system_dir = NULL;
  const char* core_assets_dir = NULL;
  std::string rebuild_save_dir;
  std::string user_dir;
  std::string sys_dir;

  /*
   *  If the GBPlayer is active, we try to reconstruct the GC save dir location
   *  with regards to content sorting and core sorting. If we do not do this,
   *  there will be a User directory with GC saves in the GB/GBC/GBA rom save path
   *  when a user has content/core sorting enabled. Since the user is trying to avoid
   *  said behavior actively if those sorting options are enabled,
   *  we should seperate them accordingly.
   */
  if (Libretro::GBPlayer_active)
  {
    std::string gba_content_dir;
    if (!Libretro::GBPlayer_rom_path.empty())
      gba_content_dir = PathLast(PathParent(Libretro::GBPlayer_rom_path));

    Libretro::environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &gba_save_dir);
    std::string base = gba_save_dir ? gba_save_dir : "";

    bool core_sort = false;
    retro_system_info sys_info{};
    retro_get_system_info(&sys_info);
    const std::string library_name = sys_info.library_name;

    if (!base.empty() && !library_name.empty())
      core_sort = Common::CaseInsensitiveEquals(PathLast(base), library_name);
    if (core_sort)
      base = PathParent(base);

    bool content_sort = false;
    std::string gc_content_dir;
    if (!gba_content_dir.empty() && !base.empty())
      content_sort = Common::CaseInsensitiveEquals(PathLast(base), gba_content_dir);
    if (content_sort)
    {
      gc_content_dir = PathLast(PathParent(game->path));
      base = PathParent(base);
    }

    rebuild_save_dir = base;
    if (content_sort)
      rebuild_save_dir = rebuild_save_dir + DIR_SEP + gc_content_dir;
    if (core_sort)
      rebuild_save_dir = rebuild_save_dir + DIR_SEP + library_name;

    save_dir = rebuild_save_dir.c_str();
    INFO_LOG(BOOT, "GBPlayer: GC save dir reconstructed to: '%s'", save_dir);
  }
  else
  {
    Libretro::environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &save_dir);
  }

  Libretro::environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &system_dir);
  Libretro::environ_cb(RETRO_ENVIRONMENT_GET_CORE_ASSETS_DIRECTORY, &core_assets_dir);
  Libretro::InitDiskControlInterface();

  if (save_dir && *save_dir)
    user_dir = std::string(save_dir) + DIR_SEP "User";
  else if (system_dir && *system_dir)
    user_dir = std::string(system_dir) + DIR_SEP "dolphin-emu" DIR_SEP "User";

  if (system_dir && *system_dir)
    sys_dir = std::string(system_dir) + DIR_SEP "dolphin-emu" DIR_SEP "Sys";
  else if (core_assets_dir && *core_assets_dir)
    sys_dir = std::string(core_assets_dir) + DIR_SEP "dolphin-emu" DIR_SEP "Sys";
  else if (save_dir && *save_dir)
    sys_dir = std::string(save_dir) + DIR_SEP "Sys";
  else
    sys_dir = "dolphin-emu" DIR_SEP "Sys";

  using namespace Libretro::Options;
  Libretro::Options::Init();
  vfs_hybrid_init(Libretro::environ_cb, nullptr);

#ifdef ANDROID
  static bool sysdir_set = false;

  if(!sysdir_set)
  {
    File::SetSysDirectory(sys_dir);
    sysdir_set = true;
  }
#else
  File::SetSysDirectory(sys_dir);
#endif

  UICommon::SetUserDirectory(user_dir);
  /* Shader caches live in the system directory whatever the save directory is: they belong to
   * the core and the host's GPU, not to a game's saves. */
  if (system_dir && *system_dir)
  {
    const std::string shader_dir = std::string(system_dir) + DIR_SEP "dolphin-emu" DIR_SEP
                                   "User" DIR_SEP "Cache" DIR_SEP "Shaders" DIR_SEP;
    File::CreateFullPath(shader_dir);
    File::SetUserPath(D_SHADERCACHE_IDX, shader_dir);
  }
  UICommon::CreateDirectories();
  UICommon::Init();
  /* the GBAs on the controller ports boot from the BIOS in the system
   * directory when it is there, where the frontend's GBA cores look too */
  if (system_dir && *system_dir)
  {
    const std::string gba_bios = std::string(system_dir) + DIR_SEP GBA_BIOS;
    if (File::Exists(gba_bios))
      File::SetUserPath(F_GBABIOS_IDX, gba_bios);
  }
  Libretro::Log::Init();

  if (Libretro::GBPlayer_active)
  {
    Config::SetBase(Config::MAIN_GBA_ROM_PATHS[Config::GBPLAYER_GBA_INDEX], Libretro::GBPlayer_rom_path);
    INFO_LOG(BOOT, "GBPlayer: GBA ROM path applied from subsystem: '%s'",
             Libretro::GBPlayer_rom_path.c_str());
    Config::SetBase(Config::MAIN_GBA_SAVES_PATH, std::string(gba_save_dir));
    INFO_LOG(BOOT, "GBPlayer: ROM save set to: '%s'", gba_save_dir);
    // In order to prevent confusion for the settings of the save location between RA and the
    // emulator we just turn this off
    Config::SetBase(Config::MAIN_GBA_SAVES_IN_ROM_PATH, false);
  }
  else
  {
    Config::SetBase(Config::MAIN_GBA_ROM_PATHS[Config::GBPLAYER_GBA_INDEX], std::string{});
  }

  Common::SetEnableAlert(false);
  Common::SetAbortOnPanicAlert(false);
  Common::RegisterMsgAlertHandler([](const char* caption, const char* text,
    bool yes_no, Common::MsgType style) -> bool
  {
    // Log the message instead of showing a popup
    WARN_LOG(COMMON, "Suppressed popup: %s - %s", caption, text);

    if (Libretro::GetOption<bool>(retroarch_core::ENABLE_LOG_POPUPS,
                            /*def=*/false))
      Libretro::Log::LogFrontEnd(style, caption, text, 2000);
    return true; // Always "continue"
  });

  NOTICE_LOG(BOOT, "SCM Git revision: %s", Common::GetScmRevGitStr().c_str());
  NOTICE_LOG(BOOT, "User Directory set to '%s'", user_dir.c_str());
  NOTICE_LOG(BOOT, "System Directory set to '%s'", sys_dir.c_str());

  const std::string codehandler = std::string(sys_dir) + DIR_SEP GECKO_CODE_HANDLER;

  if (!File::Exists(codehandler))
  {
#if defined(ANDROID) || defined(IPHONEOS)
    // for reduced area to display the message..
    static const char missing_core_files_msg[] = "IMPORTANT - Open Online Updater ->\n"
                                                 "Core System Files Downloader -> Dolphin.zip.";
#else
    static const char missing_core_files_msg[] =
        "Core file " GECKO_CODE_HANDLER " missing! Open Online Updater ->\n"
        "Core System Files Downloader -> Install Dolphin.zip. "
        "Restart core to take effect.";
#endif

    Libretro::Log::LogFrontEnd(Common::Log::LogLevel::LERROR, missing_core_files_msg, 10000);

    ERROR_LOG(BOOT, "%s", missing_core_files_msg);
  }

  // Main.Core
  Config::SetBase(Config::MAIN_CPU_CORE,
    static_cast<PowerPC::CPUCore>(
        Libretro::GetOption<int>(
            core::CPU_CORE,
            static_cast<int>(PowerPC::DefaultCPUCore()))));

#if defined(_DEBUG)
  Config::SetBase(Config::MAIN_FASTMEM, false);
  Config::SetBase(Config::MAIN_FASTMEM_ARENA, false);
#else
  Config::SetBase(Config::MAIN_FASTMEM,
                     Libretro::GetOption<bool>(core::FASTMEM, /*def=*/true));
  Config::SetBase(Config::MAIN_FASTMEM_ARENA,
                     Libretro::GetOption<bool>(core::FASTMEM_ARENA, /*def=*/true));
#endif
  Config::SetBase(Config::MAIN_ACCURATE_CPU_CACHE,
    Libretro::GetOption<bool>(core::MAIN_ACCURATE_CPU_CACHE, /*def=*/false));

  Config::SetBase(Config::MAIN_DSP_HLE,
                     Libretro::GetOption<bool>(audio::DSP_HLE, /*def=*/true));

  // dual core (true) or single core (false)
  Config::SetBase(Config::MAIN_CPU_THREAD,
    Libretro::GetOption<bool>(core::MAIN_CPU_THREAD, /*def=*/true));

  // Dual core's GPU thread kept in step with the CPU thread (Dolphin's "Synchronize GPU Thread").
  Config::SetBase(Config::MAIN_SYNC_GPU,
    Libretro::GetOption<bool>(core::MAIN_SYNC_GPU, /*def=*/false));

  Config::SetBase(Config::MAIN_LOAD_GAME_INTO_MEMORY,
    Libretro::GetOption<bool>(core::MAIN_LOAD_GAME_INTO_MEMORY, /*def=*/false));

  Config::SetBase(Config::MAIN_ENABLE_CHEATS,
                     Libretro::GetOption<bool>(core::CHEATS_ENABLED, /*def=*/false));

  Config::SetBase(Config::MAIN_SKIP_IPL, Libretro::GetOption<bool>(core::SKIP_GC_BIOS, /*def=*/true));

  const int language = Libretro::GetOption<int>(core::LANGUAGE, static_cast<int>(DiscIO::Language::English));
  Config::SetBase(Config::SYSCONF_LANGUAGE, language);
  Config::SetBase(Config::MAIN_GC_LANGUAGE, DiscIO::ToGameCubeLanguage(static_cast<DiscIO::Language>(language)));

  {
    // Overclock (cpu clock rate) — option values in option_defs used strings like "100%" etc.
    double multiplier = Libretro::GetOption<double>(core::CPU_CLOCK_RATE, 1.0);
    Config::SetBase(Config::MAIN_OVERCLOCK, multiplier);
    Config::SetBase(Config::MAIN_OVERCLOCK_ENABLE, multiplier != 1.0);
  }

  Config::SetBase(Config::MAIN_WIIMOTE_CONTINUOUS_SCANNING,
                  Libretro::GetOption<bool>(sysconf::WIIMOTE_CONTINUOUS_SCANNING,
                                             /*def=*/false) ||
                  Libretro::GetOption<std::string>(wiimote::DEFAULT_DEVICE, "frontend") == "real");
  Config::SetBase(Config::MAIN_MMU,
                  Libretro::GetOption<bool>(core::MAIN_MMU, /*def=*/false));

  Config::SetBase(Config::MAIN_FAST_DISC_SPEED,
                  Libretro::GetOption<bool>(core::FAST_DISC_SPEED, /*def=*/false));

  // Main.General

  // Main.DSP
  Config::SetBase(Config::MAIN_DSP_JIT,
                     Libretro::GetOption<bool>(audio::DSP_JIT, /*def=*/true));

  // Main.BluetoothPassthrough
  Config::SetBase(Config::MAIN_BLUETOOTH_PASSTHROUGH_ENABLED,
    Libretro::GetOption<bool>(Libretro::Options::main_bluetooth::BLUETOOTH_PASSTHROUGH, /*def=*/false));

  // SYSCONF (GC)
  {
    const std::string value =
      Libretro::GetOption<std::string>(
        sysconf_gc::SP1_DEVICE,
        "255" // ExpansionInterface::EXIDeviceType::None
      );

    const int device_int = std::stoi(value);
    const auto device = static_cast<ExpansionInterface::EXIDeviceType>(device_int);

    Config::SetBase(Config::MAIN_SERIAL_PORT_1, device);
  }
  GBAScreensSetShown(Libretro::GetOption<bool>(sysconf_gc::GBA_SCREENS, /*def=*/true));

  // SYSCONF.IPL
  Config::SetBase(Config::SYSCONF_WIDESCREEN,
                 Libretro::GetOption<bool>(sysconf::WIDESCREEN, /*def=*/true));
  Config::SetBase(Config::SYSCONF_PROGRESSIVE_SCAN,
                 Libretro::GetOption<bool>(sysconf::PROGRESSIVE_SCAN, /*def=*/true));
  Config::SetBase(Config::SYSCONF_PAL60,
                 Libretro::GetOption<bool>(sysconf::PAL60, /*def=*/true));

  // SYSCONF.BT
  Config::SetBase(Config::SYSCONF_SENSOR_BAR_POSITION,
                 Libretro::GetOption<int>(sysconf::SENSOR_BAR_POSITION, 0));
  Config::SetBase(Config::SYSCONF_WIIMOTE_MOTOR,
                 Libretro::GetOption<bool>(sysconf::ENABLE_RUMBLE, /*def=*/true));

  // Graphics.Hardware
  //Config::SetBase(Config::GFX_VSYNC,
  //               Libretro::GetOption<bool>(gfx_hardware::VSYNC, /*def=*/false));

  // Graphics.Settings
  Config::SetBase(Config::GFX_WIDESCREEN_HACK,
                 Libretro::GetOption<bool>(gfx_settings::WIDESCREEN_HACK, /*def=*/false));
  // Dolphin stretches its picture over the frame; the Aspect Ratio option
  // sets the aspect ratio the frontend shows it at (Main.cpp, FrameAspect)
  Config::SetBase(Config::GFX_ASPECT_RATIO, AspectMode::Stretch);
  Config::SetBase(Config::GFX_SAFE_TEXTURE_CACHE_COLOR_SAMPLES,
                 Libretro::GetOption<int>(gfx_settings::TEXTURE_CACHE_ACCURACY, 128));
  Config::SetBase(Config::GFX_HIRES_TEXTURES,
                 Libretro::GetOption<bool>(gfx_enhancements::LOAD_CUSTOM_TEXTURES, /*def=*/false));
  Config::SetBase(Config::GFX_CACHE_HIRES_TEXTURES,
                 Libretro::GetOption<bool>(gfx_enhancements::CACHE_CUSTOM_TEXTURES, /*def=*/false));
  // auto: decided by the renderer, once the frontend has granted one (Video::Init)
  Config::SetBase(Config::GFX_ENABLE_GPU_TEXTURE_DECODING,
                 Libretro::GetOption<bool>(gfx_settings::GPU_TEXTURE_DECODING, /*def=*/false));
  Config::SetBase(Config::GFX_ENABLE_PIXEL_LIGHTING,
                  Libretro::GetOption<bool>(gfx_settings::ENABLE_PIXEL_LIGHTING, /*def=*/false));
  Config::SetBase(Config::GFX_FAST_DEPTH_CALC,
                 Libretro::GetOption<bool>(gfx_settings::FAST_DEPTH_CALCULATION, /*def=*/true));
  Config::SetBase(Config::GFX_EFB_SCALE,
                 Libretro::GetOption<int>(gfx_settings::EFB_SCALE, /*def=*/1));
  Config::SetBase(Config::GFX_BACKEND_MULTITHREADING, false);
  Config::SetBase(Config::GFX_SHADER_COMPILATION_MODE,
                  static_cast<ShaderCompilationMode>(
                    Libretro::GetOption<int>(
                      gfx_settings::SHADER_COMPILATION_MODE,
                      static_cast<int>(ShaderCompilationMode::AsynchronousUberShaders))));
  /* as many compiler threads as the machine has to spare, for building
   * pipelines both before the game starts and while it runs */
  Config::SetBase(Config::GFX_SHADER_COMPILER_THREADS, -1);
  Config::SetBase(Config::GFX_SHADER_PRECOMPILER_THREADS, -1);

  Config::SetBase(Config::GFX_WAIT_FOR_SHADERS_BEFORE_STARTING,
    Libretro::GetOption<bool>(gfx_settings::WAIT_FOR_SHADERS, /*def=*/false));

  Config::SetBase(Config::GFX_SHADER_CACHE,
    Libretro::GetOption<bool>(gfx_settings::SHADER_CACHE, /*def=*/false) && !Common::is_uwp());

  Config::SetBase(Config::GFX_MODS_ENABLE,
    Libretro::GetOption<bool>(gfx_settings::MODS_ENABLE, /*def=*/false));

  // Graphics.Enhancements
  Config::SetBase(Config::GFX_ENHANCE_FORCE_TEXTURE_FILTERING,
                  static_cast<TextureFilteringMode>(
                    Libretro::GetOption<int>(
                      gfx_enhancements::FORCE_TEXTURE_FILTERING_MODE,
                      static_cast<int>(TextureFilteringMode::Default))));

  Config::SetBase(Config::GFX_ENHANCE_MAX_ANISOTROPY,
                  static_cast<AnisotropicFilteringMode>(
                    Libretro::GetOption<int>(
                      gfx_enhancements::MAX_ANISOTROPY,
                      static_cast<int>(AnisotropicFilteringMode::Force1x))));

  Config::SetBase(Config::GFX_ENHANCE_FORCE_TRUE_COLOR,
                          Libretro::GetOption<bool>(gfx_enhancements::FORCE_TRUE_COLOR,
                                                    /*def=*/true));

  Config::SetBase(Config::GFX_ENHANCE_DISABLE_COPY_FILTER,
                  Libretro::GetOption<bool>(gfx_enhancements::GFX_ENHANCE_DISABLE_COPY_FILTER,
                    /*def=*/true));
  Config::SetBase(Config::GFX_ENHANCE_HDR_OUTPUT,
                  Libretro::GetOption<bool>(gfx_enhancements::GFX_ENHANCE_HDR_OUTPUT,
                                            /*def=*/false));
  Config::SetBase(Config::GFX_ENHANCE_ARBITRARY_MIPMAP_DETECTION,
                  Libretro::GetOption<bool>(gfx_enhancements::GFX_ARBITRARY_MIPMAP_DETECTION, /*def=*/false));

  // Graphics.Hacks
  Config::SetBase(Config::GFX_HACK_EFB_ACCESS_ENABLE,
                 Libretro::GetOption<bool>(gfx_hacks::EFB_ACCESS_ENABLE, /*def=*/false));
  Config::SetBase(Config::GFX_HACK_EFB_DEFER_INVALIDATION,
                 Libretro::GetOption<bool>(gfx_hacks::EFB_ACCESS_DEFER_INVALIDATION, /*def=*/false));
  Config::SetBase(Config::GFX_HACK_EFB_ACCESS_TILE_SIZE,
                 Libretro::GetOption<int>(gfx_hacks::EFB_ACCESS_TILE_SIZE, /*def=*/64));
  Config::SetBase(Config::GFX_HACK_BBOX_ENABLE,
                 Libretro::GetOption<bool>(gfx_hacks::BBOX_ENABLED, /*def=*/false));
  Config::SetBase(Config::GFX_HACK_FORCE_PROGRESSIVE,
                 Libretro::GetOption<bool>(gfx_hacks::FORCE_PROGRESSIVE, /*def=*/true));
  Config::SetBase(Config::GFX_HACK_SKIP_EFB_COPY_TO_RAM,
                 Libretro::GetOption<bool>(gfx_hacks::EFB_TO_TEXTURE, /*def=*/true));
  Config::SetBase(Config::GFX_HACK_SKIP_XFB_COPY_TO_RAM,
                 Libretro::GetOption<bool>(gfx_hacks::XFB_TO_TEXTURE_ENABLE, /*def=*/true));
  Config::SetBase(Config::GFX_HACK_DISABLE_COPY_TO_VRAM,
                 Libretro::GetOption<bool>(gfx_hacks::EFB_TO_VRAM, /*def=*/false));
  Config::SetBase(Config::GFX_HACK_DEFER_EFB_COPIES,
                 Libretro::GetOption<bool>(gfx_hacks::DEFER_EFB_COPIES, /*def=*/true));
  Config::SetBase(Config::GFX_HACK_IMMEDIATE_XFB,
                 Libretro::GetOption<bool>(gfx_hacks::IMMEDIATE_XFB, /*def=*/false));
  Config::SetBase(Config::GFX_HACK_SKIP_DUPLICATE_XFBS,
                 Libretro::GetOption<bool>(gfx_hacks::SKIP_DUPE_FRAMES, /*def=*/true));
  Config::SetBase(Config::GFX_HACK_EARLY_XFB_OUTPUT,
                 Libretro::GetOption<bool>(gfx_hacks::EARLY_XFB_OUTPUT, /*def=*/true));
  Config::SetBase(Config::GFX_HACK_COPY_EFB_SCALED,
                 Libretro::GetOption<bool>(gfx_hacks::EFB_SCALED_COPY, /*def=*/true));
  Config::SetBase(Config::GFX_HACK_EFB_EMULATE_FORMAT_CHANGES,
                 Libretro::GetOption<bool>(gfx_hacks::EFB_EMULATE_FORMAT_CHANGES, /*def=*/false));
  Config::SetBase(Config::GFX_HACK_VERTEX_ROUNDING,
                 Libretro::GetOption<bool>(gfx_hacks::VERTEX_ROUNDING, /*def=*/false));
  Config::SetBase(Config::GFX_HACK_FAST_TEXTURE_SAMPLING,
                 Libretro::GetOption<bool>(gfx_hacks::FAST_TEXTURE_SAMPLING, /*def=*/true));
  #ifdef __APPLE__
  Config::SetBase(Config::GFX_HACK_NO_MIPMAPPING,
                 Libretro::GetOption<bool>(gfx_hacks::NO_MIPMAPPING, /*def=*/false));
  #endif

  switch (Libretro::GetOption<int>(gfx_settings::ANTI_ALIASING, 0))
  {
    case 1:  // 2x MSAA
      Config::SetBase(Config::GFX_MSAA, 2);
      Config::SetBase(Config::GFX_SSAA, false);
      break;
    case 2:  // 4x MSAA
      Config::SetBase(Config::GFX_MSAA, 4);
      Config::SetBase(Config::GFX_SSAA, false);
      break;
    case 3:  // 8x MSAA
      Config::SetBase(Config::GFX_MSAA, 8);
      Config::SetBase(Config::GFX_SSAA, false);
      break;
    case 4:  // 2x SSAA
      Config::SetBase(Config::GFX_MSAA, 2);
      Config::SetBase(Config::GFX_SSAA, true);
      break;
    case 5:  // 4x SSAA
      Config::SetBase(Config::GFX_MSAA, 4);
      Config::SetBase(Config::GFX_SSAA, true);
      break;
    case 6:  // 8x SSAA
      Config::SetBase(Config::GFX_MSAA, 8);
      Config::SetBase(Config::GFX_SSAA, true);
      break;
    default: // disabled
      Config::SetBase(Config::GFX_MSAA, 1);
      Config::SetBase(Config::GFX_SSAA, false);
      break;
  }

  // Graphics.GameSpecific
  Config::SetBase(Config::GFX_PERF_QUERIES_ENABLE,
                 Libretro::GetOption<bool>(gfx_gamespecific::GFX_PERF_QUERIES_ENABLE, /*def=*/false));

  if (Libretro::VR::IsEnabled())
    Libretro::VR::ApplyConfig();

#ifdef CIFACE_USE_DUALSHOCKUDPCLIENT
  // wiimote - alternative input server
  {
    const bool dsu_enabled = Libretro::GetOption<bool>(
        Libretro::Options::wiimote_dsu::DSU_ENABLED, false);

    Config::SetBase(ciface::DualShockUDPClient::Settings::SERVERS_ENABLED, dsu_enabled);

    if (dsu_enabled)
    {
      const int o1 = Libretro::GetOption<int>(Libretro::Options::wiimote_dsu::DSU_IP_1, 127);
      const int o2 = Libretro::GetOption<int>(Libretro::Options::wiimote_dsu::DSU_IP_2, 0);
      const int o3 = Libretro::GetOption<int>(Libretro::Options::wiimote_dsu::DSU_IP_3, 0);
      const int o4 = Libretro::GetOption<int>(Libretro::Options::wiimote_dsu::DSU_IP_4, 1);
      const int port = Libretro::GetOption<int>(Libretro::Options::wiimote_dsu::DSU_PORT, 26760);

      char servers_entry[48];
      snprintf(servers_entry, sizeof(servers_entry), "DS4:%d.%d.%d.%d:%d;", o1, o2, o3, o4, port);

      Config::SetBase(ciface::DualShockUDPClient::Settings::SERVERS, std::string(servers_entry));
    }
  }
#endif

  SConfig::GetInstance().bBootToPause = true;

#ifdef IPHONEOS
  bool can_jit = false;
  {
    struct retro_exec_mem_alloc probe = {};
    probe.version = 1;
    probe.size = 0;
    if (Libretro::environ_cb(RETRO_ENVIRONMENT_EXEC_MEM_ALLOC, &probe))
    {
      if (probe.mode != RETRO_EXEC_MEM_MODE_UNAVAILABLE)
        can_jit = true;
    }
    else if (!Libretro::environ_cb(RETRO_ENVIRONMENT_GET_JIT_CAPABLE, &can_jit))
      can_jit = false;
  }
  if (!can_jit)
  {
    auto current = Config::Get(Config::MAIN_CPU_CORE);
    if (current == PowerPC::CPUCore::JIT64 ||
        current == PowerPC::CPUCore::JITARM64)
    {
      Config::SetBase(Config::MAIN_CPU_CORE, PowerPC::CPUCore::CachedInterpreter);
    }

    Config::SetBase(Config::GFX_VERTEX_LOADER_TYPE, VertexLoaderType::Software);

    Core::DisplayMessage("CPU: Just in time compiler disabled as unavailable on your system");
  }
#endif
  NOTICE_LOG(BOOT, "CPU Core: %s",
             Libretro::Options::CPUCoreToString(Config::Get(Config::MAIN_CPU_CORE)).c_str());
  NOTICE_LOG(BOOT, "Fastmem enabled = %s", (Config::Get(Config::MAIN_FASTMEM)) ? "Yes" : "No");

  Libretro::Video::Init();
  // The hardware renderer was asked for and no hardware context was granted (e.g. no GLES 3): fail the load so the
  // frontend reports it, rather than run on the null backend behind a black screen.
  if (!strcmp(Libretro::Options::GetCached<const char*>(Libretro::Options::gfx_settings::RENDERER, ""), "Hardware") &&
      Libretro::Video::hw_render.context_type == RETRO_HW_CONTEXT_NONE)
  {
    ERROR_LOG(VIDEO, "No hardware render context was granted; refusing to load");
    UndoFailedLoad(false);
    return false;
  }
  /* GPU texture decoding, auto: with Vulkan, which decodes ahead of the frame's draws */
  if (Libretro::GetOption<std::string>(Libretro::Options::gfx_settings::GPU_TEXTURE_DECODING,
                                       "auto") == "auto")
  {
    Config::SetBase(Config::GFX_ENABLE_GPU_TEXTURE_DECODING,
                    Libretro::Video::hw_render.context_type == RETRO_HW_CONTEXT_VULKAN);
  }
  /* Lines and points expanded to triangles in the vertex shader with Vulkan: their pipelines have
   * no geometry shader stage, and the ubershader ones are the triangles' (a third as many). */
  Config::SetBase(Config::GFX_PREFER_VS_FOR_LINE_POINT_EXPANSION,
                  Libretro::Video::hw_render.context_type == RETRO_HW_CONTEXT_VULKAN);
  VideoBackendBase::PopulateBackendInfo();
  NOTICE_LOG(VIDEO, "Using GFX backend: %s", Config::Get(Config::MAIN_GFX_BACKEND).c_str());

  std::vector<std::string> normalized_game_paths;
  normalized_game_paths.push_back(game->path);
  std::string folder_path_str;
  std::string filename_str;
  std::string extension;
  SplitPath(normalized_game_paths.front(), &folder_path_str, &filename_str, &extension);
  for (char& c : extension)
    c = static_cast<char>(tolower(static_cast<unsigned char>(c)));

#ifdef _WIN32
  // If SplitPath only gave us "D:", rebuild the real directory from the full path
  if (folder_path_str.size() == 2 && folder_path_str[1] == ':')
  {
    // take everything up to the last backslash
    size_t last_slash = normalized_game_paths.front().find_last_of("\\/");
    if (last_slash != std::string::npos)
      folder_path_str = normalized_game_paths.front().substr(0, last_slash + 1);
  }
#endif

  if (extension == ".m3u" || extension == ".m3u8")
  {
    normalized_game_paths = ReadM3UFile(normalized_game_paths.front(), folder_path_str);
    if (normalized_game_paths.empty())
    {
      ERROR_LOG(BOOT, "Could not boot %s. M3U contains no paths", game->path);
      UndoFailedLoad(false);
      return false;
    }
  }

  for (auto& normalized_game_path : normalized_game_paths)
    Libretro::disk_paths.push_back(normalized_game_path);

  Libretro::Input::Init();

  // for Wii
  if (Libretro::Input::g_has_microphone_support)
  {
    Config::SetCurrent(Config::MAIN_EMULATE_WII_SPEAK,
      Libretro::GetOption<bool>(sysconf::WII_SPEAK_ENABLE, /*def=*/false));
    Config::SetCurrent(Config::MAIN_WII_SPEAK_MUTED,
      Libretro::GetOption<bool>(sysconf::WII_SPEAK_MUTED, /*def=*/true));

    // logitech
    Config::SetCurrent(Config::MAIN_EMULATE_LOGITECH_MIC[0],
      Libretro::GetOption<bool>(sysconf::WII_LOGI_MICROPHONE_ENABLE, /*def=*/false));
  }

  const bool disc_based_games_boot_to_wii_menu = Libretro::GetOption<bool>(Libretro::Options::core::DISC_BASED_GAMES_BOOT_TO_WII_MENU, false);
  std::unique_ptr<BootParameters> boot_params = BootParameters::GenerateFromFile(normalized_game_paths);

  if (disc_based_games_boot_to_wii_menu)
  {
    bool is_disc_title = false;
    if (!normalized_game_paths.empty())
    {
      auto volume = DiscIO::CreateDisc(normalized_game_paths.front());
      if (volume && (volume->GetVolumeType() == DiscIO::Platform::WiiDisc || volume->GetVolumeType() == DiscIO::Platform::GameCubeDisc))
        is_disc_title = true;
      else
        WARN_LOG(BOOT, "'Disc Based Games Boot to Wii System Menu' enabled but content is not a "
                       "disc based game, ignoring Wii Menu");
    }

    const std::string wii_menu_tmd_file_name = Common::GetTMDFileName(Titles::SYSTEM_MENU, Common::FromWhichRoot::Configured);
    const bool wii_menu_installed = File::Exists(wii_menu_tmd_file_name);
    if (is_disc_title && !wii_menu_installed)
      WARN_LOG(BOOT,
               "'Disc Based Games Boot to Wii System Menu' enabled but Wii Menu not found at save "
               "location: %s, booting directly instead",
               wii_menu_tmd_file_name.c_str());

    if (is_disc_title && wii_menu_installed)
    {
      Config::SetBase(Config::MAIN_DEFAULT_ISO, normalized_game_paths.front());
      boot_params = std::make_unique<BootParameters>(BootParameters::NANDTitle{Titles::SYSTEM_MENU});
    }
  }

  if (boot_params)
    Libretro::SetRegionFreeFallback(*boot_params);

  if (!BootManager::BootCore(Core::System::GetInstance(), std::move(boot_params)))
  {
    ERROR_LOG(BOOT, "Could not boot %s", game->path);
    UndoFailedLoad(true);
    return false;
  }

  Libretro::Input::InitStage2();

  const bool importCheats = Libretro::GetOption<bool>(retroarch_core::CHEATS_IMPORT, true);

  if (importCheats)
  {
    Libretro::reload_cheats_from_ini();
    Libretro::generate_cht_from_ini(filename_str);
  }

  /* The software and null renderers draw without a hardware context: no
   * context_reset comes from the frontend, so they come up here, where
   * one would follow. */
  if (Libretro::Video::hw_render.context_type == RETRO_HW_CONTEXT_NONE)
    Libretro::Video::ContextReset();

  return true;
}

bool retro_load_game_special(unsigned game_type, const struct retro_game_info* info, size_t num_info)
{
  if (game_type != Libretro::g_gbplayer_subsystem_id)
  {
    ERROR_LOG(BOOT, "retro_load_game_special: unknown game_type %u", game_type);
    return false;
  }
  if (num_info < 1 || !info)
  {
    ERROR_LOG(BOOT, "retro_load_game_special: no content info");
    return false;
  }

  const retro_game_info* gba_slot = (num_info >= 2 && info[0].path && *info[0].path) ? &info[0] : nullptr;

  const retro_game_info* gc_slot = (info[1].path && *info[1].path) ? &info[1] : nullptr;

  if (!gc_slot)
  {
    ERROR_LOG(BOOT, "GBPlayer: no GC disc path provided in slot 0");
    return false;
  }

  INFO_LOG(BOOT, "GBPlayer: GC disc  = '%s'", gc_slot->path);
  if (gba_slot)
  {
    Libretro::GBPlayer_rom_path = gba_slot->path;
    INFO_LOG(BOOT, "GBPlayer: ROM = '%s'", gba_slot->path);
  }
  else
  {
    Libretro::GBPlayer_rom_path.clear();
    INFO_LOG(BOOT, "GBPlayer: no ROM");
  }

  Libretro::GBPlayer_active = true;

  return retro_load_game(gc_slot);
}

void retro_unload_game(void)
{
  auto& system = Core::System::GetInstance();

  /* the video backend's teardown uses the context */
  Libretro::Video::D3D11Lock();
  if (Core::IsRunning(system))
  {
    Libretro::FinishFieldInFlight();
    Libretro::Video::GpuThreadStop();
    Core::Stop(system);
#if defined(__LIBUSB__)
    system.ShutdownUSBScanner();
#endif

    Core::Shutdown(system);
  }

  if (!g_context_status.IsDestroyed() && g_video_backend)
    g_video_backend->Shutdown();
  Libretro::Video::D3D11Unlock();
  /* the vertex loaders outlive a context, not the game */
  VertexLoaderManager::Clear();

  // these are disabled in Shutdown on fullscreen/window toggle
  system.GetFifo().Shutdown();

  // Rest of shutdown
  g_context_status.MarkUnitialized();
  Libretro::Input::Shutdown();
  Libretro::VR::Shutdown();
  Libretro::Log::Shutdown();
  UICommon::ShutdownControllers();
  UICommon::Shutdown();

  if (!system.IsDualCoreMode())
  {
    Core::SingleCorePostRunShutdown();

    // otherwise it is still called CPU-GPU Thread
    Common::SetCurrentThreadName("retroarch");
  }

  Core::UndeclareAsCPUThread();
  Core::UndeclareAsGPUThread();
  Libretro::ResetDiskControlState();
}

namespace Libretro
{
// Disk swapping
static bool retro_set_eject_state(bool ejected)
{
  if (eject_state == ejected)
    return false;

  eject_state = ejected;

  Libretro::FinishFieldInFlight();
  Core::RunOnCPUThread(Core::System::GetInstance(), [ejected] {
    auto& system = Core::System::GetInstance();
    Core::CPUThreadGuard guard{system};

    if (ejected)
      system.GetDVDInterface().EjectDisc(guard, DVD::EjectCause::User);
    else if (disk_index < disk_paths.size())
    {
      const std::string& path = disk_paths[disk_index];
      system.GetDVDInterface().ChangeDisc(guard, path);
    }
  }, true); // wait_for_completion = true
  Libretro::ResumeAfterField();

  return true;
}

static bool retro_get_eject_state()
{
  return eject_state;
}

static unsigned retro_get_image_index()
{
  return disk_index;
}

static bool retro_set_image_index(unsigned index)
{
  if (eject_state)
    disk_index = index;

  return eject_state;
}

static unsigned retro_get_num_images()
{
  return (unsigned)disk_paths.size();
}

static bool retro_add_image_index()
{
  disk_paths.push_back("");

  return true;
}

static bool retro_replace_image_index(unsigned index, const struct retro_game_info* info)
{
  if (index >= disk_paths.size())
    return false;

  if (!info->path)
  {
    disk_paths.erase(disk_paths.begin() + index);
    if (!disk_paths.size())
      disk_index = -1;
    else if (disk_index > index)
      disk_index--;
  }
  else
    disk_paths[index] = info->path;

  return true;
}

static bool RETRO_CALLCONV retro_set_initial_image(unsigned index, const char* path)
{
  if (index >= disk_paths.size())
    index = 0;

  disk_index = index;

  return true;
}

static bool RETRO_CALLCONV retro_get_image_path(unsigned index, char* path, size_t len)
{
  if (index >= disk_paths.size())
    return false;

  if (disk_paths[index].empty())
    return false;

  strncpy(path, disk_paths[index].c_str(), len);
  return true;
}
static bool RETRO_CALLCONV retro_get_image_label(unsigned index, char* label, size_t len)
{
  if (index >= disk_paths.size())
    return false;

  if (disk_paths[index].empty())
    return false;

  strncpy(label, disk_paths[index].c_str(), len);
  return true;
}

static void InitDiskControlInterface()
{
  static retro_disk_control_ext_callback disk_control = {
      retro_set_eject_state,
      retro_get_eject_state,
      retro_get_image_index,
      retro_set_image_index,
      retro_get_num_images,
      retro_replace_image_index,
      retro_add_image_index,
      retro_set_initial_image,
      retro_get_image_path,
      retro_get_image_label,
  };

  environ_cb(RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE, &disk_control);
}
}  // namespace Libretro
