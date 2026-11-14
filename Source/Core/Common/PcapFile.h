// Copyright 2014 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// PCAP is a standard file format for network capture files. This also extends
// to any capture of packetized intercommunication data. A PCAP holds the file it
// writes, in place: Open creates the capture file and writes its header, then
// packets are appended to it.

#pragma once

#include <cstddef>

#include "Common/CommonTypes.h"
#include "Common/IOFile.h"

namespace Common
{
class PCAP final
{
public:
  enum class LinkType : u32
  {
    Ethernet = 1,  // IEEE 802.3 Ethernet
    User = 147,    // Reserved for internal use
  };

  bool Open(const char* path, LinkType link_type,
            File::SharedAccess sh = File::SharedAccess::Default);
  void Close() { m_fp.Close(); }
  bool IsOpen() const { return m_fp.IsOpen(); }

  void AddPacket(const u8* bytes, size_t size);

private:
  File::IOFile m_fp;
};
}  // namespace Common
