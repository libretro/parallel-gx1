// Copyright 2010 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Based off of tachtig/twintig http://git.infradead.org/?p=users/segher/wii.git
// Copyright 2007,2008  Segher Boessenkool  <segher@kernel.crashing.org>
// Licensed under the terms of the GNU GPL, version 2
// http://www.gnu.org/licenses/old-licenses/gpl-2.0.txt

#include "Core/HW/WiiSave.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <lrc_hash.h>
#include <string>
#include <utility>
#include <vector>

#include "Common/Align.h"
#include "Common/CommonTypes.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Common/NandPaths.h"
#include "Common/Swap.h"
#include "Core/CommonTitles.h"
#include "Core/HW/WiiSaveStructs.h"
#include "Core/IOS/ES/ES.h"
#include "Core/IOS/FS/FileSystem.h"
#include "Core/IOS/IOS.h"
#include "Core/IOS/IOSC.h"
#include "Core/IOS/Uids.h"
#include "Core/WiiUtils.h"

namespace WiiSave
{
constexpr u8 s_sd_initial_iv[0x10]{0x21, 0x67, 0x12, 0xE6, 0xAA, 0x1F, 0x68, 0x9F,
                                   0x95, 0xC5, 0xA2, 0x23, 0x24, 0xDC, 0x6A, 0x98};
constexpr u8 s_md5_blanker[0x10]{0x0E, 0x65, 0x37, 0x81, 0x99, 0xBE, 0x45, 0x17,
                                 0xAB, 0x06, 0xEC, 0x22, 0x45, 0x1A, 0x57, 0x93};

static void HeaderMD5(const Header& header, u8* out)
{
  MD5_CTX ctx;
  MD5_Init(&ctx);
  MD5_Update(&ctx, &header, sizeof(Header));
  MD5_Final(out, &ctx);
}

constexpr u32 s_ng_id = 0x0403AC68;

namespace FS = IOS::HLE::FS;

class NandStorage final : public Storage
{
public:
  explicit NandStorage(FS::FileSystem* fs, u64 tid) : m_fs{fs}, m_tid{tid}
  {
    m_data_dir = Common::GetTitleDataPath(tid);
    InitTitleUidAndGid();
    ScanForFiles(m_data_dir);
  }

  bool SaveExists() const override
  {
    return !m_files_list.empty() ||
           (m_has_ids && m_fs->GetMetadata(m_uid, m_gid, m_data_dir + "/banner.bin"));
  }

  bool EraseSave() override
  {
    // banner.bin is not in m_files_list, delete separately
    const auto banner_delete_result =
        m_fs->Delete(IOS::PID_KERNEL, IOS::PID_KERNEL, m_data_dir + "/banner.bin");
    if (banner_delete_result != FS::ResultCode::Success)
      return false;

    for (const SaveFile& file : m_files_list)
    {
      // files in subdirs are deleted automatically when the subdir is deleted
      if (file.path.contains('/'))
        continue;

      const auto result =
          m_fs->Delete(IOS::PID_KERNEL, IOS::PID_KERNEL, m_data_dir + "/" + file.path);
      if (result != FS::ResultCode::Success)
        return false;
    }

    m_files_list.clear();
    m_files_size = 0;
    return true;
  }

  bool ReadHeader(Header* header_out) override
  {
    if (!m_has_ids)
      return false;

    const auto banner_path = m_data_dir + "/banner.bin";
    const auto banner = m_fs->OpenFile(m_uid, m_gid, banner_path.c_str(), FS::Mode::Read);
    if (!banner)
      return false;
    Header& header = *header_out;
    header = Header{};
    header.banner_size = banner->GetStatus()->size;
    if (header.banner_size > sizeof(header.banner))
    {
      ERROR_LOG(CORE, "NandStorage::ReadHeader: %s corrupted banner_size: %x", banner_path.c_str(),
                static_cast<unsigned>(header.banner_size));
      return false;
    }
    header.tid = m_tid;
    std::memcpy(header.md5, s_md5_blanker, sizeof(header.md5));
    const u8 mode = GetBinMode(banner_path);
    if (!mode || !banner->Read(header.banner, header.banner_size))
      return false;
    header.permissions = mode;
    // remove nocopy flag
    header.banner[7] &= ~1;

    u8 md5_calc[0x10];
    HeaderMD5(header, md5_calc);
    std::memcpy(header.md5, md5_calc, sizeof(header.md5));
    return true;
  }

