// Copyright 2018 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/Random.h"

#include <crypto/kdf.h>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"

namespace Common::Random
{
void Generate(void* buffer, std::size_t size)
{
  const int ret = crypto_random_bytes(static_cast<u8*>(buffer), size);
  ASSERT(ret == 0);
}
}  // namespace Common::Random
