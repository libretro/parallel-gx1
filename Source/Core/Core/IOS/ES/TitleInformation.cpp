// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/IOS/ES/ES.h"

#include <cstdio>
#include <vector>

#include "Common/Logging/Log.h"
#include "Core/HW/Memmap.h"
#include "Core/IOS/ES/Formats.h"
#include "Core/System.h"

namespace IOS::HLE
{
// Used by the GetStoredContents ioctlvs. This assumes that the first output vector
// is used for the content count (u32).
IPCReply ESDevice::GetStoredContentsCount(const ES::TMDReader& tmd, const IOCtlVRequest& request)
{
  if (request.io_vectors[0].size != sizeof(u32) || !tmd.IsValid())
    return IPCReply(ES_EINVAL);

  const u16 num_contents = static_cast<u16>(m_core.GetStoredContentsFromTMD(tmd).size());
  auto& system = GetSystem();
  auto& memory = system.GetMemory();
  memory.Write_U32(num_contents, request.io_vectors[0].address);

  INFO_LOG_FMT(IOS_ES, "GetStoredContentsCount ({:#x}):  {} content(s) for {:016x}",
               request.request, num_contents, tmd.GetTitleId());
  return IPCReply(IPC_SUCCESS);
}

// Used by the GetStoredContents ioctlvs. This assumes that the second input vector is used
// for the content count and the output vector is used to store a list of content IDs (u32s).
IPCReply ESDevice::GetStoredContents(const ES::TMDReader& tmd, const IOCtlVRequest& request)
{
  if (!tmd.IsValid())
    return IPCReply(ES_EINVAL);

  auto& system = GetSystem();
  auto& memory = system.GetMemory();
  if (request.in_vectors[1].size != sizeof(u32) ||
      request.io_vectors[0].size != memory.Read_U32(request.in_vectors[1].address) * sizeof(u32))
  {
    return IPCReply(ES_EINVAL);
  }

  const auto contents = m_core.GetStoredContentsFromTMD(tmd);
  const u32 max_content_count = memory.Read_U32(request.in_vectors[1].address);
  const u32 stored_count = static_cast<u32>(contents.size());
  const u32 count = stored_count < max_content_count ? stored_count : max_content_count;
  for (u32 i = 0; i < count; ++i)
    memory.Write_U32(contents[i].id, request.io_vectors[0].address + i * sizeof(u32));

  return IPCReply(IPC_SUCCESS);
}

IPCReply ESDevice::GetStoredContentsCount(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(1, 1) || request.in_vectors[0].size != sizeof(u64))
    return IPCReply(ES_EINVAL);

  auto& system = GetSystem();
  auto& memory = system.GetMemory();
  const u64 title_id = memory.Read_U64(request.in_vectors[0].address);
  const ES::TMDReader tmd = m_core.FindInstalledTMD(title_id);
  if (!tmd.IsValid())
    return IPCReply(FS_ENOENT);
  return GetStoredContentsCount(tmd, request);
}

IPCReply ESDevice::GetStoredContents(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(2, 1) || request.in_vectors[0].size != sizeof(u64))
    return IPCReply(ES_EINVAL);

  auto& system = GetSystem();
  auto& memory = system.GetMemory();
  const u64 title_id = memory.Read_U64(request.in_vectors[0].address);
  const ES::TMDReader tmd = m_core.FindInstalledTMD(title_id);
  if (!tmd.IsValid())
    return IPCReply(FS_ENOENT);
  return GetStoredContents(tmd, request);
}

IPCReply ESDevice::GetTMDStoredContentsCount(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(1, 1))
    return IPCReply(ES_EINVAL);

  // read into the reader straight from guest memory
  auto& system = GetSystem();
  auto& memory = system.GetMemory();
  ES::TMDReader tmd;
  u8* const bytes = tmd.ResizeBytes(request.in_vectors[0].size);
  if (bytes)
    memory.CopyFromEmu(bytes, request.in_vectors[0].address, request.in_vectors[0].size);
  return GetStoredContentsCount(tmd, request);
}

IPCReply ESDevice::GetTMDStoredContents(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(2, 1))
    return IPCReply(ES_EINVAL);

  // read into the reader straight from guest memory
  auto& system = GetSystem();
  auto& memory = system.GetMemory();
  ES::TMDReader tmd;
  u8* const tmd_bytes = tmd.ResizeBytes(request.in_vectors[0].size);
  if (tmd_bytes)
    memory.CopyFromEmu(tmd_bytes, request.in_vectors[0].address, request.in_vectors[0].size);
  if (!tmd.IsValid())
    return IPCReply(ES_EINVAL);

  u8* cert_store;
  size_t cert_store_size;
  ReturnCode ret = m_core.ReadCertStore(&cert_store, &cert_store_size);
  if (ret != IPC_SUCCESS)
    return IPCReply(ret);

  ret = m_core.VerifyContainer(ESCore::VerifyContainerType::TMD,
                               ESCore::VerifyMode::UpdateCertStore, tmd, cert_store,
                               cert_store_size);
  std::free(cert_store);
  if (ret != IPC_SUCCESS)
    return IPCReply(ret);

  return GetStoredContents(tmd, request);
}

IPCReply ESDevice::GetTitleCount(size_t title_count, const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(0, 1) || request.io_vectors[0].size != 4)
    return IPCReply(ES_EINVAL);

  auto& system = GetSystem();
  auto& memory = system.GetMemory();
  memory.Write_U32(static_cast<u32>(title_count), request.io_vectors[0].address);

  return IPCReply(IPC_SUCCESS);
}

