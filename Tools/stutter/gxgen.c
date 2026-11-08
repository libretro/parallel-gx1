/* gxgen: writes the stutter harness's synthetic corpus, FIFO logs the
 * core's FIFO player replays (.dff).
 *
 *    gxgen <out.dff> [passes]
 *
 * Each frame draws two quads in one combination of the GX state that
 * decides which pipeline a draw needs, copied from the EFB to the XFB:
 * TEV stage count and colour/alpha operations, blending (factors,
 * subtraction, logic ops), depth test and update, culling, alpha test,
 * the vertex format (position and colour component formats) and the
 * primitive (quads, triangles, strips, fans, lines, points). The frames
 * walk every combination of blending, depth, vertex format and primitive,
 * with TEV, culling, alpha test and the copy (filter coefficients, their
 * overflow, gamma) cycling alongside, once per pass; from the second pass
 * on, every pipeline has been met before. Each frame also copies a square
 * of the EFB to a texture, in a format cycling through every colour,
 * intensity and depth copy format with the EFB with and without alpha,
 * and draws it textured over a corner, some read back in another format
 * of the same size, which the texture cache reinterprets. And each frame
 * draws a texture loaded from RAM over another corner: new contents
 * every frame, in a format and size cycling through every texture format
 * (palettes in each palette format, CMPR) at 32, 64 and 128 texels, so
 * each frame decodes one. The frame's number is drawn last,
 * over the top left corner: a row of black and white 16x16 squares, bit 0
 * first, which the harness reads back to tell which frame a picture is.
 *
 * C89, no dependencies: builds with any C compiler on Linux and Windows. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned int u32;
typedef unsigned char u8;

static u32 bp[256];
static u32 cp[256];
static u32 xfmem[4096];
static u32 xfregs[88];

/* the frame being built */
static u8 *buf;
static size_t len, cap;

static void put(const void *p, size_t n)
{
   if (len + n > cap)
   {
      u8 *grown;
      cap = (len + n) * 2;
      grown = (u8 *)realloc(buf, cap);
      if (!grown)
      {
         fprintf(stderr, "gxgen: out of memory\n");
         exit(1);
      }
      buf = grown;
   }
   memcpy(buf + len, p, n);
   len += n;
}

static void put8(u32 v)
{
   u8 b = (u8)v;
   put(&b, 1);
}

static void be16(u32 v)
{
   u8 b[2];
   b[0] = (u8)(v >> 8);
   b[1] = (u8)v;
   put(b, 2);
}

static void be32(u32 v)
{
   u8 b[4];
   b[0] = (u8)(v >> 24);
   b[1] = (u8)(v >> 16);
   b[2] = (u8)(v >> 8);
   b[3] = (u8)v;
   put(b, 4);
}

static u32 f32(float f)
{
   u32 v;
   memcpy(&v, &f, 4);
   return v;
}

static void bpw(u32 reg, u32 v)
{
   put8(0x61);
   be32((reg << 24) | (v & 0xffffff));
}

static void cpw(u32 reg, u32 v)
{
   put8(0x08);
   put8(reg);
   be32(v);
}

static void xfw(u32 reg, const u32 *vals, u32 n)
{
   u32 i;
   put8(0x10);
   be32(((n - 1) << 16) | reg);
   for (i = 0; i < n; i++)
      be32(vals[i]);
}

static void xf1(u32 reg, u32 v)
{
   xfw(reg, &v, 1);
}

/* The dimensions walked, each a list of choices. */

/* TEV: stage count, and what the stages do with the rasterized colour */
#define TEV_CHOICES 6
/* blending: off, factor pairs, subtraction, logic ops */
#define BLEND_CHOICES 7
/* depth: off, tests with and without update */
#define DEPTH_CHOICES 5
/* culling: none, back, front */
#define CULL_CHOICES 3
/* alpha test: always, a compare, two compares combined */
#define ALPHA_CHOICES 3
/* the copy: its filter coefficients (w0..w6, the rows above, the row and
 * the rows below), and its gamma */
