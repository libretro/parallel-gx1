#include <cctype>
#include <compat/strl.h>
#include <cstdio>
#include <file/file_path.h>
#include <functional>
#include <iterator>
#include <libretro.h>
#include <stdlib.h>
#include <string.h>
#include <string>

#include <retro_miscellaneous.h>
#include <streams/file_stream.h>
#include <string/stdstring.h>
#include <vfs/vfs_hybrid.h>
#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/NandPaths.h"
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
#include "Core/GeckoCode.h"
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
#include "DolphinLibretro/GBPlayerHLE.h"
#include "DolphinLibretro/Input.h"
#include "DolphinLibretro/Log.h"
#include "DolphinLibretro/VR.h"
#include "DolphinLibretro/Video.h"
#include "DolphinLibretro/VideoContexts/ContextStatus.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
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
static unsigned disk_index = 0;
static bool eject_state;
/* The disc the frontend asked to start on, before the content is loaded: its index in the
 * playlist and its path, a copy; null when it asked for none */
static unsigned initial_disk_index;
static char* initial_disk_path;
/* The discs' paths, each its own copy; an image added and not yet replaced is "" */
static char** disk_paths;
static unsigned disk_count;
static unsigned disk_cap;

static void ResetDiskControlState()
{
  unsigned i;
  for (i = 0; i < disk_count; i++)
    free(disk_paths[i]);
  free(disk_paths);
  disk_paths = nullptr;
  disk_count = 0;
  disk_cap = 0;
  disk_index = 0;
  eject_state = false;
}

/* @path copied in at the end of the list; false out of memory */
static bool DiskPathsAppend(const char* path)
{
  const size_t size = strlen(path) + 1;
  char* copy;
  if (disk_count == disk_cap)
  {
    const unsigned cap = disk_cap ? disk_cap * 2 : 4;
    char** const paths = static_cast<char**>(realloc(disk_paths, cap * sizeof(*paths)));
    if (!paths)
      return false;
    disk_paths = paths;
    disk_cap = cap;
  }
  copy = static_cast<char*>(malloc(size));
  if (!copy)
    return false;
  memcpy(copy, path, size);
  disk_paths[disk_count++] = copy;
  return true;
}

/* The disc to start on, now the playlist is known: the one the frontend asked for when its
 * index and path still match the list, else the first, as the disk control interface says.
 * The request is used up either way. */
static unsigned TakeInitialDisk()
{
  unsigned index = 0;
  if (initial_disk_path && initial_disk_index < disk_count &&
      !strcmp(disk_paths[initial_disk_index], initial_disk_path))
  {
    index = initial_disk_index;
  }
  free(initial_disk_path);
  initial_disk_path = nullptr;
  initial_disk_index = 0;
  return index;
}

// GBPlayer
static std::string GBPlayer_rom_path;
static bool GBPlayer_active;
// The ROM was loaded on its own, the startup disc found in the system directory.
static bool GBPlayer_direct;

// The next load is an ordinary one unless the subsystem asks again.
static void ResetGBPlayer()
{
  GBPlayer_active = false;
  GBPlayer_direct = false;
  GBPlayer_rom_path.clear();
}

// silence forward declaration warnings
void reload_cheats_from_ini();

void reload_cheats_from_ini()
{
  // Load Dolphin INI files
  const auto& sconfig = SConfig::GetInstance();
  Common::IniFile globalIni = sconfig.LoadDefaultGameIni();
  Common::IniFile localIni = sconfig.LoadLocalGameIni();

  // Load ActionReplay codes
  ActionReplay::LoadCodes(globalIni, localIni, &Libretro::g_ar_codes);

  // Load Gecko codes
  Gecko::LoadCodes(globalIni, localIni, &Libretro::g_gecko_codes);
}

