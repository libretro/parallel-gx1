// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/IOS/ES/ES.h"

#include <cinttypes>
#include <cstddef>
#include <retro_miscellaneous.h>
#include <utility>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <lrc_hash.h>

#include "Common/Align.h"
#include "Common/Logging/Log.h"
#include "Common/NandPaths.h"
#include "Common/StringUtil.h"
#include "Core/CommonTitles.h"
#include "Core/HW/Memmap.h"
#include "Core/IOS/ES/Formats.h"
#include "Core/IOS/FS/FileSystem.h"
#include "Core/IOS/Uids.h"
#include "Core/System.h"

namespace IOS::HLE
{
static ReturnCode WriteTicket(FS::FileSystem* fs, const ES::TicketReader& ticket)
{
  const u64 title_id = ticket.GetTitleId();
  char path[Common::NAND_PATH_SIZE];
  if (ticket.IsV1Ticket())
    Common::GetV1TicketFileName(title_id, path, sizeof(path));
  else
    Common::GetTicketFileName(title_id, path, sizeof(path));

  constexpr FS::Modes ticket_modes{FS::Mode::ReadWrite, FS::Mode::ReadWrite, FS::Mode::None};
  fs->CreateFullPath(PID_KERNEL, PID_KERNEL, path, 0, ticket_modes);
  const auto file = fs->CreateAndOpenFile(PID_KERNEL, PID_KERNEL, path, ticket_modes);
  if (!file)
    return FS::ConvertResult(file.error());

  size_t raw_ticket_size;
  const u8* const raw_ticket = ticket.GetBytes(&raw_ticket_size);
  return file->Write(raw_ticket, raw_ticket_size) ? IPC_SUCCESS : ES_EIO;
}

ESCore::TitleImportExportContext::ContentContext::~ContentContext()
{
  std::free(buffer);
}

void ESCore::TitleImportExportContext::ContentContext::Reset()
{
  valid = false;
  id = 0;
  std::memset(iv, 0, sizeof(iv));
  buffer_size = 0;
}

u8* ESCore::TitleImportExportContext::ContentContext::Append(size_t size)
{
  if (size > buffer_capacity - buffer_size)
  {
    size_t capacity = buffer_capacity ? buffer_capacity : 0x10000;
    while (capacity - buffer_size < size)
      capacity *= 2;
    u8* const grown = static_cast<u8*>(std::realloc(buffer, capacity));
    if (!grown)
      return nullptr;
    buffer = grown;
    buffer_capacity = capacity;
  }
  u8* const appended = buffer + buffer_size;
  buffer_size += size;
  return appended;
}

void ESCore::TitleImportExportContext::Reset()
{
  valid = false;
  tmd = {};
  key_handle = 0;
  content.Reset();
}

void ESCore::TitleImportExportContext::DoState(PointerWrap& p)
{
  p.Do(valid);
  p.Do(key_handle);
  tmd.DoState(p);
  p.Do(content.valid);
  p.Do(content.id);
  p.Do(content.iv);
  // Same layout as the std::vector this replaced: the byte count, then the bytes.
  u32 size = static_cast<u32>(content.buffer_size);
  p.Do(size);
  if (p.IsReadMode())
  {
    content.buffer_size = 0;
    if (size && !content.Append(size))
    {
      p.SetMeasureMode();
      return;
    }
  }
  if (size)
    p.DoArray(content.buffer, size);
}

// An IPC input vector's bytes, lent where they lie in guest memory; an empty
// one is empty wherever it points.
static bool GuestBytes(const Memory::MemoryManager& memory, const IOCtlVRequest::IOVector& vector,
                       const u8** out)
{
  if (!vector.size)
  {
    *out = nullptr;
    return true;
  }
  *out = memory.GetPointerForRange(vector.address, vector.size);
  return *out != nullptr;
}

ReturnCode ESCore::ImportTicket(const u8* ticket_bytes, size_t ticket_size, const u8* cert_chain,
                                size_t cert_chain_size, TicketImportType type,
                                VerifySignature verify_signature)
{
  ES::TicketReader ticket{ticket_bytes, ticket_size};
  if (!ticket.IsValid())
    return ES_EINVAL;

  const u32 ticket_device_id = ticket.GetDeviceId();
  const u32 device_id = m_ios.GetIOSC().GetDeviceId();
  if (type == TicketImportType::PossiblyPersonalised && ticket_device_id != 0)
  {
    if (device_id != ticket_device_id)
    {
      WARN_LOG(IOS_ES, "Device ID mismatch: ticket %08x, device %08x", ticket_device_id, device_id);
      return ES_DEVICE_ID_MISMATCH;
    }
    const ReturnCode ret = ticket.Unpersonalise(m_ios.GetIOSC());
    if (ret < 0)
    {
      ERROR_LOG(IOS_ES, "ImportTicket: Failed to unpersonalise ticket for %016" PRIx64 " (%d)",
                (u64)ticket.GetTitleId(), std::to_underlying(ret));
      return ret;
    }
  }

  if (verify_signature != VerifySignature::No)
  {
    const ReturnCode verify_ret =
        VerifyContainer(VerifyContainerType::Ticket, VerifyMode::UpdateCertStore, ticket,
                        cert_chain, cert_chain_size);
    if (verify_ret != IPC_SUCCESS)
      return verify_ret;
  }

  const ReturnCode write_ret = WriteTicket(m_ios.GetFS(), ticket);
  if (write_ret != IPC_SUCCESS)
    return write_ret;

  INFO_LOG(IOS_ES, "ImportTicket: Imported ticket for title %016" PRIx64, (u64)ticket.GetTitleId());
  return IPC_SUCCESS;
}

IPCReply ESDevice::ImportTicket(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(3, 0))
    return IPCReply(ES_EINVAL);

