// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

#include "Common/CommonTypes.h"
#include "Core/Boot/Boot.h"

class DolReader final : public BootExecutableReader
{
public:
  explicit DolReader(const std::string& filename);
  DolReader(u8* data, size_t size, bool take_ownership);
  ~DolReader() override;

  bool IsValid() const override { return m_is_valid; }
  bool IsWii() const override { return m_is_wii; }
  bool IsAncast() const { return m_ancast_index >= 0; }
  u32 GetEntryPoint() const override { return m_dolheader.m_entrypoint; }
  bool LoadIntoMemory(Core::System& system, bool only_in_mem1 = false) const override;
  bool LoadSymbols(const Core::CPUThreadGuard& guard, PPCSymbolDB& ppc_symbol_db,
                   const std::string& filename) const override
  {
    return false;
  }

private:
  enum
  {
    DOL_NUM_TEXT = 7,
    DOL_NUM_DATA = 11
  };

  struct DolHeader
  {
    u32 m_text_offset[DOL_NUM_TEXT];
    u32 m_data_offset[DOL_NUM_DATA];

    u32 m_text_address[DOL_NUM_TEXT];
    u32 m_data_address[DOL_NUM_DATA];

    u32 m_text_size[DOL_NUM_TEXT];
    u32 m_data_size[DOL_NUM_DATA];

    u32 m_bss_address;
    u32 m_bss_size;
    u32 m_entrypoint;
  };
  DolHeader m_dolheader;

  struct LoadableSection
  {
    const u8* m_data;
    u32 m_size;
    u32 m_address;
    u32 m_header_section_size;
  };

  LoadableSection m_sections[DOL_NUM_TEXT + DOL_NUM_DATA];
  int m_num_sections = 0;
  int m_ancast_index = -1;

  bool m_is_valid;
  bool m_is_wii;

  bool Initialize();

  bool LoadAncastIntoMemory(Core::System& system) const;
};
