// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Copyright (C) Hector Martin "marcan" (hector@marcansoft.com)

#pragma once

#include <array>

#include "Common/CommonTypes.h"

namespace WiimoteEmu
{
using SBox = std::array<u8, 256>;

class EncryptionKey
{
public:
  void Encrypt(u8* data, u32 addr, u32 len) const;
  void Decrypt(u8* data, u32 addr, u32 len) const;

  using RandData = std::array<u8, 10>;
  using KeyData = std::array<u8, 6>;

  void GenerateTables(const RandData& rand, const KeyData& key, const SBox& sbox_a,
                      const SBox& sbox_b);

  std::array<u8, 8> ft = {};
  std::array<u8, 8> sb = {};
};

// The key an extension derives from the 0x40-0x4f data in its register; first-party and
// third-party extensions derive it from different tables.
EncryptionKey GenerateExtensionKey(const std::array<u8, 16>& key_data, bool third_party);

}  // namespace WiimoteEmu