  auto& system = GetSystem();
  auto& memory = system.GetMemory();

  const u8* bytes;
  const u8* cert_chain;
  if (!GuestBytes(memory, request.in_vectors[0], &bytes) ||
      !GuestBytes(memory, request.in_vectors[1], &cert_chain))
  {
    return IPCReply(ES_EINVAL);
  }
  return IPCReply(m_core.ImportTicket(bytes, request.in_vectors[0].size, cert_chain,
                                      request.in_vectors[1].size));
}

constexpr u8 NULL_KEY[16]{};

// Used for exporting titles and importing them back (ImportTmd and ExportTitleInit).
static ReturnCode InitBackupKey(u64 tid, u32 title_flags, IOSC& iosc, IOSC::Handle* key)
{
  // Some versions of IOS have a bug that causes it to use a zeroed key instead of the PRNG key.
  // When Nintendo decided to fix it, they added checks to keep using the zeroed key only in
  // affected titles to avoid making existing exports useless.

  // Ignore the region byte.
  const u64 title_id = tid | 0xff;
  const u32 affected_type = ES::TITLE_TYPE_0x10 | ES::TITLE_TYPE_DATA;
  if (title_id == Titles::SYSTEM_MENU || (title_flags & affected_type) != affected_type ||
      !(title_id == 0x00010005735841ff || title_id - 0x00010005735a41ff <= 0x700))
  {
    *key = IOSC::HANDLE_PRNG_KEY;
    return IPC_SUCCESS;
  }

  // Otherwise, use a null key.
  ReturnCode ret =
      iosc.CreateObject(key, IOSC::TYPE_SECRET_KEY, IOSC::ObjectSubType::AES128, PID_ES);
  return ret == IPC_SUCCESS ? iosc.ImportSecretKey(*key, NULL_KEY, PID_ES) : ret;
}

static void ResetTitleImportContext(ESCore::Context* context, IOSC& iosc)
{
  if (context->title_import_export.key_handle)
    iosc.DeleteObject(context->title_import_export.key_handle, PID_ES);
  context->title_import_export.Reset();
}

ReturnCode ESCore::ImportTmd(Context& context, const u8* tmd_bytes, size_t tmd_size,
                             u64 caller_title_id, u32 caller_title_flags)
{
  INFO_LOG(IOS_ES, "ImportTmd");

  // Ioctlv 0x2b writes the TMD to /tmp/title.tmd (for imports) and doesn't seem to write it
  // to either /import or /title. So here we simply have to set the import TMD.
  ResetTitleImportContext(&context, m_ios.GetIOSC());
  context.title_import_export.tmd.SetBytes(tmd_bytes, tmd_size);
  if (!context.title_import_export.tmd.IsValid())
    return ES_EINVAL;

  u8* cert_store;
  size_t cert_store_size;
  ReturnCode ret = ReadCertStore(&cert_store, &cert_store_size);
  if (ret != IPC_SUCCESS)
    return ret;

  ret = VerifyContainer(VerifyContainerType::TMD, VerifyMode::UpdateCertStore,
                        context.title_import_export.tmd, cert_store, cert_store_size);
  std::free(cert_store);
  if (ret != IPC_SUCCESS)
  {
    ERROR_LOG(IOS_ES, "ImportTmd: VerifyContainer failed with error %d", std::to_underlying(ret));
    return ret;
  }

  if (!InitImport(context.title_import_export.tmd))
  {
    ERROR_LOG(IOS_ES, "ImportTmd: Failed to initialise title import");
    return ES_EIO;
  }

  ret = InitBackupKey(caller_title_id, caller_title_flags, m_ios.GetIOSC(),
                      &context.title_import_export.key_handle);
  if (ret != IPC_SUCCESS)
  {
    ERROR_LOG(IOS_ES, "ImportTmd: InitBackupKey failed with error %d", std::to_underlying(ret));
    return ret;
  }

  INFO_LOG(IOS_ES, "ImportTmd: All checks passed, marking context as valid");
  context.title_import_export.valid = true;
  return IPC_SUCCESS;
}

