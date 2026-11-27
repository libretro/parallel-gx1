// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once


#include "InputCommon/ControlReference/ControlReference.h"

namespace ControllerEmu
{
enum class Translatability
{
  DoNotTranslate,
  Translate
};

class Control
{
public:
  virtual ~Control();

  template <typename T = ControlState>
  T GetState() const
  {
    return control_ref->GetState<T>();
  }

private:
  // Kept with the control, before the pointer to it
  ControlReference m_ref;

public:
  ControlReference* const control_ref;
  const Translatability translate;
  // Literals the control lends, as every control's names are
  const char* const name;
  const char* const ui_name;

protected:
  Control(ControlReference::Kind kind, Translatability translate, const char* name,
          const char* ui_name);
  Control(ControlReference::Kind kind, Translatability translate, const char* name);
};

}  // namespace ControllerEmu