#define COPY_CHOICES 5
static const u32 copy_filter[COPY_CHOICES][7] = {
   {0, 0, 21, 22, 21, 0, 0},     /* the row alone */
   {10, 0, 22, 0, 22, 0, 10},    /* every row, summing to 64 */
   {0, 0, 63, 63, 63, 0, 0},     /* the row alone, overflowing */
   {0, 0, 21, 22, 21, 0, 0},     /* the row alone, gamma 1.7 */
   {32, 32, 21, 22, 21, 32, 32}  /* every row, overflowing, gamma 2.2 */
};
static const u32 copy_gamma[COPY_CHOICES] = {0, 0, 0, 1, 2};
/* the copy to a texture: its EFB copy format, intensity, depth, and the
 * texture format that reads it back */
#define TEX_COPY_CHOICES 32
static const u32 tex_copy[TEX_COPY_CHOICES][4] = {
   /* EFB copy format, intensity, depth, texture format */
   {0x0, 0, 0, 0}, {0x8, 0, 0, 1}, {0x2, 0, 0, 2}, {0x3, 0, 0, 3},  /* R4 R8 RA4 RA8 */
   {0x0, 1, 0, 0}, {0x8, 1, 0, 1}, {0x2, 1, 0, 2}, {0x3, 1, 0, 3},  /* I4 I8 IA4 IA8 */
   {0x4, 0, 0, 4}, {0x5, 0, 0, 5}, {0x6, 0, 0, 6}, {0x7, 0, 0, 1},  /* 565 5A3 RGBA8 A8 */
   {0x9, 0, 0, 1}, {0xa, 0, 0, 1}, {0xb, 0, 0, 3}, {0xc, 0, 0, 3},  /* G8 B8 RG8 GB8 */
   {0x6, 0, 1, 6}, {0x8, 0, 1, 1}, {0x3, 0, 1, 3}, {0x9, 0, 1, 1},  /* Z24 Z8H Z16 Z8M */
   {0xa, 0, 1, 1}, {0xb, 0, 1, 3}, {0xc, 0, 1, 3}, {0x0, 0, 1, 0},  /* Z8L Z16R Z16L Z4 */
   /* read back in another format of the same size: reinterpreted */
   {0x8, 0, 0, 2}, {0x2, 0, 0, 1},                                  /* I8 as IA4, IA4 as I8 */
   {0x3, 0, 0, 4}, {0x3, 0, 0, 5}, {0x4, 0, 0, 3}, {0x4, 0, 0, 5},  /* IA8, RGB565 as the */
   {0x5, 0, 0, 3}, {0x5, 0, 0, 4}                                   /* others, RGB5A3 too */
};
/* vertex format: position float or s16, colour rgba8888, rgb565 or rgba4444 */
#define VTX_CHOICES 4
/* primitive */
#define PRIM_CHOICES 6

/* walked in every combination */
static const u32 radix[4] = {BLEND_CHOICES, DEPTH_CHOICES, VTX_CHOICES, PRIM_CHOICES};

static void tev(u32 choice)
{
   /* genmode: one colour channel, stages - 1 in bits 10..13 */
   static const u32 stages[TEV_CHOICES] = {1, 1, 2, 3, 4, 2};
   u32 n = stages[choice], i;
   bpw(0x00, 0x10 | ((n - 1) << 10) | (bp[0x00] & 0xc000));
   for (i = 0; i < n; i++)
   {
      u32 color, alpha;
      if (i == 0)
      {
         /* the first stage: rasterized colour in d, or in a with the rest zero */
         color = choice == 1 ? 0xafff : 0xfffa;
         alpha = 0xffd0;
      }
      else if (choice == 5)
      {
         /* the previous colour scaled by two and back: bias, scale */
         color = 0xfff0 | (1 << 20);
         alpha = 0xff80;
      }
      else
      {
         /* the previous stage's colour passed on */
         color = 0xfff0;
         alpha = 0xff80;
      }
      bpw(0xc0 + i * 2, color);
      bpw(0xc1 + i * 2, alpha);
   }
}

static void blend(u32 choice)
{
   /* bp 0x41: blend enable 0, logic enable 1, colour 3, alpha 4, dst
    * factor 5..7, src factor 8..10, subtract 11, logic mode 12..15 */
   u32 v = 0x18;
   switch (choice)
   {
      case 1: v |= 1 | (1 << 8); break;                  /* one, zero */
      case 2: v |= 1 | (4 << 8) | (5 << 5); break;        /* src alpha, 1 - src alpha */
      case 3: v |= 1 | (1 << 8) | (1 << 5); break;        /* one, one */
      case 4: v |= 1 | (1 << 11) | (1 << 8) | (1 << 5); break; /* subtract */
      case 5: v |= 2 | (3 << 12); break;                  /* logic copy */
      case 6: v |= 2 | (6 << 12); break;                  /* logic xor */
      default: break;
   }
   bpw(0x41, v);
}

