// Copyright 2023 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "mbedtls/md.h"

#include "Common/Crypto/HMAC.h"

namespace Common::HMAC
{
bool HMACWithSHA1(const u8* key, size_t key_size, const u8* msg, size_t msg_size, u8* out)
{
  mbedtls_md_context_t ctx;
  bool ok;
  mbedtls_md_init(&ctx);
  ok = !mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA1), 1) &&
       !mbedtls_md_hmac_starts(&ctx, key, key_size) &&
       !mbedtls_md_hmac_update(&ctx, msg, msg_size) && !mbedtls_md_hmac_finish(&ctx, out);
  mbedtls_md_free(&ctx);
  return ok;
}
}  // namespace Common::HMAC
