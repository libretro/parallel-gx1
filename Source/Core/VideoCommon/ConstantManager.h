// Copyright 2013 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/CommonTypes.h"

// all constant buffer attributes must be 16 bytes aligned, so this are the only allowed components:
typedef float float4[4];
typedef u32 uint4[4];
typedef s32 int4[4];

enum class DstBlendFactor : u32;
enum class SrcBlendFactor : u32;
enum class ZTexOp : u32;
enum class LogicOp : u32;

struct alignas(16) PixelShaderConstants
{
  int4 colors[4];
  int4 kcolors[4];
  int4 alpha;
  uint4 texdims[8];
  int4 zbias[2];
  int4 indtexscale[2];
  int4 indtexmtx[6];
  int4 fogcolor;
  int4 fogi;
  float4 fogf;
  float4 fogrange[3];
  float4 zslope;
  float efbscale[2];  // .xy

  // Constants from here onwards are only used in ubershaders, other than pack2.
  u32 genmode;                  // .z
  u32 alphaTest;                // .w
  u32 fogParam3;                // .x
  u32 fogRangeBase;             // .y
  u32 dstalpha;                 // .z
  ZTexOp ztex_op;               // .w
  u32 late_ztest;               // .x (bool)
  u32 rgba6_format;             // .y (bool)
  u32 dither;                   // .z (bool)
  u32 bounding_box;             // .w (bool)
  uint4 pack1[16];              // .xy - combiners, .z - tevind, .w - iref
  uint4 pack2[8];               // .x - tevorder, .y - tevksel, .z/.w - SamplerState tm0/tm1
  int4 konst[32];               // .rgba
  // The following are used in ubershaders when using shader_framebuffer_fetch blending
  u32 blend_enable;
  SrcBlendFactor blend_src_factor;
  SrcBlendFactor blend_src_factor_alpha;
  DstBlendFactor blend_dst_factor;
  DstBlendFactor blend_dst_factor_alpha;
  u32 blend_subtract;
  u32 blend_subtract_alpha;
  // For shader_framebuffer_fetch logic ops:
  u32 logic_op_enable;  // bool
  LogicOp logic_op_mode;
  // For custom shaders...
  u32 time_ms;
  // XF's texgen count, for ubershaders sized for more texgens than the draw has; in what was the
  // block's padding, so the block and savestates keep their size
  u32 xfmem_numTexGen;
};

struct alignas(16) VertexShaderConstants
{
  u32 components;           // .x
  u32 xfmem_dualTexInfo;    // .y
  u32 xfmem_numColorChans;  // .z
  u32 missing_color_hex;    // .w, used for change detection but not directly by shaders
  float4 missing_color_value;

  float4 posnormalmatrix[6];
  float4 projection[4];
  int4 materials[4];
  struct Light
  {
    int4 color;
    float4 cosatt;
    float4 distatt;
    float4 pos;
    float4 dir;
  };
  Light lights[8];
  float4 texmatrices[24];
  float4 transformmatrices[64];
  float4 normalmatrices[32];
  float4 posttransformmatrices[64];
  float4 pixelcentercorrection;
  float viewport[2];  // .xy
  float pad2[2];      // .zw

  // .x - texMtxInfo, .y - postMtxInfo, [0..1].z = color, [0..1].w = alpha
  uint4 xfmem_pack1[8];

  float4 cached_normal;
  float4 cached_tangent;
  float4 cached_binormal;
  // For UberShader vertex loader
  u32 vertex_stride;
  u32 vertex_offset_normals[3];
  u32 vertex_offset_position;
  u32 vertex_offset_posmtx;
  u32 vertex_offset_colors[2];
  u32 vertex_offset_texcoords[8];
};

enum class VSExpand : u32
{
  None = 0,
  Point,
  Line,
};

struct alignas(16) GeometryShaderConstants
{
  float4 stereoparams;
  float4 lineptparams;
  int4 texoffset;
  VSExpand vs_expand;  // Used by VS point/line expansion in ubershaders
  u32 pad[3];
};
