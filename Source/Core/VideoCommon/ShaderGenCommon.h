// Copyright 2012 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>

#include <fmt/format.h>

#include "Common/BitField.h"
#include "Common/CommonTypes.h"
#include "Common/EnumMap.h"
#include "Common/StringUtil.h"
#include "Common/TypeUtils.h"

#include "VideoCommon/AbstractShader.h"
#include "VideoCommon/ShaderBuild.h"
#include "VideoCommon/ShaderText.h"
#include "VideoCommon/VideoCommon.h"

/**
 * Common interface for classes that need to go through the shader generation path
 * (GenerateVertexShader, GenerateGeometryShader, GeneratePixelShader)
 * In particular, this includes the shader code generator (ShaderCode).
 * A different class (ShaderUid) can be used to uniquely identify each ShaderCode object.
 * More interesting things can be done with this, e.g. checking what shader
 * constants are being used. This can be used to optimize buffer management.
 * If the class does not use one or more of these methods (e.g. Uid class does not need code), the
 * method will be defined as a no-op by the base class, and the call
 * should be optimized out. The reason for this implementation is so that shader
 * selection/generation can be done in two passes, with only a cache lookup being
 * required if the shader has already been generated.
 */
class ShaderGeneratorInterface
{
public:
  /*
   * Used when the shader generator would write a piece of ShaderCode.
   * Can be used like printf.
   * @note In the ShaderCode implementation, this does indeed write the parameter string to an
   * internal buffer. However, you're free to do whatever you like with the parameter.
   */
  void Write(const char*, ...)
#ifdef __GNUC__
      __attribute__((format(printf, 2, 3)))
#endif
  {
  }

  /*
   * Tells us that a specific constant range (including last_index) is being used by the shader
   */
  void SetConstantsUsed(unsigned int first_index, unsigned int last_index) {}
};

/*
 * Shader UID class used to uniquely identify the ShaderCode output written in the shader generator.
 * uid_data can be any struct of parameters that uniquely identify each shader code output.
 * Unless performance is not an issue, uid_data should be tightly packed to reduce memory footprint.
 * Shader generators will write to specific uid_data fields; ShaderUid methods will only read raw
 * u32 values from a union.
 * NOTE: Because LinearDiskCache reads and writes the storage associated with a ShaderUid instance,
 * ShaderUid must be trivially copyable.
 */
template <class uid_data>
class ShaderUid : public ShaderGeneratorInterface
{
public:
  static_assert(std::is_trivially_copyable_v<uid_data>,
                "uid_data must be a trivially copyable type");

  ShaderUid() { memset(GetUidData(), 0, GetUidDataSize()); }

  bool operator==(const ShaderUid& obj) const
  {
    return memcmp(GetUidData(), obj.GetUidData(), GetUidDataSize()) == 0;
  }

  // determines the storage order inside STL containers
  bool operator<(const ShaderUid& obj) const
  {
    return memcmp(GetUidData(), obj.GetUidData(), GetUidDataSize()) < 0;
  }

  // Returns a pointer to an internally stored object of the uid_data type.
  uid_data* GetUidData() { return &data; }

  // Returns a pointer to an internally stored object of the uid_data type.
  const uid_data* GetUidData() const { return &data; }

  // Returns the raw bytes that make up the shader UID.
  const u8* GetUidDataRaw() const { return reinterpret_cast<const u8*>(&data); }

  // Returns the size of the underlying UID data structure in bytes.
  size_t GetUidDataSize() const { return sizeof(data); }

private:
  uid_data data{};
};

class ShaderCode : public ShaderGeneratorInterface
{
public:
  ShaderCode() { shader_text_init(&m_text, SHADER_TEXT_FIRST_CAP); }
  ~ShaderCode() { shader_text_free(&m_text); }
  ShaderCode(ShaderCode&& other) noexcept : m_text(other.m_text) { shader_text_init(&other.m_text, 0); }
  ShaderCode(const ShaderCode&) = delete;
  ShaderCode& operator=(const ShaderCode&) = delete;
  ShaderCode& operator=(ShaderCode&&) = delete;

