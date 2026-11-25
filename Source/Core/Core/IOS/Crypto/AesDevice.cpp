// Copyright 2023 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/IOS/Crypto/AesDevice.h"

#include <algorithm>
#include <memory>
#include <retro_miscellaneous.h>
#include <vector>

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

    std::vector<u8> input = std::vector<u8>(request.in_vectors[0].size);
    memory.CopyFromEmu(input.data(), request.in_vectors[0].address, input.size());
    memory.CopyToEmu(request.io_vectors[0].address, input.data(), input.size());
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

    std::vector<u8> input = std::vector<u8>(request.in_vectors[0].size);
    std::vector<u8> output = std::vector<u8>(request.io_vectors[0].size);
    u8 key[0x10] = {0};
    u8 iv[0x10] = {0};
    memory.CopyFromEmu(input.data(), request.in_vectors[0].address, input.size());
    memory.CopyFromEmu(key, request.in_vectors[1].address, ARRAY_SIZE(key));
    memory.CopyFromEmu(iv, request.io_vectors[1].address, ARRAY_SIZE(iv));

    // whole blocks only: a ragged size leaves output and iv as they were
    struct aes_ctx context;
    const size_t size = std::min(output.size(), input.size());
    aes_init(&context, key, ARRAY_SIZE(key));
    if (size % AES_BLOCK_SIZE == 0)
    {
      if (command == AesIoctlv::Encrypt)
        aes_cbc_encrypt(&context, iv, input.data(), output.data(), size);
      else
        aes_cbc_decrypt(&context, iv, input.data(), output.data(), size);
    }

    memory.CopyToEmu(request.io_vectors[0].address, output.data(), output.size());
    memory.CopyToEmu(request.io_vectors[1].address, iv, ARRAY_SIZE(iv));
    return_code = ReturnCode::IPC_SUCCESS;
    break;
  }
  }

  return IPCReply(return_code);
}

}  // namespace IOS::HLE
