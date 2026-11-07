// Copyright 2014 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/GeometryShaderGen.h"

#include "Common/CommonTypes.h"
#include "Common/EnumMap.h"
#include "VideoCommon/DriverDetails.h"
#include "VideoCommon/LightingShaderGen.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/XFMemory.h"

constexpr Common::EnumMap<const char*, PrimitiveType::TriangleStrip> primitives_ogl{
    "points",
    "lines",
    "triangles",
    "triangles",
};

constexpr Common::EnumMap<u32, PrimitiveType::TriangleStrip> vertex_in_map{1u, 2u, 3u, 3u};
constexpr Common::EnumMap<u32, PrimitiveType::TriangleStrip> vertex_out_map{4u, 4u, 4u, 3u};

bool geometry_shader_uid_data::IsPassthrough() const
{
  const bool stereo = g_ActiveConfig.stereo_mode != StereoMode::Off;
  const bool wireframe = g_ActiveConfig.bWireFrame;
  return primitive_type >= static_cast<u32>(PrimitiveType::Triangles) && !stereo && !wireframe;
}

GeometryShaderUid GetGeometryShaderUid(PrimitiveType primitive_type)
{
  GeometryShaderUid out;

  geometry_shader_uid_data* const uid_data = out.GetUidData();
  uid_data->primitive_type = static_cast<u32>(primitive_type);
  uid_data->numTexGens = xfmem.numTexGen.numTexGens;

  return out;
}

static void EmitVertex(ShaderCode& out, const ShaderHostConfig& host_config,
                       const geometry_shader_uid_data* uid_data, const char* vertex,
                       APIType api_type, bool wireframe, bool stereo, bool first_vertex = false);
static void EndPrimitive(ShaderCode& out, const ShaderHostConfig& host_config,
                         const geometry_shader_uid_data* uid_data, APIType api_type, bool wireframe,
                         bool stereo);

