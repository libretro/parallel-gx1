/* xfbsplit: writes a GameCube program (.dol) that shows a picture it
 * copied to the XFB in two halves, the way a game whose picture is larger
 * than the EFB does: the video interface shows the whole XFB, which no
 * single XFB copy matches. The top half is red and the bottom half blue.
 *
 *    xfbsplit <out.dol>
 *
 * With XFB copies kept only in VRAM, the XFB in RAM holds nothing but the
 * uninitialized marker under both halves, and the picture has to be built
 * from the copies; a renderer that takes the marker for no picture at all
 * shows black. xfbsplit.sh runs it and checks both colours.
 *
 * The program sets up the CPU-to-GPU FIFO and the video interface itself
 * (NTSC 480i, the XFB at 0x00100000 with both fields interleaved), sends
 * the GPU its commands through the write-gather pipe and loops:
 *   - clear the EFB to red, by copying it anywhere with a red clear
 *   - clear its bottom half to blue the same way
 *   - copy the top half (red) to the top of the XFB
 *   - copy the bottom half (blue) under it
 *
 * C89, no dependencies: builds with any C compiler on Linux and Windows. */
#include <stdio.h>
#include <string.h>

typedef unsigned int u32;
typedef unsigned char u8;

#define LOAD_ADDRESS 0x80003100u
#define FIFO_BASE 0x00400000u
#define FIFO_END (FIFO_BASE + 0xffe0u)
#define XFB_ADDRESS 0x00100000u
#define SCRATCH_ADDRESS 0x00200000u
#define XFB_STRIDE (640u * 2u)

static u32 code[2048];
static u32 ncode;

static void emit(u32 insn)
{
   if (ncode == sizeof(code) / sizeof(code[0]))
   {
      fprintf(stderr, "xfbsplit: program too long\n");
      ncode = 0;
   }
   code[ncode++] = insn;
}

/* addis rd,0,hi; ori rd,rd,lo: rd = v */
static void li32(u32 rd, u32 v)
{
   emit((15u << 26) | (rd << 21) | (v >> 16));
   emit((24u << 26) | (rd << 21) | (rd << 16) | (v & 0xffffu));
}

/* stb/sth/stw r7,off(base) of v */
static void store(u32 opcode, u32 base, u32 off, u32 v)
{
   li32(7, v);
   emit((opcode << 26) | (7u << 21) | (base << 16) | (off & 0xffffu));
}

#define STB 38u
#define STH 44u
#define STW 36u

/* the registers holding the hardware's bases */
#define R_CP 3u  /* 0xcc000000: command processor */
#define R_VI 4u  /* 0xcc002000: video interface */
#define R_PI 5u  /* 0xcc003000: processor interface */
#define R_GP 6u  /* 0xcc008000: write-gather pipe */

static u32 gp_bytes;

static void gp8(u32 v)
{
   store(STB, R_GP, 0, v & 0xffu);
   gp_bytes++;
}

/* a BP register write: 0x61, then the register and its 24 bits */
static void bp(u32 reg, u32 v)
{
   gp8(0x61);
   store(STW, R_GP, 0, (reg << 24) | (v & 0xffffffu));
   gp_bytes += 4;
}

/* a 32-bit VI register, as its two halves */
static void vi32(u32 off, u32 v)
{
   store(STH, R_VI, off, v >> 16);
   store(STH, R_VI, off + 2, v & 0xffffu);
}

/* an EFB copy to the XFB of rows y..y+h-1, cleared after it or not */
static void xfb_copy(u32 y, u32 h, u32 address, int clear)
{
   bp(0x49, y << 10);
   bp(0x4a, 639u | ((h - 1u) << 10));
   bp(0x4b, address >> 5);
   bp(0x52, (1u << 14) | (clear ? 1u << 11 : 0u) | 3u);
}

static void put32(FILE *f, u32 v)
{
   fputc((int)(v >> 24), f);
   fputc((int)((v >> 16) & 0xffu), f);
   fputc((int)((v >> 8) & 0xffu), f);
   fputc((int)(v & 0xffu), f);
}