static void depth(u32 choice)
{
   /* bp 0x40: test 0, func 1..3, update 4 */
   static const u32 v[DEPTH_CHOICES] = {0, 1 | (7 << 1), 1 | (7 << 1) | 0x10,
      1 | (3 << 1) | 0x10, 1 | (6 << 1)};
   bpw(0x40, v[choice]);
}

static void cull(u32 choice)
{
   /* genmode bits 14..15: none, back, front */
   static const u32 v[CULL_CHOICES] = {0, 2, 1};
   bp[0x00] = (bp[0x00] & ~0xc000u) | (v[choice] << 14);
}

static void alpha(u32 choice)
{
   /* bp 0xf3: ref0 0..7, ref1 8..15, comp0 16..18, comp1 19..21, logic
    * 22..23 (and, or, xor, xnor); 7 = always, 6 = greater or equal, 1 = less */
   static const u32 v[ALPHA_CHOICES] = {(7 << 16) | (7 << 19),
      (6 << 16) | (7 << 19), (6 << 16) | (1 << 19) | (1 << 22) | (0xff << 8)};
   bpw(0xf3, v[choice]);
}

/* the vertex format: cp VAT group 0, and how big a vertex is */
static u32 vtx_size;
static u32 vtx_choice;

static void vtx(u32 choice)
{
   /* position: count 0 (xyz=1), format 1..3 (3 s16, 4 float), frac 4..8;
    * colour 0: count 13 (rgba=1), format 14..16 (0 rgb565, 3 rgba4444,
    * 5 rgba8888); 30 byte dequant */
   static const u32 pos_float = 1 | (4 << 1);
   static const u32 pos_s16 = 1 | (3 << 1) | (8 << 4);
   u32 vat = 1u << 30;
   vtx_choice = choice;
   switch (choice)
   {
      case 0: vat |= pos_float | (1 << 13) | (5 << 14); vtx_size = 12 + 4; break;
      case 1: vat |= pos_s16 | (1 << 13) | (5 << 14); vtx_size = 6 + 4; break;
      case 2: vat |= pos_float | (0 << 13) | (0 << 14); vtx_size = 12 + 2; break;
      default: vat |= pos_s16 | (1 << 13) | (3 << 14); vtx_size = 6 + 2; break;
   }
   cpw(0x70, vat);
}

static void vertex(float x, float y, int white)
{
   if (vtx_choice == 0 || vtx_choice == 2)
   {
      be32(f32(x));
      be32(f32(y));
      be32(f32(0.0f));
   }
   else
   {
      be16((u32)(int)(x * 256.0f) & 0xffff);
      be16((u32)(int)(y * 256.0f) & 0xffff);
      be16(0);
   }
   switch (vtx_choice)
   {
      case 0:
      case 1: be32(white ? 0xffffffffu : 0x000000ffu); break;
      case 2: be16(white ? 0xffff : 0x0000); break;
      default: be16(white ? 0xffff : 0x000f); break;
   }
}

/* Two halves, one white and one black, as the primitive draws them. */
static void prim(u32 choice, int white_left)
{
   static const u32 cmd[PRIM_CHOICES] = {0x80, 0x90, 0x98, 0xa0, 0xa8, 0xb8};
   int half;
   for (half = 0; half < 2; half++)
   {
      float x0 = half ? 0.0f : -1.0f, x1 = half ? 1.0f : 0.0f;
      int white = half ? !white_left : white_left;
      switch (choice)
      {
         case 0: /* quads */
            put8(cmd[0]); be16(4);
            vertex(x0, 1.0f, white); vertex(x1, 1.0f, white);
            vertex(x1, -1.0f, white); vertex(x0, -1.0f, white);
            break;
         case 1: /* triangles */
            put8(cmd[1]); be16(6);
            vertex(x0, 1.0f, white); vertex(x1, 1.0f, white); vertex(x1, -1.0f, white);
            vertex(x0, 1.0f, white); vertex(x1, -1.0f, white); vertex(x0, -1.0f, white);
            break;
         case 2: /* strip */
            put8(cmd[2]); be16(4);
            vertex(x0, 1.0f, white); vertex(x1, 1.0f, white);
            vertex(x0, -1.0f, white); vertex(x1, -1.0f, white);
            break;
         case 3: /* fan */
            put8(cmd[3]); be16(4);
            vertex(x0, 1.0f, white); vertex(x1, 1.0f, white);
            vertex(x1, -1.0f, white); vertex(x0, -1.0f, white);
            break;
         case 4: /* lines: the outline */
            put8(cmd[4]); be16(8);
            vertex(x0, 1.0f, white); vertex(x1, 1.0f, white);
            vertex(x1, 1.0f, white); vertex(x1, -1.0f, white);
            vertex(x1, -1.0f, white); vertex(x0, -1.0f, white);
            vertex(x0, -1.0f, white); vertex(x0, 1.0f, white);
            break;
         default: /* points: the corners */
            put8(cmd[5]); be16(4);
            vertex(x0, 1.0f, white); vertex(x1, 1.0f, white);
            vertex(x1, -1.0f, white); vertex(x0, -1.0f, white);
            break;
      }
   }
}

