// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/Crypto/AES.h"

#include <cstring>
#include <memory>

#include "Common/Assert.h"

namespace Common::AES
{
// libretro-common's AES runs on AES-NI or the ARMv8 AES instructions where the CPU has them, and
// decrypts CBC several blocks at a time there.
Context::Context(Mode mode, const u8* key) : m_mode(mode)
{
  ASSERT(!aes_init(&m_ctx, key, KEY_SIZE));
}

bool Context::Crypt(const u8* iv, u8* iv_out, const u8* buf_in, u8* buf_out, size_t len) const
{
  u8 iv_tmp[BLOCK_SIZE] = {};
  if (len % BLOCK_SIZE)
    return false;
  if (iv)
    std::memcpy(iv_tmp, iv, BLOCK_SIZE);

  if (m_mode == Mode::Encrypt)
    aes_cbc_encrypt(&m_ctx, iv_tmp, buf_in, buf_out, len);
  else
    aes_cbc_decrypt(&m_ctx, iv_tmp, buf_in, buf_out, len);

  if (iv_out)
    std::memcpy(iv_out, iv_tmp, BLOCK_SIZE);
  return true;
}

std::unique_ptr<Context> CreateContextEncrypt(const u8* key)
{
  return std::make_unique<Context>(Mode::Encrypt, key);
}

std::unique_ptr<Context> CreateContextDecrypt(const u8* key)
{
  return std::make_unique<Context>(Mode::Decrypt, key);
}

// OFB encryption and decryption are the exact same. We don't encrypt though.
void CryptOFB(const u8* key, const u8* iv, u8* iv_out, const u8* buf_in, u8* buf_out, size_t size)
{
  struct aes_ctx aes_ctx;
  size_t iv_offset = 0;

  u8 iv_tmp[16] = {};
  if (iv)
    std::memcpy(iv_tmp, iv, 16);

  ASSERT(!aes_init(&aes_ctx, key, 16));
  for (size_t i = 0; i < size; i++)
  {
    if (!iv_offset)
      aes_encrypt_block(&aes_ctx, iv_tmp, iv_tmp);
    buf_out[i] = buf_in[i] ^ iv_tmp[iv_offset];
    iv_offset = (iv_offset + 1) & 15;
  }

  if (iv_out)
    std::memcpy(iv_out, iv_tmp, 16);
}

}  // namespace Common::AES
