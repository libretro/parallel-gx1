#include "GLContextLR.h"
#include "Common/Logging/Log.h"
#include "Core/Config/MainSettings.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/Present.h"
#include "DolphinLibretro/Common/Options.h"
#include "DolphinLibretro/VideoContexts/ContextStatus.h"
#include "DolphinLibretro/Video.h"

#include <cstring>
#include <memory>
#include <vector>

#include "Common/GL/GLExtensions/GLExtensions.h"

/* Shared contexts for the shader compiler's threads, made from the
 * frontend's: EGL, then GLX, on every Unix but Apple's, and WGL on
 * Windows, all loaded at runtime from whichever libraries the frontend
 * has, so the core links none of them. */
#if (defined(__unix__) || defined(__ANDROID__)) && !defined(__APPLE__)
#define LR_SHARED_EGL 1
#if !defined(__ANDROID__)
#define LR_SHARED_GLX 1
#endif
#include "Common/DynamicLibrary.h"
#endif
#if defined(_WIN32)
#define LR_SHARED_WGL 1
#include <windows.h>
#endif

namespace
{
// Version/profile of the frontend context, so worker contexts are created to match it.
struct CurrentGLVersion
{
  GLint major = 0;
  GLint minor = 0;
  bool core_profile = false;
};

CurrentGLVersion QueryCurrentGLVersion(bool gles)
{
  CurrentGLVersion v;
  glGetIntegerv(GL_MAJOR_VERSION, &v.major);
  glGetIntegerv(GL_MINOR_VERSION, &v.minor);
  if (!gles && (v.major > 3 || (v.major == 3 && v.minor >= 2)))
  {
    GLint mask = 0;
    glGetIntegerv(GL_CONTEXT_PROFILE_MASK, &mask);
    v.core_profile = (mask & GL_CONTEXT_CORE_PROFILE_BIT) != 0;
  }
  return v;
}

#if defined(LR_SHARED_EGL)
// EGL is loaded at runtime, from the library the frontend already has loaded.
namespace EGL
{
using EGLDisplay = void*;
using EGLContext = void*;
using EGLSurface = void*;
using EGLConfig = void*;
using EGLint = int;
using EGLBoolean = unsigned int;
using EGLenum = unsigned int;

constexpr EGLint EGL_TRUE = 1;
constexpr EGLint EGL_NONE = 0x3038;
constexpr EGLint EGL_CONFIG_ID = 0x3028;
constexpr EGLint EGL_EXTENSIONS = 0x3055;
constexpr EGLint EGL_HEIGHT = 0x3056;
constexpr EGLint EGL_WIDTH = 0x3057;
constexpr EGLint EGL_CONTEXT_CLIENT_VERSION = 0x3098;
constexpr EGLint EGL_CONTEXT_MAJOR_VERSION_KHR = 0x3098;
constexpr EGLint EGL_CONTEXT_MINOR_VERSION_KHR = 0x30FB;
constexpr EGLint EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR = 0x30FD;
constexpr EGLint EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR = 0x1;
constexpr EGLint EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT_KHR = 0x2;
constexpr EGLenum EGL_OPENGL_ES_API = 0x30A0;
constexpr EGLenum EGL_OPENGL_API = 0x30A2;

EGLBoolean (*BindAPI)(EGLenum);
EGLContext (*GetCurrentContext)();
EGLDisplay (*GetCurrentDisplay)();
EGLBoolean (*QueryContext)(EGLDisplay, EGLContext, EGLint, EGLint*);
EGLBoolean (*ChooseConfig)(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*);
EGLContext (*CreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint*);
EGLint (*GetError)();
const char* (*QueryString)(EGLDisplay, EGLint);
EGLSurface (*CreatePbufferSurface)(EGLDisplay, EGLConfig, const EGLint*);
EGLBoolean (*DestroyContext)(EGLDisplay, EGLContext);
EGLBoolean (*DestroySurface)(EGLDisplay, EGLSurface);
EGLBoolean (*MakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);

bool Load()
{
  static Common::DynamicLibrary libegl;
  static const bool loaded = [] {
    if (!libegl.Open("libEGL.so.1") && !libegl.Open("libEGL.so"))
      return false;
    return libegl.GetSymbol("eglBindAPI", &BindAPI) &&
           libegl.GetSymbol("eglGetCurrentContext", &GetCurrentContext) &&
           libegl.GetSymbol("eglGetCurrentDisplay", &GetCurrentDisplay) &&
           libegl.GetSymbol("eglQueryContext", &QueryContext) &&
           libegl.GetSymbol("eglChooseConfig", &ChooseConfig) &&
           libegl.GetSymbol("eglCreateContext", &CreateContext) &&
           libegl.GetSymbol("eglGetError", &GetError) &&
           libegl.GetSymbol("eglQueryString", &QueryString) &&
           libegl.GetSymbol("eglCreatePbufferSurface", &CreatePbufferSurface) &&
           libegl.GetSymbol("eglDestroyContext", &DestroyContext) &&
           libegl.GetSymbol("eglDestroySurface", &DestroySurface) &&
           libegl.GetSymbol("eglMakeCurrent", &MakeCurrent);
  }();
  return loaded;
}
}  // namespace EGL

class GLContextLRSharedEGL final : public GLContext
{
public:
  GLContextLRSharedEGL(Mode mode, EGL::EGLDisplay display, EGL::EGLContext context,
                       EGL::EGLSurface surface)
      : m_display(display), m_context(context), m_surface(surface)
  {
    m_opengl_mode = mode;
    m_is_shared = true;
  }