ShaderCode GenerateGeometryShaderCode(APIType api_type, const ShaderHostConfig& host_config,
                                      const geometry_shader_uid_data* uid_data)
{
  ShaderCode out;
  // Non-uid template parameters will write to the dummy data (=> gets optimized out)

  const bool wireframe = host_config.wireframe;
  const bool msaa = host_config.msaa;
  const bool ssaa = host_config.ssaa;
  const bool stereo = host_config.stereo;
  const auto primitive_type = static_cast<PrimitiveType>(uid_data->primitive_type);
  const u32 vertex_in = vertex_in_map[primitive_type];
  u32 vertex_out = vertex_out_map[primitive_type];

  if (wireframe)
    vertex_out++;

  // Insert layout parameters
  if (host_config.backend_gs_instancing)
  {
    out.Printf("layout(%s, invocations = %d) in;\n", primitives_ogl[primitive_type], stereo ? 2 : 1);
    out.Printf("layout(%s_strip, max_vertices = %u) out;\n", wireframe ? "line" : "triangle", vertex_out);
  }
  else
  {
    out.Printf("layout(%s) in;\n", primitives_ogl[primitive_type]);
    out.Printf("layout(%s_strip, max_vertices = %u) out;\n", wireframe ? "line" : "triangle", stereo ? vertex_out * 2 : vertex_out);
  }

  out.Puts(s_lighting_struct);

  /* uniforms; Direct3D binds the stage's block at b0 */
  out.Printf("UBO_BINDING(std140, %d) uniform GSBlock {\n", api_type == APIType::D3D ? 1 : 4);

  out.Puts(s_geometry_shader_uniforms);
  out.Puts("};\n");

  out.Puts("struct VS_OUTPUT {\n");
  GenerateVSOutputMembers(out, uid_data->numTexGens, host_config, "");
  out.Puts("};\n");

  if (host_config.backend_gs_instancing)
    out.Puts("#define InstanceID gl_InvocationID\n");

  out.Puts("VARYING_LOCATION(0) in VertexData {\n");
  GenerateVSOutputMembers(out, uid_data->numTexGens, host_config,
                          GetInterpolationQualifier(msaa, ssaa, true, true));
  out.Printf("} vs[%u];\n", vertex_in);

  out.Puts("VARYING_LOCATION(0) out VertexData {\n");
  GenerateVSOutputMembers(out, uid_data->numTexGens, host_config,
                          GetInterpolationQualifier(msaa, ssaa, true, false));

  out.Puts("} ps;\n");
  if (stereo && !host_config.backend_gl_layer_in_fs)
    out.Puts("flat out int layer;");

  out.Puts("void main()\n"
            "{\n");

  if (primitive_type == PrimitiveType::Lines)
  {
    out.Puts("\tVS_OUTPUT start, end;\n");
    AssignVSOutputMembers(out, "start", "vs[0]", uid_data->numTexGens, host_config);
    AssignVSOutputMembers(out, "end", "vs[1]", uid_data->numTexGens, host_config);

    GenerateLineOffset(out, "\t", "\t\t", "end.pos", "start.pos", "");
  }
  else if (primitive_type == PrimitiveType::Points)
  {
    out.Puts("\tVS_OUTPUT center;\n");
    AssignVSOutputMembers(out, "center", "vs[0]", uid_data->numTexGens, host_config);

    // Offset from center to upper right vertex
    // Lerp PointSize/2 from [0,0..VpWidth,VpHeight] to [-1,1..1,-1]
    out.Puts("\tfloat2 offset = float2("
              I_LINEPTPARAMS
              ".w / "
              I_LINEPTPARAMS
              ".x, -"
              I_LINEPTPARAMS
              ".w / "
              I_LINEPTPARAMS
              ".y) * center.pos.w;\n");
  }

  if (stereo)
  {
    // If the GPU supports invocation we don't need a for loop and can simply use the
    // invocation identifier to determine which layer we're rendering.
    if (host_config.backend_gs_instancing)
      out.Puts("\tint eye = InstanceID;\n");
    else
      out.Puts("\tfor (int eye = 0; eye < 2; ++eye) {\n");
  }

  if (wireframe)
    out.Puts("\tVS_OUTPUT first;\n");

  // Avoid D3D warning about forced unrolling of single-iteration loop
  if (vertex_in > 1)
    out.Printf("\tfor (int i = 0; i < %u; ++i) {\n", vertex_in);
  else
    out.Puts("\tint i = 0;\n");

  out.Puts("\tVS_OUTPUT f;\n");
  AssignVSOutputMembers(out, "f", "vs[i]", uid_data->numTexGens, host_config);

  if (host_config.backend_depth_clamp &&
      DriverDetails::HasBug(DriverDetails::BUG_BROKEN_CLIP_DISTANCE))
  {
    // On certain GPUs we have to consume the clip distance from the vertex shader
    // or else the other vertex shader outputs will get corrupted.
    out.Puts("\tf.clipDist0 = gl_in[i].gl_ClipDistance[0];\n"
              "\tf.clipDist1 = gl_in[i].gl_ClipDistance[1];\n");
  }

  if (stereo)
  {
    // For stereoscopy add a small horizontal offset in Normalized Device Coordinates proportional
    // to the depth of the vertex. We retrieve the depth value from the w-component of the projected
    // vertex which contains the negated z-component of the original vertex.
    // For negative parallax (out-of-screen effects) we subtract a convergence value from
    // the depth value. This results in objects at a distance smaller than the convergence
    // distance to seemingly appear in front of the screen.
    // This formula is based on page 13 of the "Nvidia 3D Vision Automatic, Best Practices Guide"
    out.Puts("\tfloat hoffset = (eye == 0) ? "
              I_STEREOPARAMS
              ".x : "
              I_STEREOPARAMS
              ".y;\n");
    out.Puts("\tf.pos.x += hoffset * (f.pos.w - "
              I_STEREOPARAMS
              ".z);\n");
  }

  if (primitive_type == PrimitiveType::Lines)
  {
    out.Puts("\tVS_OUTPUT l = f;\n"
              "\tVS_OUTPUT r = f;\n");

    out.Puts("\tl.pos.xy -= offset * l.pos.w;\n"
              "\tr.pos.xy += offset * r.pos.w;\n");

    out.Puts("\tif ("
              I_TEXOFFSET
              "[2] != 0) {\n");
    out.Puts("\tfloat texOffset = 1.0 / float("
              I_TEXOFFSET
              "[2]);\n");

    for (u32 i = 0; i < uid_data->numTexGens; ++i)
    {
      out.Printf("\tif ((("
                I_TEXOFFSET
                "[0] >> %u) & 0x1) != 0)\n", i);
      out.Printf("\t\tr.tex%u.x += texOffset;\n", i);
    }
    out.Puts("\t}\n");

    EmitVertex(out, host_config, uid_data, "l", api_type, wireframe, stereo, true);
    EmitVertex(out, host_config, uid_data, "r", api_type, wireframe, stereo);
  }
  else if (primitive_type == PrimitiveType::Points)
  {
    out.Puts("\tVS_OUTPUT ll = f;\n"
              "\tVS_OUTPUT lr = f;\n"
              "\tVS_OUTPUT ul = f;\n"
              "\tVS_OUTPUT ur = f;\n");

    out.Puts("\tll.pos.xy += float2(-1,-1) * offset;\n"
              "\tlr.pos.xy += float2(1,-1) * offset;\n"
              "\tul.pos.xy += float2(-1,1) * offset;\n"
              "\tur.pos.xy += offset;\n");

    out.Puts("\tif ("
              I_TEXOFFSET
              "[3] != 0) {\n");
    out.Puts("\tfloat2 texOffset = float2(1.0 / float("
              I_TEXOFFSET
              "[3]), 1.0 / float("
              I_TEXOFFSET
              "[3]));\n");

    for (u32 i = 0; i < uid_data->numTexGens; ++i)
    {
      out.Printf("\tif ((("
                I_TEXOFFSET
                "[1] >> %u) & 0x1) != 0) {\n", i);
      out.Printf("\t\tul.tex%u.xy += float2(0,1) * texOffset;\n", i);
      out.Printf("\t\tur.tex%u.xy += texOffset;\n", i);
      out.Printf("\t\tlr.tex%u.xy += float2(1,0) * texOffset;\n", i);
      out.Puts("\t}\n");
    }
    out.Puts("\t}\n");

    EmitVertex(out, host_config, uid_data, "ll", api_type, wireframe, stereo, true);
    EmitVertex(out, host_config, uid_data, "lr", api_type, wireframe, stereo);
    EmitVertex(out, host_config, uid_data, "ul", api_type, wireframe, stereo);
    EmitVertex(out, host_config, uid_data, "ur", api_type, wireframe, stereo);
  }
  else
  {
    EmitVertex(out, host_config, uid_data, "f", api_type, wireframe, stereo, true);
  }

  // Only close loop if previous code was in one (See D3D warning above)
  if (vertex_in > 1)
    out.Puts("\t}\n");

  EndPrimitive(out, host_config, uid_data, api_type, wireframe, stereo);

  if (stereo && !host_config.backend_gs_instancing)
    out.Puts("\t}\n");

  out.Puts("}\n");

  return out;
}

