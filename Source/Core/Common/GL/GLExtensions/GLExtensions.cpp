// Copyright 2013 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/GL/GLExtensions/GLExtensions.h"

#include <unordered_map>

#include "Common/GL/GLContext.h"
#include "Common/Logging/Log.h"

#if defined(__linux__) || defined(__APPLE__)
#include <dlfcn.h>
#endif

// gl_1_1
PFNDOLCLEARCOLORPROC dolClearColor;
PFNDOLCLEARPROC dolClear;
PFNDOLCOLORMASKPROC dolColorMask;
PFNDOLLOGICOPPROC dolLogicOp;
PFNDOLFRONTFACEPROC dolFrontFace;
PFNDOLSCISSORPROC dolScissor;
PFNDOLDRAWBUFFERPROC dolDrawBuffer;
PFNDOLENABLEPROC dolEnable;
PFNDOLDISABLEPROC dolDisable;
PFNDOLENABLECLIENTSTATEPROC dolEnableClientState;
PFNDOLGETINTEGERVPROC dolGetIntegerv;
PFNDOLGETERRORPROC dolGetError;
PFNDOLGETSTRINGPROC dolGetString;
PFNDOLFINISHPROC dolFinish;
PFNDOLFLUSHPROC dolFlush;
PFNDOLCLEARDEPTHPROC dolClearDepth;
PFNDOLDEPTHFUNCPROC dolDepthFunc;
PFNDOLDEPTHMASKPROC dolDepthMask;
PFNDOLDEPTHRANGEPROC dolDepthRange;
PFNDOLVIEWPORTPROC dolViewport;
PFNDOLDRAWARRAYSPROC dolDrawArrays;
PFNDOLDRAWELEMENTSPROC dolDrawElements;
PFNDOLPIXELSTOREIPROC dolPixelStorei;
PFNDOLREADPIXELSPROC dolReadPixels;
PFNDOLTEXPARAMETERIPROC dolTexParameteri;
PFNDOLTEXIMAGE2DPROC dolTexImage2D;
PFNDOLGENTEXTURESPROC dolGenTextures;
PFNDOLDELETETEXTURESPROC dolDeleteTextures;
PFNDOLBINDTEXTUREPROC dolBindTexture;
PFNDOLTEXSUBIMAGE2DPROC dolTexSubImage2D;

// gl_1_2
PFNDOLTEXIMAGE3DPROC dolTexImage3D;
PFNDOLTEXSUBIMAGE3DPROC dolTexSubImage3D;

// gl_1_3
PFNDOLACTIVETEXTUREARBPROC dolActiveTexture;
PFNDOLCOMPRESSEDTEXIMAGE2DPROC dolCompressedTexImage2D;
PFNDOLCOMPRESSEDTEXIMAGE3DPROC dolCompressedTexImage3D;
PFNDOLCOMPRESSEDTEXSUBIMAGE2DPROC dolCompressedTexSubImage2D;
PFNDOLCOMPRESSEDTEXSUBIMAGE3DPROC dolCompressedTexSubImage3D;

// gl_1_4
PFNDOLBLENDFUNCSEPARATEPROC dolBlendFuncSeparate;

// gl_1_5
PFNDOLBEGINQUERYPROC dolBeginQuery;
PFNDOLBINDBUFFERPROC dolBindBuffer;
PFNDOLBUFFERDATAPROC dolBufferData;
PFNDOLBUFFERSUBDATAPROC dolBufferSubData;
PFNDOLDELETEBUFFERSPROC dolDeleteBuffers;
PFNDOLDELETEQUERIESPROC dolDeleteQueries;
PFNDOLENDQUERYPROC dolEndQuery;
PFNDOLGENBUFFERSPROC dolGenBuffers;
PFNDOLGENQUERIESPROC dolGenQueries;
PFNDOLGETBUFFERSUBDATAPROC dolGetBufferSubData;
PFNDOLGETQUERYOBJECTUIVPROC dolGetQueryObjectuiv;
PFNDOLUNMAPBUFFERPROC dolUnmapBuffer;

// gl_2_0
PFNDOLATTACHSHADERPROC dolAttachShader;
PFNDOLBINDATTRIBLOCATIONPROC dolBindAttribLocation;
PFNDOLBLENDEQUATIONSEPARATEPROC dolBlendEquationSeparate;
PFNDOLCOMPILESHADERPROC dolCompileShader;
PFNDOLCREATEPROGRAMPROC dolCreateProgram;
PFNDOLCREATESHADERPROC dolCreateShader;
PFNDOLDELETEPROGRAMPROC dolDeleteProgram;
PFNDOLDELETESHADERPROC dolDeleteShader;
PFNDOLDRAWBUFFERSPROC dolDrawBuffers;
PFNDOLENABLEVERTEXATTRIBARRAYPROC dolEnableVertexAttribArray;
PFNDOLGETPROGRAMINFOLOGPROC dolGetProgramInfoLog;
PFNDOLGETPROGRAMIVPROC dolGetProgramiv;
PFNDOLGETSHADERINFOLOGPROC dolGetShaderInfoLog;
PFNDOLGETSHADERIVPROC dolGetShaderiv;
PFNDOLGETUNIFORMLOCATIONPROC dolGetUniformLocation;
PFNDOLLINKPROGRAMPROC dolLinkProgram;
PFNDOLSHADERSOURCEPROC dolShaderSource;
PFNDOLUNIFORM1IPROC dolUniform1i;
PFNDOLUSEPROGRAMPROC dolUseProgram;
PFNDOLVERTEXATTRIBPOINTERPROC dolVertexAttribPointer;

// gl_2_1

// gl_3_0
PFNDOLGETSTRINGIPROC dolGetStringi;
PFNDOLVERTEXATTRIBIPOINTERPROC dolVertexAttribIPointer;