/* 64x64 texels from the EFB's middle, where the halves meet, copied to a
 * texture in one of the formats, and drawn over the top right corner with
 * TEV taking the texture's colour and alpha alone. */
#define TEX_SIZE 64
#define TEX_ADDR 0x00200000u

static void tex_copy_draw(u32 choice, u32 gamma, u32 pixel_format)
{
   const u32 *c = tex_copy[choice];
   /* bytes per row of blocks, in 32-byte lines: 4 bits per texel (8x8
    * blocks), 8 (8x4), 16 (4x4) or 32 (4x4, two halves) */
   static const u32 tex_stride[7] = {8, 8, 8, 16, 16, 16, 32};
   /* the copy's format field: the real format rotated right by one */
   const u32 tpf = c[0] < 8 ? c[0] * 2 : (c[0] - 8) * 2 + 1;
   static const float quad[4][4] = {
      /* x, y, s, t */
      {0.5f, 1.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f, 0.0f},
      {1.0f, 0.5f, 1.0f, 1.0f}, {0.5f, 0.5f, 0.0f, 1.0f}};
   u32 i;

   bpw(0x49, (320 - TEX_SIZE / 2) | ((240 - TEX_SIZE / 2) << 10));
   bpw(0x4a, (TEX_SIZE - 1) | ((TEX_SIZE - 1) << 10));
   bpw(0x4b, TEX_ADDR >> 5);
   bpw(0x4d, tex_stride[c[3]]);
   if (c[2])
      bpw(0x43, 3); /* the depth buffer, Z24 */
   /* clamp both, the format, gamma, intensity with automatic conversion */
   bpw(0x52, 3 | (tpf << 3) | (gamma << 7) | (c[1] << 15) | (1u << 16));
   if (c[2])
      bpw(0x43, pixel_format);

   /* the texture: clamped, nearest, the copy's size, format and address */
   bpw(0x80, 0);
   bpw(0x84, 0);
   bpw(0x88, (TEX_SIZE - 1) | ((TEX_SIZE - 1) << 10) | (c[3] << 20));
   bpw(0x94, TEX_ADDR >> 5);
   bpw(0x30, TEX_SIZE - 1);
   bpw(0x31, TEX_SIZE - 1);
   /* one stage: texture map 0 with texture coordinate 0, its colour and
    * alpha alone; swap table 0 as RGBA */
   bpw(0x00, 0x11);
   bpw(0x28, (1 << 6) | (0x3f << 12) | (7 << 19));
   bpw(0xc0, 0xfff8);
   bpw(0xc1, 0xffc0);
   bpw(0xf6, 1 << 2);
   bpw(0xf7, 2 | (3 << 2));
   bpw(0x40, 0);
   bpw(0x41, 0x18);
   bpw(0xf3, (7 << 16) | (7 << 19));
   /* one texture coordinate, from the vertex's, through matrix 0 */
   xf1(0x1008, 1 | (1 << 4));
   xf1(0x103f, 1);
   xf1(0x1040, 5 << 7);
   /* VAT 1: position float xyz, colour rgba8888, texture coordinate float st */
   cpw(0x60, 1);
   cpw(0x71, 1 | (4 << 1) | (1 << 13) | (5 << 14) | (1 << 21) | (4 << 22) | (1u << 30));
   put8(0x81);
   be16(4);
   for (i = 0; i < 4; i++)
   {
      be32(f32(quad[i][0]));
      be32(f32(quad[i][1]));
      be32(f32(0.0f));
      be32(0xffffffffu);
      be32(f32(quad[i][2]));
      be32(f32(quad[i][3]));
   }
   cpw(0x60, 0);
   xf1(0x1008, 1);
   xf1(0x103f, 0);
   bpw(0xf6, 0);
   bpw(0xf7, 0);
}

