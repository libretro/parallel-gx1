// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/Crypto/SHA1.h"

#include <cstdio>
#include <memory>

namespace Common::SHA1
{
Digest CalculateDigest(const u8* msg, size_t len)
{
  Context ctx;
  ctx.Update(msg, len);
  return ctx.Finish();
}

std::string DigestToString(const Digest& digest)
{
  char out[sizeof(Digest) * 2 + 1];
  for (size_t i = 0; i < digest.size(); i++)
    snprintf(out + i * 2, 3, "%02X", digest[i]);
  return out;
}

}  // namespace Common::SHA1
