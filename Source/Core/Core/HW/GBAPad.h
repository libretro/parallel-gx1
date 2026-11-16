// Copyright 2021 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

class InputConfig;
enum class GBAPadGroup;
struct GCPadStatus;

namespace ControllerEmu
{
class ControlGroup;
}  // namespace ControllerEmu

namespace Pad
{
void InitializeGBA();
void LoadGBAConfig();

InputConfig* GetGBAConfig();

GCPadStatus GetGBAStatus(int pad_num);
void SetGBAReset(int pad_num, bool reset);

}  // namespace Pad