// gl_3_1
PFNDOLPRIMITIVERESTARTINDEXPROC dolPrimitiveRestartIndex;
PFNDOLTEXBUFFERPROC dolTexBuffer;

// gl_3_2
PFNDOLFRAMEBUFFERTEXTUREPROC dolFramebufferTexture;

// gl 4_2
PFNDOLGETINTERNALFORMATIVPROC dolGetInternalformativ;

// gl_4_3

// gl_4_4

// gl_4_5
PFNDOLGETTEXTURESUBIMAGEPROC dolGetTextureSubImage;

// ARB_uniform_buffer_object
PFNDOLBINDBUFFERBASEPROC dolBindBufferBase;
PFNDOLBINDBUFFERRANGEPROC dolBindBufferRange;
PFNDOLGETUNIFORMBLOCKINDEXPROC dolGetUniformBlockIndex;
PFNDOLUNIFORMBLOCKBINDINGPROC dolUniformBlockBinding;

// ARB_sampler_objects
PFNDOLBINDSAMPLERPROC dolBindSampler;
PFNDOLDELETESAMPLERSPROC dolDeleteSamplers;
PFNDOLGENSAMPLERSPROC dolGenSamplers;
PFNDOLSAMPLERPARAMETERFPROC dolSamplerParameterf;
PFNDOLSAMPLERPARAMETERIPROC dolSamplerParameteri;

// ARB_map_buffer_range
PFNDOLFLUSHMAPPEDBUFFERRANGEPROC dolFlushMappedBufferRange;
PFNDOLMAPBUFFERRANGEPROC dolMapBufferRange;

// ARB_vertex_array_object
PFNDOLBINDVERTEXARRAYPROC dolBindVertexArray;
PFNDOLDELETEVERTEXARRAYSPROC dolDeleteVertexArrays;
PFNDOLGENVERTEXARRAYSPROC dolGenVertexArrays;

// ARB_framebuffer_object
PFNDOLBINDFRAMEBUFFERPROC dolBindFramebuffer;
PFNDOLBLITFRAMEBUFFERPROC dolBlitFramebuffer;
PFNDOLCHECKFRAMEBUFFERSTATUSPROC dolCheckFramebufferStatus;
PFNDOLDELETEFRAMEBUFFERSPROC dolDeleteFramebuffers;
PFNDOLFRAMEBUFFERTEXTURELAYERPROC dolFramebufferTextureLayer;
PFNDOLGENFRAMEBUFFERSPROC dolGenFramebuffers;

// ARB_get_program_binary
PFNDOLGETPROGRAMBINARYPROC dolGetProgramBinary;
PFNDOLPROGRAMBINARYPROC dolProgramBinary;
PFNDOLPROGRAMPARAMETERIPROC dolProgramParameteri;

// ARB_sync
PFNDOLCLIENTWAITSYNCPROC dolClientWaitSync;
PFNDOLDELETESYNCPROC dolDeleteSync;
PFNDOLFENCESYNCPROC dolFenceSync;

// ARB_texture_multisample
PFNDOLTEXIMAGE2DMULTISAMPLEPROC dolTexImage2DMultisample;
PFNDOLTEXIMAGE3DMULTISAMPLEPROC dolTexImage3DMultisample;

// ARB_texture_storage
PFNDOLTEXSTORAGE2DPROC dolTexStorage2D;
PFNDOLTEXSTORAGE3DPROC dolTexStorage3D;

// ARB_texture_storage_multisample
PFNDOLTEXSTORAGE2DMULTISAMPLEPROC dolTexStorage2DMultisample;
PFNDOLTEXSTORAGE3DMULTISAMPLEPROC dolTexStorage3DMultisample;

// ARB_ES2_compatibility
PFNDOLCLEARDEPTHFPROC dolClearDepthf;
PFNDOLDEPTHRANGEFPROC dolDepthRangef;

// NV_primitive_restart
PFNDOLPRIMITIVERESTARTINDEXNVPROC dolPrimitiveRestartIndexNV;

// ARB_blend_func_extended
PFNDOLBINDFRAGDATALOCATIONINDEXEDPROC dolBindFragDataLocationIndexed;

// ARB_viewport_array
PFNDOLVIEWPORTINDEXEDFPROC dolViewportIndexedf;

// ARB_draw_elements_base_vertex
PFNDOLDRAWELEMENTSBASEVERTEXPROC dolDrawElementsBaseVertex;

// ARB_sample_shading

// ARB_debug_output
PFNDOLDEBUGMESSAGECALLBACKARBPROC dolDebugMessageCallbackARB;
PFNDOLDEBUGMESSAGECONTROLARBPROC dolDebugMessageControlARB;

// KHR_debug
PFNDOLDEBUGMESSAGECALLBACKPROC dolDebugMessageCallback;
PFNDOLDEBUGMESSAGECONTROLPROC dolDebugMessageControl;
PFNDOLOBJECTLABELPROC dolObjectLabel;

// ARB_buffer_storage
PFNDOLBUFFERSTORAGEPROC dolBufferStorage;

// GL_NV_occlusion_query_samples
PFNDOLGENOCCLUSIONQUERIESNVPROC dolGenOcclusionQueriesNV;
PFNDOLDELETEOCCLUSIONQUERIESNVPROC dolDeleteOcclusionQueriesNV;
PFNDOLBEGINOCCLUSIONQUERYNVPROC dolBeginOcclusionQueryNV;
PFNDOLENDOCCLUSIONQUERYNVPROC dolEndOcclusionQueryNV;
PFNDOLGETOCCLUSIONQUERYUIVNVPROC dolGetOcclusionQueryuivNV;

