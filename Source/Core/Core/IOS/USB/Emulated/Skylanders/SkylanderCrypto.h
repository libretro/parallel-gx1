// Copyright 2023 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstddef>

#include "Common/CommonTypes.h"

namespace IOS::HLE::USB::SkylanderCrypto
{
u16 ComputeCRC16(const u8* data, size_t size);
u64 ComputeCRC48(const u8* data, size_t size);
// nuid: the figure's first 4 octets
u64 CalculateKeyA(u8 sector, const u8* nuid);
void ComputeChecksumType0(const u8* data_start, u8* output);
void ComputeChecksumType1(const u8* data_start, u8* output);
void ComputeChecksumType2(const u8* data_start, u8* output);
void ComputeChecksumType3(const u8* data_start, u8* output);
void ComputeChecksumType6(const u8* data_start, u8* output);
std::array<u8, 11> ComputeToyCode(u64 code);
}  // namespace IOS::HLE::USB::SkylanderCrypto
