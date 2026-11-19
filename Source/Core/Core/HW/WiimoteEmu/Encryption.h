// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Copyright (C) Hector Martin "marcan" (hector@marcansoft.com)

#pragma once

#include "Common/CommonTypes.h"

namespace WiimoteEmu
{
typedef u8 SBox[256];

class EncryptionKey
{
public:
  void Encrypt(u8* data, u32 addr, u32 len) const;
  void Decrypt(u8* data, u32 addr, u32 len) const;

  typedef u8 RandData[10];
  typedef u8 KeyData[6];

  void GenerateTables(const RandData& rand, const KeyData& key, const SBox& sbox_a,
                      const SBox& sbox_b);

  u8 ft[8] = {};
  u8 sb[8] = {};
};

// The key an extension derives from the 0x40-0x4f data in its register; first-party and
// third-party extensions derive it from different tables.
EncryptionKey GenerateExtensionKey(const u8* key_data, bool third_party);  // 16 bytes

}  // namespace WiimoteEmu
