// Copyright 2010 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/Crypto/ec.h"

#include <cstring>

#include <crypto/pk.h>

// The Wii's curve is sect233r1 (NIST B-233), which libretro-common implements. Its points are
// 0x04 || X || Y; the Wii's are X || Y.
namespace Common::ec
{
static constexpr size_t HASH_SIZE = 20;

Signature Sign(const u8* key, const u8* hash)
{
  Signature signature{};
  if (b233_ecdsa_sign(key, hash, HASH_SIZE, signature.data(), signature.data() + 30) != 0)
    signature.fill(0);
  return signature;
}

bool VerifySignature(const u8* public_key, const u8* signature, const u8* hash)
{
  u8 point[B233_POINT_SIZE];
  point[0] = 0x04;
  std::memcpy(point + 1, public_key, 60);
  return b233_ecdsa_verify(point, hash, HASH_SIZE, signature, signature + 30) == 0;
}

SharedSecret ComputeSharedSecret(const u8* private_key, const u8* public_key)
{
  SharedSecret shared_secret{};
  u8 point[B233_POINT_SIZE];
  point[0] = 0x04;
  std::memcpy(point + 1, public_key, 60);
  if (b233_ecdh(private_key, point, shared_secret.data()) != 0)
    shared_secret.fill(0);
  return shared_secret;
}

PublicKey PrivToPub(const u8* key)
{
  PublicKey result{};
  u8 point[B233_POINT_SIZE];
  if (b233_keygen(key, point) == 0)
    std::memcpy(result.data(), point + 1, result.size());
  return result;
}
}  // namespace Common::ec
