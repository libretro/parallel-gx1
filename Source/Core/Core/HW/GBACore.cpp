// Copyright 2021 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef HAS_LIBMGBA

#include "Core/HW/GBACore.h"

#include <cstdio>
#include <cstring>
#include <mgba-util/vfs.h>
#include <mgba/core/log.h>
#include <mgba/core/timing.h>
#include <mgba/internal/gb/gb.h>
#include <mgba/internal/gba/gba.h>

#include <file/archive_file.h>
#include <lists/string_list.h>

#include <lrc_hash.h>
#include "AudioCommon/FieldAudio.h"

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/ScopeGuard.h"
#include "Common/StringUtil.h"
#include "Core/HW/GBAVFile.h"

#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/HW/SystemTimers.h"
#include "Core/Host.h"
#include "Core/System.h"
#include "VideoCommon/GBAScreens.h"

namespace HW::GBA
{
namespace
{
mLogger s_stub_logger = {
    [](mLogger*, int category, mLogLevel level, const char* format, va_list args) {}, nullptr};
}  // namespace

constexpr size_t AUDIO_BUFFER_SIZE = 512;

// libmGBA does not return the correct frequency for some GB models
static u32 GetCoreFrequency(mCore* core)
{
  return (core->platform(core) == mPLATFORM_GBA) ? GBA_ARM7TDMI_FREQUENCY : CGB_SM83_FREQUENCY;
}

static VFile* OpenROM_Archive(const char* path)
{
  VFile* vf{};
  VDir* archive = VDirOpenArchive(path);
  if (!archive)
    return nullptr;
  VFile* vf_archive =
      VDirFindFirst(archive, [](VFile* vf_) { return mCoreIsCompatible(vf_) != mPLATFORM_NONE; });
  if (vf_archive)
  {
    // read straight into the memory file the core will map
    const size_t size = static_cast<size_t>(vf_archive->size(vf_archive));
    vf = VFileMemChunk(nullptr, size);
    if (vf)
    {
      void* dst = vf->map(vf, size, MAP_WRITE);
      vf_archive->seek(vf_archive, 0, SEEK_SET);
      if (!dst || vf_archive->read(vf_archive, dst, size) != static_cast<ssize_t>(size))
      {
        vf->close(vf);
        vf = nullptr;
      }
    }
    vf_archive->close(vf_archive);
  }
  archive->close(archive);
  return vf;
}

/* The first member of a zip that is a ROM mGBA takes, each decoded straight into the memory file
 * the core will map */
static VFile* OpenROM_Zip(const char* path)
{
  VFile* vf = nullptr;
  struct string_list* entries = file_archive_get_file_list(path, nullptr);
  char member[4096];
  size_t i;

  for (i = 0; entries && i < entries->size && !vf; i++)
  {
    file_archive_entry_source_t* src;
    int64_t usize = 0, got = 0, n;
    u8* dst;

    if (snprintf(member, sizeof(member), "%s#%s", path, entries->elems[i].data) >=
        static_cast<int>(sizeof(member)))
      continue;
    if (!(src = file_archive_entry_source_open(member, &usize)))
      continue;
    if (usize > 0 && (vf = VFileMemChunk(nullptr, static_cast<size_t>(usize))) &&
        (dst = static_cast<u8*>(vf->map(vf, static_cast<size_t>(usize), MAP_WRITE))))
    {
      while (got < usize && (n = file_archive_entry_source_read(src, dst + got, usize - got)) > 0)
        got += n;
    }
    file_archive_entry_source_close(src);
    if (vf && (got != usize || mCoreIsCompatible(vf) == mPLATFORM_NONE))
    {
      vf->close(vf);
      vf = nullptr;
    }
  }
  if (vf)
    vf->seek(vf, 0, SEEK_SET);
  if (entries)
    string_list_free(entries);
  return vf;
}

static VFile* OpenReadOnlyFile(const char* path)
{
  return gba_vfile_open(path, 0);
}

static VFile* OpenROM(const char* rom_path)
{
  VFile* vf{};

  vf = OpenROM_Archive(rom_path);
  if (!vf)
    vf = OpenROM_Zip(rom_path);
  if (!vf)
    vf = OpenReadOnlyFile(rom_path);
  if (!vf)
    return nullptr;

  if (mCoreIsCompatible(vf) == mPLATFORM_NONE)
  {
    vf->close(vf);
    return nullptr;
  }
  vf->seek(vf, 0, SEEK_SET);

  return vf;
}

static std::array<u8, 20> GetROMHash(VFile* rom)
{
  size_t size = rom->size(rom);
  u8* buffer = static_cast<u8*>(rom->map(rom, size, MAP_READ));

  std::array<u8, 20> digest;
  SHA1Digest(buffer, size, digest.data());
  rom->unmap(rom, buffer, size);

  return digest;
}

Core::Core(::Core::System& system, int device_number)
    : m_device_number(device_number), m_system(system)
{
  mLogSetDefaultLogger(&s_stub_logger);

  MutexInit(&m_core_sync.videoFrameMutex);
  ConditionInit(&m_core_sync.videoFrameAvailableCond);
  ConditionInit(&m_core_sync.videoFrameRequiredCond);
  ConditionInit(&m_core_sync.audioRequiredCond);
  MutexInit(&m_core_sync.audioBufferMutex);

  retro_eventcount_init(&m_command_done);
}

Core::~Core()
{
  Stop();

  MutexDeinit(&m_core_sync.videoFrameMutex);
  ConditionDeinit(&m_core_sync.videoFrameAvailableCond);
  ConditionDeinit(&m_core_sync.videoFrameRequiredCond);
  ConditionDeinit(&m_core_sync.audioRequiredCond);
  MutexDeinit(&m_core_sync.audioBufferMutex);

  retro_eventcount_free(&m_command_done);
}

bool Core::Start(u64 gc_ticks)
{
  if (IsStarted())
    return false;

  Common::ScopeGuard start_guard{[&] { Stop(); }};

  VFile* rom{};
  Common::ScopeGuard rom_guard{[&] {
    if (rom)
      rom->close(rom);
  }};

  m_rom_path = Config::Get(Config::MAIN_GBA_ROM_PATHS[m_device_number]);
  if (!m_rom_path.empty())
  {
    rom = OpenROM(m_rom_path.c_str());
    if (!rom)
    {
      PanicAlert("Error: GBA%d failed to open the ROM in %s", m_device_number + 1,
                 m_rom_path.c_str());
      return false;
    }
    m_rom_hash = GetROMHash(rom);
  }

  m_core = rom ? mCoreFindVF(rom) : mCoreCreate(mPLATFORM_GBA);
  if (!m_core)
  {
    PanicAlert("Error: GBA%d failed to create core", m_device_number + 1);
    return false;
  }
  m_core->init(m_core);

  mCoreInitConfig(m_core, "dolphin");
  mCoreConfigSetValue(&m_core->config, "idleOptimization", "detect");
  mCoreConfigSetIntValue(&m_core->config, "useBios", 0);
  mCoreConfigSetIntValue(&m_core->config, "skipBios", 0);

  // If we eventually load GBC BIOS, then these should potentially all be "CGB".
  mCoreConfigSetValue(&m_core->config, "gb.model", "DMG");
  mCoreConfigSetValue(&m_core->config, "sgb.model", "DMG");
  mCoreConfigSetValue(&m_core->config, "cgb.model", "CGB");
  mCoreConfigSetValue(&m_core->config, "cgb.hybridModel", "CGB");
  mCoreConfigSetValue(&m_core->config, "cgb.sgbModel", "CGB");

  mCoreConfigSetIntValue(&m_core->config, "gb.colors", GB_COLORS_CGB);

  if (m_core->platform(m_core) == mPLATFORM_GBA)
  {
    LoadBIOS(File::GetUserPath(F_GBABIOS_IDX).c_str());
  }

  if (rom)
  {
    if (!m_core->loadROM(m_core, rom))
    {
      PanicAlert("Error: GBA%d failed to load the ROM in %s", m_device_number + 1,
                 m_rom_path.c_str());
      return false;
    }
    rom_guard.Dismiss();

    mGameInfo info;
    m_core->getGameInfo(m_core, &info);
    m_game_title = info.title;

    m_save_path = GetSavePath(m_rom_path, m_device_number);
    if (!m_save_path.empty() && !LoadSave(m_save_path.c_str()))
      return false;
  }

  m_last_gc_ticks = gc_ticks;
  m_gc_ticks_remainder = 0;
  m_keys = 0;

  if (m_device_number != Config::GBPLAYER_GBA_INDEX)
  {
    SetSIODriver();
    SetAVStream();
  }

  SetVideoBuffer();
  SetAudioBufferSize();
  AddCallbacks();
  SetupEvent();

  m_core->setSync(m_core, &m_core_sync);

  m_core->reset(m_core);
  m_started = true;
  start_guard.Dismiss();
  // Notify the host and handle a dimension change if that happened after reset()
  SetVideoBuffer();

  m_event_thread.Reset(StringFromFormat("GBA%d", m_device_number + 1),
                       std::bind_front(&Core::HandleEvent, this));
  if (m_device_number < GBA_SCREENS)
    GBAScreenStart((unsigned)m_device_number, DetachScreen, this);
  return true;
}

void Core::Stop()
{
  m_event_thread.Shutdown();
  /* the GBA's thread is gone: nothing renders into a lent buffer now */
  if (m_device_number < GBA_SCREENS)
    GBAScreenStop((unsigned)m_device_number);
  m_screen = false;

  if (m_core)
  {
    mCoreConfigDeinit(&m_core->config);
    m_core->deinit(m_core);
    m_core = nullptr;
  }
  m_started = false;
  m_rom_path = {};
  m_save_path = {};
  m_rom_hash = {};
  m_game_title = {};
}

void Core::Reset()
{
  Flush();
  if (!IsStarted())
    return;

  m_core->reset(m_core);
}

bool Core::IsStarted() const
{
  return m_started;
}

bool Core::LoadBIOS(const char* bios_path)
{
  VFile* vf = OpenReadOnlyFile(bios_path);
  if (!vf)
  {
    ERROR_LOG(CORE, "GBA%d failed to open the BIOS in %s", m_device_number + 1, bios_path);
    return false;
  }

  if (!m_core->loadBIOS(m_core, vf, 0))
  {
    ERROR_LOG(CORE, "GBA%d failed to load the BIOS in %s", m_device_number + 1, bios_path);
    vf->close(vf);
    return false;
  }

  return true;
}

bool Core::LoadSave(const char* save_path)
{
  VFile* vf = gba_vfile_open(save_path, 1);
  if (!vf)
  {
    PanicAlert("Error: GBA%d failed to open the save in %s", m_device_number + 1, save_path);
    return false;
  }

  if (!m_core->loadSave(m_core, vf))
  {
    PanicAlert("Error: GBA%d failed to load the save in %s", m_device_number + 1, save_path);
    vf->close(vf);
    return false;
  }

  return true;
}

void Core::SetSIODriver()
{
  if (m_core->platform(m_core) != mPLATFORM_GBA)
    return;

  m_sio_driver.core = this;
  m_sio_driver.init = [](GBASIODriver*) { return true; };
  m_sio_driver.deinit = [](GBASIODriver* driver) {
    static_cast<SIODriver*>(driver)->core->m_link_enabled = false;
  };
  m_sio_driver.reset = [](GBASIODriver* driver) {
    static_cast<SIODriver*>(driver)->core->m_link_enabled = false;
  };
  m_sio_driver.setMode = [](GBASIODriver* driver, GBASIOMode mode) {
    static_cast<SIODriver*>(driver)->core->m_link_enabled = mode == GBA_SIO_JOYBUS;
  };
  m_sio_driver.handlesMode = [](GBASIODriver*, GBASIOMode mode) { return mode == GBA_SIO_JOYBUS; };
  m_sio_driver.connectedDevices = [](GBASIODriver*) { return 1; };

  GBASIOSetDriver(&static_cast<::GBA*>(m_core->board)->sio, &m_sio_driver);
}

void Core::SetVideoBuffer()
{
  if (m_device_number == Config::GBPLAYER_GBA_INDEX)
  {
    // GBPlayer expects a GBA-sized video buffer even in GB mode.
    // Clear it first to avoid stuck colors from the previous game on switch from GBA->GB.
    m_video_buffer.clear();
    m_video_buffer.resize(std::size_t{GBA_VIDEO_HORIZONTAL_PIXELS} * GBA_VIDEO_VERTICAL_PIXELS);

    constexpr size_t GB_VIDEO_OFFSET = 1960;

    m_core->setVideoBuffer(
        m_core, m_video_buffer.data() + ((GetPlatform() == mPLATFORM_GBA) ? 0 : GB_VIDEO_OFFSET),
        GBA_VIDEO_HORIZONTAL_PIXELS);
  }
  else
  {
    u32 width;
    u32 height;
    size_t stride;
    u32* const lent = m_screen ? GBAScreenBuffer((unsigned)m_device_number, &stride) : nullptr;
    m_core->currentVideoSize(m_core, &width, &height);
    m_video_buffer.resize(std::size_t{width} * height);
    if (lent)
    {
      m_core->setVideoBuffer(m_core, lent, stride);
    }
    else
    {
      m_screen = false;
      m_core->setVideoBuffer(m_core, m_video_buffer.data(), width);
    }
  }
}

/* GBA thread, as a frame ends: the next one goes into the buffer the
 * screen lends, or the GBA's own */
void Core::ScreenFrameEnded(void* context)
{
  Core* const core = static_cast<Core*>(context);
  u32 width;
  u32 height;
  size_t stride;
  u32* next;

  core->m_core->currentVideoSize(core->m_core, &width, &height);
  next = GBAScreenFrameEnd((unsigned)core->m_device_number, width, height, &stride);
  if (next)
  {
    core->m_core->setVideoBuffer(core->m_core, next, stride);
    core->m_screen = true;
  }
  else if (core->m_screen)
  {
    core->m_screen = false;
    core->m_core->setVideoBuffer(core->m_core, core->m_video_buffer.data(), width);
  }
}

/* Any thread: the GBA's thread lets go of its screen's buffer next. */
void Core::DetachScreen(void* context)
{
  Core* const core = static_cast<Core*>(context);
  SyncEvent event{};
  event.event_type = SyncEventType::DetachScreen;
  core->PushEvent(event);
}

void Core::SetAudioBufferSize()
{
  m_core->setAudioBufferSize(m_core, AUDIO_BUFFER_SIZE);
}

void Core::AddCallbacks()
{
  mCoreCallbacks callbacks{};
  callbacks.context = this;
  callbacks.keysRead = [](void* context) {
    auto core = static_cast<Core*>(context);
    core->m_core->setKeys(core->m_core, core->m_keys);
  };
  if (m_device_number < GBA_SCREENS)
    callbacks.videoFrameEnded = ScreenFrameEnded;
  m_core->addCoreCallbacks(m_core, &callbacks);
}

/* GBA thread: mGBA's samples are read straight into this GBA's ring, which the CPU thread
 * mixes into the console's output as the field ends. */
static void QueueAudioBuffer(mAVStream* stream, mAudioBuffer* audio_buffer)
{
  Core* const core = static_cast<AVStream*>(stream)->core;
  const unsigned gba = (unsigned)core->GetDeviceNumber();
  for (;;)
  {
    int16_t* frames;
    size_t room = field_audio_gba_write_begin(gba, &frames);
    size_t read;
    if (room == 0)
    {
      /* The console has not taken the last ones: these are let go. */
      mAudioBufferClear(audio_buffer);
      return;
    }
    read = mAudioBufferRead(audio_buffer, frames, room);
    field_audio_gba_write_end(gba, read);
    if (read < room)
      return;
  }
}

static void AudioRateChanged(mAVStream* stream, unsigned rate)
{
  Core* const core = static_cast<AVStream*>(stream)->core;
  field_audio_gba_set_rate_hz((unsigned)core->GetDeviceNumber(), rate);
}

void Core::SetAVStream()
{
  m_stream = {
      .core = this,
  };

  m_stream.videoDimensionsChanged = [](mAVStream* stream, unsigned /*width*/, unsigned /*height*/) {
    auto* core = static_cast<AVStream*>(stream)->core;
    core->SetVideoBuffer();
  };
  m_stream.audioRateChanged = AudioRateChanged;
  m_stream.postAudioBuffer = QueueAudioBuffer;

  m_core->setAVStream(m_core, &m_stream);
  field_audio_gba_set_rate_hz((unsigned)m_device_number, GetAudioSampleRate());
}

void Core::SetupEvent()
{
  m_event.context = this;
  m_event.name = "Dolphin Sync";
  m_event.callback = [](mTiming* /*timing*/, void* context, u32 /*cycles_late*/) {
    auto* const core = static_cast<Core*>(context);
    core->m_waiting_for_event = false;
  };
  m_event.priority = 0x80;
}

void Core::SyncJoybus(u64 gc_ticks, u16 keys)
{
  PushEvent({
      .event_type = SyncEventType::TimeSync,
      .keys = keys,
      .run_until_ticks = gc_ticks,
  });
}

void Core::SendJoybusCommand(u64 gc_ticks, int transfer_time, u8* buffer, u16 keys)
{
  if (!IsStarted())
    return;

  m_joybus_command_transfer_time = transfer_time;
  m_joybus_command = GBASIOJOYCommand(buffer[0]);
  memcpy(m_joybus_buffer.data(), buffer + 1, sizeof(m_joybus_buffer));

  retro_atomic_store_relaxed_int(&m_command_pending, 1);

  PushEvent({
      .event_type = SyncEventType::RunCommand,
      .keys = keys,
      .run_until_ticks = gc_ticks,
  });
}

int Core::GetJoybusResponse(u8* data_out)
{
  /* The GBA thread answers the command in emulated time; wait for its reply. */
  for (;;)
  {
    int key;

    if (retro_atomic_load_acquire_int(&m_command_pending) == 0)
      break;

    key = retro_eventcount_prepare_wait(&m_command_done);

    if (retro_atomic_load_acquire_int(&m_command_pending) == 0)
    {
      retro_eventcount_cancel_wait(&m_command_done);
      break;
    }

    retro_eventcount_commit_wait(&m_command_done, key);
  }

  /* at most the joybus buffer, whatever a savestate held */
  size_t size = m_response_size;
  if (size > sizeof(m_joybus_buffer))
    size = sizeof(m_joybus_buffer);
  memcpy(data_out, m_joybus_buffer.data(), size);
  return static_cast<int>(size);
}

void Core::Flush()
{
  m_event_thread.WaitForCompletion();
}

void Core::PushEvent(SyncEvent event)
{
  m_event_thread.Push(event);
}

void Core::HandleEvent(SyncEvent event)
{
  if (event.event_type == SyncEventType::DetachScreen)
  {
    if (m_screen)
    {
      u32 width;
      u32 height;
      m_screen = false;
      m_core->currentVideoSize(m_core, &width, &height);
      m_core->setVideoBuffer(m_core, m_video_buffer.data(), width);
    }
    GBAScreenDetached((unsigned)m_device_number);
    return;
  }

  m_keys = event.keys;

  RunUntil(event.run_until_ticks);

  if (event.event_type != SyncEventType::RunCommand)
    return;

  if (m_link_enabled)
  {
    m_response_size =
        u8(GBASIOJOYSendCommand(&m_sio_driver, m_joybus_command, m_joybus_buffer.data()));
  }
  else
  {
    m_response_size = 0;
  }

  retro_atomic_store_release_int(&m_command_pending, 0);
  retro_eventcount_notify(&m_command_done);

  RunFor(m_joybus_command_transfer_time);
}

void Core::RunUntil(u64 gc_ticks)
{
  if (static_cast<s64>(gc_ticks - m_last_gc_ticks) <= 0)
    return;

  const u64 gc_frequency = m_system.GetSystemTimers().GetTicksPerSecond();
  const u32 core_frequency = GetCoreFrequency(m_core);

  mTimingSchedule(m_core->timing, &m_event,
                  static_cast<s32>((gc_ticks - m_last_gc_ticks) * core_frequency / gc_frequency));
  m_waiting_for_event = true;

  s32 begin_time = mTimingCurrentTime(m_core->timing);
  while (m_waiting_for_event)
    m_core->runLoop(m_core);
  s32 end_time = mTimingCurrentTime(m_core->timing);

  u64 d = (static_cast<u64>(end_time - begin_time) * gc_frequency) + m_gc_ticks_remainder;
  m_last_gc_ticks += d / core_frequency;
  m_gc_ticks_remainder = d % core_frequency;
}

void Core::RunFor(u64 gc_ticks)
{
  RunUntil(m_last_gc_ticks + gc_ticks);
}

void Core::DoState(PointerWrap& p)
{
  Flush();

  bool is_started = IsStarted();
  p.Do(is_started);

  if (!is_started)
  {
    Stop();
    return;
  }

  bool has_rom = !m_rom_path.empty();
  p.Do(has_rom);
  auto old_hash = m_rom_hash;
  p.Do(m_rom_hash);
  auto old_title = m_game_title;
  p.Do(m_game_title);

  if (p.IsReadMode() && (has_rom != !m_rom_path.empty() ||
                         (has_rom && (old_hash != m_rom_hash || old_title != m_game_title))))
  {
    ::Core::DisplayMessage("Incompatible ROM state in GBA%d. Aborting load state.",
                           m_device_number + 1);
    p.SetVerifyMode();
    return;
  }

  Start(0);

  p.Do(m_video_buffer);
  p.Do(m_last_gc_ticks);
  p.Do(m_gc_ticks_remainder);
  p.Do(m_keys);
  p.Do(m_link_enabled);
  p.Do(m_response_size);
  p.Do(m_joybus_buffer);

  std::vector<u8> core_state;
  core_state.resize(m_core->stateSize(m_core));

  if (p.IsWriteMode() || p.IsVerifyMode())
  {
    m_core->saveState(m_core, core_state.data());
  }

  p.Do(core_state);

  if (p.IsReadMode() && m_core->stateSize(m_core) == core_state.size())
  {
    m_core->loadState(m_core, core_state.data());
  }
}

std::string Core::GetSavePath(std::string_view rom_path, int device_number)
{
  // This is done to make the save file compatible with the save from the mGBA core in RA as it
  // saves in *.srm format.
  std::string save_path(rom_path.substr(0, rom_path.find_last_of('.')));
  save_path += ".srm";

  if (!Config::Get(Config::MAIN_GBA_SAVES_IN_ROM_PATH))
  {
    save_path =
        File::GetUserPath(D_GBASAVES_IDX) + save_path.substr(save_path.find_last_of("\\/") + 1);
  }

  return save_path;
}
}  // namespace HW::GBA
#endif  // HAS_LIBMGBA
