// Copyright 2023 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/IOS/Crypto/AesDevice.h"

#include <cstdlib>
#include <cstring>
#include <retro_miscellaneous.h>

#include <crypto/crypto.h>

#include "Common/CommonTypes.h"
#include "Core/HW/MMIO.h"
#include "Core/HW/Memmap.h"
#include "Core/System.h"

namespace IOS::HLE
{
AesDevice::AesDevice(EmulationKernel& ios, const std::string& device_name)
    : EmulationDevice(ios, device_name)
{
}

AesDevice::~AesDevice()
{
  free(m_buffer);
}

u8* AesDevice::Buffer(size_t size)
{
  if (size > m_capacity)
  {
    size_t capacity = m_capacity ? m_capacity : 64;
    while (capacity < size)
      capacity *= 2;
    u8* const grown = static_cast<u8*>(realloc(m_buffer, capacity));
    if (!grown)
      return nullptr;
    m_buffer = grown;
    m_capacity = capacity;
  }
  return m_buffer;
}

IPCReply AesDevice::Open(const OpenRequest& request)
{
  return Device::Open(request);
}

IPCReply AesDevice::IOCtlV(const IOCtlVRequest& request)
{
  auto& system = GetSystem();
  auto& memory = system.GetMemory();
  HLE::ReturnCode return_code = IPC_EINVAL;
  AesIoctlv command = static_cast<AesIoctlv>(request.request);

  switch (command)
  {
  case AesIoctlv::Copy:
  {
    if (!request.HasNumberOfValidVectors(1, 1))
      break;

    const u32 copy_size = request.in_vectors[0].size;
    u8* const input = Buffer(copy_size);
    if (!input)
      break;
    memory.CopyFromEmu(input, request.in_vectors[0].address, copy_size);
    memory.CopyToEmu(request.io_vectors[0].address, input, copy_size);
    return_code = ReturnCode::IPC_SUCCESS;
    break;
  }
  case AesIoctlv::Decrypt:
  case AesIoctlv::Encrypt:
  {
    if (!request.HasNumberOfValidVectors(2, 2))
      break;

    if (request.in_vectors[1].size != 0x10 || (request.in_vectors[1].address & 3) != 0 ||
        request.io_vectors[1].size != 0x10 || (request.io_vectors[1].address & 3) != 0)
    {
      break;
    }

    // The input is read whole before any output is written: the two may overlap
    const u32 input_size = request.in_vectors[0].size;
    const u32 output_size = request.io_vectors[0].size;
    u8* const input = Buffer(static_cast<size_t>(input_size) + output_size);
    if (!input)
      break;
    u8* const output = input + input_size;
    memset(output, 0, output_size);
    u8 key[0x10] = {0};
    u8 iv[0x10] = {0};
    memory.CopyFromEmu(input, request.in_vectors[0].address, input_size);
    memory.CopyFromEmu(key, request.in_vectors[1].address, ARRAY_SIZE(key));
    memory.CopyFromEmu(iv, request.io_vectors[1].address, ARRAY_SIZE(iv));

    // whole blocks only: a ragged size leaves output and iv as they were
    struct aes_ctx context;
    const size_t size = output_size < input_size ? output_size : input_size;
    aes_init(&context, key, ARRAY_SIZE(key));
    if (size % AES_BLOCK_SIZE == 0)
    {
      if (command == AesIoctlv::Encrypt)
        aes_cbc_encrypt(&context, iv, input, output, size);
      else
        aes_cbc_decrypt(&context, iv, input, output, size);
    }

    memory.CopyToEmu(request.io_vectors[0].address, output, output_size);
    memory.CopyToEmu(request.io_vectors[1].address, iv, ARRAY_SIZE(iv));
    return_code = ReturnCode::IPC_SUCCESS;
    break;
  }
  }

  return IPCReply(return_code);
}

}  // namespace IOS::HLE
