/* Copyright 2008 Dolphin Emulator Project
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef COMMON_HASH_H
#define COMMON_HASH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint32_t common_hash_adler32(const uint8_t *data, size_t len);
/* JUNK. DO NOT USE FOR NEW THINGS */
uint32_t common_hash_ector(const uint8_t *data, size_t len);

/* The texture cache's hash: CRC32 where the CPU has it, else MurmurHash3; of
 * every 8 bytes, or of about samples of them spread over len (0: all). */
uint64_t common_hash64(const uint8_t *src, uint32_t len, uint32_t samples);

uint32_t common_crc32_start(void);
uint32_t common_crc32_update(uint32_t crc, const uint8_t *data, size_t len);
uint32_t common_crc32_compute(const uint8_t *data, size_t len);

/* CPUDetect's answer, for the hash */
int common_cpu_has_crc32(void);

#ifdef __cplusplus
}
#endif

#endif