// Writes the codes as a RetroArch .cht named after the game, beside the system directory
static void generate_cht_from_ini(const char* file_name, size_t file_name_len)
{
  const char* sysdir = nullptr;
  char cheatFile[PATH_MAX_LENGTH];
  size_t base_len;
  size_t i;
  int len;

  if (!Libretro::environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &sysdir) || !sysdir)
    return;

  // the system directory's parent, when it has one
  base_len = strlen(sysdir);
  for (i = base_len; i; i--)
  {
    if (sysdir[i - 1] == '/' || sysdir[i - 1] == '\\')
    {
      base_len = i - 1;
      break;
    }
  }

  len = snprintf(cheatFile, sizeof(cheatFile), "%.*s/cheats/dolphin-emu/%.*s.cht", (int)base_len,
                 sysdir, (int)file_name_len, file_name);
  if (len < 0 || (size_t)len >= sizeof(cheatFile))
    return;

  if (File::Exists(cheatFile))
  {
    INFO_LOG(BOOT, "Cheats: pre-existing cht file found: %s", cheatFile);
    return;
  }

  size_t total = Libretro::g_ar_codes.count + Libretro::g_gecko_codes.count;
  if (total == 0)
  {
    INFO_LOG(BOOT, "Cheats: not generating cht file as no ActionReply or Gecko codes found");
    return;
  }

  if (!File::CreateFullPath(cheatFile))
    return;

  RFILE* const out =
      filestream_open(cheatFile, RETRO_VFS_FILE_ACCESS_WRITE, RETRO_VFS_FILE_ACCESS_HINT_NONE);
  unsigned cheat_index = 0;
  bool written = out != nullptr;

  if (written)
  {
    filestream_printf(out, "cheats = %u\n\n", static_cast<unsigned>(total));

    for (u32 a = 0; a < Libretro::g_ar_codes.count; a++)
    {
      const ActionReplay::ARCode& c = Libretro::g_ar_codes.codes[a];
      filestream_printf(out, "cheat%u_desc = \"%s\"\ncheat%u_code = \"", cheat_index, c.name,
                        cheat_index);
      for (u32 o = 0; o < c.num_ops; o++)
        filestream_printf(out, "%s%08X %08X", o ? "+" : "", c.ops[o].cmd_addr, c.ops[o].value);
      filestream_printf(out, "\"\ncheat%u_enable = %s\n\n", cheat_index++,
                        c.enabled ? "true" : "false");
    }

    for (u32 g = 0; g < Libretro::g_gecko_codes.count; g++)
    {
      const Gecko::GeckoCode& c = Libretro::g_gecko_codes.codes[g];
      filestream_printf(out, "cheat%u_desc = \"%s\"\ncheat%u_code = \"", cheat_index, c.name,
                        cheat_index);
      for (u32 l = 0; l < c.num_lines; l++)
        filestream_printf(out, "%s%s", l ? "+" : "", c.lines[l].original_line);
      filestream_printf(out, "\"\ncheat%u_enable = %s\n\n", cheat_index++,
                        c.enabled ? "true" : "false");
    }

    written = !filestream_error(out);
    written = filestream_close(out) == 0 && written;
  }

  if (!written)
  {
    ERROR_LOG(BOOT, "Cheats: unable to create a new cht file at: %s", cheatFile);
    return;
  }

  INFO_LOG(BOOT, "Cheats: cht file created successfully at: %s", cheatFile);
}

