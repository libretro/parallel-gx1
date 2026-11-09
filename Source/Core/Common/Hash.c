/* Copyright 2008 Dolphin Emulator Project
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <string.h>

#include <zlib.h>

#include "Common/Hash.h"

#if defined(_M_X86_64)
#if defined(_MSC_VER)
#include <intrin.h>
#define HASH_TARGET_SSE42
#else
#include <nmmintrin.h>
#if defined(__SSE4_2__)
#define HASH_TARGET_SSE42
#else
#define HASH_TARGET_SSE42 __attribute__((target("sse4.2")))
#endif
#endif
#elif defined(_M_ARM_64)
#ifdef _MSC_VER
#include <intrin.h>
#else
#include <arm_acle.h>
#endif
#endif

uint32_t common_hash_adler32(const uint8_t *data, size_t len)
{
   /* zlib-ng's */
   return (uint32_t)adler32_z(1, data, len);
}

/* Stupid hash - but can't go back now :) Reasonably fast. */
uint32_t common_hash_ector(const uint8_t *data, size_t len)
{
   uint32_t crc = 0;
   size_t i;
   for (i = 0; i < len; i++)
   {
      crc ^= data[i];
      crc = (crc << 3) | (crc >> 29);
   }
   return crc;
}

#define ROTL64(x, r) (((x) << (r)) | ((x) >> (64 - (r))))
#define ROTL32(x, r) (((x) << (r)) | ((x) >> (32 - (r))))

/* the step between the 8-byte words hashed, for about samples of the len / 8 */
static uint32_t hash_step(uint32_t words, uint32_t samples)
{
   uint32_t step;
   if (samples == 0)
      samples = words ? words : 1u;
   step = words / samples;
   return step < 1 ? 1 : step;
}

#ifdef _ARCH_64

/* Block mix: combine the key bits with the hash bits and scramble everything */
#define BMIX64(h1, h2, k1, k2, c1, c2) \
   do \
   { \
      k1 *= c1; \
      k1 = ROTL64(k1, 23); \
      k1 *= c2; \
      h1 ^= k1; \
      h1 += h2; \
      h2 = ROTL64(h2, 41); \
      k2 *= c2; \
      k2 = ROTL64(k2, 23); \
      k2 *= c1; \
      h2 ^= k2; \
      h2 += h1; \
      h1 = h1 * 3 + 0x52dce729; \
      h2 = h2 * 3 + 0x38495ab5; \
      c1 = c1 * 5 + 0x7b7d159c; \
      c2 = c2 * 5 + 0x6bce6396; \
   } while (0)

/* Finalization mix: avalanches all bits to within 0.05% bias */
static uint64_t fmix64(uint64_t k)
{
   k ^= k >> 33;
   k *= 0xff51afd7ed558ccdull;
   k ^= k >> 33;
   k *= 0xc4ceb9fe1a85ec53ull;
   k ^= k >> 33;
   return k;
}

static uint64_t murmur_hash3(const uint8_t *src, uint32_t len, uint32_t samples)
{
   const int nblocks = (int)(len / 16);
   const uint32_t step = hash_step(len / 8, samples);
   const uint64_t *blocks = (const uint64_t *)src;
   const uint8_t *tail = src + nblocks * 16;
   uint64_t h1 = 0x9368e53c2f6af274ull;
   uint64_t h2 = 0x586dcd208f7cd3fdull;
   uint64_t c1 = 0x87c37b91114253d5ull;
   uint64_t c2 = 0x4cf5ad432745937full;
   uint64_t k1, k2;
   int i;

   for (i = 0; i < nblocks; i += (int)step)
   {
      k1 = blocks[i * 2 + 0];
      k2 = blocks[i * 2 + 1];
      BMIX64(h1, h2, k1, k2, c1, c2);
   }

   k1 = 0;
   k2 = 0;
   switch (len & 15)
   {
   case 15: k2 ^= (uint64_t)tail[14] << 48; /* fall through */
   case 14: k2 ^= (uint64_t)tail[13] << 40; /* fall through */
   case 13: k2 ^= (uint64_t)tail[12] << 32; /* fall through */
   case 12: k2 ^= (uint64_t)tail[11] << 24; /* fall through */
   case 11: k2 ^= (uint64_t)tail[10] << 16; /* fall through */
   case 10: k2 ^= (uint64_t)tail[9] << 8; /* fall through */
   case 9: k2 ^= (uint64_t)tail[8]; /* fall through */
   case 8: k1 ^= (uint64_t)tail[7] << 56; /* fall through */
   case 7: k1 ^= (uint64_t)tail[6] << 48; /* fall through */
   case 6: k1 ^= (uint64_t)tail[5] << 40; /* fall through */
   case 5: k1 ^= (uint64_t)tail[4] << 32; /* fall through */
   case 4: k1 ^= (uint64_t)tail[3] << 24; /* fall through */
   case 3: k1 ^= (uint64_t)tail[2] << 16; /* fall through */
   case 2: k1 ^= (uint64_t)tail[1] << 8; /* fall through */
   case 1:
      k1 ^= (uint64_t)tail[0];
      BMIX64(h1, h2, k1, k2, c1, c2);
      break;
   default:
      break;
   }

   h2 ^= len;
   h1 += h2;
   h2 += h1;
   h1 = fmix64(h1);
   h2 = fmix64(h2);
   h1 += h2;
   return h1;
}

#else

