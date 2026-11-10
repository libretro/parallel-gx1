// Copyright 2023 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/Crypto/HMAC.h"

#include <crypto/kdf.h>

namespace Common::HMAC
{
bool HMACWithSHA1(const u8* key, size_t key_size, const u8* msg, size_t msg_size, u8* out)
{
  hmac_sha1(key, key_size, msg, msg_size, out);
  return true;
}
}  // namespace Common::HMAC