IPCReply ESDevice::ImportTmd(Context& context, const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(1, 0))
    return IPCReply(ES_EINVAL);

  if (!ES::IsValidTMDSize(request.in_vectors[0].size))
    return IPCReply(ES_EINVAL);

  auto& system = GetSystem();
  auto& memory = system.GetMemory();

  const u8* tmd;
  if (!GuestBytes(memory, request.in_vectors[0], &tmd))
    return IPCReply(ES_EINVAL);
  return IPCReply(m_core.ImportTmd(context, tmd, request.in_vectors[0].size,
                                   m_core.m_title_context.tmd.GetTitleId(),
                                   m_core.m_title_context.tmd.GetTitleFlags()));
}

static ReturnCode InitTitleImportKey(const u8* ticket_bytes, IOSC& iosc, IOSC::Handle* handle)
{
  ReturnCode ret =
      iosc.CreateObject(handle, IOSC::TYPE_SECRET_KEY, IOSC::ObjectSubType::AES128, PID_ES);
  if (ret != IPC_SUCCESS)
    return ret;

  u8 iv[16] = {};
  std::memcpy(iv, ticket_bytes + offsetof(ES::Ticket, title_id), sizeof(u64));
  const u8 index = ticket_bytes[offsetof(ES::Ticket, common_key_index)];
  if (index >= ARRAY_SIZE(IOSC::COMMON_KEY_HANDLES))
    return ES_INVALID_TICKET;

  return iosc.ImportSecretKey(*handle, IOSC::COMMON_KEY_HANDLES[index], iv,
                              &ticket_bytes[offsetof(ES::Ticket, title_key)], PID_ES);
}

ReturnCode ESCore::ImportTitleInit(Context& context, const u8* tmd_bytes, size_t tmd_size,
                                   const u8* cert_chain, size_t cert_chain_size,
                                   VerifySignature verify_signature)
{
  INFO_LOG(IOS_ES, "ImportTitleInit");
  ResetTitleImportContext(&context, m_ios.GetIOSC());
  context.title_import_export.tmd.SetBytes(tmd_bytes, tmd_size);
  if (!context.title_import_export.tmd.IsValid())
  {
    ERROR_LOG(IOS_ES, "Invalid TMD while adding title (size = %" PRIu64 ")", (u64)tmd_size);
    return ES_EINVAL;
  }

  // Finish a previous import (if it exists).
  FinishStaleImport(context.title_import_export.tmd.GetTitleId());

  ReturnCode ret = IPC_SUCCESS;

  if (verify_signature != VerifySignature::No)
  {
    ret = VerifyContainer(VerifyContainerType::TMD, VerifyMode::UpdateCertStore,
                          context.title_import_export.tmd, cert_chain, cert_chain_size);
    if (ret != IPC_SUCCESS)
      return ret;
  }

  const auto ticket = FindSignedTicket(context.title_import_export.tmd.GetTitleId());
  if (!ticket.IsValid())
    return ES_NO_TICKET;

  if (verify_signature != VerifySignature::No)
  {
    u8* cert_store;
    size_t cert_store_size;
    ret = ReadCertStore(&cert_store, &cert_store_size);
    if (ret != IPC_SUCCESS)
      return ret;

    ret = VerifyContainer(VerifyContainerType::Ticket, VerifyMode::DoNotUpdateCertStore, ticket,
                          cert_store, cert_store_size);
    std::free(cert_store);
    if (ret != IPC_SUCCESS)
      return ret;
  }

  size_t ticket_size;
  ret = InitTitleImportKey(ticket.GetBytes(&ticket_size), m_ios.GetIOSC(),
                           &context.title_import_export.key_handle);
  if (ret != IPC_SUCCESS)
    return ret;

  if (!InitImport(context.title_import_export.tmd))
    return ES_EIO;

  context.title_import_export.valid = true;
  return IPC_SUCCESS;
}

IPCReply ESDevice::ImportTitleInit(Context& context, const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(4, 0))
    return IPCReply(ES_EINVAL);

  if (!ES::IsValidTMDSize(request.in_vectors[0].size))
    return IPCReply(ES_EINVAL);

  auto& system = GetSystem();
  auto& memory = system.GetMemory();

  const u8* tmd;
  const u8* certs;
  if (!GuestBytes(memory, request.in_vectors[0], &tmd) ||
      !GuestBytes(memory, request.in_vectors[1], &certs))
  {
    return IPCReply(ES_EINVAL);
  }
  return IPCReply(m_core.ImportTitleInit(context, tmd, request.in_vectors[0].size, certs,
                                         request.in_vectors[1].size));
}