  ~GLContextLRSharedEGL() override
  {
    if (EGL::GetCurrentContext() == m_context)
      ClearCurrent();
    if (m_surface)
      EGL::DestroySurface(m_display, m_surface);
    EGL::DestroyContext(m_display, m_context);
  }

  bool IsHeadless() const override { return true; }

  bool MakeCurrent() override
  {
    EGL::BindAPI(m_opengl_mode == Mode::OpenGLES ? EGL::EGL_OPENGL_ES_API : EGL::EGL_OPENGL_API);
    return EGL::MakeCurrent(m_display, m_surface, m_surface, m_context) ==
           static_cast<EGL::EGLBoolean>(EGL::EGL_TRUE);
  }

  bool ClearCurrent() override
  {
    return EGL::MakeCurrent(m_display, nullptr, nullptr, nullptr) ==
           static_cast<EGL::EGLBoolean>(EGL::EGL_TRUE);
  }

  void* GetFuncAddress(const std::string& name) override
  {
    return reinterpret_cast<void*>(Libretro::Video::hw_render.get_proc_address(name.c_str()));
  }

private:
  EGL::EGLDisplay m_display;
  EGL::EGLContext m_context;
  EGL::EGLSurface m_surface;
};

std::unique_ptr<GLContext> CreateSharedEGL(GLContext::Mode mode)
{
  if (!EGL::Load())
    return nullptr;

  const EGL::EGLContext share = EGL::GetCurrentContext();
  const EGL::EGLDisplay display = EGL::GetCurrentDisplay();
  if (!share || !display)
    return nullptr;

  const bool gles = mode == GLContext::Mode::OpenGLES;
  EGL::BindAPI(gles ? EGL::EGL_OPENGL_ES_API : EGL::EGL_OPENGL_API);

  EGL::EGLint config_id = 0;
  EGL::QueryContext(display, share, EGL::EGL_CONFIG_ID, &config_id);
  const EGL::EGLint config_attribs[] = {EGL::EGL_CONFIG_ID, config_id, EGL::EGL_NONE};
  EGL::EGLConfig config = nullptr;
  EGL::EGLint num_configs = 0;
  if (!EGL::ChooseConfig(display, config_attribs, &config, 1, &num_configs) || num_configs < 1)
    config = nullptr;  // EGL_NO_CONFIG_KHR, frontend may have used EGL_KHR_no_config_context

  const CurrentGLVersion ver = QueryCurrentGLVersion(gles);
  EGL::EGLint ctx_attribs[7];
  int n = 0;
  ctx_attribs[n++] = EGL::EGL_CONTEXT_MAJOR_VERSION_KHR;
  ctx_attribs[n++] = ver.major;
  ctx_attribs[n++] = EGL::EGL_CONTEXT_MINOR_VERSION_KHR;
  ctx_attribs[n++] = ver.minor;
  if (!gles)
  {
    ctx_attribs[n++] = EGL::EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR;
    ctx_attribs[n++] = ver.core_profile ? EGL::EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR :
                                          EGL::EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT_KHR;
  }
  ctx_attribs[n] = EGL::EGL_NONE;

  EGL::EGLContext context = EGL::CreateContext(display, config, share, ctx_attribs);
  if (!context)
  {
    // No EGL_KHR_create_context, fall back to the basic attributes.
    const EGL::EGLint es_attribs[] = {EGL::EGL_CONTEXT_CLIENT_VERSION, 3, EGL::EGL_NONE};
    const EGL::EGLint gl_attribs[] = {EGL::EGL_NONE};
    context = EGL::CreateContext(display, config, share, gles ? es_attribs : gl_attribs);
  }
  if (!context)
  {
    WARN_LOG_FMT(VIDEO, "Shared context: eglCreateContext failed {:#06x}", EGL::GetError());
    return nullptr;
  }

  // Worker contexts never draw, so avoid a surface when possible.
  EGL::EGLSurface surface = nullptr;
  const char* exts = EGL::QueryString(display, EGL::EGL_EXTENSIONS);
  if (!exts || !strstr(exts, "EGL_KHR_surfaceless_context"))
  {
    const EGL::EGLint pbuffer_attribs[] = {EGL::EGL_WIDTH, 1, EGL::EGL_HEIGHT, 1, EGL::EGL_NONE};
    surface = config ? EGL::CreatePbufferSurface(display, config, pbuffer_attribs) : nullptr;
    if (!surface)
    {
      WARN_LOG_FMT(VIDEO, "Shared context: no surfaceless support and pbuffer creation failed");
      EGL::DestroyContext(display, context);
      return nullptr;
    }
  }

  INFO_LOG_FMT(VIDEO, "Created EGL shared context for shader compiling");
  return std::make_unique<GLContextLRSharedEGL>(mode, display, context, surface);
}
#endif  // LR_SHARED_EGL

#if defined(LR_SHARED_GLX)
// GLX is loaded at runtime (rather than including GL/glx.h, which conflicts with Dolphin's GL
// loader, and linking libGL), using whichever libGL the frontend already has loaded.
namespace GLX
{
using Display = void;
using GLXContext = void*;
using GLXFBConfig = void*;
using GLXDrawable = unsigned long;
using XErrorHandler = int (*)(Display*, void*);

constexpr int GLX_SCREEN = 0x800C;
constexpr int GLX_FBCONFIG_ID = 0x8013;
constexpr int GLX_CONTEXT_MAJOR_VERSION_ARB = 0x2091;
constexpr int GLX_CONTEXT_MINOR_VERSION_ARB = 0x2092;
constexpr int GLX_CONTEXT_PROFILE_MASK_ARB = 0x9126;
constexpr int GLX_CONTEXT_CORE_PROFILE_BIT_ARB = 0x1;
constexpr int GLX_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB = 0x2;
constexpr int GLX_CONTEXT_ES2_PROFILE_BIT_EXT = 0x4;

GLXContext (*GetCurrentContext)();
Display* (*GetCurrentDisplay)();
int (*QueryContext)(Display*, GLXContext, int, int*);
GLXFBConfig* (*ChooseFBConfig)(Display*, int, const int*, int*);
GLXFBConfig* (*GetFBConfigs)(Display*, int, int*);
int (*GetFBConfigAttrib)(Display*, GLXFBConfig, int, int*);
void* (*GetProcAddressARB)(const unsigned char*);
int (*MakeContextCurrent)(Display*, GLXDrawable, GLXDrawable, GLXContext);
void (*DestroyContext)(Display*, GLXContext);
GLXContext (*CreateContextAttribsARB)(Display*, GLXFBConfig, GLXContext, int, const int*);
int (*XFree)(void*);
int (*XSync)(Display*, int);
XErrorHandler (*XSetErrorHandler)(XErrorHandler);

bool Load()
{
  static Common::DynamicLibrary libgl;
  static Common::DynamicLibrary libx11;
  static const bool loaded = [] {
    if (!libgl.Open("libGL.so.1") && !libgl.Open("libGL.so"))
      return false;
    if (!libx11.Open("libX11.so.6") && !libx11.Open("libX11.so"))
      return false;
    return libgl.GetSymbol("glXGetCurrentContext", &GetCurrentContext) &&
           libgl.GetSymbol("glXGetCurrentDisplay", &GetCurrentDisplay) &&
           libgl.GetSymbol("glXQueryContext", &QueryContext) &&
           libgl.GetSymbol("glXChooseFBConfig", &ChooseFBConfig) &&
           libgl.GetSymbol("glXGetFBConfigs", &GetFBConfigs) &&
           libgl.GetSymbol("glXGetFBConfigAttrib", &GetFBConfigAttrib) &&
           libgl.GetSymbol("glXGetProcAddressARB", &GetProcAddressARB) &&
           libgl.GetSymbol("glXMakeContextCurrent", &MakeContextCurrent) &&
           libgl.GetSymbol("glXDestroyContext", &DestroyContext) &&
           libx11.GetSymbol("XFree", &XFree) && libx11.GetSymbol("XSync", &XSync) &&
           libx11.GetSymbol("XSetErrorHandler", &XSetErrorHandler) &&
           (CreateContextAttribsARB = reinterpret_cast<decltype(CreateContextAttribsARB)>(
                GetProcAddressARB(reinterpret_cast<const unsigned char*>(
                    "glXCreateContextAttribsARB")))) != nullptr;
  }();
  return loaded;
}

int IgnoreXError(Display*, void*)
{
  return 0;
}
}  // namespace GLX

class GLContextLRSharedGLX final : public GLContext
{
public:
  GLContextLRSharedGLX(Mode mode, GLX::Display* display, GLX::GLXContext context)
      : m_display(display), m_context(context)
  {
    m_opengl_mode = mode;
    m_is_shared = true;
  }

