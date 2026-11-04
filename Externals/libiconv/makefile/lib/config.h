/* libiconv config.h for the static Makefile build (Android from the NDK configure; the Apple values are the Linux ones with the Darwin differences applied). */
#ifndef LIBICONV_CONFIG_H_MAKEFILE
#define LIBICONV_CONFIG_H_MAKEFILE

#define ENABLE_EXTRA 0
#define ENABLE_RELOCATABLE 1
/* #undef mbstate_t */
#define HAVE_ICONV 0
#define ICONV_CONST
#define HAVE_GETC_UNLOCKED 1
#if defined(__ANDROID__)
#define HAVE_LANGINFO_CODESET 0
#else
#define HAVE_LANGINFO_CODESET 1
#endif
#define HAVE_MBRTOWC 1
#define HAVE_SETLOCALE 1
#define HAVE_STDDEF_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_VISIBILITY 1
#define HAVE_WCRTOMB 1
#define HAVE_WORKING_O_NOFOLLOW 1
#define WORDS_LITTLEENDIAN 1
/* #undef INSTALLPREFIX */
/* #undef inline */
/* #undef mode_t */
/* #undef ssize_t */

#endif