  bool ReadBkHeader(BkHeader* bk_hdr_out) override
  {
    BkHeader& bk_hdr = *bk_hdr_out;
    bk_hdr = BkHeader{};
    bk_hdr.size = BK_LISTED_SZ;
    bk_hdr.magic = BK_HDR_MAGIC;
    bk_hdr.ngid = s_ng_id;
    bk_hdr.number_of_files = static_cast<u32>(m_files_list.size());
    bk_hdr.size_of_files = m_files_size;
    bk_hdr.total_size = m_files_size + FULL_CERT_SZ;
    bk_hdr.tid = m_tid;
    return true;
  }

  bool ReadFiles(const SaveFile** files, size_t* count) override
  {
    *files = m_files_list.data();
    *count = m_files_list.size();
    return true;
  }

  bool WriteHeader(const Header& header) override
  {
    if (!m_has_ids)
      return false;

    const std::string banner_file_path = m_data_dir + "/banner.bin";
    const FS::Modes modes = GetFsMode(header.permissions);
    const auto file = m_fs->CreateAndOpenFile(m_uid, m_gid, banner_file_path, modes);
    return file && file->Write(header.banner, header.banner_size);
  }

  bool WriteBkHeader(const BkHeader& bk_header) override { return true; }

  bool ReadFileData(const SaveFile& save_file, std::vector<u8>* data) override
  {
    if (!m_has_ids)
      return false;
    const std::string path = m_data_dir + '/' + save_file.path;
    const auto file = m_fs->OpenFile(m_uid, m_gid, path.c_str(), FS::Mode::Read);
    if (!file)
      return false;
    data->resize(file->GetStatus()->size);
    return file->Read(data->data(), data->size()).has_value();
  }

  bool WriteFiles(Storage* source, const SaveFile* files, size_t count) override
  {
    if (!m_has_ids)
      return false;

    std::vector<u8> data;
    for (size_t i = 0; i < count; i++)
    {
      const SaveFile& file = files[i];
      const FS::Modes modes = GetFsMode(file.mode);
      const std::string path = m_data_dir + '/' + file.path;
      if (file.type == SaveFile::Type::File)
      {
        const auto raw_file = m_fs->CreateAndOpenFile(m_uid, m_gid, path, modes);
        if (!source->ReadFileData(file, &data) || !raw_file ||
            !raw_file->Write(data.data(), data.size()))
        {
          return false;
        }
      }
      else if (file.type == SaveFile::Type::Directory)
      {
        const FS::Result<FS::Metadata> meta = m_fs->GetMetadata(m_uid, m_gid, path);
        if (meta && meta->is_file)
          return false;

        const FS::ResultCode result = m_fs->CreateDirectory(m_uid, m_gid, path, 0, modes);
        if (result != FS::ResultCode::Success)
          return false;
      }
    }
    return true;
  }

private:
  void ScanForFiles(const std::string& dir)
  {
    if (!m_has_ids)
      return;

    const auto entries = m_fs->ReadDirectory(m_uid, m_gid, dir);
    if (!entries)
      return;

    for (const std::string& elem : *entries)
    {
      if (elem == "banner.bin")
        continue;

      const std::string path = dir + '/' + elem;
      const FS::Result<FS::Metadata> metadata = m_fs->GetMetadata(m_uid, m_gid, path);
      if (!metadata)
        return;

      SaveFile save_file;
      save_file.mode = GetBinMode(metadata->modes);
      save_file.attributes = 0;
      save_file.type = metadata->is_file ? SaveFile::Type::File : SaveFile::Type::Directory;
      save_file.path = path.substr(m_data_dir.size() + 1);
      m_files_list.emplace_back(std::move(save_file));
      m_files_size += sizeof(FileHDR);

      if (metadata->is_file)
        m_files_size += static_cast<u32>(Common::AlignUp(metadata->size, BLOCK_SZ));
      else
        ScanForFiles(path);
    }
  }

  void InitTitleUidAndGid()
  {
    const auto metadata = m_fs->GetMetadata(IOS::PID_KERNEL, IOS::PID_KERNEL, m_data_dir);
    if (!metadata)
      return;
    m_uid = metadata->uid;
    m_gid = metadata->gid;
    m_has_ids = true;
  }