/* The frame's RAM writes, made before its commands: the texture, and
 * its palette. */
/* in one of RAM_SLOTS places, a frame's not overwriting what the frames
 * before it, which the GPU thread may still be reading, wrote */
#define RAM_SLOTS 32
#define RAM_TEX_ADDR(k) (0x00500000u + ((k) % RAM_SLOTS) * 0x10000u)
#define RAM_TLUT_ADDR(k) (0x00800000u + ((k) % RAM_SLOTS) * 0x8000u)
#define RAM_TEX_CHOICES 17
static const u32 ram_tex[RAM_TEX_CHOICES][3] = {
   /* texture format, bits per texel, palette format */
   {0, 4, 0}, {1, 8, 0}, {2, 8, 0}, {3, 16, 0}, {4, 16, 0}, {5, 16, 0}, {6, 32, 0},
   {8, 4, 0}, {8, 4, 1}, {8, 4, 2}, {9, 8, 0}, {9, 8, 1}, {9, 8, 2},
   {10, 16, 0}, {10, 16, 1}, {10, 16, 2}, {14, 4, 0}};
static const u32 ram_tex_sizes[3] = {32, 64, 128};

struct ram_write
{
   u32 address;
   u8 *data;
   u32 size;
};
static struct ram_write *frame_writes[2];
static u32 *frame_nwrites;

static u32 rng_state;
static u32 rng(void)
{
   rng_state = rng_state * 1664525u + 1013904223u;
   return rng_state >> 8;
}

static u8 *random_bytes(u32 n, u32 seed)
{
   u8 *p = (u8 *)malloc(n);
   u32 i;
   if (!p)
   {
      fprintf(stderr, "gxgen: out of memory\n");
      exit(1);
   }
   rng_state = seed * 2654435761u + 1;
   for (i = 0; i < n; i++)
      p[i] = (u8)rng();
   return p;
}

static void ram_texture_draw(u32 k)
{
   const u32 *t = ram_tex[k % RAM_TEX_CHOICES];
   const u32 size = ram_tex_sizes[(k / 5) % 3];
   const u32 bytes = size * size * t[1] / 8;
   /* palette entries: 16, 256 or 16384 of 2 bytes, in 32-byte lines */
   const u32 entries = t[0] == 8 ? 16 : t[0] == 9 ? 256 : t[0] == 10 ? 16384 : 0;
   static const float quad[4][4] = {
      {0.5f, -0.5f, 0.0f, 0.0f}, {1.0f, -0.5f, 1.0f, 0.0f},
      {1.0f, -1.0f, 1.0f, 1.0f}, {0.5f, -1.0f, 0.0f, 1.0f}};
   u32 i, n = 0;

   frame_writes[0][k].address = RAM_TEX_ADDR(k);
   frame_writes[0][k].data = random_bytes(bytes, k);
   frame_writes[0][k].size = bytes;
   n = 1;
   if (entries)
   {
      frame_writes[1][k].address = RAM_TLUT_ADDR(k);
      frame_writes[1][k].data = random_bytes(entries * 2, k + 0x10000);
      frame_writes[1][k].size = entries * 2;
      n = 2;
      /* the palette into TMEM from 0x80000, then map 0 reads it from there */
      bpw(0x64, RAM_TLUT_ADDR(k) >> 5);
      bpw(0x65, (0x80000 >> 9) | ((entries * 2 / 32) << 10));
      bpw(0x98, (0x80000 >> 9) | (t[2] << 10));
   }
   frame_nwrites[k] = n;

   bpw(0x80, 0);
   bpw(0x84, 0);
   bpw(0x88, (size - 1) | ((size - 1) << 10) | (t[0] << 20));
   bpw(0x94, RAM_TEX_ADDR(k) >> 5);
   bpw(0x30, size - 1);
   bpw(0x31, size - 1);
   bpw(0x00, 0x11);
   bpw(0x28, (1 << 6) | (0x3f << 12) | (7 << 19));
   bpw(0xc0, 0xfff8);
   bpw(0xc1, 0xffc0);
   bpw(0xf6, 1 << 2);
   bpw(0xf7, 2 | (3 << 2));
   bpw(0x40, 0);
   bpw(0x41, 0x18);
   bpw(0xf3, (7 << 16) | (7 << 19));
   xf1(0x1008, 1 | (1 << 4));
   xf1(0x103f, 1);
   xf1(0x1040, 5 << 7);
   cpw(0x60, 1);
   cpw(0x71, 1 | (4 << 1) | (1 << 13) | (5 << 14) | (1 << 21) | (4 << 22) | (1u << 30));
   put8(0x81);
   be16(4);
   for (i = 0; i < 4; i++)
   {
      be32(f32(quad[i][0]));
      be32(f32(quad[i][1]));
      be32(f32(0.0f));
      be32(0xffffffffu);
      be32(f32(quad[i][2]));
      be32(f32(quad[i][3]));
   }
   cpw(0x60, 0);
   xf1(0x1008, 1);
   xf1(0x103f, 0);
   bpw(0xf6, 0);
   bpw(0xf7, 0);
}

