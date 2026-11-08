// Copyright 2018 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/ShaderCache.h"

#include <stdlib.h>
#include <string.h>
#include <type_traits>
#include <utility>

#include <fmt/format.h>

#include "Common/Assert.h"
#include "Common/FileUtil.h"
#include "Common/MsgHandler.h"
#include "Core/ConfigManager.h"

#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/ConstantManager.h"
#include "VideoCommon/DriverDetails.h"
#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/FramebufferShaderGen.h"
#include "VideoCommon/PipelineUtils.h"
#include "VideoCommon/Present.h"
#include "VideoCommon/Statistics.h"
#include "VideoCommon/StutterStats.h"
#include "VideoCommon/VertexLoaderManager.h"
#include "VideoCommon/VertexManagerBase.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/VideoConfig.h"


VideoCommon::ShaderCache* g_shader_cache = nullptr;

namespace VideoCommon
{
/* The compiler's work items. Each holds the uid of what it compiles, in
 * storage as large as the largest uid, and what it compiled; they are
 * taken from chunks kept for the cache's life, so a shader compiled
 * while the game runs allocates no item. */
enum : u8
{
  WORK_VERTEX_SHADER,
  WORK_VERTEX_UBER_SHADER,
  WORK_PIXEL_SHADER,
  WORK_PIXEL_UBER_SHADER,
  WORK_PIPELINE,
  WORK_UBER_PIPELINE,
  WORK_EFB_COPY_TO_VRAM_PIPELINE /* uid: the EFBCopyFormat */
};

static constexpr size_t WorkUidSize()
{
  size_t size = sizeof(VertexShaderUid);
  size = sizeof(UberShader::VertexShaderUid) > size ? sizeof(UberShader::VertexShaderUid) : size;
  size = sizeof(PixelShaderUid) > size ? sizeof(PixelShaderUid) : size;
  size = sizeof(UberShader::PixelShaderUid) > size ? sizeof(UberShader::PixelShaderUid) : size;
  size = sizeof(GXPipelineUid) > size ? sizeof(GXPipelineUid) : size;
  size = sizeof(GXUberPipelineUid) > size ? sizeof(GXUberPipelineUid) : size;
  return size;
}

struct ShaderCache::WorkItem
{
  AsyncShaderWork base;  // first
  ShaderCache* cache;
  void* result;  // AbstractShader* or AbstractPipeline*, owned until retrieved
  u8 kind;
  bool stages_ready;
  bool has_config;
  AbstractPipelineConfig config;
  alignas(8) unsigned char uid[WorkUidSize()];

