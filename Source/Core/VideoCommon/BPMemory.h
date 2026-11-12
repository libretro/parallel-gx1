// Copyright 2009 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <string>
#include <utility>

#include "Common/BitField.h"
#include "Common/BitUtils.h"
#include "Common/CommonTypes.h"
#include "Common/EnumMap.h"
#include "Common/Inline.h"

// X.h defines None to be 0 and Always to be 2, which causes problems with some of the enums
#undef None
#undef Always

enum class TextureFormat;
enum class EFBCopyFormat;
enum class TLUTFormat;

#pragma pack(4)

enum
{
  BPMEM_GENMODE = 0x00,
  BPMEM_DISPLAYCOPYFILTER = 0x01,  // 0x01 + 4
  BPMEM_IND_MTXA = 0x06,           // 0x06 + (3 * 3)
  BPMEM_IND_MTXB = 0x07,           // 0x07 + (3 * 3)
  BPMEM_IND_MTXC = 0x08,           // 0x08 + (3 * 3)
  BPMEM_IND_IMASK = 0x0F,
  BPMEM_IND_CMD = 0x10,  // 0x10 + 16
  BPMEM_SCISSORTL = 0x20,
  BPMEM_SCISSORBR = 0x21,
  BPMEM_LINEPTWIDTH = 0x22,
  BPMEM_PERF0_TRI = 0x23,
  BPMEM_PERF0_QUAD = 0x24,
  BPMEM_RAS1_SS0 = 0x25,
  BPMEM_RAS1_SS1 = 0x26,
  BPMEM_IREF = 0x27,
  BPMEM_TREF = 0x28,      // 0x28 + 8
  BPMEM_SU_SSIZE = 0x30,  // 0x30 + (2 * 8)
  BPMEM_SU_TSIZE = 0x31,  // 0x31 + (2 * 8)
  BPMEM_ZMODE = 0x40,
  BPMEM_BLENDMODE = 0x41,
  BPMEM_CONSTANTALPHA = 0x42,
  BPMEM_ZCOMPARE = 0x43,
  BPMEM_FIELDMASK = 0x44,
  BPMEM_SETDRAWDONE = 0x45,
  BPMEM_BUSCLOCK0 = 0x46,
  BPMEM_PE_TOKEN_ID = 0x47,
  BPMEM_PE_TOKEN_INT_ID = 0x48,
  BPMEM_EFB_TL = 0x49,
  BPMEM_EFB_WH = 0x4A,
  BPMEM_EFB_ADDR = 0x4B,
  BPMEM_EFB_STRIDE = 0x4D,
  BPMEM_COPYYSCALE = 0x4E,
  BPMEM_CLEAR_AR = 0x4F,
  BPMEM_CLEAR_GB = 0x50,
  BPMEM_CLEAR_Z = 0x51,
  BPMEM_TRIGGER_EFB_COPY = 0x52,
  BPMEM_COPYFILTER0 = 0x53,
  BPMEM_COPYFILTER1 = 0x54,
  BPMEM_CLEARBBOX1 = 0x55,
  BPMEM_CLEARBBOX2 = 0x56,
  BPMEM_CLEAR_PIXEL_PERF = 0x57,
  BPMEM_REVBITS = 0x58,
  BPMEM_SCISSOROFFSET = 0x59,
  BPMEM_PRELOAD_ADDR = 0x60,
  BPMEM_PRELOAD_TMEMEVEN = 0x61,
  BPMEM_PRELOAD_TMEMODD = 0x62,
  BPMEM_PRELOAD_MODE = 0x63,
  BPMEM_LOADTLUT0 = 0x64,
  BPMEM_LOADTLUT1 = 0x65,
  BPMEM_TEXINVALIDATE = 0x66,
  BPMEM_PERF1 = 0x67,
  BPMEM_FIELDMODE = 0x68,
  BPMEM_BUSCLOCK1 = 0x69,
  BPMEM_TX_SETMODE0 = 0x80,     // 0x80 + 4
  BPMEM_TX_SETMODE1 = 0x84,     // 0x84 + 4
  BPMEM_TX_SETIMAGE0 = 0x88,    // 0x88 + 4
  BPMEM_TX_SETIMAGE1 = 0x8C,    // 0x8C + 4
  BPMEM_TX_SETIMAGE2 = 0x90,    // 0x90 + 4
  BPMEM_TX_SETIMAGE3 = 0x94,    // 0x94 + 4
  BPMEM_TX_SETTLUT = 0x98,      // 0x98 + 4
  BPMEM_TX_SETMODE0_4 = 0xA0,   // 0xA0 + 4
  BPMEM_TX_SETMODE1_4 = 0xA4,   // 0xA4 + 4
  BPMEM_TX_SETIMAGE0_4 = 0xA8,  // 0xA8 + 4
  BPMEM_TX_SETIMAGE1_4 = 0xAC,  // 0xA4 + 4
  BPMEM_TX_SETIMAGE2_4 = 0xB0,  // 0xB0 + 4
  BPMEM_TX_SETIMAGE3_4 = 0xB4,  // 0xB4 + 4
  BPMEM_TX_SETTLUT_4 = 0xB8,    // 0xB8 + 4
  BPMEM_TEV_COLOR_ENV = 0xC0,   // 0xC0 + (2 * 16)
  BPMEM_TEV_ALPHA_ENV = 0xC1,   // 0xC1 + (2 * 16)
  BPMEM_TEV_COLOR_RA = 0xE0,    // 0xE0 + (2 * 4)
  BPMEM_TEV_COLOR_BG = 0xE1,    // 0xE1 + (2 * 4)
  BPMEM_FOGRANGE = 0xE8,        // 0xE8 + 6
  BPMEM_FOGPARAM0 = 0xEE,
  BPMEM_FOGBMAGNITUDE = 0xEF,
  BPMEM_FOGBEXPONENT = 0xF0,
  BPMEM_FOGPARAM3 = 0xF1,
  BPMEM_FOGCOLOR = 0xF2,
  BPMEM_ALPHACOMPARE = 0xF3,
  BPMEM_BIAS = 0xF4,
  BPMEM_ZTEX2 = 0xF5,
  BPMEM_TEV_KSEL = 0xF6,  // 0xF6 + 8
  BPMEM_BP_MASK = 0xFE,
};

