// Copyright 2023 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/IOS/Network/KD/Mail/WC24FriendList.h"

#include <algorithm>
#include <memory>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/Swap.h"

#include "Core/IOS/FS/FileSystem.h"
#include "Core/IOS/Uids.h"

namespace IOS::HLE::NWC24::Mail
{
WC24FriendList::WC24FriendList(std::shared_ptr<FS::FileSystem> fs) : m_fs{std::move(fs)}
{
  ReadFriendList();
}

void WC24FriendList::ReadFriendList()
{
  const auto file = m_fs->OpenFile(PID_KD, PID_KD, FRIEND_LIST_PATH, FS::Mode::Read);
  if (!file || !file->Read(&m_data, 1))
    return;

  const bool success = CheckFriendList();
  if (!success)
    ERROR_LOG(IOS_WC24, "There is an error in the Receive List for WC24 mail");
}

bool WC24FriendList::CheckFriendList() const
{
  // 'WcFl' magic
  if (Common::swap32(m_data.header.magic) != FRIEND_LIST_MAGIC)
  {
    ERROR_LOG(IOS_WC24, "Receive List magic mismatch (%u != %u)",
              Common::swap32(m_data.header.magic), FRIEND_LIST_MAGIC);
    return false;
  }

  return true;
}

std::vector<u64> WC24FriendList::GetUnconfirmedFriends() const
{
  std::vector<u64> friends{};
  for (u32 i = 0; i < MAX_ENTRIES; i++)
  {
    if (static_cast<FriendStatus>(Common::swap32(m_data.entries[i].status)) ==
            FriendStatus::Unconfirmed &&
        static_cast<FriendType>(Common::swap32(m_data.entries[i].friend_type)) == FriendType::Wii)
    {
      friends.push_back(Common::swap64(m_data.friend_codes.at(i)));
    }
  }

  return friends;
}

}  // namespace IOS::HLE::NWC24::Mail