// ARB_clip_control
PFNDOLCLIPCONTROLPROC dolClipControl;

// ARB_copy_image
PFNDOLCOPYIMAGESUBDATAPROC dolCopyImageSubData;

// ARB_shader_storage_buffer_object

// NV_depth_buffer_float

// ARB_shader_image_load_store
PFNDOLBINDIMAGETEXTUREPROC dolBindImageTexture;
PFNDOLMEMORYBARRIERPROC dolMemoryBarrier;

// ARB_compute_shader
PFNDOLDISPATCHCOMPUTEPROC dolDispatchCompute;

// Creates a GLFunc object that requires a feature
#define GLFUNC_REQUIRES(x, y) {(void**)&x, #x, y}
// Creates a GLFunc object with a different function suffix
// For when we want to use the same function pointer, but different function name
#define GLFUNC_SUFFIX(x, y, z) {(void**)&x, #x #y, z}
// Creates a GLFunc object that should always be able to get grabbed
// Used for Desktop OpenGL functions that should /always/ be provided.
// aka GL 1.1/1.2/1.3/1.4
#define GLFUNC_ALWAYS_REQUIRED(x) {(void**)&x, #x, "VERSION_GL"}
// Creates a GLFunc object that should be able to get grabbed
// on both GL and ES
#define GL_ES_FUNC_ALWAYS_REQUIRED(x) {(void**)&x, #x, "VERSION_GL |VERSION_GLES_2"}
// Creates a GLFunc object that should be able to get grabbed
// on both GL and ES 3.0
#define GL_ES3_FUNC_ALWAYS_REQUIRED(x) {(void**)&x, #x, "VERSION_GL |VERSION_GLES_3"}
// Creates a GLFunc object that should be able to get grabbed
// on both GL and ES 3.2
#define GL_ES32_FUNC_ALWAYS_REQUIRED(x) {(void**)&x, #x, "VERSION_GL |VERSION_GLES_3_2"}

struct GLFunc
{
  void** function_ptr;
  const char* function_name;
  const char* requirements;
};