// Tev/combiner things

// TEV scaling type
enum class TevScale : u32
{
  Scale1 = 0,
  Scale2 = 1,
  Scale4 = 2,
  Divide2 = 3
};

// TEV combiner operator
enum class TevOp : u32
{
  Add = 0,
  Sub = 1,
};

enum class TevCompareMode : u32
{
  R8 = 0,
  GR16 = 1,
  BGR24 = 2,
  RGB8 = 3,
  A8 = RGB8,
};

enum class TevComparison : u32
{
  GT = 0,
  EQ = 1,
};

// TEV color combiner input
enum class TevColorArg : u32
{
  PrevColor = 0,
  PrevAlpha = 1,
  Color0 = 2,
  Alpha0 = 3,
  Color1 = 4,
  Alpha1 = 5,
  Color2 = 6,
  Alpha2 = 7,
  TexColor = 8,
  TexAlpha = 9,
  RasColor = 10,
  RasAlpha = 11,
  One = 12,
  Half = 13,
  Konst = 14,
  Zero = 15
};

// TEV alpha combiner input
enum class TevAlphaArg : u32
{
  PrevAlpha = 0,
  Alpha0 = 1,
  Alpha1 = 2,
  Alpha2 = 3,
  TexAlpha = 4,
  RasAlpha = 5,
  Konst = 6,
  Zero = 7
};

// TEV output registers
enum class TevOutput : u32
{
  Prev = 0,
  Color0 = 1,
  Color1 = 2,
  Color2 = 3,
};

// Z-texture formats
enum class ZTexFormat : u32
{
  U8 = 0,
  U16 = 1,
  U24 = 2
};

// Z texture operator
enum class ZTexOp : u32
{
  Disabled = 0,
  Add = 1,
  Replace = 2
};

// TEV bias value
enum class TevBias : u32
{
  Zero = 0,
  AddHalf = 1,
  SubHalf = 2,
  Compare = 3
};

// Indirect texture format
enum class IndTexFormat : u32
{
  ITF_8 = 0,
  ITF_5 = 1,
  ITF_4 = 2,
  ITF_3 = 3
};

// Indirect texture bias
enum class IndTexBias : u32
{
  None = 0,
  S = 1,
  T = 2,
  ST = 3,
  U = 4,
  SU = 5,
  TU_ = 6,  // conflicts with define in PowerPC.h
  STU = 7
};

enum class IndMtxIndex : u32
{
  Off = 0,
  Matrix0 = 1,
  Matrix1 = 2,
  Matrix2 = 3,
};

enum class IndMtxId : u32
{
  Indirect = 0,
  S = 1,
  T = 2,
};

// Indirect texture bump alpha
enum class IndTexBumpAlpha : u32
{
  Off = 0,
  S = 1,
  T = 2,
  U = 3
};

// Indirect texture wrap value
enum class IndTexWrap : u32
{
  ITW_OFF = 0,
  ITW_256 = 1,
  ITW_128 = 2,
  ITW_64 = 3,
  ITW_32 = 4,
  ITW_16 = 5,
  ITW_0 = 6
};

union IND_MTXA
{
  BitField<0, 11, s32> ma;
  BitField<11, 11, s32> mb;
  BitField<22, 2, u8, u32> s0;  // bits 0-1 of scale factor
  u32 hex;
};

union IND_MTXB
{
  BitField<0, 11, s32> mc;
  BitField<11, 11, s32> md;
  BitField<22, 2, u8, u32> s1;  // bits 2-3 of scale factor
  u32 hex;
};

union IND_MTXC
{
  BitField<0, 11, s32> me;
  BitField<11, 11, s32> mf;
  BitField<22, 1, u8, u32> s2;  // bit 4 of scale factor
  // The SDK treats the scale factor as 6 bits, 2 on each column; however, hardware seems to ignore
  // the top bit.
  BitField<22, 2, u8, u32> sdk_s2;
  u32 hex;
};

struct IND_MTX
{
  IND_MTXA col0;
  IND_MTXB col1;
  IND_MTXC col2;
  u8 GetScale() const { return (col0.s0 << 0) | (col1.s1 << 2) | (col2.s2 << 4); }
};

union IND_IMASK
{
  BitField<0, 24, u32> mask;
  u32 hex;
};

struct TevStageCombiner
{
  union ColorCombiner
  {
    // abc=8bit,d=10bit
    BitField<0, 4, TevColorArg> d;
    BitField<4, 4, TevColorArg> c;
    BitField<8, 4, TevColorArg> b;
    BitField<12, 4, TevColorArg> a;

    BitField<16, 2, TevBias> bias;
    BitField<18, 1, TevOp> op;                  // Applies when bias is not compare
    BitField<18, 1, TevComparison> comparison;  // Applies when bias is compare
    BitField<19, 1, bool, u32> clamp;

    BitField<20, 2, TevScale> scale;               // Applies when bias is not compare
    BitField<20, 2, TevCompareMode> compare_mode;  // Applies when bias is compare
    BitField<22, 2, TevOutput> dest;

    u32 hex;
  };
  union AlphaCombiner
  {
    BitField<0, 2, u32> rswap;
    BitField<2, 2, u32> tswap;
    BitField<4, 3, TevAlphaArg> d;
    BitField<7, 3, TevAlphaArg> c;
    BitField<10, 3, TevAlphaArg> b;
    BitField<13, 3, TevAlphaArg> a;