/* Finalization mix: avalanches all bits to within 0.25% bias */
static uint32_t fmix32(uint32_t h)
{
   h ^= h >> 16;
   h *= 0x85ebca6bu;
   h ^= h >> 13;
   h *= 0xc2b2ae35u;
   h ^= h >> 16;
   return h;
}

#define BMIX32(h1, h2, k1, k2, c1, c2) \
   do \
   { \
      k1 *= c1; \
      k1 = ROTL32(k1, 11); \
      k1 *= c2; \
      h1 ^= k1; \
      h1 += h2; \
      h2 = ROTL32(h2, 17); \
      k2 *= c2; \
      k2 = ROTL32(k2, 11); \
      k2 *= c1; \
      h2 ^= k2; \
      h2 += h1; \
      h1 = h1 * 3 + 0x52dce729u; \
      h2 = h2 * 3 + 0x38495ab5u; \
      c1 = c1 * 5 + 0x7b7d159cu; \
      c2 = c2 * 5 + 0x6bce6396u; \
   } while (0)

static uint64_t murmur_hash3(const uint8_t *src, uint32_t len, uint32_t samples)
{
   const int nblocks = (int)(len / 8);
   const uint32_t step = hash_step(len / 4, samples);
   const uint32_t *blocks = (const uint32_t *)(src + nblocks * 8);
   const uint8_t *tail = src + nblocks * 8;
   uint32_t h1 = 0x8de1c3acu;
   uint32_t h2 = 0xbab98226u;
   uint32_t c1 = 0x95543787u;
   uint32_t c2 = 0x2ad7eb25u;
   uint32_t k1, k2;
   uint32_t out[2];
   uint64_t result;
   int i;

   for (i = -nblocks; i < 0; i += (int)step)
   {
      k1 = blocks[i * 2 + 0];
      k2 = blocks[i * 2 + 1];
      BMIX32(h1, h2, k1, k2, c1, c2);
   }

   k1 = 0;
   k2 = 0;
   switch (len & 7)
   {
   case 7: k2 ^= (uint32_t)tail[6] << 16; /* fall through */
   case 6: k2 ^= (uint32_t)tail[5] << 8; /* fall through */
   case 5: k2 ^= (uint32_t)tail[4]; /* fall through */
   case 4: k1 ^= (uint32_t)tail[3] << 24; /* fall through */
   case 3: k1 ^= (uint32_t)tail[2] << 16; /* fall through */
   case 2: k1 ^= (uint32_t)tail[1] << 8; /* fall through */
   case 1:
      k1 ^= (uint32_t)tail[0];
      BMIX32(h1, h2, k1, k2, c1, c2);
      break;
   default:
      break;
   }

   h2 ^= len;
   h1 += h2;
   h2 += h1;
   h1 = fmix32(h1);
   h2 = fmix32(h2);
   h1 += h2;
   h2 += h1;
   out[0] = h1;
   out[1] = h2;
   memcpy(&result, out, sizeof(result));
   return result;
}

#endif

#if defined(_M_X86_64) || defined(_M_ARM_64)

#if defined(_M_X86_64)
#define CRC64(h, v) _mm_crc32_u64((h), (v))
#else
#define CRC64(h, v) __crc32d((uint32_t)(h), (v))
#endif

/* four chains, so the CRC unit is never waiting on one */
#if defined(_M_X86_64)
HASH_TARGET_SSE42
#endif
static uint64_t crc_hash(const uint8_t *src, uint32_t len, uint32_t samples)
{
   uint64_t h0 = len, h1 = 0, h2 = 0, h3 = 0;
   const uint64_t *data = (const uint64_t *)src;
   const uint64_t *end = data + len / 8;
   const uint32_t step = hash_step(len / 8, samples);

   while (data < end - step * 3)
   {
      h0 = CRC64(h0, data[step * 0]);
      h1 = CRC64(h1, data[step * 1]);
      h2 = CRC64(h2, data[step * 2]);
      h3 = CRC64(h3, data[step * 3]);
      data += step * 4;
   }
   if (data < end - step * 0)
      h0 = CRC64(h0, data[step * 0]);
   if (data < end - step * 1)
      h1 = CRC64(h1, data[step * 1]);
   if (data < end - step * 2)
      h2 = CRC64(h2, data[step * 2]);

   if (len & 7)
   {
      uint64_t temp = 0;
      memcpy(&temp, end, len & 7);
      h0 = CRC64(h0, temp);
   }

   /* FIXME: is there a better way to combine these partial hashes? */
   return h0 + (h1 << 10) + (h2 << 21) + (h3 << 32);
}
#endif

/* 1 for CRC32, -1 for MurmurHash3, 0 until the first hash asks CPUDetect */
static int s_hash_crc;

uint64_t common_hash64(const uint8_t *src, uint32_t len, uint32_t samples)
{
   if (s_hash_crc == 0)
      s_hash_crc = common_cpu_has_crc32() ? 1 : -1;
#if defined(_M_X86_64) || defined(_M_ARM_64)
   if (s_hash_crc > 0)
      return crc_hash(src, len, samples);
#endif
   return murmur_hash3(src, len, samples);
}

uint32_t common_crc32_start(void)
{
   return (uint32_t)crc32_z(0L, NULL, 0);
}

uint32_t common_crc32_update(uint32_t crc, const uint8_t *data, size_t len)
{
   return (uint32_t)crc32_z(crc, data, len);
}

uint32_t common_crc32_compute(const uint8_t *data, size_t len)
{
   return common_crc32_update(common_crc32_start(), data, len);
}
