/* libcharset config.h for the static Makefile build (Android from the NDK configure; the Apple values are the Linux ones with the Darwin differences applied). */
#ifndef LIBCHARSET_CONFIG_H_MAKEFILE
#define LIBCHARSET_CONFIG_H_MAKEFILE

#if defined(__ANDROID__)
#define HAVE_COPY_FILE_RANGE 0
#elif defined(__APPLE__)
#define HAVE_COPY_FILE_RANGE 0
#else
#define HAVE_COPY_FILE_RANGE 1
#endif
#define HAVE_DLFCN_H 1
#define HAVE_INTTYPES_H 1
#if defined(__ANDROID__)
#define HAVE_LANGINFO_CODESET 0
#elif defined(__APPLE__)
#define HAVE_LANGINFO_CODESET 1
#else
#define HAVE_LANGINFO_CODESET 1
#endif
#if defined(__ANDROID__)
#define HAVE_MACH_O_DYLD_H 0
#elif defined(__APPLE__)
#define HAVE_MACH_O_DYLD_H 1
#else
#define HAVE_MACH_O_DYLD_H 0
#endif
#define HAVE_MINIX_CONFIG_H 0
#define HAVE_SETLOCALE 1
#define HAVE_STDINT_H 1
#define HAVE_STDIO_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRINGS_H 1
#define HAVE_STRING_H 1
#define HAVE_SYMLINK 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_UNISTD_H 1
#define HAVE_VISIBILITY 1
#define HAVE_WCHAR_H 1
#if defined(__ANDROID__)
#define HAVE_WORKING_O_NOATIME 1
#elif defined(__APPLE__)
#define HAVE_WORKING_O_NOATIME 0
#else
#define HAVE_WORKING_O_NOATIME 0
#endif
#define HAVE_WORKING_O_NOFOLLOW 1
#if defined(__ANDROID__)
#define HAVE__NSGETEXECUTABLEPATH 0
#elif defined(__APPLE__)
#define HAVE__NSGETEXECUTABLEPATH 1
#else
#define HAVE__NSGETEXECUTABLEPATH 0
#endif
/* #undef INSTALLPREFIX */
#define LT_OBJDIR ".libs"
/* #undef PACKAGE_BUGREPORT */
#define PACKAGE_NAME "libcharset"
#define PACKAGE_STRING "libcharset 1.5"
#define PACKAGE_TARNAME "libcharset"
/* #undef PACKAGE_URL */
#define PACKAGE_VERSION "1.5"
#define STDC_HEADERS 1

#endif
