/* mGBA flags.h for the static Makefile build: the CMake configure results for Windows, Android and Linux merged into one header (Apple follows the Linux values). */
#ifndef FLAGS_H_MAKEFILE
#define FLAGS_H_MAKEFILE

#ifndef FLAGS_H
#define FLAGS_H
#endif
#ifndef MINIMAL_CORE
/* #undef MINIMAL_CORE */
#endif
#ifndef BUILD_GL
/* #undef BUILD_GL */
#endif
#ifndef BUILD_GLES2
/* #undef BUILD_GLES2 */
#endif
#ifndef BUILD_GLES3
/* #undef BUILD_GLES3 */
#endif
#ifndef COLOR_16_BIT
/* #undef COLOR_16_BIT */
#endif
#ifndef COLOR_5_6_5
/* #undef COLOR_5_6_5 */
#endif
#ifndef DISABLE_THREADING
/* #undef DISABLE_THREADING */
#endif
#ifndef FIXED_ROM_BUFFER
/* #undef FIXED_ROM_BUFFER */
#endif
#ifndef M_CORE_GBA
#define M_CORE_GBA
#endif
#ifndef M_CORE_GB
#define M_CORE_GB
#endif
#ifndef ENABLE_DEBUGGERS
/* #undef ENABLE_DEBUGGERS */
#endif
#ifndef ENABLE_DIRECTORIES
/* #undef ENABLE_DIRECTORIES */
#endif
#ifndef ENABLE_GDB_STUB
/* #undef ENABLE_GDB_STUB */
#endif
#ifndef ENABLE_SCRIPTING
/* #undef ENABLE_SCRIPTING */
#endif
#ifndef ENABLE_VFS
#define ENABLE_VFS
#endif
#ifndef ENABLE_VFS_FD
/* #undef ENABLE_VFS_FD */
#endif
#ifndef ENABLE_VFS_FILE
/* #undef ENABLE_VFS_FILE */
#endif
#ifndef USE_DISCORD_RPC
/* #undef USE_DISCORD_RPC */
#endif
#ifndef USE_EDITLINE
/* #undef USE_EDITLINE */
#endif
#ifndef USE_ELF
/* #undef USE_ELF */
#endif
#ifndef USE_EPOXY
/* #undef USE_EPOXY */
#endif
#ifndef USE_FFMPEG
/* #undef USE_FFMPEG */
#endif
#ifndef USE_FREETYPE
/* #undef USE_FREETYPE */
#endif
#ifndef USE_JSON_C
/* #undef USE_JSON_C */
#endif
#ifndef USE_LIBAV
/* #undef USE_LIBAV */
#endif
#ifndef USE_LIBAVRESAMPLE
/* #undef USE_LIBAVRESAMPLE */
#endif
#ifndef USE_LIBSWRESAMPLE
/* #undef USE_LIBSWRESAMPLE */
#endif
#ifndef USE_LIBZIP
/* #undef USE_LIBZIP */
#endif
#ifndef USE_LUA
/* #undef USE_LUA */
#endif
#ifndef USE_LZMA
#define USE_LZMA
#endif
#ifndef USE_MINIZIP
/* #undef USE_MINIZIP */
#endif
#ifndef USE_PNG
/* #undef USE_PNG */
#endif
#ifndef USE_PTHREADS
#if defined(_WIN32)
/* #undef USE_PTHREADS */
#elif defined(__ANDROID__)
#define USE_PTHREADS
#else
#define USE_PTHREADS
#endif
#endif
#ifndef USE_SQLITE3
/* #undef USE_SQLITE3 */
#endif
#ifndef USE_ZLIB
/* #undef USE_ZLIB */
#endif
#ifndef HAVE_CRC32
/* #undef HAVE_CRC32 */
#endif
#ifndef HAVE_LOCALE
/* #undef HAVE_LOCALE */
#endif
#ifndef HAVE_LOCALTIME_R
#if defined(_WIN32)
/* #undef HAVE_LOCALTIME_R */
#elif defined(__ANDROID__)
#define HAVE_LOCALTIME_R
#else
#define HAVE_LOCALTIME_R
#endif
#endif
#ifndef HAVE_POPCOUNT32
/* #undef HAVE_POPCOUNT32 */
#endif
#ifndef HAVE_PTHREAD_NP_H
/* #undef HAVE_PTHREAD_NP_H */
#endif
#ifndef HAVE_PTHREAD_SETNAME_NP
#define HAVE_PTHREAD_SETNAME_NP
#endif
#ifndef HAVE_SNPRINTF_L
/* #undef HAVE_SNPRINTF_L */
#endif
#ifndef HAVE_STRDUP
#define HAVE_STRDUP
#endif
#ifndef HAVE_STRLCPY
#if defined(_WIN32)
/* #undef HAVE_STRLCPY */
#elif defined(__ANDROID__)
#define HAVE_STRLCPY
#else
#define HAVE_STRLCPY
#endif
#endif
#ifndef HAVE_STRTOF_L
/* #undef HAVE_STRTOF_L */
#endif
#ifndef HAVE_XLOCALE
#if defined(_WIN32)
/* #undef HAVE_XLOCALE */
#elif defined(__ANDROID__)
#define HAVE_XLOCALE
#else
/* #undef HAVE_XLOCALE */
#endif
#endif

#endif
