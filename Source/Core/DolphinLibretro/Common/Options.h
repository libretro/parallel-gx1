#pragma once

#include <cassert>
#include <string>
#include <vector>

#include <libretro.h>

#include "Common/Logging/Log.h"
#include "Core/PowerPC/PowerPC.h"
#include "DiscIO/Enums.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "VideoCommon/VideoConfig.h"

#define MODIFIER_DISABLED_CONTROL "Disabled"
#define MODIFIER_NO_MODIFIER "None"

/* The longest an option's value is kept: every value the core lists is far shorter */
enum
{
  CACHED_OPTION_RAW_SIZE = 64
};

struct CachedOption
{
  char raw[CACHED_OPTION_RAW_SIZE];
  long int_value = 0;
  double double_value = 0.0;
  bool bool_value = false;

  bool has_int = false;
  bool has_double = false;
  bool has_bool = false;
};

namespace Libretro
{
extern retro_environment_t environ_cb;

bool GetVariable(const char* key, const char*& out_value);
template <typename T>
T GetOption(const char* key, T def);

namespace Options
{
void Init();
void SetVariables();
void RegisterCache();
void CheckForUpdatedVariables();

template <typename T>
T GetCached(const char* key, T def = T{});
bool IsUpdated(const char* key);

inline const char* CPUCoreToString(PowerPC::CPUCore core)
{
  switch (core)
  {
    case PowerPC::CPUCore::Interpreter:
      return "Interpreter";
    case PowerPC::CPUCore::CachedInterpreter:
       return "Cached Interpreter";
    case PowerPC::CPUCore::JIT64:
      return "JIT64";
    case PowerPC::CPUCore::JITARM64:
      return "JIT ARM64";
#ifdef _M_ARM_32
    //case PowerPC::CPUCore::JITARM:
    //  return "JIT ARM32";
#endif
  }

  return "";
}

// ======================================================
// Core
// ======================================================
namespace core {
  inline constexpr char CPU_CORE[] = "dolphin_cpu_core";
  inline constexpr char CPU_CLOCK_RATE[] = "dolphin_cpu_clock_rate";
  inline constexpr char MAIN_CPU_THREAD[] = "dolphin_main_cpu_thread";
  inline constexpr char MAIN_SYNC_GPU[] = "dolphin_sync_gpu";
  inline constexpr char MAIN_LOAD_GAME_INTO_MEMORY[] = "dolphin_main_load_game_into_memory";
  inline constexpr char FASTMEM[] = "dolphin_fastmem";
  inline constexpr char FASTMEM_ARENA[] = "dolphin_fastmem_arena";
  inline constexpr char MAIN_ACCURATE_CPU_CACHE[] = "dolphin_main_accurate_cpu_cache";
  inline constexpr char CHEATS_ENABLED[] = "dolphin_cheats_enabled";
  inline constexpr char GECKO_CODE_HANDLER[] = "dolphin_gecko_code_handler";
  inline constexpr char SKIP_GC_BIOS[] = "dolphin_skip_gc_bios";
  inline constexpr char DISC_BASED_GAMES_BOOT_TO_WII_MENU[] = "dolphin_disc_based_games_boot_to_wii_menu";
  inline constexpr char LANGUAGE[] = "dolphin_language";
  inline constexpr char FAST_DISC_SPEED[] = "dolphin_fast_disc_speed";
  inline constexpr char MAIN_MMU[] = "dolphin_main_mmu";
}  // namespace core

// ======================================================
// Audio / DSP
// ======================================================
  namespace audio {
  inline constexpr char DSP_HLE[] = "dolphin_dsp_hle";
  inline constexpr char DSP_JIT[] = "dolphin_dsp_jit";
}  // namespace audio

// ======================================================
// Interface
// ======================================================
namespace main_interface {
  inline constexpr char LOG_LEVEL[] = "dolphin_log_level";
  inline constexpr char LOG_BOOT[] = "dolphin_log_boot_enabled";
  inline constexpr char LOG_CORE[] = "dolphin_log_core_enabled";
  inline constexpr char LOG_VIDEO[] = "dolphin_log_video_enabled";
  inline constexpr char LOG_COMMON[] = "dolphin_log_common_enabled";
  inline constexpr char LOG_HOST_GPU[] = "dolphin_log_host_gpu_enabled";
  inline constexpr char LOG_MEMMAP[] = "dolphin_log_memmap_enabled";
  inline constexpr char LOG_DSPINTERFACE[] = "dolphin_log_dspinterface_enabled";
  inline constexpr char LOG_DSPHLE[] = "dolphin_log_dsphle_enabled";
  inline constexpr char LOG_DSPLLE[] = "dolphin_log_dsplle_enabled";
  inline constexpr char LOG_DSP_MAIL[] = "dolphin_log_dsp_mail_enabled";
}  // namespace main_interface

// ======================================================
// Bluetooth
// ======================================================
namespace main_bluetooth {
  inline constexpr char BLUETOOTH_PASSTHROUGH[] = "dolphin_bluetooth_passthrough";
}  // namespace bluetooth

// ======================================================
// System Configuration (SYSCONF) - GC
// ======================================================
namespace sysconf_gc {
  inline constexpr char SP1_DEVICE[] = "dolphin_gc_sp1";
  inline constexpr char ENABLE_GAMECUBE_MIC[] = "dolphin_enable_gamecube_mic";
  inline constexpr char HOTKEY_ACTIVATE_MICROPHONE[] = "dolphin_hotkey_activate_microphone";
  inline constexpr char GBA_SCREENS[] = "dolphin_gba_screens";
}

// ======================================================
// System Configuration (SYSCONF) - Wii
// ======================================================
namespace sysconf {
  inline constexpr char WIDESCREEN[] = "dolphin_widescreen";
  inline constexpr char PROGRESSIVE_SCAN[] = "dolphin_progressive_scan";
  inline constexpr char PAL60[] = "dolphin_pal60";
  inline constexpr char SENSOR_BAR_POSITION[] = "dolphin_sensor_bar_position";
  inline constexpr char ENABLE_RUMBLE[] = "dolphin_enable_rumble";
  inline constexpr char WIIMOTE_CONTINUOUS_SCANNING[] = "dolphin_wiimote_continuous_scanning";
  inline constexpr char ALT_GC_PORTS_ON_WII[] = "dolphin_alt_gc_ports_on_wii";
  inline constexpr char WII_SPEAK_ENABLE[] = "dolphin_wiispeak_enable";
  inline constexpr char WII_SPEAK_MUTED[] = "dolphin_wiispeak_muted";
  inline constexpr char WII_LOGI_MICROPHONE_ENABLE[] = "dolphin_wii_logi_microphone_enable";
}  // namespace sysconf

// ======================================================
// Graphics > Settings
// ======================================================
namespace gfx_settings {
  inline constexpr char RENDERER[] = "dolphin_renderer";
  inline constexpr char WIDESCREEN_HACK[] = "dolphin_widescreen_hack";
  inline constexpr char ASPECT_RATIO[] = "dolphin_aspect_ratio";
  inline constexpr char CROP_OVERSCAN[] = "dolphin_crop_overscan";
  inline constexpr char EFB_SCALE[] = "dolphin_efb_scale";
  inline constexpr char SHADER_COMPILATION_MODE[] = "dolphin_shader_compilation_mode";
  inline constexpr char WAIT_FOR_SHADERS[] = "dolphin_wait_for_shaders";
  inline constexpr char SHADER_CACHE[] = "dolphin_shader_cache";
  inline constexpr char ANTI_ALIASING[] = "dolphin_anti_aliasing";
  inline constexpr char TEXTURE_CACHE_ACCURACY[] = "dolphin_texture_cache_accuracy";
  inline constexpr char GPU_TEXTURE_DECODING[] = "dolphin_gpu_texture_decoding";
  inline constexpr char ENABLE_PIXEL_LIGHTING[] = "dolphin_pixel_lighting";
  inline constexpr char FAST_DEPTH_CALCULATION[] = "dolphin_fast_depth_calculation";
  inline constexpr char MODS_ENABLE[] = "dolphin_mods_enabled";
  inline constexpr char VR_MODE[] = "dolphin_vr_mode";
}  // namespace gfx_settings

// ======================================================
// Graphics > Enhancements
// ======================================================
namespace gfx_enhancements {
  inline constexpr char MAX_ANISOTROPY[] = "dolphin_max_anisotropy";
  inline constexpr char FORCE_TEXTURE_FILTERING_MODE[] = "dolphin_force_texture_filtering_mode";
  inline constexpr char LOAD_CUSTOM_TEXTURES[] = "dolphin_load_custom_textures";
  inline constexpr char CACHE_CUSTOM_TEXTURES[] = "dolphin_cache_custom_textures";
  inline constexpr char FORCE_TRUE_COLOR[] = "dolphin_force_true_color";
  inline constexpr char GFX_ENHANCE_DISABLE_COPY_FILTER[] = "dolphin_disable_copy_filter";
  inline constexpr char GFX_ENHANCE_HDR_OUTPUT[] = "dolphin_enhance_hdr_output";
  inline constexpr char GFX_ARBITRARY_MIPMAP_DETECTION[] = "dolphin_mipmap_detection";
}  // namespace gfx_enhancements

// ======================================================
// Graphics > Hacks
// ======================================================
namespace gfx_hacks {
  inline constexpr char EFB_ACCESS_ENABLE[] = "dolphin_efb_access_enable";
  inline constexpr char EFB_ACCESS_DEFER_INVALIDATION[] = "dolphin_efb_access_defer_invalidation";
  inline constexpr char EFB_ACCESS_TILE_SIZE[] = "dolphin_efb_access_tile_size";
  inline constexpr char BBOX_ENABLED[] = "dolphin_bbox_enabled";
  inline constexpr char FORCE_PROGRESSIVE[] = "dolphin_force_progressive";
  inline constexpr char EFB_TO_TEXTURE[] = "dolphin_efb_to_texture";
  inline constexpr char XFB_TO_TEXTURE_ENABLE[] = "dolphin_xfb_to_texture_enable";
  inline constexpr char EFB_TO_VRAM[] = "dolphin_efb_to_vram";
  inline constexpr char DEFER_EFB_COPIES[] = "dolphin_defer_efb_copies";
  inline constexpr char IMMEDIATE_XFB[] = "dolphin_immediate_xfb";
  inline constexpr char SKIP_DUPE_FRAMES[] = "dolphin_skip_dupe_frames";
  inline constexpr char EARLY_XFB_OUTPUT[] = "dolphin_early_xfb_output";
  inline constexpr char EFB_SCALED_COPY[] = "dolphin_efb_scaled_copy";
  inline constexpr char EFB_EMULATE_FORMAT_CHANGES[] = "dolphin_efb_emulate_format_changes";
  inline constexpr char VERTEX_ROUNDING[] = "dolphin_vertex_rounding";
  inline constexpr char FAST_TEXTURE_SAMPLING[] = "dolphin_fast_texture_sampling";
#ifdef __APPLE__
  inline constexpr char NO_MIPMAPPING[] = "dolphin_no_mipmapping";
#endif
}  // namespace gfx_hacks

// ======================================================
// Graphics > Game Specific
// ======================================================
namespace gfx_gamespecific {
  inline constexpr char GFX_PERF_QUERIES_ENABLE[] = "dolphin_per_queries_enable";
}  // namespace gfx_gamespecific

// ======================================================
// Wiimote IR / Gyro / Swing
// ======================================================
namespace wiimote {
  // Controllers
  inline constexpr char DEFAULT_DEVICE[] = "dolphin_wiimote_default_device";

