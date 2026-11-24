// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <map>
#include <memory>
#include <string>
#include <string_view>

#include "Common/CommonTypes.h"
#include "Common/RetroDeque.h"
#include "Core/CoreTiming.h"
#include "Core/HW/SystemTimers.h"
#include "Core/IOS/IOSC.h"

class PointerWrap;

namespace Core
{
class System;
}
namespace Memory
{
class MemoryManager;
}

namespace IOS::HLE
{
namespace FS
{
class FileSystem;
}

class Device;
class ESCore;
class ESDevice;
class FSCore;
class FSDevice;
class WiiSockMan;

struct Request;
struct OpenRequest;

struct IPCReply
{
  /// Constructs a reply with an average reply time.
  /// Please avoid using this function if more accurate timings are known.
  explicit IPCReply(s32 return_value_);
  explicit IPCReply(s32 return_value_, u64 reply_delay_ticks_);

  // No reply yet: the device answers the request later through EnqueueIPCReply.
  static IPCReply None()
  {
    IPCReply reply(0, 0);
    reply.has_reply = false;
    return reply;
  }

  s32 return_value;
  u64 reply_delay_ticks;
  bool has_reply;
};

constexpr SystemTimers::TimeBaseTick IPC_OVERHEAD_TICKS = 2700_tbticks;

// Used to make it more convenient for functions to return timing information
// without having to explicitly keep track of ticks in callers.
class Ticks
{
public:
  Ticks(u64* ticks = nullptr) : m_ticks(ticks) {}

  void Add(u64 ticks)
  {
    if (m_ticks != nullptr)
      *m_ticks += ticks;
  }

private:
  u64* m_ticks = nullptr;
};

template <typename ResultProducer>
IPCReply MakeIPCReply(u64 ticks, const ResultProducer& fn)
{
  const s32 result_value = fn(Ticks{&ticks});
  return IPCReply{result_value, ticks};
}

template <typename ResultProducer>
IPCReply MakeIPCReply(const ResultProducer& fn)
{
  return MakeIPCReply(0, fn);
}

enum IPCCommandType : u32
{
  IPC_CMD_OPEN = 1,
  IPC_CMD_CLOSE = 2,
  IPC_CMD_READ = 3,
  IPC_CMD_WRITE = 4,
  IPC_CMD_SEEK = 5,
  IPC_CMD_IOCTL = 6,
  IPC_CMD_IOCTLV = 7,
  // This is used for replies to commands.
  IPC_REPLY = 8,
};

enum class MemorySetupType
{
  IOSReload,
  Full,
};

enum class HangPPC : bool
{
  No = false,
  Yes = true,
};

void RAMOverrideForIOSMemoryValues(Memory::MemoryManager& memory, MemorySetupType setup_type);

void WriteReturnValue(Memory::MemoryManager& memory, s32 value, u32 address);

// HLE for the IOS kernel: IPC, device management, syscalls, and Dolphin-specific, IOS-wide calls.
class Kernel
{
public:
  explicit Kernel(IOSC::ConsoleType console_type = IOSC::ConsoleType::Retail);
  // Not virtual: an emulation kernel is always owned, and destroyed, as one.
  ~Kernel();

  Kernel(const Kernel&) = delete;
  Kernel& operator=(const Kernel&) = delete;

  // These are *always* part of the IOS kernel and always available.
  // They are also the only available resource managers even before loading any module.
  FS::FileSystem* GetFS();
  FSCore& GetFSCore();
  ESCore& GetESCore();

  u32 GetVersion() const;

  IOSC& GetIOSC();

protected:
  explicit Kernel(u64 title_id);

  FSCore* m_fs_core = nullptr;  // owned
  ESCore* m_es_core = nullptr;  // owned

  bool m_is_responsible_for_nand_root = false;
  u64 m_title_id = 0;

  IOSC m_iosc;
  // Owned; outlives the devices, which are destroyed with the emulation kernel.
  FS::FileSystem* m_fs = nullptr;
  WiiSockMan* m_socket_manager = nullptr;  // owned
};

// HLE for an IOS tied to emulation: base kernel which may have additional modules loaded.
class EmulationKernel final : public Kernel
{
public:
  EmulationKernel(Core::System& system, u64 ios_title_id);
  ~EmulationKernel();

  // Get a resource manager by name.
  // This only works for devices which are part of the device map.
  // Not owning, like every device pointer the kernel hands out.
  Device* GetDeviceByName(const char* device_name);
  // Not owning: the device map or the descriptor table keeps the device alive.
  Device* GetDeviceByFileDescriptor(const u32 fd);

  ESDevice* GetESDevice();

  void DoState(PointerWrap& p);
  void UpdateDevices();
  void UpdateWantDeterminism(bool new_want_determinism);

  WiiSockMan* GetSocketManager();

  void HandleIPCEvent(u64 userdata);
  void UpdateIPC();

  void EnqueueIPCRequest(u32 address);
  void EnqueueIPCReply(const Request& request, s32 return_value, s64 cycles_in_future = 0,
                       CoreTiming::FromThread from = CoreTiming::FromThread::CPU);

  void SetUidForPPC(u32 uid);
  u32 GetUidForPPC() const;
  void SetGidForPPC(u16 gid);
  u16 GetGidForPPC() const;

  bool BootstrapPPC(const std::string& boot_content_path);
  bool BootIOS(u64 ios_title_id, HangPPC hang_ppc = HangPPC::No,
               const std::string& boot_content_path = {});
  void InitIPC();

  Core::System& GetSystem() const { return m_system; }

private:
  void ExecuteIPCCommand(u32 address);
  IPCReply HandleIPCCommand(const Request& request);

  // Takes ownership of a static device, replacing one of the same name.
  void AddDevice(Device* device);
  // Points a descriptor at device, destroying the OH0 device the descriptor owned, if any.
  void SetDescriptor(u32 fd, Device* device);

  void AddStaticDevices();
  s32 GetFreeDeviceID();
  IPCReply OpenDevice(OpenRequest& request);

  Core::System& m_system;

  static constexpr u8 IPC_MAX_FDS = 0x18;
  static constexpr u32 MAX_DEVICES = 64;

  struct DeviceEntry
  {
    Device* device;  // owned
    u32 created;     // devices are destroyed newest first, so a device outlives later ones
  };

  // The static devices, sorted by name: their state is saved in this order.
  DeviceEntry m_devices[MAX_DEVICES] = {};
  u32 m_device_count = 0;
  u32 m_devices_created = 0;
  // TODO: make this fdmap per process.
  // A static device, or an OH0 device the descriptor owns.
  Device* m_fdmap[IPC_MAX_FDS] = {};

  u32 m_ppc_uid = 0;
  u16 m_ppc_gid = 0;

  // made with the kernel: requests and replies queue up without allocating
  u32_deque_t m_request_queue;  // ppc -> arm
  u32_deque_t m_reply_queue;    // arm -> ppc
  u64 m_last_reply_time = 0;
  bool m_ipc_paused = false;
};

// Used for controlling and accessing an IOS instance that is tied to emulation.
void Init(Core::System& system);
void Shutdown(Core::System& system);

}  // namespace IOS::HLE
