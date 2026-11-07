// Copyright 2011 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <string_view>
#include <unordered_map>

#include <retro_atomic.h>

#include "Common/AtomicMap.h"
#include "Common/GL/GLExtensions/GLExtensions.h"
#include "Common/Quiesce.h"
#include "VideoCommon/AsyncShaderCompiler.h"

namespace OGL
{
class OGLShader;
class GLVertexFormat;
class StreamBuffer;

struct SHADER
{
  void Destroy()
  {
    DestroyShaders();
    if (glprogid)
    {
      glDeleteProgram(glprogid);
      glprogid = 0;
    }
  }

  GLuint vsid = 0;
  GLuint gsid = 0;
  GLuint psid = 0;
  GLuint glprogid = 0;

  void SetProgramVariables();
  void SetProgramBindings(bool is_compute);
  void Bind() const;
  void DestroyShaders();
};

struct PipelineProgramKey
{
  u64 vertex_shader_id;
  u64 geometry_shader_id;
  u64 pixel_shader_id;

  bool operator==(const PipelineProgramKey& rhs) const;
  bool operator<(const PipelineProgramKey& rhs) const;
};

struct PipelineProgram
{
  quiesce_node_t node{};  // first: the free callback casts it back
  PipelineProgramKey key{};
  SHADER shader;
  retro_atomic_int_t reference_count{1};
  bool binary_retrieved = false;
};

class ProgramShaderCache
{
public:
  static void BindVertexFormat(const GLVertexFormat* vertex_format);
  static void ReBindVertexFormat();
  static bool IsValidVertexFormatBound();
  static void InvalidateVertexFormat();
  static void InvalidateVertexFormatIfBound(GLuint vao);
  static void InvalidateLastProgram();

  static bool CompileComputeShader(SHADER& shader, std::string_view code);
  static GLuint CompileSingleShader(GLenum type, std::string_view code);
  static bool CheckShaderCompileResult(GLuint id, GLenum type, std::string_view code);
  static bool CheckProgramLinkResult(GLuint id, std::string_view vcode, std::string_view pcode,
                                     std::string_view gcode);
  static StreamBuffer* GetUniformBuffer();
  static u32 GetUniformBufferAlignment();
  static void UploadConstants();
  static void UploadConstants(const void* data, u32 data_size);

  static void Init();
  static void Shutdown();
  static void CreateHeader();

  // This counter increments with each shader object allocated, in order to give it a unique ID.
  // Since the shaders can be destroyed after a pipeline is created, we can't use the shader pointer
  // as a key for GL programs. For the same reason, we can't use the GL objects either. This ID is
  // guaranteed to be unique for the emulation session, even if the memory allocator or GL driver
  // re-uses pointers, therefore we won't have any collisions where the shaders attached to a
  // pipeline do not match the pipeline configuration.
  static u64 GenerateShaderID();

  static PipelineProgram* GetPipelineProgram(const GLVertexFormat* vertex_format,
                                             const OGLShader* vertex_shader,
                                             const OGLShader* geometry_shader,
                                             const OGLShader* pixel_shader, const void* cache_data,
                                             size_t cache_data_size);
  static void ReleasePipelineProgram(PipelineProgram* prog);

private:
  // Programs are made on the video thread and on the compile workers' shared contexts: one
  // slot per key, holding the live program. A program whose count fell to 0 is dead and may
  // be replaced; it is freed once no thread is looking at it.
  static bool TryReference(PipelineProgram* prog);
  static void FreePipelineProgram(quiesce_node_t* node);

  static void CreateAttributelessVAO();

  static atomic_slot_map_t* s_pipeline_programs;  // keyed by the PipelineProgramKey bytes
  static quiesce_t s_pipeline_program_quiesce;

  static u32 s_ubo_buffer_size;
  static s32 s_ubo_align;

  static GLuint s_attributeless_VBO;
  static GLuint s_attributeless_VAO;
  static GLuint s_last_VAO;
};

}  // namespace OGL