  ~GLContextLRSharedGLX() override
  {
    if (GLX::GetCurrentContext() == m_context)
      ClearCurrent();
    GLX::DestroyContext(m_display, m_context);
  }

  bool IsHeadless() const override { return true; }

  // GLX_ARB_create_context allows GL 3.0+ contexts to be made current without a drawable.
  bool MakeCurrent() override { return GLX::MakeContextCurrent(m_display, 0, 0, m_context) != 0; }
  bool ClearCurrent() override { return GLX::MakeContextCurrent(m_display, 0, 0, nullptr) != 0; }

  void* GetFuncAddress(const std::string& name) override
  {
    return reinterpret_cast<void*>(Libretro::Video::hw_render.get_proc_address(name.c_str()));
  }

private:
  GLX::Display* m_display;
  GLX::GLXContext m_context;
};

std::unique_ptr<GLContext> CreateSharedGLX(GLContext::Mode mode)
{
  if (!GLX::Load())
    return nullptr;

  GLX::GLXContext share = GLX::GetCurrentContext();
  GLX::Display* display = GLX::GetCurrentDisplay();
  if (!share || !display)
    return nullptr;

  int screen = 0;
  int fbconfig_id = 0;
  GLX::QueryContext(display, share, GLX::GLX_SCREEN, &screen);
  GLX::QueryContext(display, share, GLX::GLX_FBCONFIG_ID, &fbconfig_id);
  const int fb_attribs[] = {GLX::GLX_FBCONFIG_ID, fbconfig_id, 0};
  int num_configs = 0;
  GLX::GLXFBConfig config = nullptr;
  GLX::GLXFBConfig* configs = GLX::ChooseFBConfig(display, screen, fb_attribs, &num_configs);
  if (configs && num_configs > 0)
    config = configs[0];
  else
  {
    /* Mesa keeps its default attributes beside the ID and may match none:
     * the screen's configs, for the one with the ID */
    int i;
    if (configs)
      GLX::XFree(configs);
    configs = GLX::GetFBConfigs(display, screen, &num_configs);
    for (i = 0; configs && i < num_configs && !config; i++)
    {
      int id = 0;
      if (GLX::GetFBConfigAttrib(display, configs[i], GLX::GLX_FBCONFIG_ID, &id) == 0 &&
          id == fbconfig_id)
      {
        config = configs[i];
      }
    }
  }
  if (configs)
    GLX::XFree(configs);
  if (!config)
  {
    WARN_LOG_FMT(VIDEO, "Shared context: no GLXFBConfig matching frontend context");
    return nullptr;
  }

  const bool gles = mode == GLContext::Mode::OpenGLES;
  const CurrentGLVersion ver = QueryCurrentGLVersion(gles);
  const int profile = gles ? GLX::GLX_CONTEXT_ES2_PROFILE_BIT_EXT :
                      ver.core_profile ? GLX::GLX_CONTEXT_CORE_PROFILE_BIT_ARB :
                                         GLX::GLX_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB;
  const int ctx_attribs[] = {GLX::GLX_CONTEXT_MAJOR_VERSION_ARB, ver.major,
                             GLX::GLX_CONTEXT_MINOR_VERSION_ARB, ver.minor,
                             GLX::GLX_CONTEXT_PROFILE_MASK_ARB,  profile,
                             0};

  // A failed glXCreateContextAttribsARB raises an X error, and the default handler would
  // terminate the frontend, so swallow errors while creating.
  GLX::XSync(display, 0);
  const GLX::XErrorHandler old_handler = GLX::XSetErrorHandler(GLX::IgnoreXError);
  GLX::GLXContext context = GLX::CreateContextAttribsARB(display, config, share, 1, ctx_attribs);
  GLX::XSync(display, 0);
  GLX::XSetErrorHandler(old_handler);

  if (!context)
  {
    WARN_LOG_FMT(VIDEO, "Shared context: glXCreateContextAttribsARB failed");
    return nullptr;
  }

  INFO_LOG_FMT(VIDEO, "Created GLX shared context for shader compiling");
  return std::make_unique<GLContextLRSharedGLX>(mode, display, context);
}
#endif  // LR_SHARED_GLX
#if defined(LR_SHARED_WGL)
namespace WGL
{
constexpr int WGL_CONTEXT_MAJOR_VERSION_ARB = 0x2091;
constexpr int WGL_CONTEXT_MINOR_VERSION_ARB = 0x2092;
constexpr int WGL_CONTEXT_PROFILE_MASK_ARB = 0x9126;
constexpr int WGL_CONTEXT_CORE_PROFILE_BIT_ARB = 0x1;
constexpr int WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB = 0x2;
constexpr int WGL_CONTEXT_ES2_PROFILE_BIT_EXT = 0x4;

HGLRC(WINAPI* GetCurrentContext)();
HDC(WINAPI* GetCurrentDC)();
BOOL(WINAPI* MakeCurrent)(HDC, HGLRC);
BOOL(WINAPI* DeleteContext)(HGLRC);
PROC(WINAPI* GetExtProc)(LPCSTR);
HGLRC(WINAPI* CreateContextAttribsARB)(HDC, HGLRC, const int*);
HANDLE(WINAPI* CreatePbufferARB)(HDC, int, int, int, const int*);
HDC(WINAPI* GetPbufferDCARB)(HANDLE);
int(WINAPI* ReleasePbufferDCARB)(HANDLE, HDC);
BOOL(WINAPI* DestroyPbufferARB)(HANDLE);

/* opengl32 from the frontend, and the extensions with its context current */
bool Load()
{
  HMODULE gl = GetModuleHandleA("opengl32.dll");
  if (!gl)
    return false;
  GetCurrentContext = reinterpret_cast<decltype(GetCurrentContext)>(
      reinterpret_cast<void*>(GetProcAddress(gl, "wglGetCurrentContext")));
  GetCurrentDC = reinterpret_cast<decltype(GetCurrentDC)>(
      reinterpret_cast<void*>(GetProcAddress(gl, "wglGetCurrentDC")));
  MakeCurrent = reinterpret_cast<decltype(MakeCurrent)>(
      reinterpret_cast<void*>(GetProcAddress(gl, "wglMakeCurrent")));
  DeleteContext = reinterpret_cast<decltype(DeleteContext)>(
      reinterpret_cast<void*>(GetProcAddress(gl, "wglDeleteContext")));
  GetExtProc = reinterpret_cast<decltype(GetExtProc)>(
      reinterpret_cast<void*>(GetProcAddress(gl, "wglGetProcAddress")));
  if (!GetCurrentContext || !GetCurrentDC || !MakeCurrent || !DeleteContext || !GetExtProc)
    return false;
  CreateContextAttribsARB = reinterpret_cast<decltype(CreateContextAttribsARB)>(
      reinterpret_cast<void*>(GetExtProc("wglCreateContextAttribsARB")));
  CreatePbufferARB = reinterpret_cast<decltype(CreatePbufferARB)>(
      reinterpret_cast<void*>(GetExtProc("wglCreatePbufferARB")));
  GetPbufferDCARB = reinterpret_cast<decltype(GetPbufferDCARB)>(
      reinterpret_cast<void*>(GetExtProc("wglGetPbufferDCARB")));
  ReleasePbufferDCARB = reinterpret_cast<decltype(ReleasePbufferDCARB)>(
      reinterpret_cast<void*>(GetExtProc("wglReleasePbufferDCARB")));
  DestroyPbufferARB = reinterpret_cast<decltype(DestroyPbufferARB)>(
      reinterpret_cast<void*>(GetExtProc("wglDestroyPbufferARB")));
  return CreateContextAttribsARB != nullptr;
}
}  // namespace WGL

class GLContextLRSharedWGL final : public GLContext
{
public:
  GLContextLRSharedWGL(Mode mode, HDC dc, HGLRC context, HANDLE pbuffer)
      : m_dc(dc), m_context(context), m_pbuffer(pbuffer)
  {
    m_opengl_mode = mode;
    m_is_shared = true;
  }