  static constexpr FS::Modes GetFsMode(u8 bin_mode)
  {
    return {FS::Mode(bin_mode >> 4 & 3), FS::Mode(bin_mode >> 2 & 3), FS::Mode(bin_mode >> 0 & 3)};
  }

  static constexpr u8 GetBinMode(const FS::Modes& modes)
  {
    return u8(modes.owner) << 4 | u8(modes.group) << 2 | u8(modes.other) << 0;
  }

  u8 GetBinMode(const std::string& path) const
  {
    if (const FS::Result<FS::Metadata> meta = m_fs->GetMetadata(m_uid, m_gid, path))
      return GetBinMode(meta->modes);
    return 0;
  }

  FS::FileSystem* m_fs = nullptr;
  std::string m_data_dir;
  u64 m_tid = 0;
  bool m_has_ids = false;
  u32 m_uid = 0;
  u16 m_gid = 0;
  std::vector<SaveFile> m_files_list;
  u32 m_files_size = 0;
};

class DataBinStorage final : public Storage
{
public:
  explicit DataBinStorage(IOS::HLE::IOSC* iosc, const std::string& path, const char* mode)
      : m_iosc{*iosc}
  {
    File::CreateFullPath(path);
    m_file = File::IOFile{path, mode};
  }

  bool SaveExists() const override { return m_file.GetSize() > 0; }

  bool EraseSave() override { return m_file.GetSize() == 0 || m_file.Resize(0); }

  bool ReadHeader(Header* header_out) override
  {
    Header& header = *header_out;
    if (!m_file.Seek(0, File::SeekOrigin::Begin) || !m_file.ReadArray(&header, 1))
      return false;

    u8 iv[0x10];
    std::memcpy(iv, s_sd_initial_iv, sizeof(iv));
    m_iosc.Decrypt(IOS::HLE::IOSC::HANDLE_SD_KEY, iv, reinterpret_cast<const u8*>(&header),
                   sizeof(Header), reinterpret_cast<u8*>(&header), IOS::PID_ES);
    const u32 banner_size = header.banner_size;
    if ((banner_size < FULL_BNR_MIN) || (banner_size > FULL_BNR_MAX) ||
        (((banner_size - BNR_SZ) % ICON_SZ) != 0))
    {
      ERROR_LOG(CONSOLE, "Not a Wii save or read failure for file header size %x", banner_size);
      return false;
    }

    u8 md5_file[0x10];
    std::memcpy(md5_file, header.md5, sizeof(md5_file));
    std::memcpy(header.md5, s_md5_blanker, sizeof(header.md5));
    u8 md5_calc[0x10];
    HeaderMD5(header, md5_calc);
    if (std::memcmp(md5_file, md5_calc, sizeof(md5_file)) != 0)
    {
      ERROR_LOG(CONSOLE, "MD5 mismatch\n %016" PRIx64 "%016" PRIx64 " != %016" PRIx64 "%016" PRIx64,
                (u64)Common::swap64(md5_file), (u64)Common::swap64(md5_file + 8),
                (u64)Common::swap64(md5_calc), (u64)Common::swap64(md5_calc + 8));
      return false;
    }
    return true;
  }

  bool ReadBkHeader(BkHeader* bk_header) override
  {
    m_file.Seek(sizeof(Header), File::SeekOrigin::Begin);
    if (!m_file.ReadArray(bk_header, 1))
      return false;
    if (bk_header->size != BK_LISTED_SZ || bk_header->magic != BK_HDR_MAGIC)
      return false;
    if (bk_header->size_of_files + FULL_CERT_SZ != bk_header->total_size)
      return false;
    return true;
  }

