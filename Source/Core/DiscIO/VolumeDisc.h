// Copyright 2020 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <string>

#include "Common/CommonTypes.h"
#include "DiscIO/Volume.h"

namespace DiscIO
{
class VolumeDisc : public Volume
{
public:
  struct BootID
  {
    u32 magic;  // 0x00 (Always BTID)
    uint32_t unknown0[3];
    uint32_t unknown1[4];
    char mediaboard_type[4];  // 0x20 (GCAM for Triforce)
    uint32_t unknown2;
    uint16_t year;       // 0x28
    uint8_t month;       // 0x2A
    uint8_t day;         // 0x2B
    uint8_t video_mode;  // 0x2C unknown bitmask, resolutions + horizontal/vertical
    uint8_t unknown3;
    uint8_t type_3_compatible;  // 0x2E (Type-3 compatible titles have this set to 1)
    uint8_t unknown4;
    char game_id[8];              // 0x30
    uint32_t region_flags;        // 0x38
    uint32_t unknown6[9];
    char manufacturer[0x20];     // 0x60
    char game_name[0x20];        // 0x80
    char game_executable[0x20];  // 0xA0
    char test_executable[0x20];  // 0xC0
    char credit_types[8][0x20];  // 0xE0
  };

  std::string GetGameID(const Partition& partition = PARTITION_NONE) const;
  Country GetCountry(const Partition& partition = PARTITION_NONE) const;
  std::string GetMakerID(const Partition& partition = PARTITION_NONE) const;
  bool GetRevision(const Partition& partition, u16* revision) const;
  u32 GetSimulatedMemorySize() const;
  bool IsNKit() const;

protected:
  explicit VolumeDisc(VolumeKind kind) : Volume(kind) {}
  ~VolumeDisc() = default;

  Region RegionCodeToRegion(u32 region_code) const;
};

static_assert(sizeof(VolumeDisc::BootID) == 480);

}  // namespace DiscIO