/* FRAME_BITS squares from the top left, white for a one */
#define FRAME_BITS 12

static void frame_number(u32 k)
{
   u32 i;
   bpw(0x00, 0x10);
   bpw(0x28, bp[0x28]);
   bpw(0xc0, 0xfffa);
   bpw(0xc1, 0xffd0);
   bpw(0x40, 0);
   bpw(0x41, 0x18);
   bpw(0xf3, (7 << 16) | (7 << 19));
   /* VAT 2: position float xyz, colour rgba8888 */
   cpw(0x72, 1 | (4 << 1) | (1 << 13) | (5 << 14) | (1u << 30));
   put8(0x82);
   be16(4 * FRAME_BITS);
   for (i = 0; i < FRAME_BITS; i++)
   {
      const float x0 = (float)(i * 16) / 320.0f - 1.0f, x1 = (float)(i * 16 + 16) / 320.0f - 1.0f;
      const float y0 = 1.0f, y1 = 1.0f - 16.0f / 240.0f;
      const u32 c = (k >> i) & 1 ? 0xffffffffu : 0x000000ffu;
      be32(f32(x0)); be32(f32(y0)); be32(f32(0.0f)); be32(c);
      be32(f32(x1)); be32(f32(y0)); be32(f32(0.0f)); be32(c);
      be32(f32(x1)); be32(f32(y1)); be32(f32(0.0f)); be32(c);
      be32(f32(x0)); be32(f32(y1)); be32(f32(0.0f)); be32(c);
   }
}

static void frame(u32 k, u32 combos)
{
   static const u32 xf_viewport_n = 6, xf_proj_n = 7;
   static const u32 copy_regs[10] = {0x49, 0x4a, 0x4b, 0x4d, 0x4e, 0x4f, 0x50, 0x51, 0x53, 0x54};
   static const u32 state_regs[4] = {0x20, 0x21, 0x59, 0x28};
   u32 digit[4], c = k % combos, i;

   for (i = 0; i < 4; i++)
   {
      digit[i] = c % radix[i];
      c /= radix[i];
   }

   /* the state, all of it, as the player loads it again at the start */
   cpw(0x50, cp[0x50]);
   cpw(0x30, 0);
   xf1(0x1008, 1);
   xf1(0x1009, 1);
   xf1(0x100e, 1);
   xf1(0x1010, 1);
   xf1(0x1018, 0);
   xfw(0x101a, &xfregs[0x1a], xf_viewport_n);
   xfw(0x1020, &xfregs[0x20], xf_proj_n);
   xf1(0x103f, 0);
   xfw(0x0000, xfmem, 12);
   for (i = 0; i < 4; i++)
      bpw(state_regs[i], bp[state_regs[i]]);
   bpw(0x22, 6 | (6 << 8)); /* line width and point size, in sixths */
   /* the EFB without and with alpha */
   bpw(0x43, (k / 3) & 1);
   bpw(0x44, bp[0x44]);

   vtx(digit[2]);
   /* cycling alongside, at strides that keep meeting new partners */
   cull(k % CULL_CHOICES);
   tev(k % TEV_CHOICES); /* writes genmode with the cull bits */
   blend(digit[0]);
   depth(digit[1]);
   alpha((k / 7) % ALPHA_CHOICES);

   prim(digit[3], (k & 1) == 0);
   /* the copy filter, both copies' */
   {
      const u32 *w = copy_filter[(k / 2) % COPY_CHOICES];
      bp[0x53] = w[0] | (w[1] << 6) | (w[2] << 12) | (w[3] << 18);
      bp[0x54] = w[4] | (w[5] << 6) | (w[6] << 12);
   }
   bpw(0x53, bp[0x53]);
   bpw(0x54, bp[0x54]);
   tex_copy_draw((k / 4) % TEX_COPY_CHOICES, copy_gamma[(k / 2) % COPY_CHOICES], (k / 3) & 1);
   ram_texture_draw(k);
   frame_number(k);

   /* EFB to XFB */
   for (i = 0; i < 10; i++)
      bpw(copy_regs[i], bp[copy_regs[i]]);
   /* the gather pipe moves 32 bytes at a time: NOPs before the copy, so
    * the frame ends with it, as the player expects */
   while ((len + 5) % 32)
      put8(0);
   bpw(0x52, (1 << 14) | (1 << 11) | (copy_gamma[(k / 2) % COPY_CHOICES] << 7) | 3);
}

