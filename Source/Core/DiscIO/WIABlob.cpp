// Copyright 2018 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DiscIO/WIABlob.h"

#include <formats/rvz.h>

#include "Common/Logging/Log.h"

namespace DiscIO
{
WIARVZFileReader::WIARVZFileReader(File::DirectIOFile file, const std::string& path, bool rvz)
    : m_file(std::move(file)), m_path(path), m_rvz(rvz)
{
}

WIARVZFileReader::~WIARVZFileReader()
{
  rvz_free(m_reader);
}

// The range of the file the reader asks for, read straight into the memory
// it names; a failed read ends the operation
bool WIARVZFileReader::Fetch(const rvz_request* req)
{
  const bool ok = m_file.OffsetRead(req->offset, req->dst, req->length);
  rvz_feed(m_reader, ok ? req->length : 0);
  return ok;
}

bool WIARVZFileReader::Open()
{
  rvz_request_t req;
  int e;
  while ((e = rvz_open_step(m_reader, &req)) == RVZ_PENDING)
  {
    if (!Fetch(&req))
      return false;
  }
  return e == RVZ_OK;
}

// Runs an armed read of size octets
bool WIARVZFileReader::RunRead(int armed, u64 size)
{
  rvz_request_t req;
  int e;
  if (armed != RVZ_OK)
    return false;
  while ((e = rvz_read_step(m_reader, &req)) == RVZ_PENDING)
  {
    if (!Fetch(&req))
      return false;
  }
  return e == RVZ_OK && rvz_read_progress(m_reader) == size;
}

WIARVZFileReader* WIARVZFileReader::Create(File::DirectIOFile file, const std::string& path)
{
  u32 magic;
  bool rvz;

  if (!file.IsOpen() || !file.OffsetRead(0, reinterpret_cast<u8*>(&magic), sizeof(magic)))
    return nullptr;
  if (magic != WIA_MAGIC && magic != RVZ_MAGIC)
    return nullptr;
  rvz = magic == RVZ_MAGIC;

  WIARVZFileReader* blob = new WIARVZFileReader(std::move(file), path, rvz);
  if (!(blob->m_reader = rvz_new()) || !blob->Open())
  {
    ERROR_LOG(DISCIO, "Not a WIA or RVZ image this reads: %s", path.c_str());
    DestroyBlobReader(blob);
    return nullptr;
  }
  blob->m_compression = rvz_compression(blob->m_reader, &blob->m_level);
  blob->m_raw_size = blob->m_file.GetSize();
  blob->m_data_size = rvz_disc_size(blob->m_reader);
  blob->m_block_size = rvz_chunk_size(blob->m_reader);
  return blob;
}

BlobReader* WIARVZFileReader::CopyReader() const
{
  return Create(m_file, m_path);
}

std::string WIARVZFileReader::GetCompressionMethod() const
{
  switch (m_compression)
  {
  case RVZ_COMPRESSION_PURGE:
    return "Purge";
  case RVZ_COMPRESSION_BZIP2:
    return "bzip2";
  case RVZ_COMPRESSION_LZMA:
    return "LZMA";
  case RVZ_COMPRESSION_LZMA2:
    return "LZMA2";
  case RVZ_COMPRESSION_ZSTD:
    return "Zstandard";
  default:
    return {};
  }
}

bool WIARVZFileReader::Read(u64 offset, u64 size, u8* out_ptr)
{
  if (offset > m_data_size || size > m_data_size - offset)
    return false;
  return RunRead(rvz_read_begin(m_reader, offset, out_ptr, (size_t)size), size);
}

bool WIARVZFileReader::SupportsReadWiiDecrypted(u64 offset, u64 size,
                                                u64 partition_data_offset) const
{
  return rvz_decrypted_supported(m_reader, partition_data_offset, offset, size);
}

bool WIARVZFileReader::ReadWiiDecrypted(u64 offset, u64 size, u8* out_ptr,
                                        u64 partition_data_offset)
{
  return RunRead(rvz_read_decrypted_begin(m_reader, partition_data_offset, offset, out_ptr,
                                          (size_t)size),
                 size);
}

}  // namespace DiscIO