// A region-free title runs in the fallback region, which follows the system locale and is PAL
// when there is none, as for Kodi on LibreELEC. A Japanese title can hang on a PAL console, so a
// region-free title runs in the region its game ID names.
static void SetRegionFreeFallback(const BootParameters& boot_params)
{
  const DiscIO::Volume* volume = nullptr;
  if (const auto* disc = std::get_if<BootParameters::Disc>(&boot_params.parameters))
    volume = disc->volume;
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

static bool PathIsSeparator(char c)
{
  return c == '/' || c == '\\';
}

/* The length of the path without its last component, either separator
 * taken and trailing ones ignored: 0 when it has no parent. */
static size_t PathParent(const char* path, size_t len)
{
  while (len && PathIsSeparator(path[len - 1]))
    len--;
  while (len && !PathIsSeparator(path[len - 1]))
    len--;
  return len ? len - 1 : 0;
}

/* The path's last component, trailing separators ignored: lent from the
 * path, last_len characters long. */
static const char* PathLast(const char* path, size_t len, size_t* last_len)
{
  size_t start;
  while (len && PathIsSeparator(path[len - 1]))
    len--;
  start = len;
  while (start && !PathIsSeparator(path[start - 1]))
    start--;
  *last_len = len - start;
  return path + start;
}

/* The GameCube save directory the frontend would have given with the GB
 * Player's content and core sorting undone, game_path the disc's or NULL
 * for none; the GBA's own when that would not fit. */
static const char* RebuildGBPlayerSaveDir(const char* gba_save_dir, const char* game_path)
{
  static char rebuilt[PATH_MAX_LENGTH];
  const char* rom = Libretro::GBPlayer_rom_path.c_str();
  const size_t rom_len = Libretro::GBPlayer_rom_path.size();
  const char* base = gba_save_dir ? gba_save_dir : "";
  size_t base_len = strlen(base);
  const char* gba_content = "";
  size_t gba_content_len = 0;
  const char* gc_content = "";
  size_t gc_content_len = 0;
  const char* last;
  size_t last_len;
  retro_system_info sys_info{};
  const char* library_name;
  size_t library_name_len;
  bool core_sort = false;
  bool content_sort = false;
  int len;

  if (rom_len)
    gba_content = PathLast(rom, PathParent(rom, rom_len), &gba_content_len);

  retro_get_system_info(&sys_info);
  library_name = sys_info.library_name;
  library_name_len = strlen(library_name);

  if (base_len && library_name_len)
  {
    last = PathLast(base, base_len, &last_len);
    core_sort = Common::CaseInsensitiveEquals(std::string_view(last, last_len),
                                              std::string_view(library_name, library_name_len));
  }
  if (core_sort)
    base_len = PathParent(base, base_len);

  if (gba_content_len && base_len)
  {
    last = PathLast(base, base_len, &last_len);
    content_sort = Common::CaseInsensitiveEquals(std::string_view(last, last_len),
                                                 std::string_view(gba_content, gba_content_len));
  }
  if (content_sort)
  {
    if (game_path)
      gc_content = PathLast(game_path, PathParent(game_path, strlen(game_path)), &gc_content_len);
    base_len = PathParent(base, base_len);
  }

  len = snprintf(rebuilt, sizeof(rebuilt), "%.*s%s%.*s%s%s", (int)base_len, base,
                 content_sort ? DIR_SEP : "", (int)gc_content_len, gc_content,
                 core_sort ? DIR_SEP : "", core_sort ? library_name : "");
  if (len < 0 || (size_t)len >= sizeof(rebuilt))
    return gba_save_dir ? gba_save_dir : "";
  return rebuilt;
}

/* A load that fails returns to a frontend that calls retro_deinit next,
 * never retro_unload_game: what the load brought up comes down here, the
 * Wii Remote scanner's threads above all, which would otherwise outlive
 * the core's controllers. */
static void UndoFailedLoad(bool input_up)
{
  if (input_up)
    Libretro::Input::Shutdown();
  UICommon::ShutdownControllers();
  UICommon::Shutdown();
  Libretro::ResetDiskControlState();
  Libretro::ResetGBPlayer();
}

/* A Game Boy, Game Boy Color or Game Boy Advance ROM, by its extension */
static bool IsGameBoyRom(const char* path)
{
  const char* ext = path ? path_get_extension(path) : "";
  return string_is_equal_noncase(ext, "gba") || string_is_equal_noncase(ext, "gbc") ||
         string_is_equal_noncase(ext, "gb");
}

/* The Game Boy Player's startup disc: GBPlayer.<a disc image's extension>
 * in the dolphin-emu folder of the system directory */
static bool FindGBPlayerDisc(char* out, size_t size)
{
  static const char* const exts[] = {"rvz", "iso", "gcm", "ciso", "gcz", "wia", "tgc"};
  const char* system_dir = NULL;
  char dir[PATH_MAX_LENGTH];
  char name[16];
  size_t i;

  if (!Libretro::environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &system_dir) || !system_dir ||
      !*system_dir)
    return false;
  fill_pathname_join_special(dir, system_dir, "dolphin-emu", sizeof(dir));
  for (i = 0; i < sizeof(exts) / sizeof(exts[0]); i++)
  {
    snprintf(name, sizeof(name), "GBPlayer.%s", exts[i]);
    fill_pathname_join_special(out, dir, name, size);
    if (path_is_valid(out))
      return true;
  }
  return false;
}