ReturnCode ESCore::ImportContentBegin(Context& context, u64 title_id, u32 content_id)
{
  if (context.title_import_export.content.valid)
  {
    ERROR_LOG(IOS_ES, "Trying to add content when we haven't finished adding "
                      "another content. Unsupported.");
    return ES_EINVAL;
  }
  context.title_import_export.content.Reset();
  context.title_import_export.content.id = content_id;

  INFO_LOG(IOS_ES, "ImportContentBegin: title %016" PRIx64 ", content ID %08x", (u64)title_id,
           context.title_import_export.content.id);

  if (!context.title_import_export.valid)
    return ES_EINVAL;

  if (title_id != context.title_import_export.tmd.GetTitleId())
  {
    ERROR_LOG(IOS_ES,
              "ImportContentBegin: title id %016" PRIx64 " != TMD title id %016" PRIx64
              ", ignoring",
              (u64)title_id, (u64)context.title_import_export.tmd.GetTitleId());
    return ES_EINVAL;
  }

  // The IV for title content decryption is the lower two bytes of the
  // content index, zero extended.
  ES::Content content_info;
  if (!context.title_import_export.tmd.FindContentById(context.title_import_export.content.id,
                                                       &content_info))
    return ES_EINVAL;
  context.title_import_export.content.iv[0] = (content_info.index >> 8) & 0xFF;
  context.title_import_export.content.iv[1] = content_info.index & 0xFF;

  context.title_import_export.content.valid = true;

  // We're supposed to return a "content file descriptor" here, which is
  // passed to further AddContentData / AddContentFinish. But so far there is
  // no known content installer which performs content addition concurrently.
  // Instead we just log an error (see above) if this condition is detected.
  s32 content_fd = 0;
  return static_cast<ReturnCode>(content_fd);
}

IPCReply ESDevice::ImportContentBegin(Context& context, const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(2, 0))
    return IPCReply(ES_EINVAL);

  auto& system = GetSystem();
  auto& memory = system.GetMemory();

  u64 title_id = memory.Read_U64(request.in_vectors[0].address);
  u32 content_id = memory.Read_U32(request.in_vectors[1].address);
  return IPCReply(m_core.ImportContentBegin(context, title_id, content_id));
}

ReturnCode ESCore::ImportContentData(Context& context, u32 content_fd, const u8* data,
                                     u32 data_size)
{
  INFO_LOG(IOS_ES, "ImportContentData: content fd %08x, size %u", content_fd, data_size);
  u8* const appended = context.title_import_export.content.Append(data_size);
  if (!appended)
    return ES_EIO;
  if (data_size)
    std::memcpy(appended, data, data_size);
  return IPC_SUCCESS;
}

IPCReply ESDevice::ImportContentData(Context& context, const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(2, 0))
    return IPCReply(ES_EINVAL);

  auto& system = GetSystem();
  auto& memory = system.GetMemory();

  u32 content_fd = memory.Read_U32(request.in_vectors[0].address);
  u32 data_size = request.in_vectors[1].size;
  u8* data_start = memory.GetPointerForRange(request.in_vectors[1].address, data_size);
  // data that is not in emulated memory is not imported
  if (!data_start && data_size != 0)
    return IPCReply(ES_EINVAL);
  return IPCReply(m_core.ImportContentData(context, content_fd, data_start, data_size));
}

static bool CheckIfContentHashMatches(const u8* content, const ES::Content& info)
{
  u8 digest[20];
  SHA1Digest(content, info.size, digest);
  return !std::memcmp(digest, info.sha1, sizeof(digest));
}

static void GetImportContentPath(u64 title_id, u32 content_id, char* out, size_t out_size)
{
  std::snprintf(out, out_size, "/import/%08x/%08x/content/%08x.app",
                static_cast<u32>(title_id >> 32), static_cast<u32>(title_id), content_id);
}

ReturnCode ESCore::ImportContentEnd(Context& context, u32 content_fd)
{
  INFO_LOG(IOS_ES, "ImportContentEnd: content fd %08x", content_fd);

  if (!context.title_import_export.valid || !context.title_import_export.content.valid)
    return ES_EINVAL;

  // decrypted in place: the encrypted bytes are not needed after
  u8* const decrypted_data = context.title_import_export.content.buffer;
  const size_t decrypted_size = context.title_import_export.content.buffer_size;
  const ReturnCode decrypt_ret = m_ios.GetIOSC().Decrypt(
      context.title_import_export.key_handle, context.title_import_export.content.iv,
      decrypted_data, decrypted_size, decrypted_data, PID_ES);
  if (decrypt_ret != IPC_SUCCESS)
    return decrypt_ret;

  ES::Content content_info;
  context.title_import_export.tmd.FindContentById(context.title_import_export.content.id,
                                                  &content_info);
  // The hash covers content_info.size bytes, which all have to have been imported
  if (decrypted_size < content_info.size ||
      !CheckIfContentHashMatches(decrypted_data, content_info))
  {
    ERROR_LOG(IOS_ES, "ImportContentEnd: Hash for content %08x doesn't match", content_info.id);
    return ES_HASH_MISMATCH;
  }

  const auto fs = m_ios.GetFS();
  char content_path[Common::NAND_PATH_SIZE];
  if (content_info.IsShared())
  {
    ES::SharedContentMap shared_content{m_ios.GetFSCore()};
    std::snprintf(content_path, sizeof(content_path), "%s",
                  shared_content.AddSharedContent(content_info.sha1).c_str());
  }
  else
  {
    GetImportContentPath(context.title_import_export.tmd.GetTitleId(),
                         context.title_import_export.content.id, content_path,
                         sizeof(content_path));
  }

  char temp_path[sizeof("/tmp/") + Common::NAND_PATH_SIZE];
  const char* const last_separator = std::strrchr(content_path, '/');
  std::snprintf(temp_path, sizeof(temp_path), "/tmp/%s",
                last_separator ? last_separator + 1 : content_path);

  {
    constexpr FS::Modes content_modes{FS::Mode::ReadWrite, FS::Mode::ReadWrite, FS::Mode::None};
    const auto file = fs->CreateAndOpenFile(PID_KERNEL, PID_KERNEL, temp_path, content_modes);
    if (!file || !file->Write(decrypted_data, content_info.size))
    {
      ERROR_LOG(IOS_ES, "ImportContentEnd: Failed to write to %s", temp_path);
      return ES_EIO;
    }
  }

  const FS::ResultCode rename_result = fs->Rename(PID_KERNEL, PID_KERNEL, temp_path, content_path);
  if (rename_result != FS::ResultCode::Success)
  {
    fs->Delete(PID_KERNEL, PID_KERNEL, temp_path);
    ERROR_LOG(IOS_ES, "ImportContentEnd: Failed to move content to %s", content_path);
    return FS::ConvertResult(rename_result);
  }

  context.title_import_export.content.Reset();
  return IPC_SUCCESS;
}

