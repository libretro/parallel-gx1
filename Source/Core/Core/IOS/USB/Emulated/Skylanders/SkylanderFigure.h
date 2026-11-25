// Copyright 2023 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <string>

#include "Common/CommonTypes.h"
#include "Common/IOFile.h"

namespace IOS::HLE::USB
{
enum class Type : u8;

struct SkylanderDateTime final
{
  u8 minute;
  u8 hour;
  u8 day;
  u8 month;
  u16 year;
};

struct SkylanderData final
{
  u8 toy_code[11];
  u16 money;
  u16 hero_level;
  u32 playtime;
  // Null-terminated UTF-16 string
  u16 nickname[0x10];
  SkylanderDateTime last_reset;
  SkylanderDateTime last_placed;
};

struct Trophydata final
{
  u8 unlocked_villains;
};

struct FigureData final
{
  Type normalized_type;
  u16 figure_id;
  u16 variant_id;
  union
  {
    SkylanderData skylander_data;
    Trophydata trophy_data;
  };
};

constexpr u32 BLOCK_COUNT = 0x40;
constexpr u32 BLOCK_SIZE = 0x10;
constexpr u32 FIGURE_SIZE = BLOCK_COUNT * BLOCK_SIZE;

class SkylanderFigure
{
public:
  SkylanderFigure(const std::string& file_path);
  SkylanderFigure(File::IOFile file);
  void Save();
  void Close();
  void GetBlock(u8 index, u8* dest) const;
  void SetBlock(u8 block, const u8* buf);
  // dest: FIGURE_SIZE bytes
  void DecryptFigure(u8* dest) const;
  FigureData GetData() const;
  void SetData(FigureData* data);

private:
  void GenerateIncompleteHashIn(u8* dest) const;
  // input: FIGURE_SIZE octets
  void Encrypt(const u8* input);

  File::IOFile m_sky_file;
  u8 m_data[FIGURE_SIZE];
};
}  // namespace IOS::HLE::USB