  // General and options
  inline constexpr char HOTKEY_SIDEWAYS_TOGGLE[] = "dolphin_hotkey_sideways_toggle";
  inline constexpr char HOTKEY_UPRIGHT_TOGGLE[] = "dolphin_hotkey_upright_toggle";

  // Motion simulation
  inline constexpr char IR_MODE[] = "dolphin_ir_mode";
  inline constexpr char IR_OFFSET[] = "dolphin_ir_offset";
  inline constexpr char IR_YAW[] = "dolphin_ir_yaw";
  inline constexpr char IR_PITCH[] = "dolphin_ir_pitch";
  inline constexpr char IR_DEADZONE[] = "dolphin_ir_deadzone";
  inline constexpr char IR_MODIFIER[] = "dolphin_ir_modifier";
  inline constexpr char SWING_MODIFIER[] = "dolphin_swing_modifier";
  inline constexpr char SWING_ANGLE[] = "dolphin_swing_angle";
  inline constexpr char IR_PASSTHROUGH[] = "dolphin_ir_passthrough";
}  // namespace wiimote

// ======================================================
// RetroArch unique settings
// ======================================================
namespace retroarch_core {
  inline constexpr char CHEATS_IMPORT[] = "dolphin_cheats_import";
  // preserve settings
  inline constexpr char SAVE_LOAD_SETTINGS[] = "dolphin_save_load_settings";

#if defined(HAS_OPENGL) && defined(__WEBOS__)
  inline constexpr char SUPPORTS_BINDING_LAYOUT[] = "dolphin_supports_binding_layout";
  inline constexpr char SUPPORTS_COPY_SUB_IMAGE[] = "dolphin_supports_copy_sub_image";
  inline constexpr char SUPPORTS_GL_BASE_VERTEX[] = "dolphin_supports_gl_base_vertex";
#endif

  inline constexpr char ENABLE_DEFAULT_MOUSE_BINDINGS[] = "dolphin_default_mouse_bindings_enabled";
  inline constexpr char ENABLE_LOG_POPUPS[] = "dolphin_log_popups_enabled";
}  // namespace retroarch_core

}  // namespace Options
}  // namespace Libretro
