// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <concepts>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/EnumFormatter.h"
#include "Common/Inline.h"
#include "Common/Swap.h"
#include "VideoCommon/CPMemory.h"
#include "VideoCommon/VertexLoaderBase.h"

struct CPState;
class DataReader;

namespace OpcodeDecoder
{
enum class Opcode
{
  GX_NOP = 0x00,

  GX_LOAD_BP_REG = 0x61,
  GX_LOAD_CP_REG = 0x08,
  GX_LOAD_XF_REG = 0x10,
  GX_LOAD_INDX_A = 0x20,
  GX_LOAD_INDX_B = 0x28,
  GX_LOAD_INDX_C = 0x30,
  GX_LOAD_INDX_D = 0x38,

  GX_CMD_CALL_DL = 0x40,
  GX_CMD_UNKNOWN_METRICS = 0x44,
  GX_CMD_INVL_VC = 0x48,

  GX_PRIMITIVE_START = 0x80,
  GX_PRIMITIVE_END = 0xbf,
};

constexpr u8 GX_PRIMITIVE_MASK = 0x78;
constexpr u32 GX_PRIMITIVE_SHIFT = 3;
constexpr u8 GX_VAT_MASK = 0x07;

// These values are the values extracted using GX_PRIMITIVE_MASK
// and GX_PRIMITIVE_SHIFT.
// GX_DRAW_QUADS_2 behaves the same way as GX_DRAW_QUADS.
enum class Primitive : u8
{
  GX_DRAW_QUADS = 0x0,           // 0x80
  GX_DRAW_QUADS_2 = 0x1,         // 0x88
  GX_DRAW_TRIANGLES = 0x2,       // 0x90
  GX_DRAW_TRIANGLE_STRIP = 0x3,  // 0x98
  GX_DRAW_TRIANGLE_FAN = 0x4,    // 0xA0
  GX_DRAW_LINES = 0x5,           // 0xA8
  GX_DRAW_LINE_STRIP = 0x6,      // 0xB0
  GX_DRAW_POINTS = 0x7           // 0xB8
};

// The callback the Run and RunCommand functions below call. They are templates, so the compiler
// generates a version for each callback class with its functions inlined; this base only names
// the classes that may be passed. Each class has these, as plain members:
//   void OnXF(u16 address, u8 count, const u8* data);  // any XF command
//   void OnCP(u8 command, u32 value);  // any CP command; updates GetCPState() with LoadCPReg
//   void OnBP(u8 command, u32 value);  // any BP command
//   void OnIndexedLoad(CPArray array, u32 index, u16 address, u8 size);
//   void OnPrimitiveCommand(Primitive primitive, u8 vat, u32 vertex_size, u16 num_vertices,
//                           const u8* vertex_data);
//   void OnDisplayList(u32 address, u32 size);
//   void OnNop(u32 count);  // consecutive NOPs, merged into one call
//   void OnUnknown(u8 opcode, const u8* data);  // an unknown or unimplemented one; data[0] is it
//   void OnCommand(const u8* data, u32 size);  // after any of the above; data[0] is the opcode
//   CPState& GetCPState();  // for vertex decoding; CP commands change it
//   u32 GetVertexSize(u8 vat);
#define OPCODE_CALLBACK(sig) DOLPHIN_FORCE_INLINE sig
#define OPCODE_CALLBACK_NOINLINE(sig) sig
class Callback
{
};

namespace detail
{
// Main logic; split so that the main RunCommand can call OnCommand with the returned size.
static DOLPHIN_FORCE_INLINE u32 RunCommand(const u8* data, u32 available,
                                           std::derived_from<Callback> auto& callback)
{
  if (available < 1)
    return 0;

  const Opcode cmd = static_cast<Opcode>(data[0]);

  switch (cmd)
  {
  case Opcode::GX_NOP:
  {
    u32 count = 1;
    while (count < available && static_cast<Opcode>(data[count]) == Opcode::GX_NOP)
      count++;
    callback.OnNop(count);
    return count;
  }

  case Opcode::GX_LOAD_CP_REG:
  {
    if (available < 6)
      return 0;

    const u8 cmd2 = data[1];
    const u32 value = Common::swap32(&data[2]);

    callback.OnCP(cmd2, value);

    return 6;
  }

  case Opcode::GX_LOAD_XF_REG:
  {
    if (available < 5)
      return 0;

    const u32 cmd2 = Common::swap32(&data[1]);
    const u16 base_address = cmd2 & 0xffff;

    const u16 stream_size_temp = cmd2 >> 16;
    ASSERT_MSG(VIDEO, stream_size_temp < 16, "cmd2 = 0x{:08X}", cmd2);
    const u8 stream_size = (stream_size_temp & 0xf) + 1;

    if (available < u32(5 + stream_size * 4))
      return 0;

    callback.OnXF(base_address, stream_size, &data[5]);

    return 5 + stream_size * 4;
  }

  case Opcode::GX_LOAD_INDX_A:  // Used for position matrices
  case Opcode::GX_LOAD_INDX_B:  // Used for normal matrices
  case Opcode::GX_LOAD_INDX_C:  // Used for postmatrices
  case Opcode::GX_LOAD_INDX_D:  // Used for lights
  {
    if (available < 5)
      return 0;

    const u32 value = Common::swap32(&data[1]);

    const u32 index = value >> 16;
    const u16 address = value & 0xFFF;  // TODO: check mask
    const u8 size = ((value >> 12) & 0xF) + 1;

    // Map the command byte to its ref array.
    // GX_LOAD_INDX_A (32 = 8*4) . CPArray::XF_A (4+8 = 12)
    // GX_LOAD_INDX_B (40 = 8*5) . CPArray::XF_B (5+8 = 13)
    // GX_LOAD_INDX_C (48 = 8*6) . CPArray::XF_C (6+8 = 14)
    // GX_LOAD_INDX_D (56 = 8*7) . CPArray::XF_D (7+8 = 15)
    const auto ref_array = static_cast<CPArray>((static_cast<u8>(cmd) / 8) + 8);

    callback.OnIndexedLoad(ref_array, index, address, size);
    return 5;
  }

  case Opcode::GX_CMD_CALL_DL:
  {
    if (available < 9)
      return 0;

    const u32 address = Common::swap32(&data[1]);
    const u32 size = Common::swap32(&data[5]);

    // Force 32-byte alignment for both the address and the size.
    callback.OnDisplayList(address & ~31, size & ~31);
    return 9;
  }

  case Opcode::GX_LOAD_BP_REG:
  {
    if (available < 5)
      return 0;

    const u8 cmd2 = data[1];
    const u32 value = Common::swap24(&data[2]);

    callback.OnBP(cmd2, value);

    return 5;
  }

  default:
    if (cmd >= Opcode::GX_PRIMITIVE_START && cmd <= Opcode::GX_PRIMITIVE_END)
    {
      if (available < 3)
        return 0;

      const u8 cmdbyte = static_cast<u8>(cmd);
      const OpcodeDecoder::Primitive primitive = static_cast<OpcodeDecoder::Primitive>(
          (cmdbyte & OpcodeDecoder::GX_PRIMITIVE_MASK) >> OpcodeDecoder::GX_PRIMITIVE_SHIFT);
      const u8 vat = cmdbyte & OpcodeDecoder::GX_VAT_MASK;

      const u32 vertex_size = callback.GetVertexSize(vat);
      const u16 num_vertices = Common::swap16(&data[1]);

      if (available < 3 + num_vertices * vertex_size)
        return 0;

      callback.OnPrimitiveCommand(primitive, vat, vertex_size, num_vertices, &data[3]);

      return 3 + num_vertices * vertex_size;
    }
  }

  callback.OnUnknown(static_cast<u8>(cmd), data);
  return 1;
}
}  // namespace detail

DOLPHIN_FORCE_INLINE u32 RunCommand(const u8* data, u32 available,
                                    std::derived_from<Callback> auto& callback)
{
  const u32 size = detail::RunCommand(data, available, callback);
  if (size > 0)
  {
    callback.OnCommand(data, size);
  }
  return size;
}

DOLPHIN_FORCE_INLINE u32 Run(const u8* data, u32 available,
                             std::derived_from<Callback> auto& callback)
{
  u32 size = 0;
  while (size < available)
  {
    const u32 command_size = RunCommand(&data[size], available - size, callback);
    if (command_size == 0)
      break;
    size += command_size;
  }
  return size;
}

template <bool is_preprocess = false>
u8* RunFifo(DataReader src, u32* cycles);

}  // namespace OpcodeDecoder

template <>
struct fmt::formatter<OpcodeDecoder::Primitive>
    : EnumFormatter<OpcodeDecoder::Primitive::GX_DRAW_POINTS>
{
  static constexpr array_type names = {
      "GX_DRAW_QUADS",        "GX_DRAW_QUADS_2 (nonstandard)",
      "GX_DRAW_TRIANGLES",    "GX_DRAW_TRIANGLE_STRIP",
      "GX_DRAW_TRIANGLE_FAN", "GX_DRAW_LINES",
      "GX_DRAW_LINE_STRIP",   "GX_DRAW_POINTS",
  };
  constexpr formatter() : EnumFormatter(names) {}
};
