// Copyright 2018 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <cstring>
#include <set>
#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>

#include "Common/CommonTypes.h"
#include "Common/IOFile.h"
#include "Common/LinearDiskCache.h"

#include "VideoCommon/AbstractPipeline.h"
#include "VideoCommon/AbstractShader.h"
#include "VideoCommon/AsyncShaderCompiler.h"
#include "VideoCommon/UidTable.h"
#include "VideoCommon/GXPipelineTypes.h"
#include "VideoCommon/GeometryShaderGen.h"
#include "VideoCommon/PixelShaderGen.h"
#include "VideoCommon/RenderState.h"
#include "VideoCommon/TextureCacheBase.h"
#include "VideoCommon/TextureConversionShader.h"
#include "VideoCommon/TextureConverterShaderGen.h"
#include "VideoCommon/UberShaderPixel.h"
#include "VideoCommon/UberShaderVertex.h"
#include "VideoCommon/VertexShaderGen.h"
#include "VideoCommon/VideoEvents.h"

class NativeVertexFormat;
enum class AbstractTextureFormat : u32;
enum class APIType;
enum class TextureFormat;
enum class TLUTFormat;

namespace VideoCommon
{
/* The state draws with a GX pipeline are in, where the backend sets some of it per draw
 * (BackendInfo::uDrawState*Bits). */
struct GXDrawState
{
  RasterizationState rasterization;
  DepthState depth;
  BlendingState blending;
};

class ShaderCache final
{
public:
  /* Whether pipelines can build on threads of their own: without, asking
   * for one in the background builds it on the spot all the same. */
  bool CanCompileInBackground() const;

  ShaderCache();
  ~ShaderCache();

  // Perform at startup, create descriptor layouts, compiles all static shaders.
  bool Initialize();
  void Shutdown();

  // Compiles/loads cached shaders.
  void InitializeShaderCache();

  // Changes the shader host config. Shaders should be reloaded afterwards.
  void SetHostConfig(const ShaderHostConfig& host_config) { m_host_config.bits = host_config.bits; }

  // Reloads/recreates all shaders and pipelines.
  void Reload();

  // Retrieves all pending shaders/pipelines from the async compiler.
  void RetrieveAsyncShaders();

  // Accesses ShaderGen shader caches
  const AbstractPipeline* GetPipelineForUid(const GXPipelineUid& uid,
                                            GXDrawState* draw_state = nullptr);
  /* draw_state, if not null, gets the state its draws are in, for AbstractGfx::SetDrawState */
  const AbstractPipeline* GetUberPipelineForUid(const GXUberPipelineUid& uid,
                                                GXDrawState* draw_state = nullptr);

  // Accesses ShaderGen shader caches asynchronously.
  // The optional will be empty if this pipeline is now background compiling.
  std::optional<const AbstractPipeline*> GetPipelineForUidAsync(const GXPipelineUid& uid,
                                                                GXDrawState* draw_state = nullptr);

  // Shared shaders
  const AbstractShader* GetScreenQuadVertexShader() const
  {
    return m_screen_quad_vertex_shader;
  }
  const AbstractShader* GetTextureCopyVertexShader() const
  {
    return m_texture_copy_vertex_shader;
  }
  const AbstractShader* GetEFBCopyVertexShader() const { return m_efb_copy_vertex_shader; }
  const AbstractShader* GetTexcoordGeometryShader() const
  {
    return m_texcoord_geometry_shader;
  }
  const AbstractShader* GetTextureCopyPixelShader() const
  {
    return m_texture_copy_pixel_shader;
  }
  const AbstractShader* GetColorGeometryShader() const { return m_color_geometry_shader; }
  const AbstractShader* GetColorPixelShader() const { return m_color_pixel_shader; }

  // EFB copy to RAM/VRAM pipelines
  /* The pipeline for an EFB copy to VRAM drawn in format (TextureConversionShaderGen::
   * GetDrawnFormat), its uniforms saying the rest (GetCopyFlags): the one built for the format,
   * or, until it is, queued to be, the one for every format. */
  const AbstractPipeline* GetEFBCopyToVRAMPipeline(EFBCopyFormat format);
  /* null where its encoder failed to build */
  const AbstractPipeline* GetEFBCopyToRAMPipeline(EFBCopyFormat format);

  // RGBA8 framebuffer copy pipelines
  const AbstractPipeline* GetRGBA8CopyPipeline() const { return m_copy_rgba8_pipeline; }
  const AbstractPipeline* GetRGBA8StereoCopyPipeline() const
  {
    return m_rgba8_stereo_copy_pipeline;
  }

  // Palette texture conversion pipelines
  const AbstractPipeline* GetPaletteConversionPipeline(TLUTFormat format);

  // Texture reinterpret pipelines
  const AbstractPipeline* GetTextureReinterpretPipeline(TextureFormat from_format,
                                                        TextureFormat to_format);