bool retro_load_game(const struct retro_game_info* game)
{
  static char gbp_disc[PATH_MAX_LENGTH];
  struct retro_game_info gbp_game;

  /* A Game Boy ROM loaded on its own plays in the Game Boy Player, booted
   * from its startup disc; without the disc, on the handheld alone */
  if (!Libretro::GBPlayer_active && game && IsGameBoyRom(game->path))
  {
    if (!FindGBPlayerDisc(gbp_disc, sizeof(gbp_disc)))
    {
      const char* system_dir = NULL;
      const char* save_dir = NULL;
      Libretro::environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &system_dir);
      Libretro::environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &save_dir);
      if (gbp_hle_load(game->path, system_dir, save_dir))
        return true;
      Libretro::Log::DoLogFrontEnd(RETRO_LOG_ERROR, "Game Boy Player: the ROM cannot run", 5000);
      return false;
    }
    Libretro::GBPlayer_rom_path = game->path;
    Libretro::GBPlayer_active = true;
    Libretro::GBPlayer_direct = true;
    gbp_game = *game;
    gbp_game.path = gbp_disc;
    gbp_game.data = NULL;
    gbp_game.size = 0;
    game = &gbp_game;
  }

  Libretro::ResetDiskControlState();

  const char* save_dir = NULL;
  const char* gba_save_dir = NULL;
  const char* system_dir = NULL;
  const char* core_assets_dir = NULL;
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
    Libretro::environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &gba_save_dir);
    save_dir = RebuildGBPlayerSaveDir(gba_save_dir, Libretro::GBPlayer_direct ? NULL : game->path);
    INFO_LOG(BOOT, "GBPlayer: GC save dir reconstructed to: '%s'", save_dir);
  }
  else
  {
    Libretro::environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &save_dir);
  }

  Libretro::environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &system_dir);
  Libretro::environ_cb(RETRO_ENVIRONMENT_GET_CORE_ASSETS_DIRECTORY, &core_assets_dir);

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
    // the Game Boy Player sits on the expansion port under the console
    Config::SetBase(Config::MAIN_HSP_DEVICE, HSP::HSPDeviceType::GBPlayer);
  }
  else
  {
    Config::SetBase(Config::MAIN_GBA_ROM_PATHS[Config::GBPLAYER_GBA_INDEX], std::string{});
    // nor does it stay there for the next game
    if (Config::Get(Config::MAIN_HSP_DEVICE) == HSP::HSPDeviceType::GBPlayer)
      Config::SetBase(Config::MAIN_HSP_DEVICE, HSP::HSPDeviceType::None);
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

  // The game settings stand for the system files as a whole
  if (!File::IsDirectory((sys_dir + DIR_SEP "GameSettings").c_str()))
  {
#if defined(ANDROID) || defined(IPHONEOS)
    // for reduced area to display the message..
    static const char missing_core_files_msg[] = "IMPORTANT - Open Online Updater ->\n"
                                                 "Core System Files Downloader -> Dolphin.zip.";
#else
    static const char missing_core_files_msg[] =
        "Core system files missing! Open Online Updater ->\n"
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
  Gecko::SetCodeHandlerEnabled(Libretro::GetOption<bool>(core::GECKO_CODE_HANDLER, /*def=*/true));

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

  Config::SetBase(
      Config::MAIN_WIIMOTE_CONTINUOUS_SCANNING,
      Libretro::GetOption<bool>(sysconf::WIIMOTE_CONTINUOUS_SCANNING,
                                /*def=*/false) ||
          !strcmp(Libretro::GetOption<const char*>(wiimote::DEFAULT_DEVICE, "frontend"), "real"));
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
    /* a value that is not a number is no device, as the option's default is */
    const char* const value = Libretro::GetOption<const char*>(
        sysconf_gc::SP1_DEVICE, "255" /* ExpansionInterface::EXIDeviceType::None */);
    char* end = nullptr;
    long device_int = strtol(value, &end, 10);
    if (end == value || *end)
      device_int = 255;
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
             Libretro::Options::CPUCoreToString(Config::Get(Config::MAIN_CPU_CORE)));
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
  if (!strcmp(Libretro::GetOption<const char*>(
                  Libretro::Options::gfx_settings::GPU_TEXTURE_DECODING, "auto"),
              "auto"))
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

  path_list_t normalized_game_paths;
  path_list_init(&normalized_game_paths);
  path_list_add(&normalized_game_paths, game->path);

  /* The content's folder ends after its last '/' (or '\\' or ':' on
   * Windows, for a path like "C:x.iso"), its name runs from there to its last
   * '.' after it, and its extension is the rest. */
  const char* const game_path = game->path;
  const size_t game_path_len = strlen(game_path);
  size_t dir_end = game_path_len;
  size_t name_end = game_path_len;
  size_t i;

  while (dir_end && game_path[dir_end - 1] != '/'
#ifdef _WIN32
         && game_path[dir_end - 1] != '\\' && game_path[dir_end - 1] != ':'
#endif
  )
  {
    dir_end--;
  }
  for (i = game_path_len; i > dir_end; i--)
  {
    if (game_path[i - 1] == '.')
    {
      name_end = i - 1;
      break;
    }
  }

  const std::string_view extension(game_path + name_end, game_path_len - name_end);
  if (Common::CaseInsensitiveEquals(extension, ".m3u") ||
      Common::CaseInsensitiveEquals(extension, ".m3u8"))
  {
    const std::string folder_path(game_path, dir_end);
    ReadM3UFile(game->path, folder_path.c_str(), &normalized_game_paths);
    if (!normalized_game_paths.count)
    {
      ERROR_LOG(BOOT, "Could not boot %s. M3U contains no paths", game->path);
      path_list_free(&normalized_game_paths);
      UndoFailedLoad(false);
      return false;
    }
  }

  for (i = 0; i < normalized_game_paths.count; i++)
    Libretro::DiskPathsAppend(path_list_get(&normalized_game_paths, static_cast<unsigned>(i)));
  Libretro::disk_index = Libretro::TakeInitialDisk();
  if (Libretro::disk_index)
  {
    /* Boot starts on the first path and changes disc through the rest in turn: the list starts
     * at the disc asked for, in the playlist's order from there */
    const unsigned count = normalized_game_paths.count;
    path_list_t from_initial;
    path_list_init(&from_initial);
    for (i = 0; i < count; i++)
    {
      path_list_add(&from_initial,
                    path_list_get(&normalized_game_paths,
                                  static_cast<unsigned>((Libretro::disk_index + i) % count)));
    }
    path_list_move(&normalized_game_paths, &from_initial);
    NOTICE_LOG(BOOT, "Starting on disc %u of %u: %s", Libretro::disk_index + 1, count,
               path_list_get(&normalized_game_paths, 0));
  }

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
  // The boot takes its own copy of the paths, as it moves them out and changes their separators.
  path_list_t boot_paths;
  path_list_init(&boot_paths);
  path_list_copy(&boot_paths, &normalized_game_paths);
  BootParameters* boot_params = BootParameters::GenerateFromFile(&boot_paths);
  path_list_free(&boot_paths);

  if (disc_based_games_boot_to_wii_menu)
  {
    bool is_disc_title = false;
    if (normalized_game_paths.count)
    {
      DiscIO::VolumeDisc* volume = DiscIO::CreateDisc(path_list_get(&normalized_game_paths, 0));
      if (volume && (volume->GetVolumeType() == DiscIO::Platform::WiiDisc || volume->GetVolumeType() == DiscIO::Platform::GameCubeDisc))
        is_disc_title = true;
      else
        WARN_LOG(BOOT, "'Disc Based Games Boot to Wii System Menu' enabled but content is not a "
                       "disc based game, ignoring Wii Menu");
      DiscIO::DestroyVolume(volume);
    }

    char wii_menu_tmd_path[Common::NAND_PATH_SIZE];
    Common::GetTMDFileName(Titles::SYSTEM_MENU, wii_menu_tmd_path, sizeof(wii_menu_tmd_path));
    // The user path ends with a separator, which the NAND path starts with.
    const std::string wii_menu_tmd_file_name =
        File::GetUserPath(D_WIIROOT_IDX) + (wii_menu_tmd_path + 1);
    const bool wii_menu_installed = File::Exists(wii_menu_tmd_file_name);
    if (is_disc_title && !wii_menu_installed)
      WARN_LOG(BOOT,
               "'Disc Based Games Boot to Wii System Menu' enabled but Wii Menu not found at save "
               "location: %s, booting directly instead",
               wii_menu_tmd_file_name.c_str());

    if (is_disc_title && wii_menu_installed)
    {
      Config::SetBase(Config::MAIN_DEFAULT_ISO,
                      std::string(path_list_get(&normalized_game_paths, 0)));
      delete boot_params;
      boot_params = new BootParameters(BootParameters::NANDTitle{Titles::SYSTEM_MENU});
    }
  }

  path_list_free(&normalized_game_paths);

  if (boot_params)
    Libretro::SetRegionFreeFallback(*boot_params);

  if (!BootManager::BootCore(Core::System::GetInstance(), boot_params))
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
    Libretro::generate_cht_from_ini(game_path + dir_end, name_end - dir_end);
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

  if (gbp_hle_active())
  {
    gbp_hle_unload();
    return;
  }

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
  /* the game's cheat codes go with it */
  ActionReplay::FreeCodeList(&Libretro::g_ar_codes);
  Gecko::FreeCodeList(&Libretro::g_gecko_codes);
  Libretro::ResetGBPlayer();
  /* a power-off the frontend never heard of goes with the game */
  Libretro::TakeShutdownRequest();
}