    BitField<16, 2, TevBias> bias;
    BitField<18, 1, TevOp> op;                  // Applies when bias is not compare
    BitField<18, 1, TevComparison> comparison;  // Applies when bias is compare
    BitField<19, 1, bool, u32> clamp;

    BitField<20, 2, TevScale> scale;               // Applies when bias is not compare
    BitField<20, 2, TevCompareMode> compare_mode;  // Applies when bias is compare
    BitField<22, 2, TevOutput> dest;

    u32 hex;
  };

  ColorCombiner colorC;
  AlphaCombiner alphaC;
};

// several discoveries:
// GXSetTevIndBumpST(tevstage, indstage, matrixind)
//  if ( matrix == 2 ) realmat = 6; // 10
//  else if ( matrix == 3 ) realmat = 7; // 11
//  else if ( matrix == 1 ) realmat = 5; // 9
//  GXSetTevIndirect(tevstage, indstage, 0, 3, realmat, 6, 6, 0, 0, 0)
//  GXSetTevIndirect(tevstage+1, indstage, 0, 3, realmat+4, 6, 6, 1, 0, 0)
//  GXSetTevIndirect(tevstage+2, indstage, 0, 0, 0, 0, 0, 1, 0, 0)

union TevStageIndirect
{
  BitField<0, 2, u32> bt;  // Indirect tex stage ID
  BitField<2, 2, IndTexFormat> fmt;
  BitField<4, 3, IndTexBias> bias;
  BitField<4, 1, bool, u32> bias_s;
  BitField<5, 1, bool, u32> bias_t;
  BitField<6, 1, bool, u32> bias_u;
  BitField<7, 2, IndTexBumpAlpha> bs;  // Indicates which coordinate will become the 'bump alpha'
  // Indicates which indirect matrix is used when matrix_id is Indirect.
  // Also always indicates which indirect matrix to use for the scale factor, even with S or T.
  BitField<9, 2, IndMtxIndex> matrix_index;
  // Should be set to Indirect (0) if matrix_index is Off (0)
  BitField<11, 2, IndMtxId> matrix_id;
  BitField<13, 3, IndTexWrap> sw;         // Wrapping factor for S of regular coord
  BitField<16, 3, IndTexWrap> tw;         // Wrapping factor for T of regular coord
  BitField<19, 1, bool, u32> lb_utclod;   // Use modified or unmodified texture
                                          // coordinates for LOD computation
  BitField<20, 1, bool, u32> fb_addprev;  // true if the texture coordinate results from the
                                          // previous TEV stage should be added

  struct
  {
    u32 hex : 21;
    u32 unused : 11;
  };

  u32 fullhex;

  // If bs and matrix are zero, the result of the stage is independent of
  // the texture sample data, so we can skip sampling the texture.
  bool IsActive() const { return bs != IndTexBumpAlpha::Off || matrix_index != IndMtxIndex::Off; }
};

enum class RasColorChan : u32
{
  Color0 = 0,
  Color1 = 1,
  AlphaBump = 5,
  NormalizedAlphaBump = 6,
  Zero = 7,
};

union TwoTevStageOrders
{
  BitField<0, 3, u32> texmap_even;
  BitField<3, 3, u32> texcoord_even;
  BitField<6, 1, bool, u32> enable_tex_even;  // true if should read from texture
  BitField<7, 3, RasColorChan> colorchan_even;

  BitField<12, 3, u32> texmap_odd;
  BitField<15, 3, u32> texcoord_odd;
  BitField<18, 1, bool, u32> enable_tex_odd;  // true if should read from texture
  BitField<19, 3, RasColorChan> colorchan_odd;

  u32 hex;
  u32 getTexMap(int i) const { return i ? texmap_odd.Value() : texmap_even.Value(); }
  u32 getTexCoord(int i) const { return i ? texcoord_odd.Value() : texcoord_even.Value(); }
  u32 getEnable(int i) const { return i ? enable_tex_odd.Value() : enable_tex_even.Value(); }
  RasColorChan getColorChan(int i) const
  {
    return i ? colorchan_odd.Value() : colorchan_even.Value();
  }
};

union TEXSCALE
{
  BitField<0, 4, u32> ss0;   // Indirect tex stage 0 or 2, 2^(-ss0)
  BitField<4, 4, u32> ts0;   // Indirect tex stage 0 or 2
  BitField<8, 4, u32> ss1;   // Indirect tex stage 1 or 3
  BitField<12, 4, u32> ts1;  // Indirect tex stage 1 or 3
  u32 hex;
};

union RAS1_IREF
{
  BitField<0, 3, u32> bi0;  // Indirect tex stage 0 texmap
  BitField<3, 3, u32> bc0;  // Indirect tex stage 0 tex coord
  BitField<6, 3, u32> bi1;
  BitField<9, 3, u32> bc1;
  BitField<12, 3, u32> bi2;
  BitField<15, 3, u32> bc2;
  BitField<18, 3, u32> bi3;
  BitField<21, 3, u32> bc3;
  u32 hex;

  u32 getTexCoord(int i) const { return (hex >> (6 * i + 3)) & 7; }
  u32 getTexMap(int i) const { return (hex >> (6 * i)) & 7; }
};

// Texture structs
enum class WrapMode : u32
{
  Clamp = 0,
  Repeat = 1,
  Mirror = 2,
  // Hardware testing indicates that WrapMode set to 3 behaves the same as clamp, though this is an
  // invalid value
};

enum class MipMode : u32
{
  None = 0,
  Point = 1,
  Linear = 2,
};

enum class FilterMode : u32
{
  Near = 0,
  Linear = 1,
};

