// Copyright 2023 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/IOS/USB/Emulated/Skylanders/SkylanderFigure.h"

#include <array>
#include <cstring>
#include <retro_miscellaneous.h>
#include <string>

#include <crypto/crypto.h>
#include <lrc_hash.h>

#include "Common/BitUtils.h"
#include "Common/CommonTypes.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "Core/IOS/USB/Emulated/Skylanders/Skylander.h"
#include "Core/IOS/USB/Emulated/Skylanders/SkylanderCrypto.h"

using namespace IOS::HLE::USB::SkylanderCrypto;

namespace IOS::HLE::USB
{
SkylanderFigure::SkylanderFigure(const std::string& file_path)
{
  m_sky_file = File::IOFile(file_path, "w+b");
  std::memset(m_data, 0, sizeof(m_data));
}
// Generate a AES key without the block filled in
void SkylanderFigure::GenerateIncompleteHashIn(u8* dest) const
{
  u8 hash_in[0x56] = {};

  // copy first 2 blocks into hash
  GetBlock(0, hash_in);
  GetBlock(1, hash_in + 0x10);

  // Skip 1 byte. Is a block index that needs to be set per block.

  // Byte array of ascii string " Copyright (C) 2010 Activision. All Rights Reserved.". The space at
  // the start of the string is intentional
  static constexpr u8 HASH_CONST[0x35] = {
      0x20, 0x43, 0x6F, 0x70, 0x79, 0x72, 0x69, 0x67, 0x68, 0x74, 0x20, 0x28, 0x43, 0x29,
      0x20, 0x32, 0x30, 0x31, 0x30, 0x20, 0x41, 0x63, 0x74, 0x69, 0x76, 0x69, 0x73, 0x69,
      0x6F, 0x6E, 0x2E, 0x20, 0x41, 0x6C, 0x6C, 0x20, 0x52, 0x69, 0x67, 0x68, 0x74, 0x73,
      0x20, 0x52, 0x65, 0x73, 0x65, 0x72, 0x76, 0x65, 0x64, 0x2E, 0x20};

  memcpy(hash_in + 0x21, HASH_CONST, ARRAY_SIZE(HASH_CONST));

  memcpy(dest, hash_in, 0x56);
}
void SkylanderFigure::Encrypt(const u8* input)
{
  u8 hash_in[0x56] = {};

  GenerateIncompleteHashIn(hash_in);

  u8 encrypted[FIGURE_SIZE] = {};

  u8 current_block[BLOCK_SIZE] = {};

  // Run for every block
  for (u8 i = 0; i < 64; ++i)
  {
    memcpy(current_block, input + (i * BLOCK_SIZE), BLOCK_SIZE);

    // Skip sector trailer and the first 8 blocks
    if (((i + 1) % 4 == 0) || i < 8)
    {
      memcpy(encrypted + (i * BLOCK_SIZE), current_block, BLOCK_SIZE);
      continue;
    }

    // Block index
    hash_in[0x20] = i;

    u8 hash_out[BLOCK_SIZE] = {};

    MD5_CTX md5;
    MD5_Init(&md5);
    MD5_Update(&md5, hash_in, 0x56);
    MD5_Final(hash_out, &md5);

    struct aes_ctx aes;
    aes_init(&aes, hash_out, 16);
    aes_encrypt_block(&aes, current_block, encrypted + (i * BLOCK_SIZE));
  }

  memcpy(m_data, encrypted, FIGURE_SIZE);

  DEBUG_LOG(IOS_USB, "Encrypted skylander data: \n%s", HexDump(encrypted, FIGURE_SIZE).c_str());
}
SkylanderFigure::SkylanderFigure(File::IOFile file)
{
  m_sky_file = std::move(file);
  m_sky_file.Seek(0, File::SeekOrigin::Begin);
  m_sky_file.ReadBytes(m_data, ARRAY_SIZE(m_data));
}
void SkylanderFigure::Save()
{
  m_sky_file.Seek(0, File::SeekOrigin::Begin);
  m_sky_file.WriteBytes(m_data, FIGURE_SIZE);
}

void SkylanderFigure::GetBlock(u8 index, u8* dest) const
{
  memcpy(dest, m_data + (index * BLOCK_SIZE), BLOCK_SIZE);
}

FigureData SkylanderFigure::GetData() const
{
  FigureData figure_data = {.figure_id = Common::BitCastPtr<u16>(m_data + 0x10),
                            .variant_id = Common::BitCastPtr<u16>(m_data + 0x1C)};

  Type type = Type::Item;
  if (const SkyData* const found =
          IOS::HLE::USB::FindSkylander(figure_data.figure_id, figure_data.variant_id))
  {
    type = found->type;
  }

  figure_data.normalized_type = NormalizeSkylanderType(type);
  if (figure_data.normalized_type == Type::Skylander)
  {
    u8 decrypted[FIGURE_SIZE] = {};

    DecryptFigure(decrypted);

    // Area with highest area counter is the newest
    u16 area_offset = ((decrypted[0x89] + 1U) != decrypted[0x249]) ? 0x80 : 0x240;

    figure_data.skylander_data = {
        .money = Common::BitCastPtr<u16>(decrypted + area_offset + 0x3),
        .hero_level = Common::BitCastPtr<u16>(decrypted + area_offset + 0x5A),
        .playtime = Common::BitCastPtr<u32>(decrypted + area_offset + 0x5),
        .last_reset = {.minute = Common::BitCastPtr<u8>(decrypted + area_offset + 0x60),
                       .hour = Common::BitCastPtr<u8>(decrypted + area_offset + 0x61),
                       .day = Common::BitCastPtr<u8>(decrypted + area_offset + 0x62),
                       .month = Common::BitCastPtr<u8>(decrypted + area_offset + 0x63),
                       .year = Common::BitCastPtr<u16>(decrypted + area_offset + 0x64)},
        .last_placed = {.minute = Common::BitCastPtr<u8>(decrypted + area_offset + 0x50),
                        .hour = Common::BitCastPtr<u8>(decrypted + area_offset + 0x51),
                        .day = Common::BitCastPtr<u8>(decrypted + area_offset + 0x52),
                        .month = Common::BitCastPtr<u8>(decrypted + area_offset + 0x53),
                        .year = Common::BitCastPtr<u16>(decrypted + area_offset + 0x54)}};

    ComputeToyCode(Common::BitCastPtr<u64>(decrypted + 0x14), figure_data.skylander_data.toy_code);

    u16 nickname[16]{};

    // First nickname half
    for (size_t i = 0; i < 8; ++i)
    {
      nickname[i] = Common::BitCastPtr<u16>(decrypted + area_offset + 0x20 + (i * 2));
    }

    // Second nickname half
    for (size_t i = 0; i < 8; ++i)
    {
      nickname[i + 8] = Common::BitCastPtr<u16>(decrypted + area_offset + 0x40 + (i * 2));
    }

    std::memcpy(figure_data.skylander_data.nickname, nickname, sizeof(nickname));
  }
  else if (figure_data.normalized_type == Type::Trophy)
  {
    u8 decrypted[FIGURE_SIZE] = {};

    DecryptFigure(decrypted);

    // Area with highest area counter is the newest
    u16 area_offset = ((decrypted[0x89] + 1U) != decrypted[0x249]) ? 0x80 : 0x240;

    figure_data.trophy_data.unlocked_villains = *(decrypted + area_offset + 0x14);
  }

  return figure_data;
}
void SkylanderFigure::SetData(FigureData* figure_data)
{
  u8 decrypted[FIGURE_SIZE] = {};

  DecryptFigure(decrypted);

  if (figure_data->normalized_type == Type::Skylander)
  {
    // Only update area with lowest counter
    u16 area_offset = (decrypted[0x89] != (decrypted[0x249] + 1U)) ? 0x80 : 0x240;
    u16 other_area_offset = (area_offset == 0x80) ? 0x240 : 0x80;

    memcpy(decrypted + area_offset + 0x3, &figure_data->skylander_data.money, 2);
    memcpy(decrypted + area_offset + 0x5A, &figure_data->skylander_data.hero_level, 2);
    memcpy(decrypted + area_offset + 0x5, &figure_data->skylander_data.playtime, 4);

    {
      memcpy(decrypted + area_offset + 0x60, &figure_data->skylander_data.last_reset.minute, 1);
      memcpy(decrypted + area_offset + 0x61, &figure_data->skylander_data.last_reset.hour, 1);
      memcpy(decrypted + area_offset + 0x62, &figure_data->skylander_data.last_reset.day, 1);
      memcpy(decrypted + area_offset + 0x63, &figure_data->skylander_data.last_reset.month, 1);
      memcpy(decrypted + area_offset + 0x64, &figure_data->skylander_data.last_reset.year, 2);
    }

    {
      memcpy(decrypted + area_offset + 0x50, &figure_data->skylander_data.last_placed.minute, 1);
      memcpy(decrypted + area_offset + 0x51, &figure_data->skylander_data.last_placed.hour, 1);
      memcpy(decrypted + area_offset + 0x52, &figure_data->skylander_data.last_placed.day, 1);
      memcpy(decrypted + area_offset + 0x53, &figure_data->skylander_data.last_placed.month, 1);
      memcpy(decrypted + area_offset + 0x54, &figure_data->skylander_data.last_placed.year, 2);
    }

    {
      for (size_t i = 0; i < 8; ++i)
      {
        memcpy(decrypted + area_offset + 0x20 + (i * 2), &figure_data->skylander_data.nickname[i],
               0x2);
      }

      for (size_t i = 0; i < 8; ++i)
      {
        memcpy(decrypted + area_offset + 0x40 + (i * 2),
               &figure_data->skylander_data.nickname[8 + i], 0x2);
      }
    }

    {
      ComputeChecksumType3(decrypted + area_offset + 0x50, decrypted + area_offset + 0xA);
      ComputeChecksumType2(decrypted + area_offset + 0x10, decrypted + area_offset + 0xC);
      decrypted[area_offset + 9] = decrypted[other_area_offset + 9] + 1;
      ComputeChecksumType1(decrypted + area_offset, decrypted + area_offset + 0xE);
    }

    {
      area_offset = (decrypted[0x112] != (decrypted[0x2D2] + 1U)) ? 0x110 : 0x2D0;
      other_area_offset = (area_offset == 0x110) ? 0x2D0 : 0x110;

      decrypted[area_offset + 2] = decrypted[other_area_offset + 2] + 1;

      ComputeChecksumType6(decrypted + area_offset, decrypted + area_offset);
    }
  }
  else if (figure_data->normalized_type == Type::Trophy)
  {
    u16 area_offset = (decrypted[0x89] != (decrypted[0x249] + 1U)) ? 0x80 : 0x240;
    u16 other_area_offset = (area_offset == 0x80) ? 0x240 : 0x80;

    {
      memcpy(decrypted + area_offset + 0x14, &figure_data->trophy_data.unlocked_villains, 1);

      {
        ComputeChecksumType3(decrypted + area_offset + 0x50, decrypted + area_offset + 0xA);
        ComputeChecksumType2(decrypted + area_offset + 0x10, decrypted + area_offset + 0xC);
        decrypted[area_offset + 9] = decrypted[other_area_offset + 9] + 1;
        ComputeChecksumType1(decrypted + area_offset, decrypted + area_offset + 0xE);
      }
    }

    {
      area_offset = (decrypted[0x112] != (decrypted[0x2D2] + 1U)) ? 0x110 : 0x2D0;
      other_area_offset = (area_offset == 0x110) ? 0x2D0 : 0x110;

      decrypted[area_offset + 2] = decrypted[other_area_offset + 2] + 1;

      ComputeChecksumType6(decrypted + area_offset, decrypted + area_offset);
    }
  }

  // Encrypt again
  Encrypt(decrypted);

  Save();
}
void SkylanderFigure::DecryptFigure(u8* dest) const
{
  u8 hash_in[0x56] = {};

  GenerateIncompleteHashIn(hash_in);

  u8 decrypted[FIGURE_SIZE] = {};

  u8 current_block[BLOCK_SIZE] = {};

  // Run for every block
  for (u8 i = 0; i < BLOCK_COUNT; ++i)
  {
    GetBlock(i, current_block);

    // Skip sector trailer and the first 8 blocks
    if (((i + 1) % 4 == 0) || i < 8)
    {
      memcpy(decrypted + (i * BLOCK_SIZE), current_block, BLOCK_SIZE);
      continue;
    }

    // Check if block is all 0
    u16 total = 0;
    for (size_t j = 0; j < BLOCK_SIZE; j++)
    {
      total += current_block[j];
    }
    if (total == 0)
    {
      continue;
    }

    // Block index
    hash_in[0x20] = i;

    u8 hash_out[BLOCK_SIZE] = {};

    MD5_CTX md5;
    MD5_Init(&md5);
    MD5_Update(&md5, hash_in, 0x56);
    MD5_Final(hash_out, &md5);

    struct aes_ctx aes;
    aes_init(&aes, hash_out, 16);
    aes_decrypt_block(&aes, current_block, decrypted + (i * BLOCK_SIZE));
  }

  memcpy(dest, decrypted, FIGURE_SIZE);

  DEBUG_LOG(IOS_USB, "Decrypted skylander data: \n%s", HexDump(decrypted, FIGURE_SIZE).c_str());
}
void SkylanderFigure::Close()
{
  m_sky_file.Close();
}
void SkylanderFigure::SetBlock(u8 block, const u8* buf)
{
  memcpy(m_data + (block * BLOCK_SIZE), buf, BLOCK_SIZE);
}
}  // namespace IOS::HLE::USB
