// Copyright 2021 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <winerror.h>

#include "Common/CommonTypes.h"

namespace Common
{
/* An HRESULT as text, "message (0x........)", returned whole: pass
 * HRText(hr).text to a printf-style log or alert. */
struct HResultText
{
  char text[384];
};
HResultText HRText(HRESULT hr);
}  // namespace Common