  /* lent: valid until the code is written to again or goes */
  std::string_view GetBuffer() const
  {
    return std::string_view(shader_text_data(&m_text), shader_text_len(&m_text));
  }
  /* the same, NUL terminated */
  const char* CStr() const { return shader_text_data(&m_text); }
  /* head written in front of the text: the two as one span of head_len + Size() bytes, or
   * nullptr if head does not fit the room kept for it */
  const char* Prepend(const char* head, size_t head_len)
  {
    return shader_text_prepend(&m_text, head, head_len);
  }
  size_t Size() const { return shader_text_len(&m_text); }
  /* the text's block for a new owner: the code is empty after */
  shader_text Release()
  {
    shader_text text = m_text;
    shader_text_init(&m_text, 0);
    return text;
  }
  shader_text* Text() { return &m_text; }

  /* text as it is */
  void Puts(const char* s) { shader_text_puts(&m_text, s); }
  void Putn(const char* s, size_t n) { shader_text_putn(&m_text, s, n); }
  void Indent(unsigned n) { shader_text_indent(&m_text, n); }
  /* printf-style */
  void Printf(const char* format, ...) SHADER_TEXT_PRINTF(2, 3);

protected:
  shader_text m_text;
};

// An enum as shader text writes it: 0x<value>u and its name in a comment, from
// ShaderEnumValue and ShaderEnumName.
template <typename T>
unsigned ShaderEnumValue(T e)
{
  return static_cast<unsigned>(static_cast<std::make_unsigned_t<std::underlying_type_t<T>>>(
      static_cast<std::underlying_type_t<T>>(e)));
}
template <typename T>
const char* ShaderEnumName(T e)
{
  static constexpr fmt::formatter<T> formatter{};
  const char* const name = formatter.Name(e);
  return name ? name : "Invalid";
}

// Host config contains the settings which can influence generated shaders.
union ShaderHostConfig
{
  u32 bits;

  BitField<0, 1, bool, u32> msaa;
  BitField<1, 1, bool, u32> ssaa;
  BitField<2, 1, bool, u32> stereo;
  BitField<3, 1, bool, u32> wireframe;
  BitField<4, 1, bool, u32> per_pixel_lighting;
  BitField<5, 1, bool, u32> vertex_rounding;
  BitField<6, 1, bool, u32> fast_depth_calc;
  BitField<7, 1, bool, u32> bounding_box;
  BitField<8, 1, bool, u32> backend_dual_source_blend;
  BitField<9, 1, bool, u32> backend_geometry_shaders;
  BitField<10, 1, bool, u32> backend_early_z;
  BitField<11, 1, bool, u32> backend_bbox;
  BitField<12, 1, bool, u32> backend_gs_instancing;
  BitField<13, 1, bool, u32> backend_clip_control;
  BitField<14, 1, bool, u32> backend_ssaa;
  BitField<15, 1, bool, u32> backend_atomics;
  BitField<16, 1, bool, u32> backend_depth_clamp;
  BitField<17, 1, bool, u32> backend_reversed_depth_range;
  BitField<18, 1, bool, u32> backend_bitfield;
  BitField<19, 1, bool, u32> backend_dynamic_sampler_indexing;
  BitField<20, 1, bool, u32> backend_shader_framebuffer_fetch;
  BitField<21, 1, bool, u32> backend_logic_op;
  BitField<22, 1, bool, u32> backend_palette_conversion;
  BitField<23, 1, bool, u32> enable_validation_layer;
  BitField<24, 1, bool, u32> manual_texture_sampling;
  BitField<25, 1, bool, u32> manual_texture_sampling_custom_texture_sizes;
  BitField<26, 1, bool, u32> backend_sampler_lod_bias;
  BitField<27, 1, bool, u32> backend_dynamic_vertex_loader;
  BitField<28, 1, bool, u32> backend_vs_point_line_expand;
  BitField<29, 1, bool, u32> backend_gl_layer_in_fs;

  static ShaderHostConfig GetCurrent();
};

