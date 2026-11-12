// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>

#include <crypto/crypto.h>

#include "Common/CommonTypes.h"

// Dolphin only uses/implements AES-128-CBC.

namespace Common::AES
{
enum class Mode
{
  Decrypt,
  Encrypt,
};

// AES-128-CBC with one key
class Context final
{
public:
  static constexpr size_t KEY_SIZE = 16;
  static constexpr size_t BLOCK_SIZE = 16;

  Context(Mode mode, const u8* key);
  // len must be a multiple of BLOCK_SIZE; a null iv is all zeros. buf_in and buf_out may be the
  // same buffer.
  bool Crypt(const u8* iv, u8* iv_out, const u8* buf_in, u8* buf_out, size_t len) const;
  bool Crypt(const u8* iv, const u8* buf_in, u8* buf_out, size_t len) const
  {
    return Crypt(iv, nullptr, buf_in, buf_out, len);
  }
  bool CryptIvZero(const u8* buf_in, u8* buf_out, size_t len) const
  {
    return Crypt(nullptr, nullptr, buf_in, buf_out, len);
  }

private:
  Mode m_mode;
  struct aes_ctx m_ctx;
};

std::unique_ptr<Context> CreateContextEncrypt(const u8* key);
std::unique_ptr<Context> CreateContextDecrypt(const u8* key);

// OFB decryption for WiiConnect24
void CryptOFB(const u8* key, const u8* iv, u8* iv_out, const u8* buf_in, u8* buf_out, size_t size);

}  // namespace Common::AES
