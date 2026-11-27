// Copyright 2009 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

#include "Common/CommonTypes.h"
#include "Common/SymbolDB.h"

// This has functionality overlapping Debugger_Symbolmap. Should merge that stuff in here later.
class PPCSymbolDB final : public Common::SymbolDB
{
public:
  PPCSymbolDB();
  ~PPCSymbolDB();

  void AddKnownSymbol(u32 startAddr, u32 size, const std::string& name,
                      Common::Symbol::Type type = Common::Symbol::Type::Function);

  const Common::Symbol* GetSymbolFromAddr(u32 addr) const;

  bool LoadMapOnBoot();
  bool LoadMap(std::string filename, bool bad = false);

private:
  static void AddKnownSymbol(u32 startAddr, u32 size, const std::string& name,
                             Common::Symbol::Type type, XFuncMap* functions);
};