const GLFunc gl_function_array[] = {
    // gl_1_1
    GLFUNC_ALWAYS_REQUIRED(glLogicOp),
    GLFUNC_ALWAYS_REQUIRED(glDrawBuffer),
    GLFUNC_ALWAYS_REQUIRED(glEnableClientState),
    GLFUNC_ALWAYS_REQUIRED(glClearDepth),
    GLFUNC_ALWAYS_REQUIRED(glDepthRange),
    GL_ES_FUNC_ALWAYS_REQUIRED(glTexImage2D),
    GL_ES_FUNC_ALWAYS_REQUIRED(glClearColor),
    GL_ES_FUNC_ALWAYS_REQUIRED(glClear),
    GL_ES_FUNC_ALWAYS_REQUIRED(glColorMask),
    GL_ES_FUNC_ALWAYS_REQUIRED(glFrontFace),
    GL_ES_FUNC_ALWAYS_REQUIRED(glScissor),
    GL_ES_FUNC_ALWAYS_REQUIRED(glEnable),
    GL_ES_FUNC_ALWAYS_REQUIRED(glDisable),
    GL_ES_FUNC_ALWAYS_REQUIRED(glFinish),
    GL_ES_FUNC_ALWAYS_REQUIRED(glFlush),
    GL_ES_FUNC_ALWAYS_REQUIRED(glDepthFunc),
    GL_ES_FUNC_ALWAYS_REQUIRED(glDepthMask),
    GL_ES_FUNC_ALWAYS_REQUIRED(glViewport),
    GL_ES_FUNC_ALWAYS_REQUIRED(glDrawArrays),
    GL_ES_FUNC_ALWAYS_REQUIRED(glDrawElements),
    GL_ES_FUNC_ALWAYS_REQUIRED(glPixelStorei),
    GL_ES_FUNC_ALWAYS_REQUIRED(glReadPixels),
    GL_ES_FUNC_ALWAYS_REQUIRED(glTexParameteri),
    GL_ES_FUNC_ALWAYS_REQUIRED(glGenTextures),
    GL_ES_FUNC_ALWAYS_REQUIRED(glDeleteTextures),
    GL_ES_FUNC_ALWAYS_REQUIRED(glBindTexture),
    GL_ES_FUNC_ALWAYS_REQUIRED(glTexSubImage2D),

    // gl_1_2
    GL_ES3_FUNC_ALWAYS_REQUIRED(glTexImage3D),
    GL_ES3_FUNC_ALWAYS_REQUIRED(glTexSubImage3D),

    // gl_1_3
    GL_ES_FUNC_ALWAYS_REQUIRED(glActiveTexture),
    GL_ES_FUNC_ALWAYS_REQUIRED(glCompressedTexImage2D),
    GL_ES_FUNC_ALWAYS_REQUIRED(glCompressedTexSubImage2D),
    GL_ES3_FUNC_ALWAYS_REQUIRED(glCompressedTexImage3D),
    GL_ES3_FUNC_ALWAYS_REQUIRED(glCompressedTexSubImage3D),

    // gl_1_4
    GL_ES_FUNC_ALWAYS_REQUIRED(glBlendFuncSeparate),

    // gl_1_5
    GLFUNC_ALWAYS_REQUIRED(glGetBufferSubData),
    GL_ES_FUNC_ALWAYS_REQUIRED(glBindBuffer),
    GL_ES_FUNC_ALWAYS_REQUIRED(glBufferData),
    GL_ES_FUNC_ALWAYS_REQUIRED(glBufferSubData),
    GL_ES_FUNC_ALWAYS_REQUIRED(glDeleteBuffers),
    GL_ES_FUNC_ALWAYS_REQUIRED(glGenBuffers),
    GL_ES3_FUNC_ALWAYS_REQUIRED(glBeginQuery),
    GL_ES3_FUNC_ALWAYS_REQUIRED(glDeleteQueries),
    GL_ES3_FUNC_ALWAYS_REQUIRED(glEndQuery),
    GL_ES3_FUNC_ALWAYS_REQUIRED(glGenQueries),
    GL_ES3_FUNC_ALWAYS_REQUIRED(glGetQueryObjectuiv),
    GL_ES3_FUNC_ALWAYS_REQUIRED(glUnmapBuffer),

    // gl_2_0
    GL_ES_FUNC_ALWAYS_REQUIRED(glAttachShader),
    GL_ES_FUNC_ALWAYS_REQUIRED(glBindAttribLocation),
    GL_ES_FUNC_ALWAYS_REQUIRED(glBlendEquationSeparate),
    GL_ES_FUNC_ALWAYS_REQUIRED(glCompileShader),
    GL_ES_FUNC_ALWAYS_REQUIRED(glCreateProgram),
    GL_ES_FUNC_ALWAYS_REQUIRED(glCreateShader),
    GL_ES_FUNC_ALWAYS_REQUIRED(glDeleteProgram),
    GL_ES_FUNC_ALWAYS_REQUIRED(glDeleteShader),
    GL_ES_FUNC_ALWAYS_REQUIRED(glEnableVertexAttribArray),
    GL_ES_FUNC_ALWAYS_REQUIRED(glGetProgramInfoLog),
    GL_ES_FUNC_ALWAYS_REQUIRED(glGetProgramiv),
    GL_ES_FUNC_ALWAYS_REQUIRED(glGetShaderInfoLog),
    GL_ES_FUNC_ALWAYS_REQUIRED(glGetShaderiv),
    GL_ES_FUNC_ALWAYS_REQUIRED(glGetUniformLocation),
    GL_ES_FUNC_ALWAYS_REQUIRED(glLinkProgram),
    GL_ES_FUNC_ALWAYS_REQUIRED(glShaderSource),
    GL_ES_FUNC_ALWAYS_REQUIRED(glUniform1i),
    GL_ES_FUNC_ALWAYS_REQUIRED(glUseProgram),
    GL_ES_FUNC_ALWAYS_REQUIRED(glVertexAttribPointer),
    GL_ES3_FUNC_ALWAYS_REQUIRED(glDrawBuffers),

    // gl_2_1

    // gl_3_0
    GLFUNC_REQUIRES(glGetStringi, "VERSION_3_0 |VERSION_GLES_3"),
    GLFUNC_REQUIRES(glVertexAttribIPointer, "VERSION_3_0 |VERSION_GLES_3"),

    // gl_3_1
    GLFUNC_REQUIRES(glPrimitiveRestartIndex, "VERSION_3_1"),
    GLFUNC_REQUIRES(glTexBuffer, "VERSION_3_1 |VERSION_GLES_3_2"),

    // gl_3_2
    GLFUNC_REQUIRES(glFramebufferTexture, "VERSION_3_2 |VERSION_GLES_3_2"),

    // gl_4_2
    GLFUNC_REQUIRES(glGetInternalformativ, "VERSION_4_2 |VERSION_GLES_3"),
    GLFUNC_REQUIRES(glBindImageTexture, "VERSION_4_2"),
    GLFUNC_REQUIRES(glMemoryBarrier, "VERSION_4_2"),
    GLFUNC_REQUIRES(glTexStorage2D, "VERSION_4_2"),
    GLFUNC_REQUIRES(glTexStorage3D, "VERSION_4_2"),

    // gl_4_3
    GLFUNC_REQUIRES(glDispatchCompute, "VERSION_4_3"),
    GLFUNC_REQUIRES(glCopyImageSubData, "VERSION_4_3"),
    GLFUNC_REQUIRES(glTexStorage2DMultisample, "VERSION_4_3"),
    GLFUNC_REQUIRES(glTexStorage3DMultisample, "VERSION_4_3"),
    GLFUNC_REQUIRES(glDebugMessageControl, "VERSION_4_3"),
    GLFUNC_REQUIRES(glDebugMessageCallback, "VERSION_4_3"),
    GLFUNC_REQUIRES(glObjectLabel, "VERSION_4_3"),

    // gl_4_4
    GLFUNC_REQUIRES(glBufferStorage, "VERSION_4_4"),

    // gl_4_5
    GLFUNC_REQUIRES(glClipControl, "VERSION_4_5"),
    GLFUNC_REQUIRES(glGetTextureSubImage, "VERSION_4_5"),
    // AMD's video driver is trash and doesn't expose these function pointers
    // Remove them for now until they learn how to implement the spec properly.

    // ARB_uniform_buffer_object
    GLFUNC_REQUIRES(glBindBufferBase, "GL_ARB_uniform_buffer_object |VERSION_GLES_3"),
    GLFUNC_REQUIRES(glBindBufferRange, "GL_ARB_uniform_buffer_object |VERSION_GLES_3"),
    GLFUNC_REQUIRES(glGetUniformBlockIndex, "GL_ARB_uniform_buffer_object |VERSION_GLES_3"),
    GLFUNC_REQUIRES(glUniformBlockBinding, "GL_ARB_uniform_buffer_object |VERSION_GLES_3"),

    // ARB_sampler_objects
    GLFUNC_REQUIRES(glBindSampler, "GL_ARB_sampler_objects |VERSION_GLES_3"),
    GLFUNC_REQUIRES(glDeleteSamplers, "GL_ARB_sampler_objects |VERSION_GLES_3"),
    GLFUNC_REQUIRES(glGenSamplers, "GL_ARB_sampler_objects |VERSION_GLES_3"),
    GLFUNC_REQUIRES(glSamplerParameterf, "GL_ARB_sampler_objects |VERSION_GLES_3"),
    GLFUNC_REQUIRES(glSamplerParameteri, "GL_ARB_sampler_objects |VERSION_GLES_3"),

    // ARB_map_buffer_range
    GLFUNC_REQUIRES(glFlushMappedBufferRange, "GL_ARB_map_buffer_range |VERSION_GLES_3"),
    GLFUNC_REQUIRES(glMapBufferRange, "GL_ARB_map_buffer_range |VERSION_GLES_3"),

    // ARB_vertex_array_object
    GLFUNC_REQUIRES(glBindVertexArray, "GL_ARB_vertex_array_object |VERSION_GLES_3"),
    GLFUNC_REQUIRES(glDeleteVertexArrays, "GL_ARB_vertex_array_object |VERSION_GLES_3"),
    GLFUNC_REQUIRES(glGenVertexArrays, "GL_ARB_vertex_array_object |VERSION_GLES_3"),

    // APPLE_vertex_array_object
    GLFUNC_SUFFIX(glBindVertexArray, APPLE,
                  "GL_APPLE_vertex_array_object !GL_ARB_vertex_array_object"),
    GLFUNC_SUFFIX(glDeleteVertexArrays, APPLE,
                  "GL_APPLE_vertex_array_object !GL_ARB_vertex_array_object"),
    GLFUNC_SUFFIX(glGenVertexArrays, APPLE,
                  "GL_APPLE_vertex_array_object !GL_ARB_vertex_array_object"),

    // ARB_framebuffer_object
    GLFUNC_REQUIRES(glBindFramebuffer, "GL_ARB_framebuffer_object |VERSION_GLES_2"),
    GLFUNC_REQUIRES(glBlitFramebuffer, "GL_ARB_framebuffer_object |VERSION_GLES_3"),
    GLFUNC_REQUIRES(glCheckFramebufferStatus, "GL_ARB_framebuffer_object |VERSION_GLES_2"),
    GLFUNC_REQUIRES(glDeleteFramebuffers, "GL_ARB_framebuffer_object |VERSION_GLES_2"),
    GLFUNC_REQUIRES(glFramebufferTextureLayer, "GL_ARB_framebuffer_object |VERSION_GLES_3"),
    GLFUNC_REQUIRES(glGenFramebuffers, "GL_ARB_framebuffer_object |VERSION_GLES_2"),

    // ARB_get_program_binary
    GLFUNC_REQUIRES(glGetProgramBinary, "GL_ARB_get_program_binary |VERSION_GLES_3"),
    GLFUNC_REQUIRES(glProgramBinary, "GL_ARB_get_program_binary |VERSION_GLES_3"),
    GLFUNC_REQUIRES(glProgramParameteri, "GL_ARB_get_program_binary |VERSION_GLES_3"),

    // ARB_sync
    GLFUNC_REQUIRES(glClientWaitSync, "GL_ARB_sync |VERSION_GLES_3"),
    GLFUNC_REQUIRES(glDeleteSync, "GL_ARB_sync |VERSION_GLES_3"),
    GLFUNC_REQUIRES(glFenceSync, "GL_ARB_sync |VERSION_GLES_3"),

    // ARB_texture_multisample
    GLFUNC_REQUIRES(glTexImage2DMultisample, "GL_ARB_texture_multisample"),
    GLFUNC_REQUIRES(glTexImage3DMultisample, "GL_ARB_texture_multisample"),

    // ARB_texture_storage
    GLFUNC_REQUIRES(glTexStorage2D, "GL_ARB_texture_storage !VERSION_4_2 |VERSION_GLES_3"),
    GLFUNC_REQUIRES(glTexStorage3D, "GL_ARB_texture_storage !VERSION_4_2 |VERSION_GLES_3"),

    // ARB_texture_storage_multisample
    GLFUNC_REQUIRES(glTexStorage2DMultisample,
                    "GL_ARB_texture_storage_multisample !VERSION_4_3 |VERSION_GLES_3_1"),
    GLFUNC_REQUIRES(glTexStorage3DMultisample,
                    "GL_ARB_texture_storage_multisample !VERSION_4_3 |VERSION_GLES_3_2"),
    GLFUNC_SUFFIX(glTexStorage3DMultisample, OES,
                  "GL_OES_texture_storage_multisample_2d_array !VERSION_GLES_3_2"),

    // ARB_ES2_compatibility
    GLFUNC_REQUIRES(glClearDepthf, "GL_ARB_ES2_compatibility |VERSION_GLES_2"),
    GLFUNC_REQUIRES(glDepthRangef, "GL_ARB_ES2_compatibility |VERSION_GLES_2"),

    // NV_primitive_restart
    GLFUNC_REQUIRES(glPrimitiveRestartIndexNV, "GL_NV_primitive_restart"),

    // ARB_blend_func_extended
    GLFUNC_REQUIRES(glBindFragDataLocationIndexed, "GL_ARB_blend_func_extended"),

    // ARB_viewport_array
    GLFUNC_REQUIRES(glViewportIndexedf, "GL_ARB_viewport_array"),

    // ARB_draw_elements_base_vertex
    GLFUNC_REQUIRES(glDrawElementsBaseVertex, "GL_ARB_draw_elements_base_vertex |VERSION_GLES_3_2"),

    // OES_draw_elements_base_vertex
    GLFUNC_SUFFIX(glDrawElementsBaseVertex, OES,
                  "GL_OES_draw_elements_base_vertex !VERSION_GLES_3_2"),

    // EXT_draw_elements_base_vertex
    GLFUNC_SUFFIX(
        glDrawElementsBaseVertex, EXT,
        "GL_EXT_draw_elements_base_vertex !GL_OES_draw_elements_base_vertex !VERSION_GLES_3_2"),

    // ARB_sample_shading

    // OES_sample_shading

    // ARB_debug_output
    GLFUNC_REQUIRES(glDebugMessageCallbackARB, "GL_ARB_debug_output"),
    GLFUNC_REQUIRES(glDebugMessageControlARB, "GL_ARB_debug_output"),

    // KHR_debug
    GLFUNC_SUFFIX(glDebugMessageCallback, KHR, "GL_KHR_debug VERSION_GLES_3"),
    GLFUNC_SUFFIX(glDebugMessageControl, KHR, "GL_KHR_debug VERSION_GLES_3"),
    GLFUNC_SUFFIX(glObjectLabel, KHR, "GL_KHR_debug VERSION_GLES_3"),
    GLFUNC_REQUIRES(glDebugMessageCallback,
                    "GL_KHR_debug !VERSION_GLES_3 !VERSION_GL_4_3 |VERSION_GLES_3_2"),
    GLFUNC_REQUIRES(glDebugMessageControl,
                    "GL_KHR_debug !VERSION_GLES_3 !VERSION_GL_4_3 |VERSION_GLES_3_2"),
    GLFUNC_REQUIRES(glObjectLabel,
                    "GL_KHR_debug !VERSION_GLES_3 !VERSION_GL_4_3 |VERSION_GLES_3_2"),

    // ARB_buffer_storage
    GLFUNC_REQUIRES(glBufferStorage, "GL_ARB_buffer_storage !VERSION_4_4"),

    // EXT_buffer_storage
    GLFUNC_SUFFIX(glBufferStorage, EXT,
                  "GL_EXT_buffer_storage !GL_ARB_buffer_storage !VERSION_4_4"),

    // EXT_geometry_shader
    GLFUNC_SUFFIX(glFramebufferTexture, EXT, "GL_EXT_geometry_shader !VERSION_3_2"),

    // NV_occlusion_query_samples
    GLFUNC_REQUIRES(glGenOcclusionQueriesNV, "GL_NV_occlusion_query_samples"),
    GLFUNC_REQUIRES(glDeleteOcclusionQueriesNV, "GL_NV_occlusion_query_samples"),
    GLFUNC_REQUIRES(glBeginOcclusionQueryNV, "GL_NV_occlusion_query_samples"),
    GLFUNC_REQUIRES(glEndOcclusionQueryNV, "GL_NV_occlusion_query_samples"),
    GLFUNC_REQUIRES(glGetOcclusionQueryuivNV, "GL_NV_occlusion_query_samples"),

    // ARB_clip_control
    GLFUNC_REQUIRES(glClipControl, "GL_ARB_clip_control !VERSION_4_5"),

    // ARB_copy_image
    GLFUNC_REQUIRES(glCopyImageSubData, "GL_ARB_copy_image !VERSION_4_3 |VERSION_GLES_3_2"),

    // NV_copy_image
    GLFUNC_SUFFIX(glCopyImageSubData, NV, "GL_NV_copy_image !GL_ARB_copy_image !VERSION_GLES_3_2"),

    // OES_copy_image
    GLFUNC_SUFFIX(glCopyImageSubData, OES, "GL_OES_copy_image !VERSION_GLES_3_2"),

    // EXT_copy_image
    GLFUNC_SUFFIX(glCopyImageSubData, EXT,
                  "GL_EXT_copy_image !GL_OES_copy_image !VERSION_GLES_3_2"),

    // EXT_texture_buffer
    GLFUNC_SUFFIX(glTexBuffer, OES, "GL_OES_texture_buffer !VERSION_GLES_3_2"),

    // EXT_texture_buffer
    GLFUNC_SUFFIX(glTexBuffer, EXT,
                  "GL_EXT_texture_buffer !GL_OES_texture_buffer !VERSION_GLES_3_2"),

    // EXT_blend_func_extended
    GLFUNC_SUFFIX(glBindFragDataLocationIndexed, EXT, "GL_EXT_blend_func_extended"),

    // ARB_shader_storage_buffer_object

    // NV_depth_buffer_float

    // ARB_shader_image_load_store
    GLFUNC_REQUIRES(glBindImageTexture,
                    "GL_ARB_shader_image_load_store !VERSION_4_2 |VERSION_GLES_3_1"),
    GLFUNC_REQUIRES(glMemoryBarrier,
                    "GL_ARB_shader_image_load_store !VERSION_4_2 |VERSION_GLES_3_1"),

    // ARB_compute_shader
    GLFUNC_REQUIRES(glDispatchCompute, "GL_ARB_compute_shader !VERSION_4_3 |VERSION_GLES_3_1"),

    // ARB_get_texture_sub_image
    GLFUNC_REQUIRES(glGetTextureSubImage, "GL_ARB_get_texture_sub_image !VERSION_4_5"),
};

