/* Copyright 2026 Dolphin Emulator Project
 * SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * The text of a shader as it is generated: one growing block of chars.
 * C89.
 *
 *  - Text is appended as it is (shader_text_puts, shader_text_putn) or
 *    formatted printf-style (shader_text_printf); the text stays NUL
 *    terminated.
 *  - Room is kept in front of the text for a header a backend puts
 *    before it (shader_text_prepend): the header is written into that
 *    room, and the header and the text are one span without the text
 *    being copied.
 *  - A failed allocation leaves the text as it was and marks it failed;
 *    everything appended after that is dropped, and the owner checks
 *    shader_text_failed once, at the end.
 */

#ifndef VIDEOCOMMON_SHADER_TEXT_H
#define VIDEOCOMMON_SHADER_TEXT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__) || defined(__clang__)
#define SHADER_TEXT_PRINTF(f, a) __attribute__((format(printf, f, a)))
#else
#define SHADER_TEXT_PRINTF(f, a)
#endif

/* What a backend puts in front of the text fits in this. */
#define SHADER_TEXT_HEAD_ROOM 4096
/* the text a generator starts with room for */
#define SHADER_TEXT_FIRST_CAP 16384

struct shader_text
{
   char  *block;   /* SHADER_TEXT_HEAD_ROOM bytes, then the text, then its NUL */
   size_t len;     /* of the text */
   size_t cap;     /* for the text, its NUL included */
   int    failed;
};

/* Starts empty, with room for cap bytes of text before it grows. */
void shader_text_init(struct shader_text *t, size_t cap);
void shader_text_free(struct shader_text *t);
void shader_text_clear(struct shader_text *t);

void shader_text_puts(struct shader_text *t, const char *s);
void shader_text_putn(struct shader_text *t, const char *s, size_t n);
/* n spaces */
void shader_text_indent(struct shader_text *t, unsigned n);
void shader_text_printf(struct shader_text *t, const char *fmt, ...) SHADER_TEXT_PRINTF(2, 3);

/* The text, NUL terminated; "" when nothing was written. */
const char *shader_text_data(const struct shader_text *t);
size_t shader_text_len(const struct shader_text *t);
int shader_text_failed(const struct shader_text *t);

/* Writes head into the room in front of the text and returns where the
 * two begin, as one span of head_len + len bytes; NULL if head does not
 * fit the room. Another prepend replaces it. */
const char *shader_text_prepend(struct shader_text *t, const char *head, size_t head_len);

#ifdef __cplusplus
}
#endif

#endif