int main(int argc, char **argv)
{
   FILE *f;
   u32 i;

   if (argc < 2)
   {
      fprintf(stderr, "usage: xfbsplit <out.dol>\n");
      return 2;
   }

   li32(R_CP, 0xcc000000u);
   li32(R_VI, 0xcc002000u);
   li32(R_PI, 0xcc003000u);
   li32(R_GP, 0xcc008000u);

   /* the FIFO: the CPU writes it through the gather pipe, the GPU reads it,
    * linked (PI base, end and write pointer; CP the same and its read
    * pointer, watermarks and distance) */
   store(STH, R_CP, 0x02, 0);
   store(STH, R_CP, 0x20, FIFO_BASE & 0xffffu);
   store(STH, R_CP, 0x22, FIFO_BASE >> 16);
   store(STH, R_CP, 0x24, FIFO_END & 0xffffu);
   store(STH, R_CP, 0x26, FIFO_END >> 16);
   store(STH, R_CP, 0x28, 0xc000u);
   store(STH, R_CP, 0x2a, 0);
   store(STH, R_CP, 0x2c, 0x4000u);
   store(STH, R_CP, 0x2e, 0);
   store(STH, R_CP, 0x30, 0);
   store(STH, R_CP, 0x32, 0);
   store(STH, R_CP, 0x34, FIFO_BASE & 0xffffu);
   store(STH, R_CP, 0x36, FIFO_BASE >> 16);
   store(STH, R_CP, 0x38, FIFO_BASE & 0xffffu);
   store(STH, R_CP, 0x3a, FIFO_BASE >> 16);
   store(STW, R_PI, 0x0c, FIFO_BASE);
   store(STW, R_PI, 0x10, FIFO_END);
   store(STW, R_PI, 0x14, FIFO_BASE);
   store(STH, R_CP, 0x02, 0x11); /* GP read and link enable */

   /* the video interface: NTSC 480i, 640 pixels a line, both fields from one
    * XFB, the bottom field a line down */
   store(STH, R_VI, 0x00, (240u << 4) | 6u);               /* active lines, equalization */
   vi32(0x04, (71u << 24) | (105u << 16) | 429u);            /* HCS, HCE, half line width */
   vi32(0x08, 64u | (162u << 7) | (373u << 17));             /* HSY, HBE, HBS */
   vi32(0x0c, (3u << 16) | 24u);                             /* odd field PSB, PRB */
   vi32(0x10, (2u << 16) | 25u);                             /* even field PSB, PRB */
   vi32(0x14, 12u | (520u << 5) | (12u << 16) | (520u << 21)); /* odd blanking */
   vi32(0x18, 13u | (519u << 5) | (13u << 16) | (519u << 21)); /* even blanking */
   vi32(0x1c, (1u << 28) | (XFB_ADDRESS >> 5));               /* top field */
   vi32(0x24, (1u << 28) | ((XFB_ADDRESS + XFB_STRIDE) >> 5)); /* bottom field */
   store(STH, R_VI, 0x48, (40u << 8) | 80u); /* 40 x 16 pixels a line, 2 lines a field line */
   store(STH, R_VI, 0x6c, 0);
   store(STH, R_VI, 0x02, 1); /* enable, interlaced, NTSC */

   /* the GPU: copies update colour and alpha, the EFB is RGB8 with Z24, the
    * copy filter is 0 0 21 22 21 0 0 (sum 64), the XFB 640 pixels a line */
   bp(0x41, 0x18);
   bp(0x43, 0);
   bp(0x53, 0x595000u);
   bp(0x54, 0x000015u);
   bp(0x4d, XFB_STRIDE / 32u);
   bp(0x4e, 0x100);
   bp(0x51, 0);

   bp(0x4f, 0xffffu); /* clear: alpha 255, red 255 */
   bp(0x50, 0x0000u); /* green 0, blue 0 */
   xfb_copy(0, 480, SCRATCH_ADDRESS, 1);
   bp(0x4f, 0xff00u);
   bp(0x50, 0x00ffu); /* blue */
   xfb_copy(240, 240, SCRATCH_ADDRESS, 1);

   xfb_copy(0, 240, XFB_ADDRESS, 0);
   xfb_copy(240, 240, XFB_ADDRESS + 240u * XFB_STRIDE, 0);

   /* NOPs to the next 32 bytes and 32 more: the gather pipe sends whole
    * blocks of 32 */
   while (gp_bytes % 32u)
      gp8(0);
   for (i = 0; i < 32u; i++)
      gp8(0);

   emit(0x7c0004acu); /* sync */
   emit(0x48000000u); /* b . */
   while (ncode % 8u) /* the section's size a multiple of 32 bytes, as loaders read */
      emit(0x60000000u); /* nop */

   f = fopen(argv[1], "wb");
   if (!f)
   {
      fprintf(stderr, "xfbsplit: cannot write %s\n", argv[1]);
      return 1;
   }
   /* the header: the code is text section 0, at 0x100 in the file */
   put32(f, 0x100);
   for (i = 1; i < 18; i++)
      put32(f, 0);
   put32(f, LOAD_ADDRESS);
   for (i = 1; i < 18; i++)
      put32(f, 0);
   put32(f, ncode * 4u);
   for (i = 1; i < 18; i++)
      put32(f, 0);
   put32(f, 0); /* bss address */
   put32(f, 0); /* bss size */
   put32(f, LOAD_ADDRESS);
   for (i = 0; i < 7; i++)
      put32(f, 0);
   for (i = 0; i < ncode; i++)
      put32(f, code[i]);
   fclose(f);
   printf("%s: %u instructions\n", argv[1], ncode);
   return 0;
}