  ~GLContextLRSharedWGL() override
  {
    if (WGL::GetCurrentContext() == m_context)
      ClearCurrent();
    WGL::DeleteContext(m_context);
    if (m_pbuffer)
    {
      WGL::ReleasePbufferDCARB(m_pbuffer, m_dc);
      WGL::DestroyPbufferARB(m_pbuffer);
    }
  }

  bool IsHeadless() const override { return true; }
  bool MakeCurrent() override { return WGL::MakeCurrent(m_dc, m_context) != FALSE; }
  bool ClearCurrent() override { return WGL::MakeCurrent(nullptr, nullptr) != FALSE; }

  void* GetFuncAddress(const std::string& name) override
  {
    return reinterpret_cast<void*>(Libretro::Video::hw_render.get_proc_address(name.c_str()));
  }

private:
  HDC m_dc;
  HGLRC m_context;
  HANDLE m_pbuffer;
};

/* A context sharing the frontend's, current on a 1x1 pbuffer of the
 * frontend's pixel format, or on the frontend's own DC without
 * WGL_ARB_pbuffer. */
std::unique_ptr<GLContext> CreateSharedWGL(GLContext::Mode mode)
{
  if (!WGL::Load())
    return nullptr;

  const HGLRC share = WGL::GetCurrentContext();
  const HDC frontend_dc = WGL::GetCurrentDC();
  if (!share || !frontend_dc)
    return nullptr;

  const bool gles = mode == GLContext::Mode::OpenGLES;
  const CurrentGLVersion ver = QueryCurrentGLVersion(gles);
  const int profile = gles ? WGL::WGL_CONTEXT_ES2_PROFILE_BIT_EXT :
                      ver.core_profile ? WGL::WGL_CONTEXT_CORE_PROFILE_BIT_ARB :
                                         WGL::WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB;
  const int ctx_attribs[] = {WGL::WGL_CONTEXT_MAJOR_VERSION_ARB, ver.major,
                             WGL::WGL_CONTEXT_MINOR_VERSION_ARB, ver.minor,
                             WGL::WGL_CONTEXT_PROFILE_MASK_ARB,  profile,
                             0};

  HANDLE pbuffer = nullptr;
  HDC dc = frontend_dc;
  if (WGL::CreatePbufferARB && WGL::GetPbufferDCARB && WGL::ReleasePbufferDCARB &&
      WGL::DestroyPbufferARB)
  {
    const int pbuffer_attribs[] = {0};
    pbuffer = WGL::CreatePbufferARB(frontend_dc, GetPixelFormat(frontend_dc), 1, 1,
                                    pbuffer_attribs);
    HDC pbuffer_dc = pbuffer ? WGL::GetPbufferDCARB(pbuffer) : nullptr;
    if (pbuffer_dc)
      dc = pbuffer_dc;
    else if (pbuffer)
    {
      WGL::DestroyPbufferARB(pbuffer);
      pbuffer = nullptr;
    }
  }

  const HGLRC context = WGL::CreateContextAttribsARB(dc, share, ctx_attribs);
  if (!context)
  {
    WARN_LOG_FMT(VIDEO, "Shared context: wglCreateContextAttribsARB failed {:#x}", GetLastError());
    if (pbuffer)
    {
      WGL::ReleasePbufferDCARB(pbuffer, dc);
      WGL::DestroyPbufferARB(pbuffer);
    }
    return nullptr;
  }

  INFO_LOG_FMT(VIDEO, "Created WGL shared context for shader compiling ({})",
               pbuffer ? "pbuffer" : "frontend DC");
  return std::make_unique<GLContextLRSharedWGL>(mode, dc, context, pbuffer);
}
#endif  // LR_SHARED_WGL

}  // namespace