IPCReply ESDevice::ImportContentEnd(Context& context, const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(1, 0))
    return IPCReply(ES_EINVAL);

  auto& system = GetSystem();
  auto& memory = system.GetMemory();

  u32 content_fd = memory.Read_U32(request.in_vectors[0].address);
  return IPCReply(m_core.ImportContentEnd(context, content_fd));
}

static bool HasAllRequiredContents(Kernel& ios, const ES::TMDReader& tmd)
{
  const u64 title_id = tmd.GetTitleId();
  const ES::SharedContentMap shared_content_map{ios.GetFSCore()};
  for (u16 i = 0; i < tmd.GetNumContents(); ++i)
  {
    ES::Content content;
    if (!tmd.GetContent(i, &content))
      continue;
    if (content.IsOptional())
      continue;

    if (content.IsShared())
    {
      std::string filename;
      if (!shared_content_map.GetFilenameFromSHA1(content.sha1, &filename))
        return false;
      continue;
    }

    // Note: the import hasn't been finalised yet, so the whole title directory
    // is still in /import, not /title.
    char path[Common::NAND_PATH_SIZE];
    GetImportContentPath(title_id, content.id, path, sizeof(path));
    if (!ios.GetFS()->GetMetadata(PID_KERNEL, PID_KERNEL, path).has_value())
      return false;
  }
  return true;
}

ReturnCode ESCore::ImportTitleDone(Context& context)
{
  if (!context.title_import_export.valid || context.title_import_export.content.valid)
  {
    ERROR_LOG(IOS_ES, "ImportTitleDone: No title import, or a content import is still in progress");
    return ES_EINVAL;
  }

  // For system titles, make sure all listed, non-optional contents have been imported.
  const u64 title_id = context.title_import_export.tmd.GetTitleId();
  if (title_id - 0x100000001LL <= 0x100 &&
      !HasAllRequiredContents(m_ios, context.title_import_export.tmd))
  {
    ERROR_LOG(IOS_ES, "ImportTitleDone: Some required contents are missing");
    return ES_EINVAL;
  }

  if (!WriteImportTMD(context.title_import_export.tmd))
  {
    ERROR_LOG(IOS_ES, "ImportTitleDone: Failed to write import TMD");
    return ES_EIO;
  }

  if (!FinishImport(context.title_import_export.tmd))
  {
    ERROR_LOG(IOS_ES, "ImportTitleDone: Failed to finalise title import");
    return ES_EIO;
  }

  INFO_LOG(IOS_ES, "ImportTitleDone: title %016" PRIx64, (u64)title_id);
  ResetTitleImportContext(&context, m_ios.GetIOSC());
  return IPC_SUCCESS;
}

IPCReply ESDevice::ImportTitleDone(Context& context, const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(0, 0))
    return IPCReply(ES_EINVAL);

  return IPCReply(m_core.ImportTitleDone(context));
}

ReturnCode ESCore::ImportTitleCancel(Context& context)
{
  // The TMD buffer can exist without a valid title import context.
  size_t tmd_size;
  context.title_import_export.tmd.GetBytes(&tmd_size);
  if (!tmd_size ||
      context.title_import_export.content.valid)
    return ES_EINVAL;

  if (context.title_import_export.valid)
  {
    const u64 title_id = context.title_import_export.tmd.GetTitleId();
    FinishStaleImport(title_id);
    INFO_LOG(IOS_ES, "ImportTitleCancel: title %016" PRIx64, (u64)title_id);
  }

  ResetTitleImportContext(&context, m_ios.GetIOSC());
  return IPC_SUCCESS;
}

IPCReply ESDevice::ImportTitleCancel(Context& context, const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(0, 0))
    return IPCReply(ES_EINVAL);

  return IPCReply(m_core.ImportTitleCancel(context));
}

static bool CanDeleteTitle(u64 title_id)
{
  // IOS only allows deleting non-system titles (or a system title higher than 00000001-00000101).
  return static_cast<u32>(title_id >> 32) != 0x00000001 || static_cast<u32>(title_id) > 0x101;
}