enum class LODType : u32
{
  Edge = 0,
  Diagonal = 1,
};

enum class MaxAniso
{
  One = 0,
  Two = 1,
  Four = 2,
};

union TexMode0
{
  BitField<0, 2, WrapMode> wrap_s;
  BitField<2, 2, WrapMode> wrap_t;
  BitField<4, 1, FilterMode> mag_filter;
  BitField<5, 2, MipMode> mipmap_filter;
  BitField<7, 1, FilterMode> min_filter;
  BitField<8, 1, LODType> diag_lod;
  BitField<9, 8, s32> lod_bias;
  BitField<19, 2, MaxAniso> max_aniso;
  BitField<21, 1, bool, u32> lod_clamp;
  u32 hex;
};

union TexMode1
{
  BitField<0, 8, u32> min_lod;
  BitField<8, 8, u32> max_lod;
  u32 hex;
};

union TexImage0
{
  BitField<0, 10, u32> width;    // Actually w-1
  BitField<10, 10, u32> height;  // Actually h-1
  BitField<20, 4, TextureFormat> format;
  u32 hex;
};

union TexImage1
{
  BitField<0, 15, u32> tmem_even;  // TMEM line index for even LODs
  BitField<15, 3, u32> cache_width;
  BitField<18, 3, u32> cache_height;
  // true if this texture is managed manually (false means we'll
  // autofetch the texture data whenever it changes)
  BitField<21, 1, bool, u32> cache_manually_managed;
  u32 hex;
};

union TexImage2
{
  BitField<0, 15, u32> tmem_odd;  // tmem line index for odd LODs
  BitField<15, 3, u32> cache_width;
  BitField<18, 3, u32> cache_height;
  u32 hex;
};

union TexImage3
{
  BitField<0, 24, u32> image_base;  // address in memory >> 5 (was 20 for GC)
  u32 hex;
};

union TexTLUT
{
  BitField<0, 10, u32> tmem_offset;
  BitField<10, 2, TLUTFormat> tlut_format;
  u32 hex;
};

union ZTex1
{
  BitField<0, 24, u32> bias;
  u32 hex;
};

union ZTex2
{
  BitField<0, 2, ZTexFormat> type;
  BitField<2, 2, ZTexOp> op;
  u32 hex;
};

// Geometry/other structs
enum class CullMode : u32
{
  None = 0,
  Back = 1,   // cull back-facing primitives
  Front = 2,  // cull front-facing primitives
  All = 3,    // cull all primitives
};

union GenMode
{
  BitField<0, 4, u32> numtexgens;
  BitField<4, 3, u32> numcolchans;
  BitField<7, 1, u32> unused;              // 1 bit unused?
  BitField<8, 1, bool, u32> flat_shading;  // unconfirmed
  BitField<9, 1, bool, u32> multisampling;
  // This value is 1 less than the actual number (0-15 map to 1-16).
  // In other words there is always at least 1 tev stage
  BitField<10, 4, u32> numtevstages;
  BitField<14, 2, CullMode> cull_mode;
  BitField<16, 3, u32> numindstages;
  BitField<19, 1, bool, u32> zfreeze;

  u32 hex;
};

enum class AspectRatioAdjustment
{
  DontAdjust = 0,
  Adjust = 1,
};

union LPSize
{
  BitField<0, 8, u32> linesize;   // in 1/6th pixels
  BitField<8, 8, u32> pointsize;  // in 1/6th pixels
  BitField<16, 3, u32> lineoff;
  BitField<19, 3, u32> pointoff;
  // interlacing: adjust for pixels having AR of 1/2
  BitField<22, 1, AspectRatioAdjustment> adjust_for_aspect_ratio;
  u32 hex;
};

union ScissorPos
{
  // The top bit is ignored, and not included in the mask used by GX SDK functions
  // (though libogc includes it for the bottom coordinate (only) for some reason)
  // x_full and y_full include that bit for the FIFO analyzer, though it is usually unset.
  // The SDK also adds 342 to these values.
  BitField<0, 11, u32> y;
  BitField<0, 12, u32> y_full;
  BitField<12, 11, u32> x;
  BitField<12, 12, u32> x_full;
  u32 hex;
};

union ScissorOffset
{
  // The scissor offset ignores the top bit (though it isn't masked off by the GX SDK).
  // Each value is also divided by 2 (so 0-511 map to 0-1022).
  // x_full and y_full include that top bit for the FIFO analyzer, though it is usually unset.
  // The SDK also adds 342 to each value (before dividing it).
  BitField<0, 9, u32> x;
  BitField<0, 10, u32> x_full;
  BitField<10, 9, u32> y;
  BitField<10, 10, u32> y_full;
  u32 hex;
};

union X10Y10
{
  BitField<0, 10, u32> x;
  BitField<10, 10, u32> y;
  u32 hex;
};

// Framebuffer/pixel stuff (incl fog)
enum class SrcBlendFactor : u32
{
  Zero = 0,
  One = 1,
  DstClr = 2,
  InvDstClr = 3,
  SrcAlpha = 4,
  InvSrcAlpha = 5,
  DstAlpha = 6,
  InvDstAlpha = 7
};

enum class DstBlendFactor : u32
{
  Zero = 0,
  One = 1,
  SrcClr = 2,
  InvSrcClr = 3,
  SrcAlpha = 4,
  InvSrcAlpha = 5,
  DstAlpha = 6,
  InvDstAlpha = 7
};

enum class LogicOp : u32
{
  Clear = 0,
  And = 1,
  AndReverse = 2,
  Copy = 3,
  AndInverted = 4,
  NoOp = 5,
  Xor = 6,
  Or = 7,
  Nor = 8,
  Equiv = 9,
  Invert = 10,
  OrReverse = 11,
  CopyInverted = 12,
  OrInverted = 13,
  Nand = 14,
  Set = 15
};