static void le32(FILE *f, u32 v)
{
   u8 b[4];
   b[0] = (u8)v;
   b[1] = (u8)(v >> 8);
   b[2] = (u8)(v >> 16);
   b[3] = (u8)(v >> 24);
   fwrite(b, 1, 4, f);
}

static void le64(FILE *f, unsigned long v)
{
   le32(f, (u32)v);
   le32(f, 0);
}

static void zeros(FILE *f, unsigned long n)
{
   static const u8 z[4096] = {0};
   while (n)
   {
      unsigned long k = n < sizeof(z) ? n : sizeof(z);
      fwrite(z, 1, k, f);
      n -= k;
   }
}

int main(int argc, char **argv)
{
   static const float viewport[6] = {320.0f, -240.0f, 16777215.0f, 342.0f + 320.0f,
      342.0f + 240.0f, 16777215.0f};
   static const float proj[6] = {1.0f, 0.0f, 1.0f, 0.0f, 0.0f, -0.5f};
   const unsigned long hdr = 128, frame_info = 8 + 4 + 4 + 4 + 8 + 4 + 32;
   unsigned long off, bp_off, cp_off, xfm_off, xfr_off, tex_off, fl_off;
   u32 passes = argc > 2 ? (u32)atoi(argv[2]) : 2;
   u32 combos = 1, nframes, k, i;
   unsigned long *data_off, *mu_off;
   size_t *data_len;
   u8 **data;
   FILE *f;

   if (argc < 2 || !passes)
   {
      fprintf(stderr, "usage: gxgen <out.dff> [passes]\n");
      return 2;
   }
   for (i = 0; i < 4; i++)
      combos *= radix[i];
   nframes = combos * passes;

   /* position matrix 0: identity, 3 rows of 4 */
   for (i = 0; i < 3; i++)
      xfmem[i * 4 + i] = f32(1.0f);
   xfregs[0x08] = 1;
   xfregs[0x09] = 1;
   xfregs[0x0e] = 1;
   xfregs[0x10] = 1;
   for (i = 0; i < 6; i++)
      xfregs[0x1a + i] = f32(viewport[i]);
   for (i = 0; i < 6; i++)
      xfregs[0x20 + i] = f32(proj[i]);
   xfregs[0x26] = 1; /* orthographic */

   cp[0x50] = (1 << 9) | (1 << 13); /* position and colour 0 direct */
   cp[0x70] = 1 | (4 << 1) | (1 << 13) | (5 << 14) | (1u << 30);

   bp[0x00] = 0x10;
   bp[0x20] = 342 | (342 << 12);
   bp[0x21] = (342 + 479) | ((342 + 639) << 12);
   bp[0x59] = 171 | (171 << 10);
   bp[0x28] = 0x3f | (0x3f << 12) | (7 << 19);
   bp[0xc0] = 0xfffa;
   bp[0xc1] = 0xffd0;
   bp[0xf3] = 0x3f0000;
   bp[0x41] = 0x18;
   bp[0x44] = 3;
   bp[0x4a] = 639 | (479 << 10);
   bp[0x4b] = 0x00100000 >> 5;
   bp[0x4d] = 640 * 2 / 32;
   bp[0x4e] = 0x100;
   bp[0x4f] = 0xff80;
   bp[0x50] = 0x8080;
   /* copy filter: coefficients 0 0 21 22 21 0 0 (sum 64); all zero would
    * scale every copy to black */
   bp[0x53] = 0x595000;
   bp[0x54] = 0x000015;

   frame_writes[0] = (struct ram_write *)calloc(nframes, sizeof(struct ram_write));
   frame_writes[1] = (struct ram_write *)calloc(nframes, sizeof(struct ram_write));
   frame_nwrites = (u32 *)calloc(nframes, sizeof(u32));
   mu_off = (unsigned long *)calloc(nframes, sizeof(*mu_off));
   if (!frame_writes[0] || !frame_writes[1] || !frame_nwrites || !mu_off)
      return 1;
   data = (u8 **)calloc(nframes, sizeof(*data));
   data_len = (size_t *)calloc(nframes, sizeof(*data_len));
   data_off = (unsigned long *)calloc(nframes, sizeof(*data_off));
   if (!data || !data_len || !data_off)
      return 1;
   for (k = 0; k < nframes; k++)
   {
      buf = NULL;
      len = cap = 0;
      frame(k, combos);
      data[k] = buf;
      data_len[k] = len;
   }

   off = hdr;
   bp_off = off; off += 256 * 4;
   cp_off = off; off += 256 * 4;
   xfm_off = off; off += 4096 * 4;
   xfr_off = off; off += 88 * 4;
   tex_off = off; off += 1024 * 1024;
   fl_off = off; off += frame_info * nframes;
   for (k = 0; k < nframes; k++)
   {
      data_off[k] = off;
      off += (unsigned long)data_len[k];
   }
   /* each frame's RAM writes (24 bytes each), then what they write */
   for (k = 0; k < nframes; k++)
   {
      mu_off[k] = off;
      off += 24ul * frame_nwrites[k];
   }

   f = fopen(argv[1], "wb");
   if (!f)
   {
      fprintf(stderr, "gxgen: cannot write %s\n", argv[1]);
      return 1;
   }
   /* header */
   le32(f, 0x0d01f1f0);
   le32(f, 5);
   le32(f, 1);
   le64(f, bp_off); le32(f, 256);
   le64(f, cp_off); le32(f, 256);
   le64(f, xfm_off); le32(f, 4096);
   le64(f, xfr_off); le32(f, 88);
   le64(f, fl_off); le32(f, nframes); le32(f, 0);
   le64(f, tex_off); le32(f, 1024 * 1024);
   le32(f, 0x01800000);
   le32(f, 0x04000000);
   zeros(f, 8 + 24);
   for (i = 0; i < 256; i++)
      le32(f, bp[i]);
   for (i = 0; i < 256; i++)
      le32(f, cp[i]);
   for (i = 0; i < 4096; i++)
      le32(f, xfmem[i]);
   for (i = 0; i < 88; i++)
      le32(f, xfregs[i]);
   zeros(f, 1024 * 1024);
   for (k = 0; k < nframes; k++)
   {
      le64(f, data_off[k]);
      le32(f, (u32)data_len[k]);
      le32(f, 0x00300000);
      le32(f, 0x003fffe0);
      le64(f, mu_off[k]);
      le32(f, frame_nwrites[k]);
      zeros(f, 32);
   }
   for (k = 0; k < nframes; k++)
   {
      fwrite(data[k], 1, data_len[k], f);
      free(data[k]);
   }
   for (k = 0; k < nframes; k++)
   {
      for (i = 0; i < frame_nwrites[k]; i++)
      {
         const struct ram_write *w = &frame_writes[i][k];
         le32(f, 0); /* before the frame's first command */
         le32(f, w->address);
         le64(f, off);
         le32(f, w->size);
         fputc(1, f); /* a texture */
         zeros(f, 3);
         off += w->size;
      }
   }
   for (k = 0; k < nframes; k++)
   {
      for (i = 0; i < frame_nwrites[k]; i++)
      {
         fwrite(frame_writes[i][k].data, 1, frame_writes[i][k].size, f);
         free(frame_writes[i][k].data);
      }
   }
   fclose(f);
   free(data);
   free(data_len);
   free(data_off);
   free(mu_off);
   free(frame_writes[0]);
   free(frame_writes[1]);
   free(frame_nwrites);
   printf("%s: %u combinations, %u frames\n", argv[1], combos, nframes);
   return 0;
}
