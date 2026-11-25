// Copyright 2023 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>

#include "Common/CommonTypes.h"

namespace IOS::HLE::USB::SkylanderCrypto
{
u16 ComputeCRC16(const u8* data, size_t size);
void ComputeChecksumType1(const u8* data_start, u8* output);
void ComputeChecksumType2(const u8* data_start, u8* output);
void ComputeChecksumType3(const u8* data_start, u8* output);
void ComputeChecksumType6(const u8* data_start, u8* output);
// out: 11 bytes
void ComputeToyCode(u64 code, u8* out);
}  // namespace IOS::HLE::USB::SkylanderCrypto