union BlendMode
{
  BitField<0, 1, bool, u32> blend_enable;
  BitField<1, 1, bool, u32> logic_op_enable;
  BitField<2, 1, bool, u32> dither;
  BitField<3, 1, bool, u32> color_update;
  BitField<4, 1, bool, u32> alpha_update;
  BitField<5, 3, DstBlendFactor> dst_factor;
  BitField<8, 3, SrcBlendFactor> src_factor;
  BitField<11, 1, bool, u32> subtract;
  BitField<12, 4, LogicOp> logic_mode;

  u32 hex;

  bool UseLogicOp() const;
};

union FogParam0
{
  BitField<0, 11, u32> mant;
  BitField<11, 8, u32> exp;
  BitField<19, 1, u32> sign;

  u32 hex;
  float FloatValue() const;
};

enum class FogProjection : u32
{
  Perspective = 0,
  Orthographic = 1,
};

enum class FogType : u32
{
  Off = 0,
  Linear = 2,
  Exp = 4,
  ExpSq = 5,
  BackwardsExp = 6,
  BackwardsExpSq = 7,
};

union FogParam3
{
  BitField<0, 11, u32> c_mant;
  BitField<11, 8, u32> c_exp;
  BitField<19, 1, u32> c_sign;
  BitField<20, 1, FogProjection> proj;
  BitField<21, 3, FogType> fsel;

  u32 hex;
  float FloatValue() const;
};

union FogRangeKElement
{
  BitField<0, 12, u32> HI;
  BitField<12, 12, u32> LO;

  // TODO: Which scaling coefficient should we use here? This is just a guess!
  float GetValue(int i) const { return (i ? HI.Value() : LO.Value()) / 256.f; }
  u32 HEX;
};

struct FogRangeParams
{
  union RangeBase
  {
    BitField<0, 10, u32> Center;  // viewport center + 342
    BitField<10, 1, bool, u32> Enabled;
    u32 hex;
  };
  RangeBase Base;
  FogRangeKElement K[5];
};

// final eq: ze = A/(B_MAG - (Zs>>B_SHF));
struct FogParams
{
  FogParam0 a;
  u32 b_magnitude;
  u32 b_shift;  // b's exp + 1?
  FogParam3 c_proj_fsel;

  union FogColor
  {
    BitField<0, 8, u32> b;
    BitField<8, 8, u32> g;
    BitField<16, 8, u32> r;
    u32 hex;
  };

  FogColor color;  // 0:b 8:g 16:r - nice!

  // Special case where a and c are infinite and the sign matches, resulting in a result of NaN.
  bool IsNaNCase() const;
  float GetA() const;

  // amount to subtract from eyespacez after range adjustment
  float GetC() const;
};

enum class CompareMode : u32
{
  Never = 0,
  Less = 1,
  Equal = 2,
  LEqual = 3,
  Greater = 4,
  NEqual = 5,
  GEqual = 6,
  Always = 7
};

union ZMode
{
  BitField<0, 1, bool, u32> test_enable;
  BitField<1, 3, CompareMode> func;
  BitField<4, 1, bool, u32> update_enable;

  u32 hex;
};

union ConstantAlpha
{
  BitField<0, 8, u32> alpha;
  BitField<8, 1, bool, u32> enable;
  u32 hex;
};

union FieldMode
{
  // adjust vertex tex LOD computation to account for interlacing
  BitField<0, 1, AspectRatioAdjustment> texLOD;
  u32 hex;
};

enum class FieldMaskState : u32
{
  Skip = 0,
  Write = 1,
};

union FieldMask
{
  // Fields are written to the EFB only if their bit is set to write.
  BitField<0, 1, FieldMaskState> odd;
  BitField<1, 1, FieldMaskState> even;
  u32 hex;
};

enum class PixelFormat : u32
{
  RGB8_Z24 = 0,
  RGBA6_Z24 = 1,
  RGB565_Z16 = 2,
  Z24 = 3,
  Y8 = 4,
  U8 = 5,
  V8 = 6,
  YUV420 = 7,
  INVALID_FMT = 0xffffffff,  // Used by Dolphin to represent a missing value.
};

enum class DepthFormat : u32
{
  ZLINEAR = 0,
  ZNEAR = 1,
  ZMID = 2,
  ZFAR = 3,

  // It seems these Z formats aren't supported/were removed ?
  ZINV_LINEAR = 4,
  ZINV_NEAR = 5,
  ZINV_MID = 6,
  ZINV_FAR = 7
};

union PEControl
{
  BitField<0, 3, PixelFormat> pixel_format;
  BitField<3, 3, DepthFormat> zformat;
  BitField<6, 1, bool, u32> early_ztest;

  u32 hex;
};

// Texture coordinate stuff

union TCInfo
{
  BitField<0, 16, u32> scale_minus_1;
  BitField<16, 1, bool, u32> range_bias;
  BitField<17, 1, bool, u32> cylindric_wrap;
  // These bits only have effect in the s field of TCoordInfo
  BitField<18, 1, bool, u32> line_offset;
  BitField<19, 1, bool, u32> point_offset;
  u32 hex;
};

struct TCoordInfo
{
  TCInfo s;
  TCInfo t;
};

enum class TevRegType : u32
{
  Color = 0,
  Constant = 1,
};

struct TevReg
{
  // TODO: Check if Konst uses all 11 bits or just 8
  union RA
  {
    u32 hex;

    BitField<0, 11, s32> red;
    BitField<12, 11, s32> alpha;
    BitField<23, 1, TevRegType, u32> type;
  };
  union BG
  {
    u32 hex;