  bool ReadFiles(const SaveFile** files_out, size_t* count) override
  {
    BkHeader bk_header;
    if (!ReadBkHeader(&bk_header) ||
        !m_file.Seek(sizeof(Header) + sizeof(BkHeader), File::SeekOrigin::Begin))
      return false;

    std::vector<SaveFile>& files = m_files;
    files.clear();
    for (u32 i = 0; i < bk_header.number_of_files; ++i)
    {
      SaveFile save_file;
      FileHDR file_hdr;
      if (!m_file.ReadArray(&file_hdr, 1) || file_hdr.magic != FILE_HDR_MAGIC)
        return false;

      save_file.mode = file_hdr.permissions;
      save_file.attributes = file_hdr.attrib;
      const SaveFile::Type type = static_cast<SaveFile::Type>(file_hdr.type);
      if (type != SaveFile::Type::Directory && type != SaveFile::Type::File)
        return false;
      save_file.type = type;
      save_file.path = std::string{file_hdr.name, strnlen(file_hdr.name, sizeof(file_hdr.name))};
      if (type == SaveFile::Type::File)
      {
        const u32 size = file_hdr.size;
        const u32 rounded_size = Common::AlignUp<u32>(size, BLOCK_SZ);
        const u64 pos = m_file.Tell();

        save_file.data_pos = pos;
        save_file.data_size = size;
        std::memcpy(save_file.iv, file_hdr.iv, sizeof(save_file.iv));
        m_file.Seek(pos + rounded_size, File::SeekOrigin::Begin);
      }
      files.emplace_back(std::move(save_file));
    }
    *files_out = files.data();
    *count = files.size();
    return true;
  }

  bool WriteHeader(const Header& header) override
  {
    Header encrypted_header;
    u8 iv[0x10];
    std::memcpy(iv, s_sd_initial_iv, sizeof(iv));
    m_iosc.Encrypt(IOS::HLE::IOSC::HANDLE_SD_KEY, iv, reinterpret_cast<const u8*>(&header),
                   sizeof(Header), reinterpret_cast<u8*>(&encrypted_header), IOS::PID_ES);
    return m_file.Seek(0, File::SeekOrigin::Begin) && m_file.WriteArray(&encrypted_header, 1);
  }

  bool WriteBkHeader(const BkHeader& bk_header) override
  {
    return m_file.Seek(sizeof(Header), File::SeekOrigin::Begin) && m_file.WriteArray(&bk_header, 1);
  }

  bool ReadFileData(const SaveFile& save_file, std::vector<u8>* data) override
  {
    const u32 rounded_size = Common::AlignUp<u32>(save_file.data_size, BLOCK_SZ);
    data->resize(rounded_size);
    if (!m_file.Seek(save_file.data_pos, File::SeekOrigin::Begin) ||
        !m_file.ReadBytes(data->data(), rounded_size))
    {
      return false;
    }
    u8 iv[0x10];
    std::memcpy(iv, save_file.iv, sizeof(iv));
    m_iosc.Decrypt(IOS::HLE::IOSC::HANDLE_SD_KEY, iv, data->data(), rounded_size, data->data(),
                   IOS::PID_ES);
    data->resize(save_file.data_size);
    return true;
  }