namespace GLExtensions
{
// Private members and functions
static u32 s_gl_version;
static std::unordered_map<std::string, bool> s_extension_list;

// Private initialization functions
bool InitFunctionPointers(GLContext* context);

// Reads the next space-separated word of a list into *word; returns nullptr at the end.
static const char* NextWord(const char* p, std::string* word)
{
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
    p++;
  if (!*p)
    return nullptr;
  const char* start = p;
  while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r')
    p++;
  word->assign(start, p - start);
  return p;
}

// Initializes the extension list the old way
static void InitExtensionList21()
{
  const char* extensions = (const char*)glGetString(GL_EXTENSIONS);
  std::string tmp;
  for (const char* p = extensions; p && (p = NextWord(p, &tmp));)
    s_extension_list[tmp] = true;
}

static void InitExtensionList(GLContext* context)
{
  s_extension_list.clear();
  if (context->IsGLES())
  {
    switch (s_gl_version)
    {
    default:
    case 320:
      s_extension_list["VERSION_GLES_3_2"] = true;
      [[fallthrough]];
    case 310:
      s_extension_list["VERSION_GLES_3_1"] = true;
      [[fallthrough]];
    case 300:
      s_extension_list["VERSION_GLES_3"] = true;
      break;
    }

    // We always have ES 2.0
    s_extension_list["VERSION_GLES_2"] = true;
  }
  else
  {
    // Some OpenGL implementations chose to not expose core extensions as extensions
    // Let's add them to the list manually depending on which version of OpenGL we have
    // We need to be slightly careful here
    // When an extension got merged in to core, the naming may have changed

    // This has intentional fall through
    switch (s_gl_version)
    {
    default:
    case 450:
    {
      static const char* const gl450exts[] = {
          "GL_ARB_ES3_1_compatibility",
          "GL_ARB_clip_control",
          "GL_ARB_conditional_render_inverted",
          "GL_ARB_cull_distance",
          "GL_ARB_derivative_control",
          "GL_ARB_direct_state_access",
          "GL_ARB_get_texture_sub_image",
          "GL_ARB_robustness",
          "GL_ARB_shader_texture_image_samples",
          "GL_ARB_texture_barrier",
          "VERSION_4_5",
      };
      for (auto it : gl450exts)
        s_extension_list[it] = true;
      [[fallthrough]];
    }
    case 440:
    {
      static const char* const gl440exts[] = {
          "GL_ARB_buffer_storage",
          "GL_ARB_clear_texture",
          "GL_ARB_enhanced_layouts",
          "GL_ARB_multi_bind",
          "GL_ARB_query_buffer_object",
          "GL_ARB_texture_mirror_clamp_to_edge",
          "GL_ARB_texture_stencil8",
          "GL_ARB_vertex_type_10f_11f_11f_rev",
          "VERSION_4_4",
      };
      for (auto it : gl440exts)
        s_extension_list[it] = true;
      [[fallthrough]];
    }
    case 430:
    {
      static const char* const gl430exts[] = {
          "GL_ARB_ES3_compatibility",
          "GL_ARB_arrays_of_arrays",
          "GL_ARB_clear_buffer_object",
          "GL_ARB_compute_shader",
          "GL_ARB_copy_image",
          "GL_ARB_explicit_uniform_location",
          "GL_ARB_fragment_layer_viewport",
          "GL_ARB_framebuffer_no_attachments",
          "GL_ARB_internalformat_query2",
          "GL_ARB_invalidate_subdata",
          "GL_ARB_multi_draw_indirect",
          "GL_ARB_program_interface_query",
          "GL_ARB_shader_image_size",
          "GL_ARB_shader_storage_buffer_object",
          "GL_ARB_stencil_texturing",
          "GL_ARB_texture_buffer_range",
          "GL_ARB_texture_query_levels",
          "GL_ARB_texture_storage_multisample",
          "GL_ARB_texture_view",
          "GL_ARB_vertex_attrib_binding",
          "VERSION_4_3",
      };
      for (auto it : gl430exts)
        s_extension_list[it] = true;
      [[fallthrough]];
    }
    case 420:
    {
      static const char* const gl420exts[] = {
          "GL_ARB_base_instance",
          "GL_ARB_compressed_texture_pixel_storage",
          "GL_ARB_conservative_depth",
          "GL_ARB_internalformat_query",
          "GL_ARB_map_buffer_alignment",
          "GL_ARB_shader_atomic_counters",
          "GL_ARB_shader_image_load_store",
          "GL_ARB_shading_language_420pack",
          "GL_ARB_shading_language_packing",
          "GL_ARB_texture_compression_BPTC",
          "GL_ARB_texture_storage",
          "GL_ARB_transform_feedback_instanced",
          "VERSION_4_2",
      };
      for (auto it : gl420exts)
        s_extension_list[it] = true;
      [[fallthrough]];
    }
    case 410:
    {
      static const char* const gl410exts[] = {
          "GL_ARB_ES2_compatibility",
          "GL_ARB_get_program_binary",
          "GL_ARB_separate_shader_objects",
          "GL_ARB_shader_precision",
          "GL_ARB_vertex_attrib_64_bit",
          "GL_ARB_viewport_array",
          "VERSION_4_1",
      };
      for (auto it : gl410exts)
        s_extension_list[it] = true;
      [[fallthrough]];
    }
    case 400:
    {
      static const char* const gl400exts[] = {
          "GL_ARB_draw_indirect",
          "GL_ARB_gpu_shader5",
          "GL_ARB_gpu_shader_fp64",
          "GL_ARB_sample_shading",
          "GL_ARB_shader_subroutine",
          "GL_ARB_tessellation_shader",
          "GL_ARB_texture_buffer_object_rgb32",
          "GL_ARB_texture_cube_map_array",
          "GL_ARB_texture_gather",
          "GL_ARB_texture_query_lod",
          "GL_ARB_transform_feedback2",
          "GL_ARB_transform_feedback3",
          "VERSION_4_0",
      };
      for (auto it : gl400exts)
        s_extension_list[it] = true;
      [[fallthrough]];
    }
    case 330:
    {
      static const char* const gl330exts[] = {
          "GL_ARB_shader_bit_encoding",
          "GL_ARB_blend_func_extended",
          "GL_ARB_explicit_attrib_location",
          "GL_ARB_occlusion_query2",
          "GL_ARB_sampler_objects",
          "GL_ARB_texture_swizzle",
          "GL_ARB_timer_query",
          "GL_ARB_instanced_arrays",
          "GL_ARB_texture_rgb10_a2ui",
          "GL_ARB_vertex_type_2_10_10_10_rev",
          "VERSION_3_3",
      };
      for (auto it : gl330exts)
        s_extension_list[it] = true;
      [[fallthrough]];
    }
    case 320:
    {
      static const char* const gl320exts[] = {
          "GL_ARB_geometry_shader4",
          "GL_ARB_sync",
          "GL_ARB_vertex_array_bgra",
          "GL_ARB_draw_elements_base_vertex",
          "GL_ARB_seamless_cube_map",
          "GL_ARB_texture_multisample",
          "GL_ARB_fragment_coord_conventions",
          "GL_ARB_provoking_vertex",
          "GL_ARB_depth_clamp",
          "VERSION_3_2",
      };
      for (auto it : gl320exts)
        s_extension_list[it] = true;
      [[fallthrough]];
    }
    case 310:
    {
      // Can't add NV_primitive_restart since function name changed
      static const char* const gl310exts[] = {
          "GL_ARB_draw_instanced",
          "GL_ARB_copy_buffer",
          "GL_ARB_texture_buffer_object",
          "GL_ARB_texture_rectangle",
          "GL_ARB_uniform_buffer_object",
          //"GL_NV_primitive_restart",
          "VERSION_3_1",
      };
      for (auto it : gl310exts)
        s_extension_list[it] = true;
      [[fallthrough]];
    }
    case 300:
    {
      // Quite a lot of these had their names changed when merged in to core
      // Disable the ones that have
      static const char* const gl300exts[] = {
          "GL_ARB_map_buffer_range",
          "GL_ARB_color_buffer_float",
          "GL_ARB_texture_float",
          "GL_ARB_half_float_pixel",
          "GL_ARB_framebuffer_object",
          "GL_ARB_texture_float",
          "GL_ARB_vertex_array_object",
          "GL_ARB_depth_buffer_float",
          //"GL_EXT_texture_integer",
          //"GL_EXT_gpu_shader4",
          //"GL_APPLE_flush_buffer_range",
          //"GL_EXT_packed_float",
          //"GL_EXT_texture_shared_exponent",
          //"GL_NV_half_float",
          //"GL_EXT_framebuffer_sRGB",
          //"GL_EXT_draw_buffers2",
          //"GL_EXT_texture_integer",
          //"GL_EXT_texture_array",
          //"GL_EXT_texture_compression_rgtc",
          //"GL_EXT_transform_feedback",
          //"GL_NV_conditional_render",
          "VERSION_3_0",
      };
      for (auto it : gl300exts)
        s_extension_list[it] = true;
      [[fallthrough]];
    }
    case 210:
    case 200:
    case 150:
    case 140:
    case 130:
    case 121:
    case 120:
    case 110:
    case 100:
      break;
    }
    // So we can easily determine if we are running desktop GL
    s_extension_list["VERSION_GL"] = true;
  }

  if (s_gl_version < 300)
  {
    InitExtensionList21();
    return;
  }
  GLint NumExtension = 0;
  glGetIntegerv(GL_NUM_EXTENSIONS, &NumExtension);
  for (GLint i = 0; i < NumExtension; ++i)
    s_extension_list[std::string((const char*)glGetStringi(GL_EXTENSIONS, i))] = true;
}
static void InitVersion()
{
  GLint major, minor;
  glGetIntegerv(GL_MAJOR_VERSION, &major);
  glGetIntegerv(GL_MINOR_VERSION, &minor);
  if (glGetError() == GL_NO_ERROR)
    s_gl_version = major * 100 + minor * 10;
  else
    s_gl_version = 210;
}

static void* GetFuncAddress(GLContext* context, const std::string& name, void** func)
{
  *func = context->GetFuncAddress(name);
  if (*func == nullptr)
  {
#if defined(__linux__) || defined(__APPLE__)
    // Give it a second try with dlsym
    *func = dlsym(RTLD_NEXT, name.c_str());
#endif
    if (*func == nullptr)
      ERROR_LOG(VIDEO, "Couldn't load function %s", name.c_str());
  }
  return *func;
}

// Public members
u32 Version()
{
  return s_gl_version;
}
bool Supports(const std::string& name)
{
  return s_extension_list[name];
}

bool Init(GLContext* context)
{
  // Grab a few functions for initial checking
  // We need them to grab the extension list
  // Also to check if there is an error grabbing the version
  if (GetFuncAddress(context, "glGetIntegerv", (void**)&glGetIntegerv) == nullptr)
    return false;
  if (GetFuncAddress(context, "glGetString", (void**)&glGetString) == nullptr)
    return false;
  if (GetFuncAddress(context, "glGetError", (void**)&glGetError) == nullptr)
    return false;

  InitVersion();

  // We need to use glGetStringi to get the extension list
  // if we are using GLES3 or a GL version greater than 2.1
  if (s_gl_version > 210 &&
      GetFuncAddress(context, "glGetStringi", (void**)&glGetStringi) == nullptr)
    return false;

  InitExtensionList(context);

  return InitFunctionPointers(context);
}

// Private initialization functions
static bool HasFeatures(const char* extensions)
{
  bool result = true;
  std::string tmp;
  for (const char* p = extensions; (p = NextWord(p, &tmp));)
  {
    if (tmp[0] == '!')
      result &= !s_extension_list[tmp.erase(0, 1)];
    else if (tmp[0] == '|')
      result |= s_extension_list[tmp.erase(0, 1)];
    else
      result &= s_extension_list[tmp];
  }
  return result;
}
bool InitFunctionPointers(GLContext* context)
{
  bool result = true;
  for (const auto& it : gl_function_array)
    if (HasFeatures(it.requirements))
      result &= !!GetFuncAddress(context, it.function_name, it.function_ptr);
  return result;
}
}  // namespace GLExtensions