  template <typename T>
  T& Uid()
  {
    return *reinterpret_cast<T*>(uid);
  }
};

static_assert(std::is_trivially_destructible_v<VertexShaderUid> &&
                  std::is_trivially_destructible_v<UberShader::VertexShaderUid> &&
                  std::is_trivially_destructible_v<PixelShaderUid> &&
                  std::is_trivially_destructible_v<UberShader::PixelShaderUid> &&
                  std::is_trivially_destructible_v<GXPipelineUid> &&
                  std::is_trivially_destructible_v<GXUberPipelineUid> &&
                  std::is_trivially_destructible_v<AbstractPipelineConfig>,
              "work items are copied in byte for byte and never destroyed");

struct ShaderCache::WorkChunk
{
  WorkChunk* next;
  WorkItem items[64];
};

ShaderCache::ShaderCache() : m_api_type{APIType::Nothing}
{
  uid_table_init(&m_gx_pipeline_cache, sizeof(GXPipelineUid));
  uid_table_init(&m_gx_uber_pipeline_cache, sizeof(GXUberPipelineUid));
  uid_table_init(&m_recorded_uber_pipeline_uids, sizeof(GXUberPipelineUid));
  uid_table_init(&m_efb_copy_to_ram_pipelines, sizeof(u32) * 3);
  uid_table_init(&m_texture_reinterpret_pipelines, sizeof(u32) * 2);
  uid_table_init(&m_texture_decoding_shaders, sizeof(u32) * 2);
  uid_array_init(&m_pending_gx_pipeline_uids, sizeof(SerializedGXPipelineUid));
  uid_array_init(&m_pending_gx_uber_pipeline_uids, sizeof(SerializedGXUberPipelineUid));
  uid_array_init(&m_unwritten_gx_pipelines, sizeof(GXPipelineUid));
}

static AbstractShader* ShaderOf(const uid_table_value* value)
{
  return static_cast<AbstractShader*>(value->object);
}

static AbstractPipeline* PipelineOf(const uid_table_value* value)
{
  return static_cast<AbstractPipeline*>(value->object);
}

/* Deletes every pipeline a table holds, keeping their uids as not compiling. */
static void DropPipelines(uid_table* cache)
{
  size_t cursor = 0;
  const void* key;
  uid_table_value* value;
  while ((value = uid_table_next(cache, &cursor, &key)))
  {
    AbstractPipelineDelete(PipelineOf(value));
    value->object = nullptr;
    value->pending = 0;
  }
}

/* Deletes every shader a table holds, and forgets their uids. */
static void DropShaders(uid_table* cache)
{
  size_t cursor = 0;
  const void* key;
  uid_table_value* value;
  while ((value = uid_table_next(cache, &cursor, &key)))
    AbstractShaderDelete(ShaderOf(value));
  uid_table_clear(cache);
}

/* A pipeline built with a shader of its own, which goes once it is built. */
static AbstractPipeline* PipelineOnce(const AbstractPipelineConfig& config, AbstractShader* shader)
{
  AbstractPipeline* const pipeline = g_gfx->CreatePipeline(config);
  AbstractShaderDelete(shader);
  return pipeline;
}

/* Keeps a pipeline, or that it failed, under key; the table holds it from then on. */
static const AbstractPipeline* KeepPipeline(uid_table* cache, const void* key,
                                            AbstractPipeline* pipeline)
{
  uid_table_value* value = uid_table_get(cache, key);
  if (!value)
  {
    AbstractPipelineDelete(pipeline);
    return nullptr;
  }
  value->object = pipeline;
  return PipelineOf(value);
}

ShaderCache::~ShaderCache()
{
  // The compiler hands back what it still holds before the items go.
  m_async_shader_compiler.StopWorkerThreads();
  m_async_shader_compiler.ClearAllWork();
  ClearCaches();
  uid_table_free(&m_gx_pipeline_cache);
  uid_table_free(&m_gx_uber_pipeline_cache);
  uid_table_free(&m_recorded_uber_pipeline_uids);
  uid_table_free(&m_efb_copy_to_ram_pipelines);
  uid_table_free(&m_texture_reinterpret_pipelines);
  uid_table_free(&m_texture_decoding_shaders);
  uid_array_free(&m_pending_gx_pipeline_uids);
  uid_array_free(&m_pending_gx_uber_pipeline_uids);
  uid_array_free(&m_unwritten_gx_pipelines);
  while (m_work_chunks)
  {
    WorkChunk* next = m_work_chunks->next;
    free(m_work_chunks);
    m_work_chunks = next;
  }
}

bool ShaderCache::Initialize()
{
  m_api_type = g_backend_info.api_type;
  m_host_config.bits = ShaderHostConfig::GetCurrent().bits;

  if (!CompileSharedPipelines())
    return false;

  /* where the driver compiles on threads of its own, programs are started a few a frame
   * and drawn with once linked */
  m_async_shader_compiler.SetDeferred(g_gfx->CompilesShadersOnItsOwnThreads());
  m_frame_end_handler = GetVideoEvents().after_frame_event.Register(
      [this](Core::System&) { RetrieveAsyncShaders(); });
  return true;
}

void ShaderCache::InitializeShaderCache()
{
  m_async_shader_compiler.ResizeWorkerThreads(g_ActiveConfig.GetShaderPrecompilerThreads());

  // Load shader and UID caches.
  if (g_ActiveConfig.bShaderCache && m_api_type != APIType::Nothing)
  {
    LoadCaches();
    LoadPipelineUIDCache();
    if (g_ActiveConfig.UsingUberShaders())
      LoadUberPipelineUIDCache();
  }

  /* With ubershaders, every draw without its specialized pipeline yet is
   * drawn with an ubershader one, which a draw would otherwise build on
   * the spot: every one there can be is built before the game starts, the
   * ones this game drew with before first. Most share what the driver
   * compiles, which is set per draw. The specialized ones known go on
   * building in the background while it runs. Without threads to build
   * on, the asynchronous modes draw as Synchronous does
   * (VertexManagerBase::UpdatePipelineObject), and nothing more is built
   * before the game starts than that would. Where the driver compiles on
   * threads of its own instead, a few a frame (AsyncShaderCompiler
   * deferred), every ubershader pipeline there can be is not built: each
   * still costs the frontend's thread the start of its compile. */
  if (g_ActiveConfig.UsingUberShaders() && CanCompileInBackground())
  {
    CompileMissingUberPipelines();
    if (m_async_shader_compiler.HasWorkerThreads())
    {
      QueueUberShaderPipelines();
      CompileMissingUberPipelines();
    }
    WaitForAsyncCompiler();
  }

  // Compile all known UIDs.
  CompileMissingPipelines();
  if (g_ActiveConfig.bWaitForShadersBeforeStarting)
    WaitForAsyncCompiler();

  // Switch to the runtime shader compiler thread configuration.
  m_async_shader_compiler.ResizeWorkerThreads(g_ActiveConfig.GetShaderCompilerThreads());
}

bool ShaderCache::CanCompileInBackground() const
{
  return (m_async_shader_compiler.HasWorkerThreads() || m_async_shader_compiler.IsDeferred());
}

void ShaderCache::Reload()
{
  WaitForAsyncCompiler();
  ClosePipelineUIDCache();
  ClearCaches();

  if (!CompileSharedPipelines())
    PanicAlertFmt("Failed to compile shared pipelines after reload.");

  if (g_ActiveConfig.bShaderCache)
    LoadCaches();

  // Switch to the precompiling shader configuration while we rebuild.
  m_async_shader_compiler.ResizeWorkerThreads(g_ActiveConfig.GetShaderPrecompilerThreads());

  // We don't need to explicitly recompile the individual ubershaders here, as the pipelines
  // UIDs are still be in the map. Therefore, when these are rebuilt, the shaders will also
  // be recompiled.
  CompileMissingPipelines();
  /* the ubershader pipelines ready again before a draw needs one */
  if (g_ActiveConfig.bWaitForShadersBeforeStarting || g_ActiveConfig.UsingUberShaders())
    WaitForAsyncCompiler();
  m_async_shader_compiler.ResizeWorkerThreads(g_ActiveConfig.GetShaderCompilerThreads());
}

void ShaderCache::RetrieveAsyncShaders()
{
  m_async_shader_compiler.RetrieveWorkItems();

  /* the cache data of those the driver has finished */
  size_t i = 0;
  while (i < m_unwritten_gx_pipelines.count)
  {
    const GXPipelineUid* const uid =
        static_cast<const GXPipelineUid*>(uid_array_at(&m_unwritten_gx_pipelines, i));
    const uid_table_value* const it = uid_table_find(&m_gx_pipeline_cache, uid);
    if (it && it->object && PipelineOf(it)->IsBuilding())
    {
      i++;
      continue;
    }
    if (it && it->object && PipelineOf(it)->IsReady())
      WriteGXPipelineCacheData(*uid, PipelineOf(it));
    uid_array_swap_remove(&m_unwritten_gx_pipelines, i);
  }
}

void ShaderCache::Shutdown()
{
  // This may leave shaders uncommitted to the cache, but it's better than blocking shutdown
  // until everything has finished compiling.
  m_async_shader_compiler.StopWorkerThreads();
  m_async_shader_compiler.ClearAllWork();

  ClosePipelineUIDCache();
}

static void UberPipelineKey(APIType api_type, const ShaderHostConfig& host_config,
                            const GXUberPipelineUid& uid, GXUberPipelineUid* key,
                            GXDrawState* draw_state);

const AbstractPipeline* ShaderCache::GetPipelineForUid(const GXPipelineUid& uid)
{
  const uid_table_value* const it = uid_table_find(&m_gx_pipeline_cache, &uid);
  if (it && !it->pending)
    return PipelineOf(it);

  STUTTER_DRAW_MARK(t);
  const bool exists_in_cache = it != nullptr;
  AbstractPipeline* pipeline = nullptr;
  std::optional<AbstractPipelineConfig> pipeline_config = GetGXPipelineConfig(uid);
  if (pipeline_config)
    pipeline = g_gfx->CreatePipeline(*pipeline_config);
  if (g_ActiveConfig.bShaderCache && !exists_in_cache)
    AppendGXPipelineUID(uid);
  const AbstractPipeline* const kept = InsertGXPipeline(uid, pipeline);
  STUTTER_DRAW_SINCE(STUTTER_DRAW_GX_PIPELINE, t);
  return kept;
}

std::optional<const AbstractPipeline*> ShaderCache::GetPipelineForUidAsync(const GXPipelineUid& uid)
{
  const uid_table_value* const it = uid_table_find(&m_gx_pipeline_cache, &uid);
  if (it)
  {
    // compiling in the background, or not
    if (!it->pending)
      return PipelineOf(it);
    else
      return {};
  }

  AppendGXPipelineUID(uid);
  QueuePipelineCompile(uid, COMPILE_PRIORITY_ONDEMAND_PIPELINE);
  return {};
}

const AbstractPipeline* ShaderCache::GetUberPipelineForUid(const GXUberPipelineUid& uid,
                                                           GXDrawState* draw_state)
{
  GXUberPipelineUid key;
  /* every one a draw uses is recorded, to be built before the game
   * starts next time */
  if (m_gx_uber_pipeline_uid_cache_file.IsOpen() &&
      !uid_table_find(&m_recorded_uber_pipeline_uids, &uid))
    AppendGXUberPipelineUID(uid);
  UberPipelineKey(m_api_type, m_host_config, uid, &key, draw_state);
  const uid_table_value* const it = uid_table_find(&m_gx_uber_pipeline_cache, &key);
  if (it && !it->pending)
    return PipelineOf(it);

  STUTTER_DRAW_MARK(t);
  AbstractPipeline* pipeline = nullptr;
  std::optional<AbstractPipelineConfig> pipeline_config = GetGXPipelineConfig(key);
  if (pipeline_config)
    pipeline = g_gfx->CreatePipeline(*pipeline_config);
  const AbstractPipeline* const kept = InsertGXUberPipeline(key, pipeline);
  STUTTER_DRAW_SINCE(STUTTER_DRAW_UBER_PIPELINE, t);
  return kept;
}

void ShaderCache::WaitForAsyncCompiler()
{
  bool running = true;

  STUTTER_MARK(t);
  // Compiled while the frontend waits; nothing is drawn meanwhile.
  while (running &&
         (m_async_shader_compiler.HasPendingWork() || m_async_shader_compiler.HasCompletedWork()))
  {
    running = m_async_shader_compiler.WaitUntilCompletion();

    m_async_shader_compiler.RetrieveWorkItems();
  }
  STUTTER_SINCE(STUTTER_DRAW_WAIT, t);
}

template <typename SerializedUidType, typename UidType>
static void SerializePipelineUid(const UidType& uid, SerializedUidType& serialized_uid)
{
  // Convert to disk format. Ensure all padding bytes are zero.
  std::memset(reinterpret_cast<u8*>(&serialized_uid), 0, sizeof(serialized_uid));
  serialized_uid.vertex_decl = uid.vertex_format->GetVertexDeclaration();
  serialized_uid.vs_uid = uid.vs_uid;
  serialized_uid.gs_uid = uid.gs_uid;
  serialized_uid.ps_uid = uid.ps_uid;
  serialized_uid.rasterization_state_bits = uid.rasterization_state.hex;
  serialized_uid.depth_state_bits = uid.depth_state.hex;
  serialized_uid.blending_state_bits = uid.blending_state.hex;
}

template <typename UidType, typename SerializedUidType>
static void UnserializePipelineUid(const SerializedUidType& uid, UidType& real_uid)
{
  real_uid.vertex_format = VertexLoaderManager::GetOrCreateMatchingFormat(uid.vertex_decl);
  real_uid.vs_uid = uid.vs_uid;
  real_uid.gs_uid = uid.gs_uid;
  real_uid.ps_uid = uid.ps_uid;
  real_uid.rasterization_state.hex = uid.rasterization_state_bits;
  real_uid.depth_state.hex = uid.depth_state_bits;
  real_uid.blending_state.hex = uid.blending_state_bits;
}

template <ShaderStage stage, typename K, typename T>
void ShaderCache::LoadShaderCache(T& cache, APIType api_type, const char* type, bool include_gameid)
{
  class CacheReader : public Common::LinearDiskCacheReader<K, u8>
  {
  public:
    CacheReader(T& cache_) : cache(cache_) {}
    void Read(const K& key, const u8* value, u32 value_size) override
    {
      AbstractShader* const shader = g_gfx->CreateShaderFromBinary(stage, value, value_size);
      if (shader)
      {
        uid_table_value* const entry = uid_table_get(&cache.shader_map, &key);
        if (!entry)
        {
          AbstractShaderDelete(shader);
          return;
        }
        AbstractShaderDelete(ShaderOf(entry));
        entry->object = shader;
        entry->pending = 0;

        switch (stage)
        {
        case ShaderStage::Vertex:
          INCSTAT(g_stats.num_vertex_shaders_created);
          INCSTAT(g_stats.num_vertex_shaders_alive);
          break;
        case ShaderStage::Pixel:
          INCSTAT(g_stats.num_pixel_shaders_created);
          INCSTAT(g_stats.num_pixel_shaders_alive);
          break;
        default:
          break;
        }
      }
    }

  private:
    T& cache;
  };

  std::string filename = GetDiskShaderCacheFileName(api_type, type, include_gameid, true);
  CacheReader reader(cache);
  u32 count = cache.disk_cache.OpenAndRead(filename, reader);
  INFO_LOG_FMT(VIDEO, "Loaded {} cached shaders from {}", count, filename);
}

template <typename T>
void ShaderCache::ClearShaderCache(T& cache)
{
  cache.disk_cache.Sync();
  cache.disk_cache.Close();
  DropShaders(&cache.shader_map);
}

template <typename KeyType, typename DiskKeyType, typename T>
void ShaderCache::LoadPipelineCache(T& cache, Common::LinearDiskCache<DiskKeyType, u8>& disk_cache,
                                    APIType api_type, const char* type, bool include_gameid)
{
  class CacheReader : public Common::LinearDiskCacheReader<DiskKeyType, u8>
  {
  public:
    CacheReader(ShaderCache* this_ptr_, T& cache_) : this_ptr(this_ptr_), cache(cache_) {}
    bool AnyFailed() const { return failed; }
    void Read(const DiskKeyType& key, const u8* value, u32 value_size) override
    {
      KeyType real_uid;
      UnserializePipelineUid(key, real_uid);

      // Skip those which are already compiled.
      if (failed || uid_table_find(&cache, &real_uid))
        return;

      auto config = this_ptr->GetGXPipelineConfig(real_uid);
      if (!config)
        return;

      AbstractPipeline* const pipeline = g_gfx->CreatePipeline(*config, value, value_size);
      if (!pipeline)
      {
        // If any of the pipelines fail to create, consider the cache stale.
        failed = true;
        return;
      }

      uid_table_value* const entry = uid_table_get(&cache, &real_uid);
      if (!entry)
      {
        AbstractPipelineDelete(pipeline);
        return;
      }
      entry->object = pipeline;
      entry->pending = 0;
    }

  private:
    ShaderCache* this_ptr;
    T& cache;
    bool failed = false;
  };

  std::string filename = GetDiskShaderCacheFileName(api_type, type, include_gameid, true);
  CacheReader reader(this, cache);
  const u32 count = disk_cache.OpenAndRead(filename, reader);
  INFO_LOG_FMT(VIDEO, "Loaded {} cached pipelines from {}", count, filename);

  // If any of the pipelines in the cache failed to create, it's likely because of a change of
  // driver version, or system configuration. In this case, when the UID cache picks up the pipeline
  // later on, we'll write a duplicate entry to the pipeline cache. There's also no point in keeping
  // the old cache data around, so discard and recreate the disk cache.
  if (reader.AnyFailed())
  {
    WARN_LOG_FMT(VIDEO, "Failed to load one or more pipelines from cache '{}'. Discarding.",
                 filename);
    disk_cache.Close();
    File::Delete(filename);
    disk_cache.OpenAndRead(filename, reader);
  }
}

template <typename T, typename Y>
void ShaderCache::ClearPipelineCache(T& cache, Y& disk_cache)
{
  disk_cache.Sync();
  disk_cache.Close();

  // Set the pending flag to false, and destroy the pipeline.
  DropPipelines(&cache);
}

void ShaderCache::LoadCaches()
{
  // Ubershader caches, if present.
  if (g_backend_info.bSupportsShaderBinaries)
  {
    LoadShaderCache<ShaderStage::Vertex, UberShader::VertexShaderUid>(m_uber_vs_cache, m_api_type,
                                                                      "uber-vs", false);
    LoadShaderCache<ShaderStage::Pixel, UberShader::PixelShaderUid>(m_uber_ps_cache, m_api_type,
                                                                    "uber-ps", false);

    // We also share geometry shaders, as there aren't many variants.
    if (m_host_config.backend_geometry_shaders)
      LoadShaderCache<ShaderStage::Geometry, GeometryShaderUid>(m_gs_cache, m_api_type, "gs",
                                                                false);

    // Specialized shaders, gameid-specific.
    LoadShaderCache<ShaderStage::Vertex, VertexShaderUid>(m_vs_cache, m_api_type, "specialized-vs",
                                                          true);
    LoadShaderCache<ShaderStage::Pixel, PixelShaderUid>(m_ps_cache, m_api_type, "specialized-ps",
                                                        true);
  }

  if (g_backend_info.bSupportsPipelineCacheData)
  {
    LoadPipelineCache<GXPipelineUid, SerializedGXPipelineUid>(
        m_gx_pipeline_cache, m_gx_pipeline_disk_cache, m_api_type, "specialized-pipeline", true);
    LoadPipelineCache<GXUberPipelineUid, SerializedGXUberPipelineUid>(
        m_gx_uber_pipeline_cache, m_gx_uber_pipeline_disk_cache, m_api_type, "uber-pipeline",
        false);
  }
}

void ShaderCache::ClearCaches()
{
  uid_array_clear(&m_unwritten_gx_pipelines);
  ClearPipelineCache(m_gx_pipeline_cache, m_gx_pipeline_disk_cache);
  ClearShaderCache(m_vs_cache);
  ClearShaderCache(m_gs_cache);
  ClearShaderCache(m_ps_cache);

  ClearPipelineCache(m_gx_uber_pipeline_cache, m_gx_uber_pipeline_disk_cache);
  ClearShaderCache(m_uber_vs_cache);
  ClearShaderCache(m_uber_ps_cache);

  AbstractShaderDelete(m_screen_quad_vertex_shader);
  m_screen_quad_vertex_shader = nullptr;
  AbstractShaderDelete(m_texture_copy_vertex_shader);
  m_texture_copy_vertex_shader = nullptr;
  AbstractShaderDelete(m_efb_copy_vertex_shader);
  m_efb_copy_vertex_shader = nullptr;
  AbstractShaderDelete(m_texcoord_geometry_shader);
  m_texcoord_geometry_shader = nullptr;
  AbstractShaderDelete(m_color_geometry_shader);
  m_color_geometry_shader = nullptr;
  AbstractShaderDelete(m_texture_copy_pixel_shader);
  m_texture_copy_pixel_shader = nullptr;
  AbstractShaderDelete(m_color_pixel_shader);
  m_color_pixel_shader = nullptr;

  AbstractPipelineDelete(m_efb_copy_to_vram_pipeline);
  m_efb_copy_to_vram_pipeline = nullptr;
  for (size_t i = 0; i < 16; i++)
  {
    AbstractPipelineDelete(m_efb_copy_format_pipelines[i]);
    m_efb_copy_format_pipelines[i] = nullptr;
    m_efb_copy_format_asked[i] = false;
  }
  DropPipelines(&m_efb_copy_to_ram_pipelines);
  uid_table_clear(&m_efb_copy_to_ram_pipelines);
  AbstractPipelineDelete(m_copy_rgba8_pipeline);
  m_copy_rgba8_pipeline = nullptr;
  AbstractPipelineDelete(m_rgba8_stereo_copy_pipeline);
  m_rgba8_stereo_copy_pipeline = nullptr;
  for (AbstractPipeline*& pipeline : m_palette_conversion_pipelines)
  {
    AbstractPipelineDelete(pipeline);
    pipeline = nullptr;
  }
  DropPipelines(&m_texture_reinterpret_pipelines);
  uid_table_clear(&m_texture_reinterpret_pipelines);
  DropShaders(&m_texture_decoding_shaders);

  SETSTAT(g_stats.num_pixel_shaders_created, 0);
  SETSTAT(g_stats.num_pixel_shaders_alive, 0);
  SETSTAT(g_stats.num_vertex_shaders_created, 0);
  SETSTAT(g_stats.num_vertex_shaders_alive, 0);
}

void ShaderCache::CompileMissingPipelines()
{
  // Queue all uids with a null pipeline for compilation.
  /* queuing one marks it compiling: no uid is added meanwhile */
  size_t cursor = 0;
  const void* key;
  uid_table_value* value;
  while ((value = uid_table_next(&m_gx_pipeline_cache, &cursor, &key)))
  {
    if (!value->object)
      QueuePipelineCompile(*static_cast<const GXPipelineUid*>(key),
                           COMPILE_PRIORITY_SHADERCACHE_PIPELINE);
  }
  cursor = 0;
  while ((value = uid_table_next(&m_gx_uber_pipeline_cache, &cursor, &key)))
  {
    if (!value->object)
      QueueUberPipelineCompile(*static_cast<const GXUberPipelineUid*>(key),
                               COMPILE_PRIORITY_UBERSHADER_PIPELINE);
  }
}

AbstractShader* ShaderCache::CompileVertexShader(const VertexShaderUid& uid) const
{
  ShaderCode source_code =
      GenerateVertexShaderCode(m_api_type, m_host_config, uid.GetUidData());
  return g_gfx->CreateShaderFromSource(ShaderStage::Vertex, source_code);
}

AbstractShader*
ShaderCache::CompileVertexUberShader(const UberShader::VertexShaderUid& uid) const
{
  ShaderCode source_code =
      UberShader::GenVertexShader(m_api_type, m_host_config, uid.GetUidData());
  return g_gfx->CreateShaderFromSource(ShaderStage::Vertex, source_code,
                                       fmt::to_string(*uid.GetUidData()));
}

AbstractShader* ShaderCache::CompilePixelShader(const PixelShaderUid& uid) const
{
  ShaderCode source_code =
      GeneratePixelShaderCode(m_api_type, m_host_config, uid.GetUidData());
  return g_gfx->CreateShaderFromSource(ShaderStage::Pixel, source_code);
}

AbstractShader*
ShaderCache::CompilePixelUberShader(const UberShader::PixelShaderUid& uid) const
{
  ShaderCode source_code =
      UberShader::GenPixelShader(m_api_type, m_host_config, uid.GetUidData());
  return g_gfx->CreateShaderFromSource(ShaderStage::Pixel, source_code,
                                       fmt::to_string(*uid.GetUidData()));
}

const AbstractShader* ShaderCache::InsertVertexShader(const VertexShaderUid& uid,
                                                      AbstractShader* shader)
{
  uid_table_value* const entry = uid_table_get(&m_vs_cache.shader_map, &uid);
  if (!entry)
  {
    AbstractShaderDelete(shader);
    return nullptr;
  }
  entry->pending = 0;

  if (shader && entry->object)
  {
    AbstractShaderDelete(shader);
    shader = nullptr;
  }
  if (shader)
  {
    if (g_ActiveConfig.bShaderCache && g_backend_info.bSupportsShaderBinaries)
    {
      size_t binary_size;
      const u8* binary = shader->GetBinary(&binary_size);
      if (binary_size)
        m_vs_cache.disk_cache.Append(uid, binary, static_cast<u32>(binary_size));
    }
    INCSTAT(g_stats.num_vertex_shaders_created);
    INCSTAT(g_stats.num_vertex_shaders_alive);
    entry->object = shader;
  }

  return ShaderOf(entry);
}

const AbstractShader* ShaderCache::InsertVertexUberShader(const UberShader::VertexShaderUid& uid,
                                                          AbstractShader* shader)
{
  uid_table_value* const entry = uid_table_get(&m_uber_vs_cache.shader_map, &uid);
  if (!entry)
  {
    AbstractShaderDelete(shader);
    return nullptr;
  }
  entry->pending = 0;

  if (shader && entry->object)
  {
    AbstractShaderDelete(shader);
    shader = nullptr;
  }
  if (shader)
  {
    if (g_ActiveConfig.bShaderCache && g_backend_info.bSupportsShaderBinaries)
    {
      size_t binary_size;
      const u8* binary = shader->GetBinary(&binary_size);
      if (binary_size)
        m_uber_vs_cache.disk_cache.Append(uid, binary, static_cast<u32>(binary_size));
    }
    INCSTAT(g_stats.num_vertex_shaders_created);
    INCSTAT(g_stats.num_vertex_shaders_alive);
    entry->object = shader;
  }

  return ShaderOf(entry);
}

const AbstractShader* ShaderCache::InsertPixelShader(const PixelShaderUid& uid,
                                                     AbstractShader* shader)
{
  uid_table_value* const entry = uid_table_get(&m_ps_cache.shader_map, &uid);
  if (!entry)
  {
    AbstractShaderDelete(shader);
    return nullptr;
  }
  entry->pending = 0;

  if (shader && entry->object)
  {
    AbstractShaderDelete(shader);
    shader = nullptr;
  }
  if (shader)
  {
    if (g_ActiveConfig.bShaderCache && g_backend_info.bSupportsShaderBinaries)
    {
      size_t binary_size;
      const u8* binary = shader->GetBinary(&binary_size);
      if (binary_size)
        m_ps_cache.disk_cache.Append(uid, binary, static_cast<u32>(binary_size));
    }
    INCSTAT(g_stats.num_pixel_shaders_created);
    INCSTAT(g_stats.num_pixel_shaders_alive);
    entry->object = shader;
  }

  return ShaderOf(entry);
}

const AbstractShader* ShaderCache::InsertPixelUberShader(const UberShader::PixelShaderUid& uid,
                                                         AbstractShader* shader)
{
  uid_table_value* const entry = uid_table_get(&m_uber_ps_cache.shader_map, &uid);
  if (!entry)
  {
    AbstractShaderDelete(shader);
    return nullptr;
  }
  entry->pending = 0;

  if (shader && entry->object)
  {
    AbstractShaderDelete(shader);
    shader = nullptr;
  }
  if (shader)
  {
    if (g_ActiveConfig.bShaderCache && g_backend_info.bSupportsShaderBinaries)
    {
      size_t binary_size;
      const u8* binary = shader->GetBinary(&binary_size);
      if (binary_size)
        m_uber_ps_cache.disk_cache.Append(uid, binary, static_cast<u32>(binary_size));
    }
    INCSTAT(g_stats.num_pixel_shaders_created);
    INCSTAT(g_stats.num_pixel_shaders_alive);
    entry->object = shader;
  }

  return ShaderOf(entry);
}

const AbstractShader* ShaderCache::CreateGeometryShader(const GeometryShaderUid& uid)
{
  ShaderCode source_code =
      GenerateGeometryShaderCode(m_api_type, m_host_config, uid.GetUidData());
  AbstractShader* shader =
      g_gfx->CreateShaderFromSource(ShaderStage::Geometry, source_code,
                                    fmt::format("Geometry shader: {}", *uid.GetUidData()));

  uid_table_value* const entry = uid_table_get(&m_gs_cache.shader_map, &uid);
  if (!entry)
  {
    AbstractShaderDelete(shader);
    return nullptr;
  }
  entry->pending = 0;

  if (shader && entry->object)
  {
    AbstractShaderDelete(shader);
    shader = nullptr;
  }
  if (shader)
  {
    if (g_ActiveConfig.bShaderCache && g_backend_info.bSupportsShaderBinaries)
    {
      size_t binary_size;
      const u8* binary = shader->GetBinary(&binary_size);
      if (binary_size)
        m_gs_cache.disk_cache.Append(uid, binary, static_cast<u32>(binary_size));
    }
    entry->object = shader;
  }

  return ShaderOf(entry);
}

bool ShaderCache::NeedsGeometryShader(const GeometryShaderUid& uid) const
{
  return m_host_config.backend_geometry_shaders && !uid.GetUidData()->IsPassthrough();
}

bool ShaderCache::UseGeometryShaderForEFBCopies() const
{
  return m_host_config.backend_geometry_shaders && m_host_config.stereo;
}

AbstractPipelineConfig ShaderCache::GetGXPipelineConfig(
    const NativeVertexFormat* vertex_format, const AbstractShader* vertex_shader,
    const AbstractShader* geometry_shader, const AbstractShader* pixel_shader,
    const RasterizationState& rasterization_state, const DepthState& depth_state,
    const BlendingState& blending_state, AbstractPipelineUsage usage)
{
  AbstractPipelineConfig config = {};
  config.usage = usage;
  config.vertex_format = vertex_format;
  config.vertex_shader = vertex_shader;
  config.geometry_shader = geometry_shader;
  config.pixel_shader = pixel_shader;
  config.rasterization_state = rasterization_state;
  config.depth_state = depth_state;
  config.blending_state = blending_state;
  config.framebuffer_state = g_framebuffer_manager->GetEFBFramebufferState();
  return config;
}

std::optional<AbstractPipelineConfig>
ShaderCache::GetGXPipelineConfig(const GXPipelineUid& config_in)
{
  GXPipelineUid config = VideoCommon::ApplyDriverBugs(config_in);
  const AbstractShader* vs;
  const uid_table_value* const vs_iter = uid_table_find(&m_vs_cache.shader_map, &config.vs_uid);
  if (vs_iter && !vs_iter->pending)
    vs = ShaderOf(vs_iter);
  else
    vs = InsertVertexShader(config.vs_uid, CompileVertexShader(config.vs_uid));

  PixelShaderUid ps_uid = config.ps_uid;
  ClearUnusedPixelShaderUidBits(m_api_type, m_host_config, &ps_uid);

  const AbstractShader* ps;
  const uid_table_value* const ps_iter = uid_table_find(&m_ps_cache.shader_map, &ps_uid);
  if (ps_iter && !ps_iter->pending)
    ps = ShaderOf(ps_iter);
  else
    ps = InsertPixelShader(ps_uid, CompilePixelShader(ps_uid));

  if (!vs || !ps)
    return {};

  const AbstractShader* gs = nullptr;
  if (NeedsGeometryShader(config.gs_uid))
  {
    const uid_table_value* const gs_iter = uid_table_find(&m_gs_cache.shader_map, &config.gs_uid);
    if (gs_iter && !gs_iter->pending)
      gs = ShaderOf(gs_iter);
    else
      gs = CreateGeometryShader(config.gs_uid);
    if (!gs)
      return {};
  }

  return GetGXPipelineConfig(config.vertex_format, vs, gs, ps, config.rasterization_state,
                             config.depth_state, config.blending_state, AbstractPipelineUsage::GX);
}

/* Edits the uid in place, based on driver bugs and other special configurations. */
static void ApplyUberDriverBugs(GXUberPipelineUid* out)
{
  static bool s_warned_logic_op;
  if (g_backend_info.bSupportsDynamicVertexLoader)
    out->vertex_format = nullptr;

  // If framebuffer fetch is available, we can emulate logic ops in the fragment shader
  // and don't need the below blend approximation
  if (out->blending_state.logic_op_enable && !g_backend_info.bSupportsLogicOp &&
      !g_backend_info.bSupportsFramebufferFetch)
  {
    if (!s_warned_logic_op && !out->blending_state.LogicOpApproximationIsExact())
    {
      s_warned_logic_op = true;
      WARN_LOG_FMT(VIDEO,
                   "Approximating logic op with blending, this will produce incorrect rendering.");
    }
    out->blending_state.ApproximateLogicOpWithBlending();
  }

  if (g_backend_info.bSupportsFramebufferFetch)
  {
    // Always blend in shader
    const u32 color_update = out->blending_state.color_update.Value();
    const u32 alpha_update = out->blending_state.alpha_update.Value();
    out->blending_state.hex = 0;
    out->blending_state.color_update = color_update;
    out->blending_state.alpha_update = alpha_update;
    out->ps_uid.GetUidData()->no_dual_src = true;
  }
  else if (!g_backend_info.bSupportsDualSourceBlend ||
           (DriverDetails::HasBug(DriverDetails::BUG_BROKEN_DUAL_SOURCE_BLENDING) &&
            !out->blending_state.RequiresDualSrc()))
  {
    out->blending_state.use_dual_src = false;
    out->ps_uid.GetUidData()->no_dual_src = true;
  }

  if (g_ActiveConfig.UseVSForLinePointExpand())
  {
    // All primitives are expanded to triangles in the vertex shader
    PrimitiveType prim = g_backend_info.bSupportsPrimitiveRestart ? PrimitiveType::TriangleStrip :
                                                                    PrimitiveType::Triangles;
    out->rasterization_state.primitive = prim;
    out->gs_uid.GetUidData()->primitive_type = static_cast<u32>(prim);
  }
}

/* Whether the backend sets some of an ubershader pipeline's state per draw: its pipelines are
 * then kept under the uid as the driver bugs leave it, without that state. */
static bool UberDrawStateSet()
{
  return (g_backend_info.uDrawStateRasterizationBits | g_backend_info.uDrawStateDepthBits |
          g_backend_info.uDrawStateBlendingBits) != 0;
}

/* The key an ubershader pipeline for uid is kept under, padding and all, and, where the backend
 * sets some of its state per draw, the state the draws are in. The pixel shader uid is then the
 * one the shader is built from, without the bits it does not read. */
static void UberPipelineKey(APIType api_type, const ShaderHostConfig& host_config,
                            const GXUberPipelineUid& uid, GXUberPipelineUid* key,
                            GXDrawState* draw_state)
{
  // GXUberPipelineUid is not trivially copyable, as RasterizationState and BlendingState aren't:
  // copied byte for byte, padding and all, as the key compares.
  memcpy(static_cast<void*>(key), static_cast<const void*>(&uid), sizeof(*key));
  if (!UberDrawStateSet())
    return;
  ApplyUberDriverBugs(key);
  UberShader::ClearUnusedPixelShaderUidBits(api_type, host_config, &key->ps_uid);
  if (draw_state)
  {
    draw_state->rasterization = key->rasterization_state;
    draw_state->depth = key->depth_state;
    draw_state->blending = key->blending_state;
  }
  key->rasterization_state.hex &= ~g_backend_info.uDrawStateRasterizationBits;
  key->depth_state.hex &= ~g_backend_info.uDrawStateDepthBits;
  key->blending_state.hex &= ~g_backend_info.uDrawStateBlendingBits;
}

/* The uid an ubershader pipeline kept under key is built from: the key itself where the driver
 * bugs are applied to it already. */
static void UberPipelineBuildUid(const GXUberPipelineUid& key, GXUberPipelineUid* uid)
{
  memcpy(static_cast<void*>(uid), static_cast<const void*>(&key), sizeof(*uid));
  if (!UberDrawStateSet())
    ApplyUberDriverBugs(uid);
}

std::optional<AbstractPipelineConfig>
ShaderCache::GetGXPipelineConfig(const GXUberPipelineUid& config_in)
{
  GXUberPipelineUid config;
  UberPipelineBuildUid(config_in, &config);
  const AbstractShader* vs;
  const uid_table_value* const vs_iter = uid_table_find(&m_uber_vs_cache.shader_map, &config.vs_uid);
  if (vs_iter && !vs_iter->pending)
    vs = ShaderOf(vs_iter);
  else
    vs = InsertVertexUberShader(config.vs_uid, CompileVertexUberShader(config.vs_uid));

  UberShader::PixelShaderUid ps_uid = config.ps_uid;
  UberShader::ClearUnusedPixelShaderUidBits(m_api_type, m_host_config, &ps_uid);

  const AbstractShader* ps;
  const uid_table_value* const ps_iter = uid_table_find(&m_uber_ps_cache.shader_map, &ps_uid);
  if (ps_iter && !ps_iter->pending)
    ps = ShaderOf(ps_iter);
  else
    ps = InsertPixelUberShader(ps_uid, CompilePixelUberShader(ps_uid));

  if (!vs || !ps)
    return {};

  const AbstractShader* gs = nullptr;
  if (NeedsGeometryShader(config.gs_uid))
  {
    const uid_table_value* const gs_iter = uid_table_find(&m_gs_cache.shader_map, &config.gs_uid);
    if (gs_iter && !gs_iter->pending)
      gs = ShaderOf(gs_iter);
    else
      gs = CreateGeometryShader(config.gs_uid);
    if (!gs)
      return {};
  }

  return GetGXPipelineConfig(config.vertex_format, vs, gs, ps, config.rasterization_state,
                             config.depth_state, config.blending_state,
                             AbstractPipelineUsage::GXUber);
}

const AbstractPipeline* ShaderCache::InsertGXPipeline(const GXPipelineUid& config,
                                                      AbstractPipeline* pipeline)
{
  uid_table_value* const entry = uid_table_get(&m_gx_pipeline_cache, &config);
  if (!entry)
  {
    AbstractPipelineDelete(pipeline);
    return nullptr;
  }
  entry->pending = 0;
  if (pipeline && entry->object)
  {
    AbstractPipelineDelete(pipeline);
    pipeline = nullptr;
  }
  if (pipeline)
  {
    entry->object = pipeline;

    if (g_ActiveConfig.bShaderCache)
    {
      /* its data is there to read once the driver is done with it */
      if (PipelineOf(entry)->IsBuilding())
        uid_array_push(&m_unwritten_gx_pipelines, &config);
      else
        WriteGXPipelineCacheData(config, PipelineOf(entry));
    }
  }

  return PipelineOf(entry);
}

void ShaderCache::WriteGXPipelineCacheData(const GXPipelineUid& config,
                                           const AbstractPipeline* pipeline)
{
  auto cache_data = pipeline->GetCacheData();
  if (!cache_data.empty())
  {
    SerializedGXPipelineUid disk_uid;
    SerializePipelineUid(config, disk_uid);
    m_gx_pipeline_disk_cache.Append(disk_uid, cache_data.data(),
                                    static_cast<u32>(cache_data.size()));
  }
}

const AbstractPipeline*
ShaderCache::InsertGXUberPipeline(const GXUberPipelineUid& config,
                                  AbstractPipeline* pipeline)
{
  uid_table_value* const entry = uid_table_get(&m_gx_uber_pipeline_cache, &config);
  if (!entry)
  {
    AbstractPipelineDelete(pipeline);
    return nullptr;
  }
  entry->pending = 0;
  if (pipeline && entry->object)
  {
    AbstractPipelineDelete(pipeline);
    pipeline = nullptr;
  }
  if (pipeline)
  {
    entry->object = pipeline;

    if (g_ActiveConfig.bShaderCache)
    {
      auto cache_data = PipelineOf(entry)->GetCacheData();
      /* a key without its vertex format has no uid to write */
      if (!cache_data.empty() && config.vertex_format)
      {
        SerializedGXUberPipelineUid disk_uid;
        SerializePipelineUid(config, disk_uid);
        m_gx_uber_pipeline_disk_cache.Append(disk_uid, cache_data.data(),
                                             static_cast<u32>(cache_data.size()));
      }
    }
  }

  return PipelineOf(entry);
}

void ShaderCache::LoadPipelineUIDCache()
{
  constexpr u32 CACHE_FILE_MAGIC = 0x44495550;  // PUID
  constexpr size_t CACHE_HEADER_SIZE = sizeof(u32) + sizeof(u32);
  std::string filename =
      File::GetUserPath(D_CACHE_IDX) + SConfig::GetInstance().GetGameID() + ".uidcache";
  if (m_gx_pipeline_uid_cache_file.Open(filename, "rb+"))
  {
    // If an existing case exists, validate the version before reading entries.
    u32 existing_magic;
    u32 existing_version;
    bool uid_file_valid = false;
    if (m_gx_pipeline_uid_cache_file.ReadBytes(&existing_magic, sizeof(existing_magic)) &&
        m_gx_pipeline_uid_cache_file.ReadBytes(&existing_version, sizeof(existing_version)) &&
        existing_magic == CACHE_FILE_MAGIC && existing_version == GX_PIPELINE_UID_VERSION)
    {
      // Ensure the expected size matches the actual size of the file. If it doesn't, it means
      // the cache file may be corrupted, and we should not proceed with loading potentially
      // garbage or invalid UIDs.
      const u64 file_size = m_gx_pipeline_uid_cache_file.GetSize();
      const size_t uid_count =
          static_cast<size_t>(file_size - CACHE_HEADER_SIZE) / sizeof(SerializedGXPipelineUid);
      const size_t expected_size = uid_count * sizeof(SerializedGXPipelineUid) + CACHE_HEADER_SIZE;
      uid_file_valid = file_size == expected_size;
      if (uid_file_valid)
      {
        for (size_t i = 0; i < uid_count; i++)
        {
          SerializedGXPipelineUid serialized_uid;
          if (m_gx_pipeline_uid_cache_file.ReadBytes(&serialized_uid, sizeof(serialized_uid)))
          {
            // This just adds the pipeline to the map, it is compiled later.
            AddSerializedGXPipelineUID(serialized_uid);
          }
          else
          {
            uid_file_valid = false;
            break;
          }
        }
      }

      // We open the file for reading and writing, so we must seek to the end before writing.
      if (uid_file_valid)
        uid_file_valid = m_gx_pipeline_uid_cache_file.Seek(expected_size, File::SeekOrigin::Begin);
    }

    // If the file is invalid, close it. We re-open and truncate it below.
    if (!uid_file_valid)
      m_gx_pipeline_uid_cache_file.Close();
  }

  // If the file is not open, it means it was either corrupted or didn't exist.
  if (!m_gx_pipeline_uid_cache_file.IsOpen())
  {
    if (m_gx_pipeline_uid_cache_file.Open(filename, "wb"))
    {
      // Write the version identifier.
      m_gx_pipeline_uid_cache_file.WriteBytes(&CACHE_FILE_MAGIC, sizeof(GX_PIPELINE_UID_VERSION));
      m_gx_pipeline_uid_cache_file.WriteBytes(&GX_PIPELINE_UID_VERSION,
                                              sizeof(GX_PIPELINE_UID_VERSION));

      // Write any current UIDs out to the file.
      // This way, if we load a UID cache where the data was incomplete (e.g. Dolphin crashed),
      // we don't lose the existing UIDs which were previously at the beginning.
      size_t cursor = 0;
      const void* key;
      while (uid_table_next(&m_gx_pipeline_cache, &cursor, &key))
        AppendGXPipelineUID(*static_cast<const GXPipelineUid*>(key));
    }
  }

  INFO_LOG_FMT(VIDEO, "Read {} pipeline UIDs from {}", m_gx_pipeline_cache.count, filename);
}

void ShaderCache::ClosePipelineUIDCache()
{
  // This is left as a method in case we need to append extra data to the file in the future.
  if (m_gx_pipeline_uid_cache_file.IsOpen() && m_pending_gx_pipeline_uids.count &&
      !m_gx_pipeline_uid_cache_file.WriteArray(
          static_cast<const SerializedGXPipelineUid*>(uid_array_at(&m_pending_gx_pipeline_uids, 0)),
          m_pending_gx_pipeline_uids.count))
  {
    WARN_LOG_FMT(VIDEO, "Writing pipeline UIDs to cache failed.");
  }
  uid_array_clear(&m_pending_gx_pipeline_uids);
  if (m_gx_uber_pipeline_uid_cache_file.IsOpen() && m_pending_gx_uber_pipeline_uids.count &&
      !m_gx_uber_pipeline_uid_cache_file.WriteArray(
          static_cast<const SerializedGXUberPipelineUid*>(uid_array_at(&m_pending_gx_uber_pipeline_uids, 0)),
          m_pending_gx_uber_pipeline_uids.count))
  {
    WARN_LOG_FMT(VIDEO, "Writing ubershader pipeline UIDs to cache failed.");
  }
  uid_array_clear(&m_pending_gx_uber_pipeline_uids);
  uid_table_clear(&m_recorded_uber_pipeline_uids);
  m_gx_uber_pipeline_uid_cache_file.Close();
  m_gx_pipeline_uid_cache_file.Close();
}

void ShaderCache::LoadUberPipelineUIDCache()
{
  const u32 magic = 0x44495555;  // UUID
  const u32 header_size = sizeof(u32) + sizeof(u32);
  const std::string filename =
      File::GetUserPath(D_CACHE_IDX) + SConfig::GetInstance().GetGameID() + ".uberuidcache";
  File::IOFile& file = m_gx_uber_pipeline_uid_cache_file;
  bool valid = false;

  if (file.Open(filename, "rb+"))
  {
    u32 existing_magic;
    u32 existing_version;
    if (file.ReadBytes(&existing_magic, sizeof(existing_magic)) &&
        file.ReadBytes(&existing_version, sizeof(existing_version)) && existing_magic == magic &&
        existing_version == GX_PIPELINE_UID_VERSION)
    {
      const u64 file_size = file.GetSize();
      const u64 count = (file_size - header_size) / sizeof(SerializedGXUberPipelineUid);
      u64 i;

      valid = file_size == count * sizeof(SerializedGXUberPipelineUid) + header_size;
      for (i = 0; valid && i < count; i++)
      {
        SerializedGXUberPipelineUid serialized_uid;
        GXUberPipelineUid real_uid;
        if (!file.ReadBytes(&serialized_uid, sizeof(serialized_uid)))
        {
          valid = false;
          break;
        }
        UnserializePipelineUid(serialized_uid, real_uid);
        /* known, not built: built with the ubershaders before the game
         * starts */
        {
          GXUberPipelineUid key;
          UberPipelineKey(m_api_type, m_host_config, real_uid, &key, nullptr);
          uid_table_get(&m_gx_uber_pipeline_cache, &key);
        }
        uid_table_get(&m_recorded_uber_pipeline_uids, &real_uid);
      }
      if (valid)
        valid = file.Seek(file_size, File::SeekOrigin::Begin);
    }
    if (!valid)
      file.Close();
  }
  if (!file.IsOpen() && file.Open(filename, "wb"))
  {
    file.WriteBytes(&magic, sizeof(magic));
    file.WriteBytes(&GX_PIPELINE_UID_VERSION, sizeof(GX_PIPELINE_UID_VERSION));
  }
  /* what a session adds is written when the cache closes: room enough
   * that adding is no allocation in the middle of a game */
  uid_array_reserve(&m_pending_gx_uber_pipeline_uids, 1024);
  uid_array_reserve(&m_pending_gx_pipeline_uids, 1024);
  uid_array_reserve(&m_unwritten_gx_pipelines, 256);
}

void ShaderCache::AppendGXUberPipelineUID(const GXUberPipelineUid& config)
{
  SerializedGXUberPipelineUid disk_uid;

  if (!m_gx_uber_pipeline_uid_cache_file.IsOpen())
    return;
  SerializePipelineUid(config, disk_uid);
  uid_array_push(&m_pending_gx_uber_pipeline_uids, &disk_uid);
  uid_table_get(&m_recorded_uber_pipeline_uids, &config);
}

void ShaderCache::CompileMissingUberPipelines()
{
  /* queuing one marks it compiling: no uid is added meanwhile */
  size_t cursor = 0;
  const void* key;
  uid_table_value* value;
  while ((value = uid_table_next(&m_gx_uber_pipeline_cache, &cursor, &key)))
  {
    if (!value->object && !value->pending)
      QueueUberPipelineCompile(*static_cast<const GXUberPipelineUid*>(key),
                               COMPILE_PRIORITY_UBERSHADER_PIPELINE);
  }
}

void ShaderCache::AddSerializedGXPipelineUID(const SerializedGXPipelineUid& uid)
{
  GXPipelineUid real_uid;
  UnserializePipelineUid(uid, real_uid);

  // Flagged as empty with a null pipeline object, for later compilation, if it is new.
  uid_table_get(&m_gx_pipeline_cache, &real_uid);
}

void ShaderCache::AppendGXPipelineUID(const GXPipelineUid& config)
{
  if (!m_gx_pipeline_uid_cache_file.IsOpen())
    return;

  SerializedGXPipelineUid disk_uid;
  SerializePipelineUid(config, disk_uid);
  // Keep filesystem writes off the draw path. Save on reload or shutdown.
  uid_array_push(&m_pending_gx_pipeline_uids, &disk_uid);
}

ShaderCache::WorkItem* ShaderCache::AllocWork(u8 kind, u32 priority)
{
  WorkItem* item;
  if (!m_free_work)
  {
    WorkChunk* chunk = static_cast<WorkChunk*>(calloc(1, sizeof(WorkChunk)));
    size_t i;
    if (!chunk)
      return nullptr;
    chunk->next = m_work_chunks;
    m_work_chunks = chunk;
    for (i = 0; i < sizeof(chunk->items) / sizeof(chunk->items[0]); i++)
      ReleaseWork(&chunk->items[i]);
  }
  item = m_free_work;
  m_free_work = reinterpret_cast<WorkItem*>(item->base.link.next);
  item->base.compile = WorkCompile;
  item->base.retrieve = WorkRetrieve;
  item->base.discard = WorkDiscard;
  item->base.priority = priority;
  item->cache = this;
  item->result = nullptr;
  item->kind = kind;
  item->stages_ready = true;
  item->has_config = false;
  return item;
}

void ShaderCache::ReleaseWork(WorkItem* item)
{
  item->base.link.next = reinterpret_cast<mpsc_stack_node_t*>(m_free_work);
  m_free_work = item;
}

/* Whether the stages a pipeline needs are compiled, queuing those not yet
 * asked for; when they are, the configuration to build it with. */
void ShaderCache::QueueWork(WorkItem* item)
{
  const u32 priority = item->base.priority;

  if (item->kind == WORK_PIPELINE)
  {
    const GXPipelineUid& uid = item->Uid<GXPipelineUid>();
    GXPipelineUid actual_uid = ApplyDriverBugs(uid);
    bool ready = true;

    const uid_table_value* const vs_it = uid_table_find(&m_vs_cache.shader_map, &actual_uid.vs_uid);
    ready &= vs_it && !vs_it->pending;
    if (!vs_it)
      QueueVertexShaderCompile(actual_uid.vs_uid, priority);

    PixelShaderUid ps_uid = actual_uid.ps_uid;
    ClearUnusedPixelShaderUidBits(m_api_type, m_host_config, &ps_uid);
    const uid_table_value* const ps_it = uid_table_find(&m_ps_cache.shader_map, &ps_uid);
    ready &= ps_it && !ps_it->pending;
    if (!ps_it)
      QueuePixelShaderCompile(ps_uid, priority);

    // If a stage is missing, this item does nothing but come back next frame.
    item->stages_ready = ready;
    item->has_config = false;
    if (ready)
    {
      std::optional<AbstractPipelineConfig> config = GetGXPipelineConfig(uid);
      if (config)
      {
        item->config = *config;
        item->has_config = true;
      }
    }
  }
  else if (item->kind == WORK_UBER_PIPELINE)
  {
    const GXUberPipelineUid& uid = item->Uid<GXUberPipelineUid>();
    GXUberPipelineUid actual_uid;
    UberPipelineBuildUid(uid, &actual_uid);
    bool ready = true;

    const uid_table_value* const vs_it = uid_table_find(&m_uber_vs_cache.shader_map, &actual_uid.vs_uid);
    ready &= vs_it && !vs_it->pending;
    if (!vs_it)
      QueueVertexUberShaderCompile(actual_uid.vs_uid, priority);

    UberShader::PixelShaderUid ps_uid = actual_uid.ps_uid;
    UberShader::ClearUnusedPixelShaderUidBits(m_api_type, m_host_config, &ps_uid);
    const uid_table_value* const ps_it = uid_table_find(&m_uber_ps_cache.shader_map, &ps_uid);
    ready &= ps_it && !ps_it->pending;
    if (!ps_it)
      QueuePixelUberShaderCompile(ps_uid, priority);

    item->stages_ready = ready;
    item->has_config = false;
    if (ready)
    {
      std::optional<AbstractPipelineConfig> config = GetGXPipelineConfig(uid);
      if (config)
      {
        item->config = *config;
        item->has_config = true;
      }
    }
  }

  m_async_shader_compiler.QueueWorkItem(&item->base, priority);
}

void ShaderCache::WorkCompile(AsyncShaderWork* work)
{
  WorkItem* item = reinterpret_cast<WorkItem*>(work);
  const ShaderCache* cache = item->cache;
  switch (item->kind)
  {
  case WORK_VERTEX_SHADER:
    item->result = cache->CompileVertexShader(item->Uid<VertexShaderUid>());
    break;
  case WORK_VERTEX_UBER_SHADER:
    item->result =
        cache->CompileVertexUberShader(item->Uid<UberShader::VertexShaderUid>());
    break;
  case WORK_PIXEL_SHADER:
    item->result = cache->CompilePixelShader(item->Uid<PixelShaderUid>());
    break;
  case WORK_PIXEL_UBER_SHADER:
    item->result =
        cache->CompilePixelUberShader(item->Uid<UberShader::PixelShaderUid>());
    break;
  case WORK_EFB_COPY_TO_VRAM_PIPELINE:
    item->result = cache->CompileEFBCopyToVRAMPipeline(&item->Uid<EFBCopyFormat>());
    break;
  default:
    if (item->has_config)
      item->result = g_gfx->CreatePipeline(item->config);
    break;
  }
}

void ShaderCache::WorkRetrieve(AsyncShaderWork* work)
{
  WorkItem* item = reinterpret_cast<WorkItem*>(work);
  ShaderCache* cache = item->cache;
  void* result = item->result;
  item->result = nullptr;
  switch (item->kind)
  {
  case WORK_VERTEX_SHADER:
    cache->InsertVertexShader(item->Uid<VertexShaderUid>(),
                              static_cast<AbstractShader*>(result));
    break;
  case WORK_VERTEX_UBER_SHADER:
    cache->InsertVertexUberShader(
        item->Uid<UberShader::VertexShaderUid>(),
        static_cast<AbstractShader*>(result));
    break;
  case WORK_PIXEL_SHADER:
    cache->InsertPixelShader(item->Uid<PixelShaderUid>(),
                             static_cast<AbstractShader*>(result));
    break;
  case WORK_PIXEL_UBER_SHADER:
    cache->InsertPixelUberShader(
        item->Uid<UberShader::PixelShaderUid>(),
        static_cast<AbstractShader*>(result));
    break;
  case WORK_PIPELINE:
    if (!item->stages_ready)
    {
      // Queued again for next frame.
      cache->QueueWork(item);
      return;
    }
    cache->InsertGXPipeline(item->Uid<GXPipelineUid>(), static_cast<AbstractPipeline*>(result));
    break;
  case WORK_UBER_PIPELINE:
    if (!item->stages_ready)
    {
      cache->QueueWork(item);
      return;
    }
    cache->InsertGXUberPipeline(
        item->Uid<GXUberPipelineUid>(),
        static_cast<AbstractPipeline*>(result));
    break;
  case WORK_EFB_COPY_TO_VRAM_PIPELINE:
    /* one that failed leaves the one for every format drawing */
    cache->m_efb_copy_format_pipelines[static_cast<u32>(item->Uid<EFBCopyFormat>()) & 15] =
        static_cast<AbstractPipeline*>(result);
    break;
  }
  cache->ReleaseWork(item);
}

void ShaderCache::WorkDiscard(AsyncShaderWork* work)
{
  WorkItem* item = reinterpret_cast<WorkItem*>(work);
  if (item->result)
  {
    if (item->kind == WORK_PIPELINE || item->kind == WORK_UBER_PIPELINE ||
        item->kind == WORK_EFB_COPY_TO_VRAM_PIPELINE)
      AbstractPipelineDelete(static_cast<AbstractPipeline*>(item->result));
    else
      AbstractShaderDelete(static_cast<AbstractShader*>(item->result));
    item->result = nullptr;
  }
  item->cache->ReleaseWork(item);
}

void ShaderCache::QueueVertexShaderCompile(const VertexShaderUid& uid, u32 priority)
{
  WorkItem* item = AllocWork(WORK_VERTEX_SHADER, priority);
  if (!item)
    return;
  memcpy(item->uid, &uid, sizeof(uid));
  {
    uid_table_value* const entry = uid_table_get(&m_vs_cache.shader_map, &uid);
    if (entry)
      entry->pending = 1;
  }
  QueueWork(item);
}

void ShaderCache::QueueVertexUberShaderCompile(const UberShader::VertexShaderUid& uid, u32 priority)
{
  WorkItem* item = AllocWork(WORK_VERTEX_UBER_SHADER, priority);
  if (!item)
    return;
  memcpy(item->uid, &uid, sizeof(uid));
  {
    uid_table_value* const entry = uid_table_get(&m_uber_vs_cache.shader_map, &uid);
    if (entry)
      entry->pending = 1;
  }
  QueueWork(item);
}

void ShaderCache::QueuePixelShaderCompile(const PixelShaderUid& uid, u32 priority)
{
  WorkItem* item = AllocWork(WORK_PIXEL_SHADER, priority);
  if (!item)
    return;
  memcpy(item->uid, &uid, sizeof(uid));
  {
    uid_table_value* const entry = uid_table_get(&m_ps_cache.shader_map, &uid);
    if (entry)
      entry->pending = 1;
  }
  QueueWork(item);
}

void ShaderCache::QueuePixelUberShaderCompile(const UberShader::PixelShaderUid& uid, u32 priority)
{
  WorkItem* item = AllocWork(WORK_PIXEL_UBER_SHADER, priority);
  if (!item)
    return;
  memcpy(item->uid, &uid, sizeof(uid));
  {
    uid_table_value* const entry = uid_table_get(&m_uber_ps_cache.shader_map, &uid);
    if (entry)
      entry->pending = 1;
  }
  QueueWork(item);
}

void ShaderCache::QueuePipelineCompile(const GXPipelineUid& uid, u32 priority)
{
  WorkItem* item = AllocWork(WORK_PIPELINE, priority);
  if (!item)
    return;
  memcpy(item->uid, &uid, sizeof(uid));
  QueueWork(item);
  {
    uid_table_value* const entry = uid_table_get(&m_gx_pipeline_cache, &uid);
    if (entry)
      entry->pending = 1;
  }
}

void ShaderCache::QueueUberPipelineCompile(const GXUberPipelineUid& uid, u32 priority)
{
  WorkItem* item = AllocWork(WORK_UBER_PIPELINE, priority);
  if (!item)
    return;
  memcpy(item->uid, &uid, sizeof(uid));
  QueueWork(item);
  {
    uid_table_value* const entry = uid_table_get(&m_gx_uber_pipeline_cache, &uid);
    if (entry)
      entry->pending = 1;
  }
}

void ShaderCache::QueueUberShaderPipelines()
{
  // Create a dummy vertex format with no attributes.
  // All attributes will be enabled in GetUberVertexFormat.
  PortableVertexDeclaration dummy_vertex_decl = {};
  dummy_vertex_decl.position.components = 4;
  dummy_vertex_decl.position.type = ComponentFormat::Float;
  dummy_vertex_decl.position.enable = true;
  dummy_vertex_decl.stride = sizeof(float) * 4;
  NativeVertexFormat* dummy_vertex_format =
      VertexLoaderManager::GetUberVertexFormat(dummy_vertex_decl);
  auto QueueDummyPipeline =
      [&](const UberShader::VertexShaderUid& vs_uid, const GeometryShaderUid& gs_uid,
          const UberShader::PixelShaderUid& ps_uid, const BlendingState& blend) {
        GXUberPipelineUid config;
        config.vertex_format = dummy_vertex_format;
        config.vs_uid = vs_uid;
        config.gs_uid = gs_uid;
        config.ps_uid = ps_uid;
        config.rasterization_state = RenderState::GetCullBackFaceRasterizationState(
            static_cast<PrimitiveType>(gs_uid.GetUidData()->primitive_type));
        config.depth_state = RenderState::GetNoDepthTestingDepthState();
        config.blending_state = blend;
        if (ps_uid.GetUidData()->uint_output)
        {
          // uint_output is only ever enabled when logic ops are enabled.
          config.blending_state.logic_op_enable = true;
          config.blending_state.logic_mode = LogicOp::And;
        }

        // an empty entry, compiled afterwards, if it is new
        GXUberPipelineUid key;
        UberPipelineKey(m_api_type, m_host_config, config, &key, nullptr);
        uid_table_get(&m_gx_uber_pipeline_cache, &key);
      };

  // Populate the pipeline configs with empty entries, these will be compiled afterwards.
  UberShader::VertexShaderUid vuids[UberShader::VERTEX_UBERSHADER_UIDS];
  UberShader::PixelShaderUid puids[UberShader::PIXEL_UBERSHADER_UIDS];
  GeometryShaderUid guids[GEOMETRY_SHADER_UIDS];
  const u32 num_vuids = UberShader::EnumerateVertexShaderUids(vuids);
  const u32 num_puids = UberShader::EnumeratePixelShaderUids(puids);
  const u32 num_guids = EnumerateGeometryShaderUids(guids);
  for (u32 v = 0; v < num_vuids; v++)
  {
    const UberShader::VertexShaderUid& vuid = vuids[v];
    for (u32 p = 0; p < num_puids; p++)
    {
      // UIDs must have compatible texgens, a mismatching combination will never be queried.
      if (vuid.GetUidData()->num_texgens != puids[p].GetUidData()->num_texgens)
        continue;

      UberShader::PixelShaderUid cleared_puid = puids[p];
      UberShader::ClearUnusedPixelShaderUidBits(m_api_type, m_host_config, &cleared_puid);
      for (u32 g = 0; g < num_guids; g++)
      {
        const GeometryShaderUid& guid = guids[g];
        if (guid.GetUidData()->numTexGens != vuid.GetUidData()->num_texgens ||
            (!guid.GetUidData()->IsPassthrough() && !m_host_config.backend_geometry_shaders))
        {
          continue;
        }
        BlendingState blend = RenderState::GetNoBlendingBlendState();
        QueueDummyPipeline(vuid, guid, cleared_puid, blend);
        /* a logic op draws with dual-source output still asked for, as
         * BlendingState::Generate leaves it */
        if (cleared_puid.GetUidData()->uint_output && !cleared_puid.GetUidData()->no_dual_src)
        {
          blend.use_dual_src = true;
          QueueDummyPipeline(vuid, guid, cleared_puid, blend);
          blend.use_dual_src = false;
        }
        if (g_backend_info.bSupportsDynamicVertexLoader)
        {
          // Not all GPUs need all the pipeline state compiled into shaders, so they tend to key
          // compiled shaders based on some subset of the pipeline state.
          // Some test results:
          //   (GPUs tested: AMD Radeon Pro 5600M, Nvidia GT 750M, Intel UHD 630,
          //    Intel Iris Pro 5200, Apple M1)
          // MacOS Metal:
          //  - AMD, Nvidia, Intel GPUs: Shaders are keyed on vertex layout and whether or not
          //    dual source blend is enabled.  That's it.
          //  - Apple GPUs: Shaders are keyed on vertex layout and all blending settings.  We use
          //    framebuffer fetch here, so the only blending settings used by ubershaders are the
          //    alpha_update and color_update ones.  Also keyed on primitive type, but Metal
          //    supports setting it to "unknown" and we do for ubershaders (but MoltenVK won't).
          // Windows Vulkan:
          //  - AMD, Nvidia: Definitely keyed on dual source blend, but the others seem more random
          //    Changing a setting on one shader will require a recompile, but changing the same
          //    setting on another won't.  Compiling a copy with alpha_update off, color_update off,
          //    and one with DSB on seems to get pretty good coverage though.
          // Windows D3D12:
          //  - AMD: Keyed on dual source blend and vertex layout
          //  - Nvidia Kepler: No recompiles for changes to vertex layout or blend
          blend.alpha_update = false;
          QueueDummyPipeline(vuid, guid, cleared_puid, blend);
          blend.alpha_update = true;
          blend.color_update = false;
          QueueDummyPipeline(vuid, guid, cleared_puid, blend);
          blend.color_update = true;
          if (!cleared_puid.GetUidData()->no_dual_src && !cleared_puid.GetUidData()->uint_output)
          {
            blend.blend_enable = true;
            blend.use_dual_src = true;
            blend.src_factor = SrcBlendFactor::SrcAlpha;
            blend.dst_factor = DstBlendFactor::InvSrcAlpha;
            QueueDummyPipeline(vuid, guid, cleared_puid, blend);
          }
        }
      }
    }
  }
}

AbstractPipeline* ShaderCache::CompileEFBCopyToVRAMPipeline(const EFBCopyFormat* format) const
{
  AbstractShader* const shader = g_gfx->CreateShaderFromSource(
      ShaderStage::Pixel, TextureConversionShaderGen::GeneratePixelShader(m_api_type, format),
      "EFB copy to VRAM pixel shader");
  AbstractPipelineConfig config = {};
  if (!shader)
    return nullptr;
  config.vertex_format = nullptr;
  config.vertex_shader = m_efb_copy_vertex_shader;
  config.geometry_shader =
      UseGeometryShaderForEFBCopies() ? m_texcoord_geometry_shader : nullptr;
  config.pixel_shader = shader;
  config.rasterization_state = RenderState::GetNoCullRasterizationState(PrimitiveType::Triangles);
  config.depth_state = RenderState::GetNoDepthTestingDepthState();
  config.blending_state = RenderState::GetNoBlendingBlendState();
  config.framebuffer_state = RenderState::GetRGBA8FramebufferState();
  config.usage = AbstractPipelineUsage::Utility;
  return PipelineOnce(config, shader);
}

const AbstractPipeline* ShaderCache::GetEFBCopyToVRAMPipeline(EFBCopyFormat format)
{
  const u32 i = static_cast<u32>(format) & 15;
  if (m_efb_copy_format_pipelines[i])
    return m_efb_copy_format_pipelines[i];
  /* built on a worker, while the one for every format draws */
  if (!m_efb_copy_format_asked[i] && m_async_shader_compiler.HasWorkerThreads())
  {
    WorkItem* const item = AllocWork(WORK_EFB_COPY_TO_VRAM_PIPELINE, COMPILE_PRIORITY_ONDEMAND_PIPELINE);
    if (item)
    {
      memcpy(item->uid, &format, sizeof(format));
      m_efb_copy_format_asked[i] = true;
      m_async_shader_compiler.QueueWorkItem(&item->base, item->base.priority);
    }
  }
  return m_efb_copy_to_vram_pipeline;
}

const AbstractPipeline* ShaderCache::GetEFBCopyToRAMPipeline(const EFBCopyParams& uid)
{
  /* the fields alone, not the padding between them */
  const u32 key[3] = {static_cast<u32>(uid.efb_format), static_cast<u32>(uid.copy_format),
                      static_cast<u32>(uid.depth) | static_cast<u32>(uid.yuv) << 1 |
                          static_cast<u32>(uid.all_copy_filter_coefs_needed) << 2 |
                          static_cast<u32>(uid.copy_filter_can_overflow) << 3 |
                          static_cast<u32>(uid.apply_gamma) << 4};
  const uid_table_value* found = uid_table_find(&m_efb_copy_to_ram_pipelines, key);
  if (found)
    return PipelineOf(found);
  STUTTER_MARK(t);
  ShaderCode shader_code =
      TextureConversionShaderTiled::GenerateEncodingShader(uid, m_api_type);
  AbstractShader* const shader =
      g_gfx->CreateShaderFromSource(ShaderStage::Pixel, shader_code,
                                    fmt::format("EFB copy to RAM pixel shader: {}", uid));
  if (!shader)
  {
    uid_table_get(&m_efb_copy_to_ram_pipelines, key);
    return nullptr;
  }

  AbstractPipelineConfig config = {};
  config.vertex_shader = m_screen_quad_vertex_shader;
  config.pixel_shader = shader;
  config.rasterization_state = RenderState::GetNoCullRasterizationState(PrimitiveType::Triangles);
  config.depth_state = RenderState::GetNoDepthTestingDepthState();
  config.blending_state = RenderState::GetNoBlendingBlendState();
  config.framebuffer_state = RenderState::GetColorFramebufferState(AbstractTextureFormat::BGRA8);
  config.usage = AbstractPipelineUsage::Utility;
  const AbstractPipeline* const kept =
      KeepPipeline(&m_efb_copy_to_ram_pipelines, key, PipelineOnce(config, shader));
  STUTTER_SINCE(STUTTER_DRAW_EFB_COPY_TO_RAM, t);
  return kept;
}

bool ShaderCache::CompileSharedPipelines()
{
  m_screen_quad_vertex_shader = g_gfx->CreateShaderFromSource(
      ShaderStage::Vertex, FramebufferShaderGen::GenerateScreenQuadVertexShader(),
      "Screen quad vertex shader");
  m_texture_copy_vertex_shader = g_gfx->CreateShaderFromSource(
      ShaderStage::Vertex, FramebufferShaderGen::GenerateTextureCopyVertexShader(),
      "Texture copy vertex shader");
  m_efb_copy_vertex_shader = g_gfx->CreateShaderFromSource(
      ShaderStage::Vertex, TextureConversionShaderGen::GenerateVertexShader(m_api_type), "EFB copy vertex shader");
  if (!m_screen_quad_vertex_shader || !m_texture_copy_vertex_shader || !m_efb_copy_vertex_shader)
    return false;

  if (UseGeometryShaderForEFBCopies())
  {
    m_texcoord_geometry_shader = g_gfx->CreateShaderFromSource(
        ShaderStage::Geometry, FramebufferShaderGen::GeneratePassthroughGeometryShader(1, 0), "Texcoord passthrough geometry shader");
    m_color_geometry_shader = g_gfx->CreateShaderFromSource(
        ShaderStage::Geometry, FramebufferShaderGen::GeneratePassthroughGeometryShader(0, 1), "Color passthrough geometry shader");
    if (!m_texcoord_geometry_shader || !m_color_geometry_shader)
      return false;
  }

  m_texture_copy_pixel_shader = g_gfx->CreateShaderFromSource(
      ShaderStage::Pixel, FramebufferShaderGen::GenerateTextureCopyPixelShader(),
      "Texture copy pixel shader");
  m_color_pixel_shader = g_gfx->CreateShaderFromSource(
      ShaderStage::Pixel, FramebufferShaderGen::GenerateColorPixelShader(),
      "Color pixel shader");
  if (!m_texture_copy_pixel_shader || !m_color_pixel_shader)
    return false;

  AbstractPipelineConfig config;
  config.vertex_format = nullptr;
  config.vertex_shader = m_texture_copy_vertex_shader;
  config.geometry_shader = nullptr;
  config.pixel_shader = m_texture_copy_pixel_shader;
  config.rasterization_state = RenderState::GetNoCullRasterizationState(PrimitiveType::Triangles);
  config.depth_state = RenderState::GetNoDepthTestingDepthState();
  config.blending_state = RenderState::GetNoBlendingBlendState();
  config.framebuffer_state = RenderState::GetRGBA8FramebufferState();
  config.usage = AbstractPipelineUsage::Utility;
  m_copy_rgba8_pipeline = g_gfx->CreatePipeline(config);
  if (!m_copy_rgba8_pipeline)
    return false;

  m_efb_copy_to_vram_pipeline = CompileEFBCopyToVRAMPipeline(nullptr);
  if (!m_efb_copy_to_vram_pipeline)
    return false;

  if (UseGeometryShaderForEFBCopies())
  {
    config.geometry_shader = m_texcoord_geometry_shader;
    m_rgba8_stereo_copy_pipeline = g_gfx->CreatePipeline(config);
    if (!m_rgba8_stereo_copy_pipeline)
      return false;
  }

  if (m_host_config.backend_palette_conversion)
  {
    config.vertex_shader = m_screen_quad_vertex_shader;
    config.geometry_shader = nullptr;

    for (size_t i = 0; i < NUM_PALETTE_CONVERSION_SHADERS; i++)
    {
      TLUTFormat format = static_cast<TLUTFormat>(i);
      AbstractShader* const shader = g_gfx->CreateShaderFromSource(
          ShaderStage::Pixel,
          TextureConversionShaderTiled::GeneratePaletteConversionShader(format, m_api_type),
          fmt::format("Palette conversion pixel shader: {}", format));
      if (!shader)
        return false;

      config.pixel_shader = shader;
      m_palette_conversion_pipelines[i] = PipelineOnce(config, shader);
      if (!m_palette_conversion_pipelines[i])
        return false;
    }
  }

  return true;
}

const AbstractPipeline* ShaderCache::GetPaletteConversionPipeline(TLUTFormat format)
{
  ASSERT(static_cast<size_t>(format) < NUM_PALETTE_CONVERSION_SHADERS);
  return m_palette_conversion_pipelines[static_cast<size_t>(format)];
}

const AbstractPipeline* ShaderCache::GetTextureReinterpretPipeline(TextureFormat from_format,
                                                                   TextureFormat to_format)
{
  const u32 key[2] = {static_cast<u32>(from_format), static_cast<u32>(to_format)};
  const uid_table_value* found = uid_table_find(&m_texture_reinterpret_pipelines, key);
  if (found)
    return PipelineOf(found);
  /* tried once: a format pair with no shader stays without */
  if (!uid_table_get(&m_texture_reinterpret_pipelines, key))
    return nullptr;
  STUTTER_MARK(t);

  ShaderCode shader_source =
      FramebufferShaderGen::GenerateTextureReinterpretShader(from_format, to_format);
  if (shader_source.GetBuffer().empty())
    return nullptr;

  AbstractShader* const shader = g_gfx->CreateShaderFromSource(
      ShaderStage::Pixel, shader_source,
      fmt::format("Texture reinterpret pixel shader: {} to {}", from_format, to_format));
  if (!shader)
    return nullptr;

  AbstractPipelineConfig config;
  config.vertex_format = nullptr;
  config.vertex_shader = m_screen_quad_vertex_shader;
  config.geometry_shader = nullptr;
  config.pixel_shader = shader;
  config.rasterization_state = RenderState::GetNoCullRasterizationState(PrimitiveType::Triangles);
  config.depth_state = RenderState::GetNoDepthTestingDepthState();
  config.blending_state = RenderState::GetNoBlendingBlendState();
  config.framebuffer_state = RenderState::GetRGBA8FramebufferState();
  config.usage = AbstractPipelineUsage::Utility;
  const AbstractPipeline* const kept =
      KeepPipeline(&m_texture_reinterpret_pipelines, key, PipelineOnce(config, shader));
  STUTTER_SINCE(STUTTER_DRAW_REINTERPRET, t);
  return kept;
}

const AbstractShader*
ShaderCache::GetTextureDecodingShader(TextureFormat format,
                                      std::optional<TLUTFormat> palette_format)
{
  const u32 key[2] = {static_cast<u32>(format),
                      static_cast<u32>(palette_format.value_or(TLUTFormat::IA8))};
  const uid_table_value* found = uid_table_find(&m_texture_decoding_shaders, key);
  if (found)
    return ShaderOf(found);
  /* tried once: a format with no shader stays without */
  if (!uid_table_get(&m_texture_decoding_shaders, key))
    return nullptr;
  STUTTER_MARK(t);

  ShaderCode shader_source =
      TextureConversionShaderTiled::GenerateDecodingShader(format, palette_format, APIType::OpenGL);
  if (shader_source.GetBuffer().empty())
    return nullptr;

  const std::string name =
      palette_format.has_value() ?
          fmt::format("Texture decoding compute shader: {}, {}", format, *palette_format) :
          fmt::format("Texture decoding compute shader: {}", format);

  AbstractShader* const shader =
      g_gfx->CreateShaderFromSource(ShaderStage::Compute, shader_source, name);
  if (!shader)
    return nullptr;

  uid_table_value* value = uid_table_get(&m_texture_decoding_shaders, key);
  value->object = shader;
  STUTTER_SINCE(STUTTER_DRAW_DECODE, t);
  return ShaderOf(value);
}
}  // namespace VideoCommon