  bool WriteFiles(Storage* source, const SaveFile* files, size_t count) override
  {
    if (!m_file.Seek(sizeof(Header) + sizeof(BkHeader), File::SeekOrigin::Begin))
      return false;

    std::vector<u8> data;
    for (size_t i = 0; i < count; i++)
    {
      const SaveFile& save_file = files[i];
      FileHDR file_hdr{};
      file_hdr.magic = FILE_HDR_MAGIC;
      file_hdr.permissions = save_file.mode;
      file_hdr.attrib = save_file.attributes;
      file_hdr.type = static_cast<u8>(save_file.type);
      if (save_file.path.length() > sizeof(file_hdr.name))
        return false;
      /* zero-padded, unterminated when it fills the field */
      std::memcpy(file_hdr.name, save_file.path.data(), save_file.path.length());

      const bool has_data = file_hdr.type == 1;
      if (has_data)
      {
        if (!source->ReadFileData(save_file, &data))
          return false;
        file_hdr.size = static_cast<u32>(data.size());
      }

      if (!m_file.WriteArray(&file_hdr, 1))
        return false;

      if (has_data)
      {
        // zero-padded to whole blocks, encrypted in place
        data.resize(Common::AlignUp(data.size(), BLOCK_SZ));
        m_iosc.Encrypt(IOS::HLE::IOSC::HANDLE_SD_KEY, file_hdr.iv, data.data(), data.size(),
                       data.data(), IOS::PID_ES);
        if (!m_file.WriteBytes(data.data(), data.size()))
          return false;
      }
    }

    if (!WriteSignatures())
    {
      ERROR_LOG(CORE, "WiiSave::WriteFiles: Failed to write signatures");
      return false;
    }
    return true;
  }

private:
  bool WriteSignatures()
  {
    BkHeader bk_header;
    if (!ReadBkHeader(&bk_header))
      return false;

    // Read data to sign.
    u8 data_sha1[20];
    {
      const u32 data_size = bk_header.size_of_files + sizeof(BkHeader);
      u8 chunk[2048];
      struct sha1_state sha1;
      sha1_stream_init(&sha1);
      m_file.Seek(sizeof(Header), File::SeekOrigin::Begin);
      for (u32 done = 0; done < data_size;)
      {
        const u32 n = std::min<u32>(data_size - done, sizeof(chunk));
        if (!m_file.ReadBytes(chunk, n))
          return false;
        sha1_stream_update(&sha1, chunk, n);
        done += n;
      }
      sha1_stream_final(&sha1, data_sha1);
    }

    // Sign the data.
    IOS::CertECC ap_cert;
    u8 ap_sig[60];
    m_iosc.Sign(ap_sig, reinterpret_cast<u8*>(&ap_cert), Titles::SYSTEM_MENU, data_sha1,
                static_cast<u32>(sizeof(data_sha1)));

    // Write signatures.
    if (!m_file.Seek(0, File::SeekOrigin::End))
      return false;
    const u32 SIGNATURE_END_MAGIC = Common::swap32(0x2f536969);
    const IOS::CertECC device_certificate = m_iosc.GetDeviceCertificate();
    return m_file.WriteArray(ap_sig, sizeof(ap_sig)) &&
           m_file.WriteArray(&SIGNATURE_END_MAGIC, 1) &&
           m_file.WriteArray(&device_certificate, 1) && m_file.WriteArray(&ap_cert, 1);
  }

  IOS::HLE::IOSC& m_iosc;
  File::IOFile m_file;
  std::vector<SaveFile> m_files;  // what ReadFiles lends
};

Storage* MakeNandStorage(FS::FileSystem* fs, u64 tid)
{
  return new NandStorage{fs, tid};
}

Storage* MakeDataBinStorage(IOS::HLE::IOSC* iosc, const std::string& path, const char* mode)
{
  return new DataBinStorage{iosc, path, mode};
}

void FreeStorage(Storage* storage)
{
  delete storage;
}

static CopyResult CopyWithHeader(Storage* source, Storage* dest, Header* header)
{
  // first make sure we can read all the data from the source
  if (!source->ReadHeader(header))
  {
    ERROR_LOG(CORE, "WiiSave::Copy: Failed to read header");
    return CopyResult::CorruptedSource;
  }

  BkHeader bk_header;
  if (!source->ReadBkHeader(&bk_header))
  {
    ERROR_LOG(CORE, "WiiSave::Copy: Failed to read bk header");
    return CopyResult::CorruptedSource;
  }

  const Storage::SaveFile* files;
  size_t file_count;
  if (!source->ReadFiles(&files, &file_count))
  {
    ERROR_LOG(CORE, "WiiSave::Copy: Failed to read files");
    return CopyResult::CorruptedSource;
  }

  // once we have confirmed we can read the source, erase corresponding save in the destination
  if (dest->SaveExists())
  {
    if (!dest->EraseSave())
    {
      ERROR_LOG(CORE, "WiiSave::Copy: Failed to erase existing save");
      return CopyResult::Error;
    }
  }

  // and then write it to the destination
  if (!dest->WriteHeader(*header))
  {
    ERROR_LOG(CORE, "WiiSave::Copy: Failed to write header");
    return CopyResult::Error;
  }

  if (!dest->WriteBkHeader(bk_header))
  {
    ERROR_LOG(CORE, "WiiSave::Copy: Failed to write bk header");
    return CopyResult::Error;
  }

  if (!dest->WriteFiles(source, files, file_count))
  {
    ERROR_LOG(CORE, "WiiSave::Copy: Failed to write files");
    return CopyResult::Error;
  }

  return CopyResult::Success;
}

CopyResult Copy(Storage* source, Storage* dest)
{
  // The header holds the whole banner, too big for the stack.
  Header* const header = new Header;
  const CopyResult result = CopyWithHeader(source, dest, header);
  delete header;
  return result;
}

}  // namespace WiiSave