static void EmitVertex(ShaderCode& out, const ShaderHostConfig& host_config,
                       const geometry_shader_uid_data* uid_data, const char* vertex,
                       APIType api_type, bool wireframe, bool stereo, bool first_vertex)
{
  if (wireframe && first_vertex)
    out.Printf("\tif (i == 0) first = %s;\n", vertex);

  // Vulkan NDC space has Y pointing down (right-handed NDC space).
  if (api_type == APIType::Vulkan)
    out.Printf("\tgl_Position = float4(%s.pos.x, -%s.pos.y, %s.pos.z, %s.pos.w);\n", vertex, vertex, vertex, vertex);
  else
    out.Printf("\tgl_Position = %s.pos;\n", vertex);

  if (host_config.backend_depth_clamp)
  {
    out.Printf("\tgl_ClipDistance[0] = %s.clipDist0;\n", vertex);
    out.Printf("\tgl_ClipDistance[1] = %s.clipDist1;\n", vertex);
  }
  AssignVSOutputMembers(out, "ps", vertex, uid_data->numTexGens, host_config);

  if (stereo)
  {
    // Select the output layer
    out.Puts("\tgl_Layer = eye;\n");
    if (!host_config.backend_gl_layer_in_fs)
      out.Puts("\tlayer = eye;\n");
  }

  out.Puts("\tEmitVertex();\n");
}

static void EndPrimitive(ShaderCode& out, const ShaderHostConfig& host_config,
                         const geometry_shader_uid_data* uid_data, APIType api_type, bool wireframe,
                         bool stereo)
{
  if (wireframe)
    EmitVertex(out, host_config, uid_data, "first", api_type, wireframe, stereo);

  out.Puts("\tEndPrimitive();\n");
}

u32 EnumerateGeometryShaderUids(GeometryShaderUid* uids)
{
  const PrimitiveType primitives[3] = {g_backend_info.bSupportsPrimitiveRestart ?
                                           PrimitiveType::TriangleStrip :
                                           PrimitiveType::Triangles,
                                       PrimitiveType::Lines, PrimitiveType::Points};
  u32 count = 0;
  u32 p, texgens;

  for (p = 0; p < 3; p++)
  {
    for (texgens = 0; texgens <= 8; texgens++)
    {
      geometry_shader_uid_data* const guid = uids[count].GetUidData();
      uids[count] = GeometryShaderUid();
      guid->primitive_type = static_cast<u32>(primitives[p]);
      guid->numTexGens = texgens;
      count++;
    }
  }
  return count;
}