// Gets the filename of the specified type of cache object (e.g. vertex shader, pipeline).
std::string GetDiskShaderCacheFileName(APIType api_type, const char* type, bool include_gameid,
                                       bool include_host_config, bool include_api = true);

void WriteIsNanHeader(ShaderCode& out, APIType api_type);
void WriteBitfieldExtractHeader(ShaderCode& out, APIType api_type,
                                const ShaderHostConfig& host_config);

void GenerateVSOutputMembers(ShaderCode& object, u32 texgens, const ShaderHostConfig& host_config,
                             const char* qualifier);

void AssignVSOutputMembers(ShaderCode& object, const char* a, const char* b, u32 texgens,
                           const ShaderHostConfig& host_config);

void GenerateLineOffset(ShaderCode& object, const char* indent0, const char* indent1,
                        const char* pos_a, const char* pos_b, const char* sign);

void GenerateVSLineExpansion(ShaderCode& object, const char* indent, u32 texgens);

void GenerateVSPointExpansion(ShaderCode& object, const char* indent, u32 texgens);

/* ---- the same pieces, built by calls ---- */

/* An expression made afresh each time: var, var[index_var] or var[index]. */
struct sb_place
{
  struct rslc_var* var;
  struct rslc_var* index_var;
  int index;  // -1 for none
};
uint32_t sb_place_ref(const struct shader_build* sb, const struct sb_place* p);

/* VS_OUTPUT's members as GenerateVSOutputMembers lays them out, each with quals (RSLC_B_*);
 * the index of each in it, -1 where absent. */
struct vs_output_layout
{
  const struct rslc_type* types[20];
  const char* names[20];
  unsigned quals[20];
  u32 n;
  u32 texgens;
  int pos, colors_0, colors_1, clipDist0, clipDist1, tex0, clipPos, Normal, WorldPos;
};
void BuildVSOutputLayout(const struct shader_build* sb, u32 texgens,
                         const ShaderHostConfig& host_config, unsigned quals,
                         struct vs_output_layout* layout);
/* The member quals GetInterpolationQualifier gives: centroid or sample under MSAA. */
unsigned BuildInterpolationQuals(bool msaa, bool ssaa);
/* a.member = b.member, in AssignVSOutputMembers' order */
void BuildAssignVSOutputMembers(const struct shader_build* sb, const struct vs_output_layout* layout,
                                const struct sb_place* a, const struct sb_place* b,
                                const ShaderHostConfig& host_config);
/* struct Light, as s_lighting_struct declares it */
const struct rslc_type* BuildLightStruct(const struct shader_build* sb);
/* s_geometry_shader_uniforms' types and names, at types + n and names + n: returns n + 4 */
u32 BuildGeometryShaderUniforms(const struct shader_build* sb, const struct rslc_type** types,
                                const char** names, u32 n);
/* GenerateLineOffset: float2 offset, declared and set; pos_a and pos_b are float4 positions
 * made afresh by the calls, sign a float (nullptr for none) */
typedef uint32_t (*sb_expr_fn)(const struct shader_build* sb, const void* ctx);
struct rslc_var* BuildLineOffset(const struct shader_build* sb, struct rslc_var* clinept,
                                 sb_expr_fn pos_a, const void* a_ctx, sb_expr_fn pos_b,
                                 const void* b_ctx, struct rslc_var* sign);
/* dolphin_isnan: the function Direct3D needs (nullptr elsewhere), and a call of it */
struct rslc_func* BuildIsNanHeader(const struct shader_build* sb, APIType api_type);
uint32_t sb_isnan(const struct shader_build* sb, const struct rslc_func* isnan_fn, uint32_t e);

