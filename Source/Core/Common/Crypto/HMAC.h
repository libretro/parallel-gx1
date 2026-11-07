// Copyright 2023 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/CommonTypes.h"

namespace Common::HMAC
{
// HMAC with the SHA1 message digest. Excepted output length is 20 bytes.
bool HMACWithSHA1(const u8* key, size_t key_size, const u8* msg, size_t msg_size, u8* out);
}  // namespace Common::HMAC
