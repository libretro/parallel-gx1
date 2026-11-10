// Copyright 2023 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/IOS/Crypto/Sha.h"

#include <cstring>
#include <optional>

#include <lrc_hash.h>

#include "Common/CommonTypes.h"
#include "Core/HW/MMIO.h"
#include "Core/HW/Memmap.h"
#include "Core/System.h"

namespace IOS::HLE
{
ShaDevice::ShaDevice(EmulationKernel& ios, const std::string& device_name)
    : EmulationDevice(ios, device_name)
{
}

std::optional<IPCReply> ShaDevice::Open(const OpenRequest& request)
{
  return Device::Open(request);
}

// The engine context holds the chaining state and the byte count, low word first;
// the stream counts bits. Only whole blocks pass between commands.
static void ConvertContext(const ShaDevice::ShaContext& src, sha1_state* dest)
{
  const u64 bytes = (static_cast<u64>(src.length[1]) << 32) | src.length[0];
  for (size_t i = 0; i < 5; i++)
    dest->digest[i] = src.states[i];
  dest->length_low = static_cast<u32>(bytes << 3);
  dest->length_high = static_cast<u32>(bytes >> 29);
  std::memset(dest->block, 0, sizeof(dest->block));
  dest->block_index = static_cast<int>(bytes & 63);
  dest->computed = 0;
  dest->corrupted = 0;
}

static void ConvertContext(const sha1_state& src, ShaDevice::ShaContext* dest)
{
  const u64 bytes = ((static_cast<u64>(src.length_high) << 32) | src.length_low) >> 3;
  for (size_t i = 0; i < 5; i++)
    dest->states[i] = src.digest[i];
  dest->length[0] = static_cast<u32>(bytes);
  dest->length[1] = static_cast<u32>(bytes >> 32);
}

HLE::ReturnCode ShaDevice::ProcessShaCommand(ShaIoctlv command, const IOCtlVRequest& request)
{
  auto& system = GetSystem();
  auto& memory = system.GetMemory();
  bool ok = true;
  u8 output_hash[20] = {};
  sha1_state context;
  ShaDevice::ShaContext engine_context;
  memory.CopyFromEmu(&engine_context, request.io_vectors[0].address, sizeof(ShaDevice::ShaContext));
  ConvertContext(engine_context, &context);

  // reset the context
  if (command == ShaIoctlv::InitState)
  {
    sha1_stream_init(&context);
  }
  else
  {
    const u32 size = request.in_vectors[0].size;
    const u8* const input = memory.GetPointerForRange(request.in_vectors[0].address, size);
    if (!input && size)
      ok = false;
    else
      sha1_stream_update(&context, input, size);
    if (ok && command == ShaIoctlv::FinalizeState)
      ok = sha1_stream_final(&context, output_hash);
  }

  ConvertContext(context, &engine_context);
  memory.CopyToEmu(request.io_vectors[0].address, &engine_context, sizeof(ShaDevice::ShaContext));
  if (ok && command == ShaIoctlv::FinalizeState)
    memory.CopyToEmu(request.io_vectors[1].address, output_hash, sizeof(output_hash));

  return ok ? HLE::ReturnCode::IPC_SUCCESS : HLE::ReturnCode::IPC_EACCES;
}

std::optional<IPCReply> ShaDevice::IOCtlV(const IOCtlVRequest& request)
{
  HLE::ReturnCode return_code = IPC_EINVAL;
  ShaIoctlv command = static_cast<ShaIoctlv>(request.request);

  switch (command)
  {
  case ShaIoctlv::InitState:
  case ShaIoctlv::ContributeState:
  case ShaIoctlv::FinalizeState:
    if (!request.HasNumberOfValidVectors(1, 2))
      break;

    return_code = ProcessShaCommand(command, request);
    break;

  case ShaIoctlv::ShaCommandUnknown:
    break;
  }

  return IPCReply(return_code);
}

}  // namespace IOS::HLE