IPCReply ESDevice::GetTitles(const u64* titles, size_t title_count, const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(1, 1))
    return IPCReply(ES_EINVAL);

  auto& system = GetSystem();
  auto& memory = system.GetMemory();
  const size_t max_count = memory.Read_U32(request.in_vectors[0].address);
  const size_t count = max_count < title_count ? max_count : title_count;
  for (size_t i = 0; i < count; i++)
  {
    memory.Write_U64(titles[i], request.io_vectors[0].address + static_cast<u32>(i) * sizeof(u64));
    INFO_LOG_FMT(IOS_ES, "     title {:016x}", titles[i]);
  }
  return IPCReply(IPC_SUCCESS);
}

IPCReply ESDevice::GetTitleCount(const IOCtlVRequest& request)
{
  const std::vector<u64> titles = m_core.GetInstalledTitles();
  INFO_LOG_FMT(IOS_ES, "GetTitleCount: {} titles", titles.size());
  return GetTitleCount(titles.size(), request);
}

IPCReply ESDevice::GetTitles(const IOCtlVRequest& request)
{
  const std::vector<u64> titles = m_core.GetInstalledTitles();
  return GetTitles(titles.data(), titles.size(), request);
}

IPCReply ESDevice::GetStoredTMDSize(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(1, 1))
    return IPCReply(ES_EINVAL);

  auto& system = GetSystem();
  auto& memory = system.GetMemory();
  const u64 title_id = memory.Read_U64(request.in_vectors[0].address);
  const ES::TMDReader tmd = m_core.FindInstalledTMD(title_id);
  if (!tmd.IsValid())
    return IPCReply(FS_ENOENT);

  size_t raw_tmd_size;
  tmd.GetBytes(&raw_tmd_size);
  const u32 tmd_size = static_cast<u32>(raw_tmd_size);
  memory.Write_U32(tmd_size, request.io_vectors[0].address);

  INFO_LOG_FMT(IOS_ES, "GetStoredTMDSize: {} bytes  for {:016x}", tmd_size, title_id);

  return IPCReply(IPC_SUCCESS);
}

IPCReply ESDevice::GetStoredTMD(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(2, 1))
    return IPCReply(ES_EINVAL);

  auto& system = GetSystem();
  auto& memory = system.GetMemory();
  const u64 title_id = memory.Read_U64(request.in_vectors[0].address);
  const ES::TMDReader tmd = m_core.FindInstalledTMD(title_id);
  if (!tmd.IsValid())
    return IPCReply(FS_ENOENT);

  // TODO: actually use this param in when writing to the outbuffer :/
  const u32 MaxCount = memory.Read_U32(request.in_vectors[1].address);

  size_t raw_tmd_size;
  const u8* const raw_tmd = tmd.GetBytes(&raw_tmd_size);
  if (raw_tmd_size != request.io_vectors[0].size)
    return IPCReply(ES_EINVAL);

  memory.CopyToEmu(request.io_vectors[0].address, raw_tmd, raw_tmd_size);

  INFO_LOG_FMT(IOS_ES, "GetStoredTMD: title {:016x} (buffer size: {})", title_id, MaxCount);
  return IPCReply(IPC_SUCCESS);
}

IPCReply ESDevice::GetOwnedTitleCount(const IOCtlVRequest& request)
{
  const std::vector<u64> titles = m_core.GetTitlesWithTickets();
  INFO_LOG_FMT(IOS_ES, "GetOwnedTitleCount: {} titles", titles.size());
  return GetTitleCount(titles.size(), request);
}

IPCReply ESDevice::GetOwnedTitles(const IOCtlVRequest& request)
{
  const std::vector<u64> titles = m_core.GetTitlesWithTickets();
  return GetTitles(titles.data(), titles.size(), request);
}

IPCReply ESDevice::GetBoot2Version(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(0, 1))
    return IPCReply(ES_EINVAL);

  INFO_LOG_FMT(IOS_ES, "IOCTL_ES_GETBOOT2VERSION");

  // as of 26/02/2012, this was latest bootmii version
  auto& system = GetSystem();
  auto& memory = system.GetMemory();
  memory.Write_U32(4, request.io_vectors[0].address);
  return IPCReply(IPC_SUCCESS);
}

IPCReply ESDevice::GetSharedContentsCount(const IOCtlVRequest& request) const
{
  if (!request.HasNumberOfValidVectors(0, 1) || request.io_vectors[0].size != sizeof(u32))
    return IPCReply(ES_EINVAL);

  const u32 count = m_core.GetSharedContentsCount();
  auto& system = GetSystem();
  auto& memory = system.GetMemory();
  memory.Write_U32(count, request.io_vectors[0].address);

  INFO_LOG_FMT(IOS_ES, "GetSharedContentsCount: {} contents", count);
  return IPCReply(IPC_SUCCESS);
}

IPCReply ESDevice::GetSharedContents(const IOCtlVRequest& request) const
{
  if (!request.HasNumberOfValidVectors(1, 1) || request.in_vectors[0].size != sizeof(u32))
    return IPCReply(ES_EINVAL);

  auto& system = GetSystem();
  auto& memory = system.GetMemory();
  const u32 max_count = memory.Read_U32(request.in_vectors[0].address);
  if (request.io_vectors[0].size != 20 * max_count)
    return IPCReply(ES_EINVAL);

  const std::vector<std::array<u8, 20>> hashes = m_core.GetSharedContents();
  const u32 hash_count = static_cast<u32>(hashes.size());
  const u32 count = hash_count < max_count ? hash_count : max_count;
  memory.CopyToEmu(request.io_vectors[0].address, hashes.data(), 20 * count);

  INFO_LOG_FMT(IOS_ES, "GetSharedContents: {} contents ({} requested)", count, max_count);
  return IPCReply(IPC_SUCCESS);
}
}  // namespace IOS::HLE