    BitField<0, 11, s32> blue;
    BitField<12, 11, s32> green;
    BitField<23, 1, TevRegType, u32> type;
  };

  RA ra;
  BG bg;
};

enum class ColorChannel : u32
{
  Red = 0,
  Green = 1,
  Blue = 2,
  Alpha = 3,
};

enum class KonstSel : u32
{
  V1 = 0,
  V7_8 = 1,
  V3_4 = 2,
  V5_8 = 3,
  V1_2 = 4,
  V3_8 = 5,
  V1_4 = 6,
  V1_8 = 7,
  // 8-11 are invalid values that output 0 (8-15 for alpha)
  K0 = 12,  // Color only
  K1 = 13,  // Color only
  K2 = 14,  // Color only
  K3 = 15,  // Color only
  K0_R = 16,
  K1_R = 17,
  K2_R = 18,
  K3_R = 19,
  K0_G = 20,
  K1_G = 21,
  K2_G = 22,
  K3_G = 23,
  K0_B = 24,
  K1_B = 25,
  K2_B = 26,
  K3_B = 27,
  K0_A = 28,
  K1_A = 29,
  K2_A = 30,
  K3_A = 31,
};

union TevKSel
{
  BitField<0, 2, ColorChannel> swap_rb;  // Odd ksel number: red; even: blue
  BitField<2, 2, ColorChannel> swap_ga;  // Odd ksel number: green; even: alpha
  BitField<4, 5, KonstSel> kcsel_even;
  BitField<9, 5, KonstSel> kasel_even;
  BitField<14, 5, KonstSel> kcsel_odd;
  BitField<19, 5, KonstSel> kasel_odd;
  u32 hex;
};

struct AllTevKSels
{
  std::array<TevKSel, 8> ksel;

  KonstSel GetKonstColor(u32 tev_stage) const
  {
    const u32 ksel_num = tev_stage >> 1;
    const bool odd = tev_stage & 1;
    const auto& cur_ksel = ksel[ksel_num];
    return odd ? cur_ksel.kcsel_odd.Value() : cur_ksel.kcsel_even.Value();
  }
  KonstSel GetKonstAlpha(u32 tev_stage) const
  {
    const u32 ksel_num = tev_stage >> 1;
    const bool odd = tev_stage & 1;
    const auto& cur_ksel = ksel[ksel_num];
    return odd ? cur_ksel.kasel_odd.Value() : cur_ksel.kasel_even.Value();
  }
  Common::EnumMap<ColorChannel, ColorChannel::Alpha> GetSwapTable(u32 swap_table_id) const
  {
    const u32 rg_ksel_num = swap_table_id << 1;
    const u32 ba_ksel_num = rg_ksel_num + 1;
    const auto& rg_ksel = ksel[rg_ksel_num];
    const auto& ba_ksel = ksel[ba_ksel_num];
    return {rg_ksel.swap_rb, rg_ksel.swap_ga, ba_ksel.swap_rb, ba_ksel.swap_ga};
  }
};

enum class AlphaTestOp : u32
{
  And = 0,
  Or = 1,
  Xor = 2,
  Xnor = 3
};

enum class AlphaTestResult
{
  Undetermined = 0,
  Fail = 1,
  Pass = 2,
};

union AlphaTest
{
  BitField<0, 8, u32> ref0;
  BitField<8, 8, u32> ref1;
  BitField<16, 3, CompareMode> comp0;
  BitField<19, 3, CompareMode> comp1;
  BitField<22, 2, AlphaTestOp> logic;

  u32 hex;

  DOLPHIN_FORCE_INLINE AlphaTestResult TestResult() const
  {
    switch (logic)
    {
    case AlphaTestOp::And:
      if (comp0 == CompareMode::Always && comp1 == CompareMode::Always)
        return AlphaTestResult::Pass;
      if (comp0 == CompareMode::Never || comp1 == CompareMode::Never)
        return AlphaTestResult::Fail;
      break;

    case AlphaTestOp::Or:
      if (comp0 == CompareMode::Always || comp1 == CompareMode::Always)
        return AlphaTestResult::Pass;
      if (comp0 == CompareMode::Never && comp1 == CompareMode::Never)
        return AlphaTestResult::Fail;
      break;

    case AlphaTestOp::Xor:
      if ((comp0 == CompareMode::Always && comp1 == CompareMode::Never) ||
          (comp0 == CompareMode::Never && comp1 == CompareMode::Always))
        return AlphaTestResult::Pass;
      if ((comp0 == CompareMode::Always && comp1 == CompareMode::Always) ||
          (comp0 == CompareMode::Never && comp1 == CompareMode::Never))
        return AlphaTestResult::Fail;
      break;

    case AlphaTestOp::Xnor:
      if ((comp0 == CompareMode::Always && comp1 == CompareMode::Never) ||
          (comp0 == CompareMode::Never && comp1 == CompareMode::Always))
        return AlphaTestResult::Fail;
      if ((comp0 == CompareMode::Always && comp1 == CompareMode::Always) ||
          (comp0 == CompareMode::Never && comp1 == CompareMode::Never))
        return AlphaTestResult::Pass;
      break;

    default:
      return AlphaTestResult::Undetermined;
    }
    return AlphaTestResult::Undetermined;
  }
};

enum class FrameToField : u32
{
  Progressive = 0,
  InterlacedEven = 2,
  InterlacedOdd = 3,
};

enum class GammaCorrection : u32
{
  Gamma1_0 = 0,
  Gamma1_7 = 1,
  Gamma2_2 = 2,
  // Hardware testing indicates this behaves the same as Gamma2_2
  Invalid2_2 = 3,
};

union UPE_Copy
{
  u32 Hex;

