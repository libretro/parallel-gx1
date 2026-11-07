// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DiscIO/FileBlob.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "Common/Assert.h"
#include "Common/FileUtil.h"
#include "Common/MsgHandler.h"

namespace DiscIO
{
PlainFileReader::PlainFileReader(File::DirectIOFile file) : m_file(std::move(file))
{
  m_size = m_file.GetSize();
}

std::unique_ptr<PlainFileReader> PlainFileReader::Create(File::DirectIOFile file)
{
  if (file.IsOpen())
    return std::unique_ptr<PlainFileReader>(new PlainFileReader(std::move(file)));

  return nullptr;
}

std::unique_ptr<BlobReader> PlainFileReader::CopyReader() const
{
  return Create(m_file);
}

bool PlainFileReader::Read(u64 offset, u64 nbytes, u8* out_ptr)
{
  return m_file.OffsetRead(offset, out_ptr, nbytes);
}

}  // namespace DiscIO