ReturnCode ESCore::DeleteTitle(u64 title_id)
{
  if (!CanDeleteTitle(title_id))
    return ES_EINVAL;

  char title_dir[Common::NAND_PATH_SIZE];
  Common::GetTitlePath(title_id, title_dir, sizeof(title_dir));
  return FS::ConvertResult(m_ios.GetFS()->Delete(PID_KERNEL, PID_KERNEL, title_dir));
}

IPCReply ESDevice::DeleteTitle(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(1, 0) || request.in_vectors[0].size != 8)
    return IPCReply(ES_EINVAL);

  auto& system = GetSystem();
  auto& memory = system.GetMemory();

  const u64 title_id = memory.Read_U64(request.in_vectors[0].address);
  return IPCReply(m_core.DeleteTitle(title_id));
}

ReturnCode ESCore::DeleteTicket(const u8* ticket_view)
{
  // a view that is not in the emulated software's memory
  if (!ticket_view)
    return ES_EINVAL;

  const auto fs = m_ios.GetFS();
  const u64 title_id = Common::swap64(ticket_view + offsetof(ES::TicketView, title_id));

  if (!CanDeleteTitle(title_id))
    return ES_EINVAL;

  auto ticket = FindSignedTicket(title_id);
  if (!ticket.IsValid())
    return FS_ENOENT;

  const bool was_v1_ticket = ticket.IsV1Ticket();
  char ticket_path[Common::NAND_PATH_SIZE];
  if (was_v1_ticket)
    Common::GetV1TicketFileName(title_id, ticket_path, sizeof(ticket_path));
  else
    Common::GetTicketFileName(title_id, ticket_path, sizeof(ticket_path));

  const u64 ticket_id = Common::swap64(ticket_view + offsetof(ES::TicketView, ticket_id));
  ticket.DeleteTicket(ticket_id);

  size_t new_ticket_size;
  const u8* const new_ticket = ticket.GetBytes(&new_ticket_size);

  if (new_ticket_size)
  {
    ASSERT(ticket.IsValid());
    ASSERT(ticket.IsV1Ticket() == was_v1_ticket);
    const auto file = fs->OpenFile(PID_KERNEL, PID_KERNEL, ticket_path, FS::Mode::ReadWrite);
    if (!file || !file->Write(new_ticket, new_ticket_size))
      return ES_EIO;
  }
  else
  {
    // Delete the ticket file if it is now empty.
    fs->Delete(PID_KERNEL, PID_KERNEL, ticket_path);
  }

  // Delete the ticket directory if it is now empty.
  const std::string ticket_parent_dir =
      StringFromFormat("/ticket/%08x", static_cast<u32>(title_id >> 32));
  FS::DirectoryList ticket_parent_dir_entries;
  if (fs->ReadDirectory(PID_KERNEL, PID_KERNEL, ticket_parent_dir.c_str(),
                        &ticket_parent_dir_entries) == FS::ResultCode::Success &&
      ticket_parent_dir_entries.count == 0)
  {
    fs->Delete(PID_KERNEL, PID_KERNEL, ticket_parent_dir.c_str());
  }
  FS::FreeDirectoryList(&ticket_parent_dir_entries);

  return IPC_SUCCESS;
}

IPCReply ESDevice::DeleteTicket(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(1, 0) ||
      request.in_vectors[0].size != sizeof(ES::TicketView))
  {
    return IPCReply(ES_EINVAL);
  }

  auto& system = GetSystem();
  auto& memory = system.GetMemory();
  return IPCReply(m_core.DeleteTicket(
      memory.GetPointerForRange(request.in_vectors[0].address, sizeof(ES::TicketView))));
}

ReturnCode ESCore::DeleteTitleContent(u64 title_id) const
{
  if (!CanDeleteTitle(title_id))
    return ES_EINVAL;

  char content_dir[Common::NAND_PATH_SIZE];
  Common::GetTitleContentPath(title_id, content_dir, sizeof(content_dir));
  FS::DirectoryList files;
  const FS::ResultCode result =
      m_ios.GetFS()->ReadDirectory(PID_KERNEL, PID_KERNEL, content_dir, &files);
  if (result != FS::ResultCode::Success)
    return FS::ConvertResult(result);

  for (u32 i = 0; i < files.count; i++)
  {
    const char* const file_name = files.names[i];
    if (std::strlen(file_name) == 12 && !std::strcmp(file_name + 8, ".app"))
    {
      char path[Common::NAND_PATH_SIZE];
      Common::GetTitleContentFilePath(title_id, file_name, path, sizeof(path));
      m_ios.GetFS()->Delete(PID_KERNEL, PID_KERNEL, path);
    }
  }
  FS::FreeDirectoryList(&files);

  return IPC_SUCCESS;
}

IPCReply ESDevice::DeleteTitleContent(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(1, 0) || request.in_vectors[0].size != sizeof(u64))
    return IPCReply(ES_EINVAL);

  auto& system = GetSystem();
  auto& memory = system.GetMemory();
  return IPCReply(m_core.DeleteTitleContent(memory.Read_U64(request.in_vectors[0].address)));
}