  BitField<0, 1, bool, u32> clamp_top;     // if set clamp top
  BitField<1, 1, bool, u32> clamp_bottom;  // if set clamp bottom
  BitField<2, 1, u32> unknown_bit;
  BitField<3, 4, u32> target_pixel_format;  // realformat is (fmt/2)+((fmt&1)*8).... for some reason
                                            // the msb is the lsb (pattern: cycling right shift)
  BitField<7, 2, GammaCorrection> gamma;
  // "mipmap" filter... false = no filter (scale 1:1) ; true = box filter (scale 2:1)
  BitField<9, 1, bool, u32> half_scale;
  BitField<10, 1, bool, u32> scale_invert;  // if set vertical scaling is on
  BitField<11, 1, bool, u32> clear;
  BitField<12, 2, FrameToField> frame_to_field;
  BitField<14, 1, bool, u32> copy_to_xfb;
  BitField<15, 1, bool, u32> intensity_fmt;  // if set, is an intensity format (I4,I8,IA4,IA8)
  // if false automatic color conversion by texture format and pixel type
  BitField<16, 1, bool, u32> auto_conv;

  EFBCopyFormat tp_realFormat() const
  {
    return static_cast<EFBCopyFormat>(target_pixel_format / 2 + (target_pixel_format & 1) * 8);
  }
};

union CopyFilterCoefficients
{
  using Values = std::array<u8, 7>;

  u64 Hex;

  BitField<0, 32, u32, u64> Low;
  BitField<0, 6, u64> w0;
  BitField<6, 6, u64> w1;
  BitField<12, 6, u64> w2;
  BitField<18, 6, u64> w3;
  BitField<32, 32, u32, u64> High;
  BitField<32, 6, u64> w4;
  BitField<38, 6, u64> w5;
  BitField<44, 6, u64> w6;

  Values GetCoefficients() const
  {
    return {{
        static_cast<u8>(w0),
        static_cast<u8>(w1),
        static_cast<u8>(w2),
        static_cast<u8>(w3),
        static_cast<u8>(w4),
        static_cast<u8>(w5),
        static_cast<u8>(w6),
    }};
  }
};

union BPU_PreloadTileInfo
{
  BitField<0, 15, u32> count;
  BitField<15, 2, u32> type;  // TODO: enum for this?
  u32 hex;
};

union BPU_LoadTlutInfo
{
  BitField<0, 10, u32> tmem_addr;
  BitField<10, 11, u32> tmem_line_count;
  u32 hex;
};

struct BPS_TmemConfig
{
  u32 preload_addr;
  u32 preload_tmem_even;
  u32 preload_tmem_odd;
  BPU_PreloadTileInfo preload_tile_info;
  u32 tlut_src;
  BPU_LoadTlutInfo tlut_dest;
  u32 texinvalidate;
};

union AllTexUnits;

// The addressing of the texture units is a bit non-obvious.
// This struct abstracts the complexity away.
union TexUnitAddress
{
  enum class Register : u32
  {
    SETMODE0 = 0,
    SETMODE1 = 1,
    SETIMAGE0 = 2,
    SETIMAGE1 = 3,
    SETIMAGE2 = 4,
    SETIMAGE3 = 5,
    SETTLUT = 6,
    UNKNOWN = 7,
  };

  BitField<0, 2, u32> UnitIdLow;
  BitField<2, 3, Register> Reg;
  BitField<5, 1, u32> UnitIdHigh;

  BitField<0, 6, u32> FullAddress;
  u32 hex;

  TexUnitAddress() : hex(0) {}
  TexUnitAddress(u32 unit_id, Register reg = Register::SETMODE0) : hex(0)
  {
    UnitIdLow = unit_id & 3;
    UnitIdHigh = unit_id >> 2;
    Reg = reg;
  }

  static TexUnitAddress FromBPAddress(u32 Address)
  {
    TexUnitAddress Val;
    // Clear upper two bits (which should always be 0x80)
    Val.FullAddress = Address & 0x3f;
    return Val;
  }

  u32 GetUnitID() const { return UnitIdLow | (UnitIdHigh << 2); }

private:
  friend AllTexUnits;

  size_t GetOffset() const { return FullAddress; }
  size_t GetBPAddress() const { return FullAddress | 0x80; }

  static constexpr size_t ComputeOffset(u32 unit_id)
  {
    // FIXME: Would be nice to construct a TexUnitAddress and get its offset,
    // but that doesn't seem to be possible in c++17

    // So we manually re-implement the calculation
    return (unit_id & 3) | ((unit_id & 4) << 3);
  }
};
static_assert(sizeof(TexUnitAddress) == sizeof(u32));

// A view of the registers of a single TexUnit
struct TexUnit
{
  TexMode0 texMode0;
  u32 : 32;  // doing u32 : 96 is legal according to the standard, but msvc
  u32 : 32;  // doesn't like it. So we stack multiple lines of u32 : 32;
  u32 : 32;
  TexMode1 texMode1;
  u32 : 32;
  u32 : 32;
  u32 : 32;
  TexImage0 texImage0;
  u32 : 32;
  u32 : 32;
  u32 : 32;
  TexImage1 texImage1;
  u32 : 32;
  u32 : 32;
  u32 : 32;
  TexImage2 texImage2;
  u32 : 32;
  u32 : 32;
  u32 : 32;
  TexImage3 texImage3;
  u32 : 32;
  u32 : 32;
  u32 : 32;
  TexTLUT texTlut;
  u32 : 32;
  u32 : 32;
  u32 : 32;
  u32 unknown;
};
static_assert(sizeof(TexUnit) == sizeof(u32) * 4 * 7 + sizeof(u32));

union AllTexUnits
{
  std::array<u32, 8 * 8> AllRegisters;