  // Texture decoding compute shaders
  const AbstractShader* GetTextureDecodingShader(TextureFormat format,
                                                 std::optional<TLUTFormat> palette_format);

private:
  static constexpr size_t NUM_PALETTE_CONVERSION_SHADERS = 3;

  void WaitForAsyncCompiler();
  void LoadCaches();
  void ClearCaches();
  void LoadPipelineUIDCache();
  void ClosePipelineUIDCache();
  void CompileMissingPipelines();
  void QueueUberShaderPipelines();
  /* format null: the pipeline for every format */
  AbstractPipeline* CompileEFBCopyToVRAMPipeline(const EFBCopyFormat* format) const;
  AbstractPipeline* CompileEFBCopyToRAMPipeline(EFBCopyFormat format) const;
  bool QueueEFBCopyToRAMPipelines();
  bool CompileSharedPipelines();
  void CompileTextureConversionPipelines();
  const AbstractPipeline* BuildTextureReinterpretPipeline(TextureFormat from_format,
                                                          TextureFormat to_format);
  const AbstractShader* BuildTextureDecodingShader(TextureFormat format,
                                                   std::optional<TLUTFormat> palette_format);

  // GX shader compiler methods
  AbstractShader* CompileVertexShader(const VertexShaderUid& uid) const;
  AbstractShader*
  CompileVertexUberShader(const UberShader::VertexShaderUid& uid) const;
  AbstractShader* CompilePixelShader(const PixelShaderUid& uid) const;
  AbstractShader*
  CompilePixelUberShader(const UberShader::PixelShaderUid& uid) const;
  const AbstractShader* InsertVertexShader(const VertexShaderUid& uid,
                                           AbstractShader* shader);
  const AbstractShader* InsertVertexUberShader(const UberShader::VertexShaderUid& uid,
                                               AbstractShader* shader);
  const AbstractShader* InsertPixelShader(const PixelShaderUid& uid,
                                          AbstractShader* shader);
  const AbstractShader* InsertPixelUberShader(const UberShader::PixelShaderUid& uid,
                                              AbstractShader* shader);
  const AbstractShader* CreateGeometryShader(const GeometryShaderUid& uid);
  bool NeedsGeometryShader(const GeometryShaderUid& uid) const;

  // Should we use geometry shaders for EFB copies?
  bool UseGeometryShaderForEFBCopies() const;

  // GX pipeline compiler methods
  AbstractPipelineConfig
  GetGXPipelineConfig(const NativeVertexFormat* vertex_format, const AbstractShader* vertex_shader,
                      const AbstractShader* geometry_shader, const AbstractShader* pixel_shader,
                      const RasterizationState& rasterization_state, const DepthState& depth_state,
                      const BlendingState& blending_state, AbstractPipelineUsage usage);
  std::optional<AbstractPipelineConfig> GetGXPipelineConfig(const GXPipelineUid& uid);
  std::optional<AbstractPipelineConfig> GetGXPipelineConfig(const GXUberPipelineUid& uid);
  void WriteGXPipelineCacheData(const GXPipelineUid& config, const AbstractPipeline* pipeline);
  const AbstractPipeline* InsertGXPipeline(const GXPipelineUid& config,
                                           AbstractPipeline* pipeline);
  const AbstractPipeline* InsertGXUberPipeline(const GXUberPipelineUid& config,
                                               AbstractPipeline* pipeline);
  void AddSerializedGXPipelineUID(const SerializedGXPipelineUid& uid);
  void AppendGXPipelineUID(const GXPipelineUid& config);
  void LoadUberPipelineUIDCache();
  void AppendGXUberPipelineUID(const GXUberPipelineUid& config);
  /* queues the ubershader pipelines known and not built yet */
  void CompileMissingUberPipelines();

  // ASync Compiler Methods
  void QueueVertexShaderCompile(const VertexShaderUid& uid, u32 priority);
  void QueueVertexUberShaderCompile(const UberShader::VertexShaderUid& uid, u32 priority);
  void QueuePixelShaderCompile(const PixelShaderUid& uid, u32 priority);
  void QueuePixelUberShaderCompile(const UberShader::PixelShaderUid& uid, u32 priority);
  void QueuePipelineCompile(const GXPipelineUid& uid, u32 priority);
  void QueueUberPipelineCompile(const GXUberPipelineUid& uid, u32 priority);

  // The compiler's work items, kept here and reused (ShaderCache.cpp). Video thread only.
  struct WorkItem;
  struct WorkChunk;
  WorkItem* AllocWork(u8 kind, u32 priority);
  void ReleaseWork(WorkItem* item);
  void QueueWork(WorkItem* item);
  static void WorkCompile(AsyncShaderWork* work);
  static void WorkRetrieve(AsyncShaderWork* work);
  static void WorkDiscard(AsyncShaderWork* work);
  WorkItem* m_free_work = nullptr;
  WorkChunk* m_work_chunks = nullptr;