ReturnCode ESCore::DeleteContent(u64 title_id, u32 content_id) const
{
  if (!CanDeleteTitle(title_id))
    return ES_EINVAL;

  const auto tmd = FindInstalledTMD(title_id);
  if (!tmd.IsValid())
    return FS_ENOENT;

  ES::Content content;
  if (!tmd.FindContentById(content_id, &content))
    return ES_EINVAL;

  char content_name[16];
  char path[Common::NAND_PATH_SIZE];
  std::snprintf(content_name, sizeof(content_name), "%08x.app", content_id);
  Common::GetTitleContentFilePath(title_id, content_name, path, sizeof(path));
  return FS::ConvertResult(m_ios.GetFS()->Delete(PID_KERNEL, PID_KERNEL, path));
}

IPCReply ESDevice::DeleteContent(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(2, 0) || request.in_vectors[0].size != sizeof(u64) ||
      request.in_vectors[1].size != sizeof(u32))
  {
    return IPCReply(ES_EINVAL);
  }

  auto& system = GetSystem();
  auto& memory = system.GetMemory();
  return IPCReply(m_core.DeleteContent(memory.Read_U64(request.in_vectors[0].address),
                                       memory.Read_U32(request.in_vectors[1].address)));
}

ReturnCode ESCore::ExportTitleInit(Context& context, u64 title_id, u8* tmd_bytes, u32 tmd_size,
                                   u64 caller_title_id, u32 caller_title_flags)
{
  // No concurrent title import/export is allowed.
  if (context.title_import_export.valid)
    return ES_EINVAL;

  const auto tmd = FindInstalledTMD(title_id);
  if (!tmd.IsValid())
    return FS_ENOENT;

  ResetTitleImportContext(&context, m_ios.GetIOSC());
  context.title_import_export.tmd = tmd;

  const ReturnCode ret = InitBackupKey(caller_title_id, caller_title_flags, m_ios.GetIOSC(),
                                       &context.title_import_export.key_handle);
  if (ret != IPC_SUCCESS)
    return ret;

  size_t raw_tmd_size;
  const u8* const raw_tmd = context.title_import_export.tmd.GetBytes(&raw_tmd_size);
  // the same size, and a buffer for it in the emulated software's memory
  if (tmd_size != raw_tmd_size || !tmd_bytes)
    return ES_EINVAL;

  std::memcpy(tmd_bytes, raw_tmd, raw_tmd_size);

  context.title_import_export.valid = true;
  return IPC_SUCCESS;
}

IPCReply ESDevice::ExportTitleInit(Context& context, const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(1, 1) || request.in_vectors[0].size != 8)
    return IPCReply(ES_EINVAL);

  auto& system = GetSystem();
  auto& memory = system.GetMemory();

  const u64 title_id = memory.Read_U64(request.in_vectors[0].address);
  const u32 tmd_size = request.io_vectors[0].size;
  u8* tmd_bytes = memory.GetPointerForRange(request.io_vectors[0].address, tmd_size);

  return IPCReply(m_core.ExportTitleInit(context, title_id, tmd_bytes, tmd_size,
                                         m_core.m_title_context.tmd.GetTitleId(),
                                         m_core.m_title_context.tmd.GetTitleFlags()));
}

ReturnCode ESCore::ExportContentBegin(Context& context, u64 title_id, u32 content_id)
{
  context.title_import_export.content.Reset();
  if (!context.title_import_export.valid ||
      context.title_import_export.tmd.GetTitleId() != title_id)
  {
    ERROR_LOG(IOS_ES, "Tried to use ExportContentBegin with an invalid title export context.");
    return ES_EINVAL;
  }

  ES::Content content_info;
  if (!context.title_import_export.tmd.FindContentById(content_id, &content_info))
    return ES_EINVAL;

  context.title_import_export.content.id = content_id;
  context.title_import_export.content.valid = true;

  const s32 ret = OpenContent(context.title_import_export.tmd, content_info.index, 0);
  if (ret < 0)
  {
    ResetTitleImportContext(&context, m_ios.GetIOSC());
    return static_cast<ReturnCode>(ret);
  }

  context.title_import_export.content.iv[0] = (content_info.index >> 8) & 0xFF;
  context.title_import_export.content.iv[1] = content_info.index & 0xFF;

  // IOS returns a content ID which is passed to further content calls.
  return static_cast<ReturnCode>(ret);
}

IPCReply ESDevice::ExportContentBegin(Context& context, const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(2, 0) || request.in_vectors[0].size != 8 ||
      request.in_vectors[1].size != 4)
    return IPCReply(ES_EINVAL);

  auto& system = GetSystem();
  auto& memory = system.GetMemory();

  const u64 title_id = memory.Read_U64(request.in_vectors[0].address);
  const u32 content_id = memory.Read_U32(request.in_vectors[1].address);

  return IPCReply(m_core.ExportContentBegin(context, title_id, content_id));
}