  const TexUnit& GetUnit(u32 UnitId) const
  {
    auto address = TexUnitAddress(UnitId);
    const u32* ptr = &AllRegisters[address.GetOffset()];
    return *reinterpret_cast<const TexUnit*>(ptr);
  }

private:
  // For debuggers since GetUnit can be optimised out in release builds
  template <u32 UnitId>
  struct TexUnitPadding
  {
    static_assert(UnitId != 0, "Can't use 0 as sizeof(std::array<u32, 0>) != 0");
    std::array<u32, TexUnitAddress::ComputeOffset(UnitId)> pad;
  };

  TexUnit tex0;
  struct
  {
    TexUnitPadding<1> pad1;
    TexUnit tex1;
  };
  struct
  {
    TexUnitPadding<2> pad2;
    TexUnit tex2;
  };
  struct
  {
    TexUnitPadding<3> pad3;
    TexUnit tex3;
  };
  struct
  {
    TexUnitPadding<4> pad4;
    TexUnit tex4;
  };
  struct
  {
    TexUnitPadding<5> pad5;
    TexUnit tex5;
  };
  struct
  {
    TexUnitPadding<6> pad6;
    TexUnit tex6;
  };
  struct
  {
    TexUnitPadding<7> pad7;
    TexUnit tex7;
  };
};
static_assert(sizeof(AllTexUnits) == 8 * 8 * sizeof(u32));

// All of BP memory

struct BPCmd
{
  int address;
  int changes;
  int newvalue;
};

enum class EmulatedZ : u32
{
  Disabled = 0,
  Early = 1,
  Late = 2,
  ForcedEarly = 3,
  EarlyWithFBFetch = 4,
  EarlyWithZComplocHack = 5,
};

struct BPMemory
{
  GenMode genMode;             // 0x00
  u32 display_copy_filter[4];  // 0x01-0x04
  u32 unknown;                 // 0x05
  // indirect matrices (set by GXSetIndTexMtx, selected by TevStageIndirect::matrix_index)
  // abc form a 2x3 offset matrix, there's 3 such matrices
  // the 3 offset matrices can either be indirect type, S-type, or T-type
  // 6bit scale factor s is distributed across IND_MTXA/B/C.
  // before using matrices scale by 2^-(s-17)
  IND_MTX indmtx[3];                  // 0x06-0x0e: GXSetIndTexMtx, 2x3 matrices
  IND_IMASK imask;                    // 0x0f
  TevStageIndirect tevind[16];        // 0x10-0x1f: GXSetTevIndirect
  ScissorPos scissorTL;               // 0x20
  ScissorPos scissorBR;               // 0x21
  LPSize lineptwidth;                 // 0x22
  u32 sucounter;                      // 0x23
  u32 rascounter;                     // 0x24
  TEXSCALE texscale[2];               // 0x25,0x26: GXSetIndTexCoordScale
  RAS1_IREF tevindref;                // 0x27: GXSetIndTexOrder
  TwoTevStageOrders tevorders[8];     // 0x28-0x2f
  TCoordInfo texcoords[8];            // 0x30-0x3f: s,t,s,t,s,t,s,t...
  ZMode zmode;                        // 0x40
  BlendMode blendmode;                // 0x41
  ConstantAlpha dstalpha;             // 0x42
  PEControl zcontrol;                 // 0x43: GXSetZCompLoc, GXPixModeSync
  FieldMask fieldmask;                // 0x44
  u32 drawdone;                       // 0x45: bit1=1 if end of list
  u32 unknown5;                       // 0x46: clock?
  u32 petoken;                        // 0x47
  u32 petokenint;                     // 0x48
  X10Y10 copyTexSrcXY;                // 0x49
  X10Y10 copyTexSrcWH;                // 0x4a
  u32 copyTexDest;                    // 0x4b: CopyAddress (GXDispCopy and GXTexCopy use it)
  u32 unknown6;                       // 0x4c
  u32 copyDestStride;                 // 0x4d
  u32 dispcopyyscale;                 // 0x4e
  u32 clearcolorAR;                   // 0x4f
  u32 clearcolorGB;                   // 0x50
  u32 clearZValue;                    // 0x51
  UPE_Copy triggerEFBCopy;            // 0x52
  CopyFilterCoefficients copyfilter;  // 0x53,0x54
  u32 boundbox0;                      // 0x55
  u32 boundbox1;                      // 0x56
  u32 unknown7[2];                    // 0x57,0x58
  ScissorOffset scissorOffset;        // 0x59
  u32 unknown8[6];                    // 0x5a-0x5f
  BPS_TmemConfig tmem_config;         // 0x60-0x66
  u32 metric;                         // 0x67
  FieldMode fieldmode;                // 0x68
  u32 unknown10[7];                   // 0x69-0x6f
  u32 unknown11[16];                  // 0x70-0x7f
  AllTexUnits tex;                    // 0x80-0xbf
  TevStageCombiner combiners[16];     // 0xc0-0xdf
  TevReg tevregs[4];                  // 0xe0-0xe7
  FogRangeParams fogRange;            // 0xe8-0xed
  FogParams fog;                      // 0xee-0xf2
  AlphaTest alpha_test;               // 0xf3
  ZTex1 ztex1;                        // 0xf4
  ZTex2 ztex2;                        // 0xf5
  AllTevKSels tevksel;                // 0xf6-0xfd
  u32 bpMask;                         // 0xfe
  u32 unknown18;                      // 0xff

  EmulatedZ GetEmulatedZ() const
  {
    if (!zmode.test_enable)
      return EmulatedZ::Disabled;
    if (zcontrol.early_ztest)
      return EmulatedZ::Early;
    else
      return EmulatedZ::Late;
  }
};

#pragma pack()

extern BPMemory bpmem;

void LoadBPReg(u8 reg, u32 value, int cycles_into_future);
void LoadBPRegPreprocess(u8 reg, u32 value, int cycles_into_future);