namespace Libretro
{
// Disk swapping
/* CPU thread: the disc out, or the selected one in; the context is the bool asking for out */
static void SetEjectStateOnCPU(void* context)
{
  auto& system = Core::System::GetInstance();

  if (*static_cast<const bool*>(context))
    system.GetDVDInterface().EjectDisc(DVD::EjectCause::User);
  else if (disk_index < disk_count)
    system.GetDVDInterface().ChangeDisc(disk_paths[disk_index]);
}

static bool retro_set_eject_state(bool ejected)
{
  if (eject_state == ejected)
    return false;

  eject_state = ejected;

  Libretro::FinishFieldInFlight();
  Core::RunOnCPUThread(Core::System::GetInstance(), SetEjectStateOnCPU, &ejected);
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
  return disk_count;
}

static bool retro_add_image_index()
{
  return DiskPathsAppend("");
}

static bool retro_replace_image_index(unsigned index, const struct retro_game_info* info)
{
  if (index >= disk_count)
    return false;

  if (!info->path)
  {
    free(disk_paths[index]);
    memmove(disk_paths + index, disk_paths + index + 1,
            (disk_count - index - 1) * sizeof(*disk_paths));
    disk_count--;
    if (!disk_count)
      disk_index = -1;
    else if (disk_index > index)
      disk_index--;
  }
  else
  {
    const size_t size = strlen(info->path) + 1;
    char* const copy = static_cast<char*>(malloc(size));
    if (!copy)
      return false;
    memcpy(copy, info->path, size);
    free(disk_paths[index]);
    disk_paths[index] = copy;
  }

  return true;
}

/* Called before the content is loaded, when the playlist is not known yet: the request is kept
 * and checked against it once it is (TakeInitialDisk). */
static bool RETRO_CALLCONV retro_set_initial_image(unsigned index, const char* path)
{
  free(initial_disk_path);
  initial_disk_path = nullptr;
  if (path && *path)
  {
    const size_t size = strlen(path) + 1;
    initial_disk_path = static_cast<char*>(malloc(size));
    if (initial_disk_path)
      memcpy(initial_disk_path, path, size);
  }
  initial_disk_index = index;
  return true;
}

static bool RETRO_CALLCONV retro_get_image_path(unsigned index, char* path, size_t len)
{
  if (index >= disk_count || !disk_paths[index][0])
    return false;

  strlcpy(path, disk_paths[index], len);
  return true;
}
static bool RETRO_CALLCONV retro_get_image_label(unsigned index, char* label, size_t len)
{
  if (index >= disk_count || !disk_paths[index][0])
    return false;

  strlcpy(label, disk_paths[index], len);
  return true;
}

void InitDiskControlInterface()
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
