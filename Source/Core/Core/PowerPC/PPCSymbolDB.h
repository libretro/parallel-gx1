// Copyright 2009 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/CommonTypes.h"
#include "Common/SymbolDB.h"

class PPCSymbolDB final : public Common::SymbolDB
{
public:
  PPCSymbolDB();
  ~PPCSymbolDB();

  void AddKnownSymbol(u32 startAddr, u32 size, const char* name,
                      Common::Symbol::Type type = Common::Symbol::Type::Function);

  const Common::Symbol* GetSymbolFromAddr(u32 addr) const;

  bool LoadMapOnBoot();
  bool LoadMap(const char* filename, bool bad = false);

private:
  static void AddKnownSymbol(u32 startAddr, u32 size, const char* name, Common::Symbol::Type type,
                             Common::SymbolList* functions);
};
