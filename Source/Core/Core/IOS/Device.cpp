// Copyright 2016 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/IOS/Device.h"

#include <algorithm>
#include <cinttypes>
#include <map>
#include <utility>

#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/SystemTimers.h"
#include "Core/IOS/IOS.h"
#include "Core/System.h"

namespace IOS::HLE
{
Request::Request(Core::System& system, const u32 address_) : address(address_)
{
  auto& memory = system.GetMemory();
  command = static_cast<IPCCommandType>(memory.Read_U32(address));
  fd = memory.Read_U32(address + 8);
}

OpenRequest::OpenRequest(Core::System& system, const u32 address_) : Request(system, address_)
{
  auto& memory = system.GetMemory();
  path = memory.GetString(memory.Read_U32(address + 0xc));
  flags = static_cast<OpenMode>(memory.Read_U32(address + 0x10));
  const EmulationKernel* ios = system.GetIOS();
  if (ios)
  {
    uid = ios->GetUidForPPC();
    gid = ios->GetGidForPPC();
  }
}

ReadWriteRequest::ReadWriteRequest(Core::System& system, const u32 address_)
    : Request(system, address_)
{
  auto& memory = system.GetMemory();
  buffer = memory.Read_U32(address + 0xc);
  size = memory.Read_U32(address + 0x10);
}

SeekRequest::SeekRequest(Core::System& system, const u32 address_) : Request(system, address_)
{
  auto& memory = system.GetMemory();
  offset = memory.Read_U32(address + 0xc);
  mode = static_cast<SeekMode>(memory.Read_U32(address + 0x10));
}

IOCtlRequest::IOCtlRequest(Core::System& system, const u32 address_) : Request(system, address_)
{
  auto& memory = system.GetMemory();
  request = memory.Read_U32(address + 0x0c);
  buffer_in = memory.Read_U32(address + 0x10);
  buffer_in_size = memory.Read_U32(address + 0x14);
  buffer_out = memory.Read_U32(address + 0x18);
  buffer_out_size = memory.Read_U32(address + 0x1c);
}

IOCtlVRequest::IOCtlVRequest(Core::System& system, const u32 address_) : Request(system, address_)
{
  auto& memory = system.GetMemory();
  request = memory.Read_U32(address + 0x0c);
  const u32 in_number = memory.Read_U32(address + 0x10);
  const u32 out_number = memory.Read_U32(address + 0x14);
  const u32 vectors_base = memory.Read_U32(address + 0x18);  // address to vectors

  /* past the most a request keeps, the rest are not read: a count from the emulated software, which
   * no IOS call takes that many of */
  const u32 in_kept = in_number < MAX_VECTORS ? in_number : MAX_VECTORS;
  const u32 out_kept = out_number < MAX_VECTORS ? out_number : MAX_VECTORS;
  u32 i;

  if (in_kept != in_number || out_kept != out_number)
  {
    ERROR_LOG(IOS, "IOCtlV %#x at %#010x: %u in and %u io vectors, more than the %u kept", request,
              address, in_number, out_number, MAX_VECTORS);
  }
  for (i = 0; i < in_kept; i++)
  {
    in_vectors.items[i].address = memory.Read_U32(vectors_base + i * 8);
    in_vectors.items[i].size = memory.Read_U32(vectors_base + i * 8 + 4);
  }
  in_vectors.count = in_kept;
  for (i = 0; i < out_kept; i++)
  {
    const u32 offset = (in_number + i) * 8;
    io_vectors.items[i].address = memory.Read_U32(vectors_base + offset);
    io_vectors.items[i].size = memory.Read_U32(vectors_base + offset + 4);
  }
  io_vectors.count = out_kept;
}

const IOCtlVRequest::IOVector& IOCtlVRequest::IOVectors::at(size_t i) const
{
  static const IOVector none;
  if (i < count)
    return items[i];
  ERROR_LOG(IOS, "IOCtlV vector %u asked for, of %u", static_cast<unsigned>(i), count);
  return none;
}

const IOCtlVRequest::IOVector* IOCtlVRequest::GetVector(size_t index) const
{
  if (index >= in_vectors.size() + io_vectors.size())
    return nullptr;
  if (index < in_vectors.size())
    return &in_vectors[index];
  return &io_vectors[index - in_vectors.size()];
}

bool IOCtlVRequest::HasNumberOfValidVectors(const size_t in_count, const size_t io_count) const
{
  if (in_vectors.size() != in_count || io_vectors.size() != io_count)
    return false;

  auto IsValidVector = [](const auto& vector) { return vector.size == 0 || vector.address != 0; };
  return std::ranges::all_of(in_vectors, IsValidVector) &&
         std::ranges::all_of(io_vectors, IsValidVector);
}

void IOCtlRequest::Log(std::string_view device_name, Common::Log::LogType type,
                       Common::Log::LogLevel verbosity) const
{
  GENERIC_LOG(type, verbosity, "%.*s (fd %u) - IOCtl %#x (in_size=%#x, out_size=%#x)",
              static_cast<int>(device_name.size()), device_name.data(), fd, request, buffer_in_size,
              buffer_out_size);
}

void IOCtlRequest::Dump(Core::System& system, const std::string& description,
                        Common::Log::LogType type, Common::Log::LogLevel level) const
{
  auto& memory = system.GetMemory();

  Log("===== " + description, type, level);
  GENERIC_LOG(
      type, level, "In buffer\n%s",
      HexDump(memory.GetPointerForRange(buffer_in, buffer_in_size), buffer_in_size).c_str());
  GENERIC_LOG(
      type, level, "Out buffer\n%s",
      HexDump(memory.GetPointerForRange(buffer_out, buffer_out_size), buffer_out_size).c_str());
}

void IOCtlRequest::DumpUnknown(Core::System& system, const std::string& description,
                               Common::Log::LogType type, Common::Log::LogLevel level) const
{
  Dump(system, "Unknown IOCtl - " + description, type, level);
}

void IOCtlVRequest::Dump(Core::System& system, std::string_view description,
                         Common::Log::LogType type, Common::Log::LogLevel level) const
{
  auto& memory = system.GetMemory();

  GENERIC_LOG(type, level, "===== %.*s (fd %u) - IOCtlV %#x (%" PRIu64 " in, %" PRIu64 " io)",
              static_cast<int>(description.size()), description.data(), fd, request,
              (u64)in_vectors.size(), (u64)io_vectors.size());

  size_t i = 0;
  for (const auto& vector : in_vectors)
  {
    GENERIC_LOG(
        type, level, "in[%" PRIu64 "] (size=%#x):\n%s", (u64)(i++), vector.size,
        HexDump(memory.GetPointerForRange(vector.address, vector.size), vector.size).c_str());
  }

  i = 0;
  for (const auto& vector : io_vectors)
    GENERIC_LOG(type, level, "io[%" PRIu64 "] (size=%#x)", (u64)(i++), vector.size);
}

void IOCtlVRequest::DumpUnknown(Core::System& system, const std::string& description,
                                Common::Log::LogType type, Common::Log::LogLevel level) const
{
  Dump(system, "Unknown IOCtlV - " + description, type, level);
}

Device::Device(Kernel& ios, std::string device_name, const DeviceType type)
    : m_ios(ios), m_name(std::move(device_name)), m_device_type(type)
{
}

void Device::DoState(PointerWrap& p)
{
  p.Do(m_name);
  p.Do(m_device_type);
  p.Do(m_is_active);
}

std::optional<IPCReply> Device::Open(const OpenRequest& request)
{
  m_is_active = true;
  return IPCReply{IPC_SUCCESS};
}

std::optional<IPCReply> Device::Close(u32 fd)
{
  m_is_active = false;
  return IPCReply{IPC_SUCCESS};
}

std::optional<IPCReply> Device::Unsupported(const Request& request)
{
  static const std::map<IPCCommandType, std::string_view> names{{
      {IPC_CMD_READ, "Read"},
      {IPC_CMD_WRITE, "Write"},
      {IPC_CMD_SEEK, "Seek"},
      {IPC_CMD_IOCTL, "IOCtl"},
      {IPC_CMD_IOCTLV, "IOCtlV"},
  }};

  WARN_LOG(IOS, "%s does not support %.*s()", m_name.c_str(),
           static_cast<int>(names.at(request.command).size()), names.at(request.command).data());
  return IPCReply{IPC_EINVAL};
}
}  // namespace IOS::HLE