GLContextLR::GLContextLR()
{
}

GLContextLR::~GLContextLR() = default;

void* GLContextLR::GetFuncAddress(const std::string& name)
{
  return (void*)Libretro::Video::hw_render.get_proc_address(name.c_str());
}

bool GLContextLR::Initialize(const WindowSystemInfo& wsi, bool stereo, bool core)
{
  const int efbScale = Libretro::Options::GetCached<int>(Libretro::Options::gfx_settings::EFB_SCALE, 1);
  m_backbuffer_width = Libretro::Video::GetAdjustedBaseWidth() * efbScale;
  m_backbuffer_height = Libretro::Video::GetAdjustedBaseHeight() * efbScale;

  switch (Libretro::Video::hw_render.context_type)
  {
    case RETRO_HW_CONTEXT_OPENGLES_VERSION:
    case RETRO_HW_CONTEXT_OPENGLES3:
      m_opengl_mode = Mode::OpenGLES;
      break;
    case RETRO_HW_CONTEXT_OPENGL_CORE:
    case RETRO_HW_CONTEXT_OPENGL:
    default:
      m_opengl_mode = Mode::OpenGL;
      break;
  }

  if (!g_context_status.IsReady())
  {
    m_initialized = false;
    return false;
  }

  m_initialized = true;

  return true;
}

void GLContextLR::Swap()
{
  Libretro::Video::video_cb(RETRO_HW_FRAME_BUFFER_VALID, m_backbuffer_width, m_backbuffer_height,
                            0);
}


std::unique_ptr<GLContext> GLContextLR::CreateSharedContext()
{
  // Must be called on the thread the frontend's context is current on.
#if defined(LR_SHARED_EGL)
  if (auto context = CreateSharedEGL(m_opengl_mode))
    return context;
#endif
#if defined(LR_SHARED_GLX)
  if (auto context = CreateSharedGLX(m_opengl_mode))
    return context;
#endif
#if defined(LR_SHARED_WGL)
  if (auto context = CreateSharedWGL(m_opengl_mode))
    return context;
#endif
  return nullptr;
}

void GLContextLR::Shutdown()
{
  m_initialized = false;
}