/* s_shader_uniforms' members, in order */
enum shader_uniform
{
  SU_COMPONENTS,
  SU_DUALTEXINFO,
  SU_NUMCOLORCHANS,
  SU_MISSING_COLOR_HEX,
  SU_MISSING_COLOR_VALUE,
  SU_POSNORMALMATRIX,
  SU_PROJECTION,
  SU_MATERIALS,
  SU_LIGHTS,
  SU_TEXMATRICES,
  SU_TRANSFORMMATRICES,
  SU_NORMALMATRICES,
  SU_POSTTRANSFORMMATRICES,
  SU_PIXELCENTERCORRECTION,
  SU_VIEWPORT_SIZE,
  SU_PACK1,
  SU_CACHED_NORMAL,
  SU_CACHED_TANGENT,
  SU_CACHED_BINORMAL,
  SU_VERTEX_STRIDE,
  SU_OFFSET_RAWNORMAL,
  SU_OFFSET_RAWTANGENT,
  SU_OFFSET_RAWBINORMAL,
  SU_OFFSET_RAWPOS,
  SU_OFFSET_POSMTX,
  SU_OFFSET_RAWCOLOR0,
  SU_OFFSET_RAWCOLOR1,
  SU_OFFSET_RAWTEX,
  SU_COUNT
};
/* s_shader_uniforms' types and names at types + n and names + n, light the struct Light:
 * returns n + SU_COUNT */
u32 BuildShaderUniforms(const struct shader_build* sb, const struct rslc_type* light,
                        const struct rslc_type** types, const char** names, u32 n);

/* What GenerateVSLineExpansion and GenerateVSPointExpansion read and write: o a VS_OUTPUT
 * laid out as layout, the GSBlock's members by name. */
struct vs_expansion
{
  struct rslc_var* o;
  const struct vs_output_layout* layout;
  struct rslc_var* is_right;
  struct rslc_var* is_bottom;
  struct rslc_var* other_pos;
  struct rslc_var* projection;
  struct rslc_var* clinept;
  struct rslc_var* texoffset;
};
void BuildVSLineExpansion(const struct shader_build* sb, const struct vs_expansion* x);
void BuildVSPointExpansion(const struct shader_build* sb, const struct vs_expansion* x);

// We use the flag "centroid" to fix some MSAA rendering bugs. With MSAA, the
// pixel shader will be executed for each pixel which has at least one passed sample.
// So there may be rendered pixels where the center of the pixel isn't in the primitive.
// As the pixel shader usually renders at the center of the pixel, this position may be
// outside the primitive. This will lead to sampling outside the texture, sign changes, ...
// As a workaround, we interpolate at the centroid of the coveraged pixel, which
// is always inside the primitive.
// Without MSAA, this flag is defined to have no effect.
const char* GetInterpolationQualifier(bool msaa, bool ssaa, bool in_glsl_interface_block = false,
                                      bool in = false);

// bitfieldExtract generator for BitField types: the text in a buffer of its own, for the
// statement it is written in.
struct ShaderBitfieldExtract
{
  char text[128];
};
template <auto ptr_to_bitfield_member>
ShaderBitfieldExtract BitfieldExtract(const char* source)
{
  using BitFieldT = Common::MemberType<ptr_to_bitfield_member>;
  ShaderBitfieldExtract extract;
  snprintf(extract.text, sizeof(extract.text), "bitfieldExtract(%s(%s), %u, %u)",
           BitFieldT::IsSigned() ? "int" : "uint", source,
           static_cast<u32>(BitFieldT::StartBit()), static_cast<u32>(BitFieldT::NumBits()));
  return extract;
}

// A tree of ifs on variable, values[i] written where it is i: indexes low <= i < high.
void WriteSwitchTree(ShaderCode& out, const char* variable, const char* const* values,
                     const char* const* names, u32 indent, u32 low, u32 high);

template <auto last_member>
void WriteSwitch(ShaderCode& out, const char* variable,
                 const Common::EnumMap<const char*, last_member>& values, u32 indent)
{
  using enum_type = decltype(last_member);
  const u32 count = static_cast<u32>(last_member) + 1;
  const char* value_texts[static_cast<u32>(last_member) + 1];
  const char* names[static_cast<u32>(last_member) + 1];
  u32 i;

  for (i = 0; i < count; i++)
  {
    value_texts[i] = values[static_cast<enum_type>(i)];
    names[i] = ShaderEnumName(static_cast<enum_type>(i));
  }
  WriteSwitchTree(out, variable, value_texts, names, indent, 0, count);
}