  // Populating various caches.
  template <ShaderStage stage, typename K, typename T>
  void LoadShaderCache(T& cache, APIType api_type, const char* type, bool include_gameid);
  template <typename T>
  void ClearShaderCache(T& cache);
  template <typename KeyType, typename DiskKeyType, typename T>
  void LoadPipelineCache(T& cache, Common::LinearDiskCache<DiskKeyType, u8>& disk_cache,
                         APIType api_type, const char* type, bool include_gameid);
  template <typename T, typename Y>
  void ClearPipelineCache(T& cache, Y& disk_cache);

  // Priorities for compiling. The lower the value, the sooner the pipeline is compiled.
  // The shader cache is compiled last, as it is the least likely to be required. On demand
  // shaders are always compiled before pending ubershaders, as we want to use the ubershader
  // for as few frames as possible, otherwise we risk framerate drops.
  enum : u32
  {
    COMPILE_PRIORITY_ONDEMAND_PIPELINE = 100,
    COMPILE_PRIORITY_UBERSHADER_PIPELINE = 200,
    COMPILE_PRIORITY_SHADERCACHE_PIPELINE = 300
  };

  // Configuration bits.
  APIType m_api_type;
  ShaderHostConfig m_host_config = {};
  AsyncShaderCompiler m_async_shader_compiler;

  // Shared shaders
  AbstractShader* m_screen_quad_vertex_shader = nullptr;
  AbstractShader* m_texture_copy_vertex_shader = nullptr;
  AbstractShader* m_efb_copy_vertex_shader = nullptr;
  AbstractShader* m_texcoord_geometry_shader = nullptr;
  AbstractShader* m_color_geometry_shader = nullptr;
  AbstractShader* m_texture_copy_pixel_shader = nullptr;
  AbstractShader* m_color_pixel_shader = nullptr;

  // GX Shader Caches: each uid's AbstractShader, owned here, and whether it is compiling
  template <typename Uid>
  struct ShaderModuleCache
  {
    ShaderModuleCache() { uid_table_init(&shader_map, sizeof(Uid)); }
    ~ShaderModuleCache() { uid_table_free(&shader_map); }
    uid_table shader_map;
    Common::LinearDiskCache<Uid, u8> disk_cache;
  };
  ShaderModuleCache<VertexShaderUid> m_vs_cache;
  ShaderModuleCache<GeometryShaderUid> m_gs_cache;
  ShaderModuleCache<PixelShaderUid> m_ps_cache;
  ShaderModuleCache<UberShader::VertexShaderUid> m_uber_vs_cache;
  ShaderModuleCache<UberShader::PixelShaderUid> m_uber_ps_cache;

  // GX Pipeline Caches: each uid's AbstractPipeline, owned here, and whether it is compiling
  uid_table m_gx_pipeline_cache;
  uid_table m_gx_uber_pipeline_cache;
  File::IOFile m_gx_pipeline_uid_cache_file;
  uid_array m_pending_gx_pipeline_uids; /* SerializedGXPipelineUid */
  /* The ubershader pipelines a game drew with, as for the specialized
   * ones: compiled while the game boots next time, so a hybrid draw
   * finds its ubershader ready instead of building it on the spot. */
  File::IOFile m_gx_uber_pipeline_uid_cache_file;
  uid_array m_pending_gx_uber_pipeline_uids; /* SerializedGXUberPipelineUid */
  /* those already in the file or written to it this session */
  uid_table m_recorded_uber_pipeline_uids;
  // Pipelines whose cache data is written once their compile, on the driver's threads, is done.
  uid_array m_unwritten_gx_pipelines; /* GXPipelineUid */
  Common::LinearDiskCache<SerializedGXPipelineUid, u8> m_gx_pipeline_disk_cache;
  Common::LinearDiskCache<SerializedGXUberPipelineUid, u8> m_gx_uber_pipeline_disk_cache;

  // EFB copy to VRAM/RAM pipelines
  AbstractPipeline* m_efb_copy_to_vram_pipeline = nullptr; /* every format */
  /* by EFBCopyFormat: built, and whether one is queued or built */
  AbstractPipeline* m_efb_copy_format_pipelines[16] = {};
  bool m_efb_copy_format_asked[16] = {};
  AbstractPipeline* m_efb_copy_to_ram_pipelines[16] = {}; /* by EFBCopyFormat */

  // Copy pipeline for RGBA8 textures
  AbstractPipeline* m_copy_rgba8_pipeline = nullptr;
  AbstractPipeline* m_rgba8_stereo_copy_pipeline = nullptr;

  // Palette conversion pipelines
  AbstractPipeline* m_palette_conversion_pipelines[NUM_PALETTE_CONVERSION_SHADERS] = {};

  // Texture reinterpreting pipeline
  uid_table m_texture_reinterpret_pipelines; /* by u32 from, to format */

  // Texture decoding shaders
  uid_table m_texture_decoding_shaders; /* by u32 format, palette format */

  Common::EventHook m_frame_end_handler;
};

}  // namespace VideoCommon

extern VideoCommon::ShaderCache* g_shader_cache;