ReturnCode ESCore::ExportContentData(Context& context, u32 content_fd, u8* data, u32 data_size)
{
  if (!context.title_import_export.valid || !context.title_import_export.content.valid || !data ||
      data_size == 0)
  {
    CloseContent(content_fd, 0);
    context.title_import_export.Reset();
    return ES_EINVAL;
  }

  // Read into the guest's buffer and encrypted there, in place.
  const s32 read_size = ReadContent(content_fd, data, data_size, 0);
  if (read_size < 0)
  {
    CloseContent(content_fd, 0);
    ResetTitleImportContext(&context, m_ios.GetIOSC());
    return ES_SHORT_READ;
  }

  // IOS aligns the buffer to 32 bytes. Since we also need to align it to 16 bytes,
  // let's just follow IOS here; what was not read, and the padding, are zeroes.
  const u32 aligned_size = Common::AlignUp(data_size, 32u);
  std::memset(data + read_size, 0, aligned_size - static_cast<u32>(read_size));

  return m_ios.GetIOSC().Encrypt(context.title_import_export.key_handle,
                                 context.title_import_export.content.iv, data, aligned_size, data,
                                 PID_ES);
}

IPCReply ESDevice::ExportContentData(Context& context, const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(1, 1) || request.in_vectors[0].size != 4 ||
      request.io_vectors[0].size == 0)
  {
    return IPCReply(ES_EINVAL);
  }

  auto& system = GetSystem();
  auto& memory = system.GetMemory();

  const u32 content_fd = memory.Read_U32(request.in_vectors[0].address);
  const u32 bytes_to_read = request.io_vectors[0].size;
  u8* data = memory.GetPointerForRange(request.io_vectors[0].address, bytes_to_read);

  return IPCReply(m_core.ExportContentData(context, content_fd, data, bytes_to_read));
}

ReturnCode ESCore::ExportContentEnd(Context& context, u32 content_fd)
{
  if (!context.title_import_export.valid || !context.title_import_export.content.valid)
    return ES_EINVAL;
  return static_cast<ReturnCode>(CloseContent(content_fd, 0));
}

IPCReply ESDevice::ExportContentEnd(Context& context, const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(1, 0) || request.in_vectors[0].size != 4)
    return IPCReply(ES_EINVAL);

  auto& system = GetSystem();
  auto& memory = system.GetMemory();

  const u32 content_fd = memory.Read_U32(request.in_vectors[0].address);
  return IPCReply(m_core.ExportContentEnd(context, content_fd));
}

ReturnCode ESCore::ExportTitleDone(Context& context)
{
  ResetTitleImportContext(&context, m_ios.GetIOSC());
  return IPC_SUCCESS;
}

IPCReply ESDevice::ExportTitleDone(Context& context, const IOCtlVRequest& request)
{
  return IPCReply(m_core.ExportTitleDone(context));
}

namespace
{
struct SharedContentUse
{
  const ESCore* core;
  const u8* sha1;
  bool is_used_by_system_title;
};

bool IsSharedContentUsedBy(u64 title_id, void* context)
{
  SharedContentUse* const use = static_cast<SharedContentUse*>(context);
  if (!ES::IsTitleType(title_id, ES::TitleType::System))
    return true;

  const auto tmd = use->core->FindInstalledTMD(title_id);
  if (!tmd.IsValid())
  {
    use->is_used_by_system_title = true;
    return false;
  }

  for (u16 i = 0; i < tmd.GetNumContents(); ++i)
  {
    ES::Content content;
    if (tmd.GetContent(i, &content) && !std::memcmp(content.sha1, use->sha1, sizeof(content.sha1)))
    {
      use->is_used_by_system_title = true;
      return false;
    }
  }
  return true;
}
}  // namespace

ReturnCode ESCore::DeleteSharedContent(const u8* sha1) const
{
  ES::SharedContentMap map{m_ios.GetFSCore()};
  std::string content_path;
  if (!map.GetFilenameFromSHA1(sha1, &content_path))
    return ES_EINVAL;

  // Check whether the shared content is used by a system title.
  SharedContentUse use = {this, sha1, false};
  ForEachInstalledTitle(IsSharedContentUsedBy, &use);

  // Any shared content used by a system title cannot be deleted.
  if (use.is_used_by_system_title)
    return ES_EINVAL;

  // Delete the shared content and update the content map.
  const auto delete_result = m_ios.GetFS()->Delete(PID_KERNEL, PID_KERNEL, content_path.c_str());
  if (delete_result != FS::ResultCode::Success)
    return FS::ConvertResult(delete_result);

  if (!map.DeleteSharedContent(sha1))
    return ES_EIO;

  return IPC_SUCCESS;
}

IPCReply ESDevice::DeleteSharedContent(const IOCtlVRequest& request)
{
  u8 sha1[20];
  if (!request.HasNumberOfValidVectors(1, 0) || request.in_vectors[0].size != sizeof(sha1))
    return IPCReply(ES_EINVAL);

  auto& system = GetSystem();
  auto& memory = system.GetMemory();

  memory.CopyFromEmu(sha1, request.in_vectors[0].address, request.in_vectors[0].size);
  return IPCReply(m_core.DeleteSharedContent(sha1));
}
}  // namespace IOS::HLE