// Constant variable names
#define I_COLORS "color"
#define I_KCOLORS "k"
#define I_ALPHA "alphaRef"
#define I_TEXDIMS "texdim"
#define I_ZBIAS "czbias"
#define I_INDTEXSCALE "cindscale"
#define I_INDTEXMTX "cindmtx"
#define I_FOGCOLOR "cfogcolor"
#define I_FOGI "cfogi"
#define I_FOGF "cfogf"
#define I_FOGRANGE "cfogrange"
#define I_ZSLOPE "czslope"
#define I_EFBSCALE "cefbscale"

#define I_POSNORMALMATRIX "cpnmtx"
#define I_PROJECTION "cproj"
#define I_MATERIALS "cmtrl"
#define I_LIGHTS "clights"
#define I_TEXMATRICES "ctexmtx"
#define I_TRANSFORMMATRICES "ctrmtx"
#define I_NORMALMATRICES "cnmtx"
#define I_POSTTRANSFORMMATRICES "cpostmtx"
#define I_PIXELCENTERCORRECTION "cpixelcenter"
#define I_VIEWPORT_SIZE "cviewport"
#define I_CACHED_NORMAL "cnormal"
#define I_CACHED_TANGENT "ctangent"
#define I_CACHED_BINORMAL "cbinormal"

#define I_STEREOPARAMS "cstereo"
#define I_LINEPTPARAMS "clinept"
#define I_TEXOFFSET "ctexoffset"

static const char s_shader_uniforms[] = "\tuint    components;\n"
                                        "\tuint    xfmem_dualTexInfo;\n"
                                        "\tuint    xfmem_numColorChans;\n"
                                        "\tuint    missing_color_hex;\n"
                                        "\tfloat4  missing_color_value;\n"
                                        "\tfloat4 " I_POSNORMALMATRIX "[6];\n"
                                        "\tfloat4 " I_PROJECTION "[4];\n"
                                        "\tint4 " I_MATERIALS "[4];\n"
                                        "\tLight " I_LIGHTS "[8];\n"
                                        "\tfloat4 " I_TEXMATRICES "[24];\n"
                                        "\tfloat4 " I_TRANSFORMMATRICES "[64];\n"
                                        "\tfloat4 " I_NORMALMATRICES "[32];\n"
                                        "\tfloat4 " I_POSTTRANSFORMMATRICES "[64];\n"
                                        "\tfloat4 " I_PIXELCENTERCORRECTION ";\n"
                                        "\tfloat2 " I_VIEWPORT_SIZE ";\n"
                                        "\tuint4   xfmem_pack1[8];\n"
                                        "\tfloat4 " I_CACHED_NORMAL ";\n"
                                        "\tfloat4 " I_CACHED_TANGENT ";\n"
                                        "\tfloat4 " I_CACHED_BINORMAL ";\n"
                                        "\tuint vertex_stride;\n"
                                        "\tuint vertex_offset_rawnormal;\n"
                                        "\tuint vertex_offset_rawtangent;\n"
                                        "\tuint vertex_offset_rawbinormal;\n"
                                        "\tuint vertex_offset_rawpos;\n"
                                        "\tuint vertex_offset_posmtx;\n"
                                        "\tuint vertex_offset_rawcolor0;\n"
                                        "\tuint vertex_offset_rawcolor1;\n"
                                        "\tuint4 vertex_offset_rawtex[2];\n"  // std140 is pain
                                        "\t#define xfmem_texMtxInfo(i) (xfmem_pack1[(i)].x)\n"
                                        "\t#define xfmem_postMtxInfo(i) (xfmem_pack1[(i)].y)\n"
                                        "\t#define xfmem_color(i) (xfmem_pack1[(i)].z)\n"
                                        "\t#define xfmem_alpha(i) (xfmem_pack1[(i)].w)\n";

static const char s_geometry_shader_uniforms[] = "\tfloat4 " I_STEREOPARAMS ";\n"
                                                 "\tfloat4 " I_LINEPTPARAMS ";\n"
                                                 "\tint4 " I_TEXOFFSET ";\n"
                                                 "\tuint vs_expand;\n";

