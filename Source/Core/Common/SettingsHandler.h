/* Copyright 2008 Dolphin Emulator Project
 * SPDX-License-Identifier: GPL-2.0-or-later */

/* Thanks to Treeki for writing the original class - 29/01/2012 */

#ifndef DOLPHIN_COMMON_SETTINGSHANDLER_H
#define DOLPHIN_COMMON_SETTINGSHANDLER_H

#include <stddef.h>
#include <stdint.h>

#include <boolean.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The encrypted 256 bytes of a Wii setting.txt */
#define SETTINGS_SIZE 0x100
typedef uint8_t settings_buffer_t[SETTINGS_SIZE];

/* Large enough for any value in a setting.txt, terminator included */
#define SETTINGS_VALUE_SIZE (SETTINGS_SIZE + 1)

typedef struct settings_writer
{
  settings_buffer_t buffer;
  uint32_t position;
  uint32_t key;
} settings_writer_t;

void settings_writer_init(settings_writer_t *writer);
/* Appends "key=value\r\n"; what runs past the buffer is dropped. */
void settings_writer_add(settings_writer_t *writer, const char *key, const char *value);

typedef struct settings_reader
{
  /* decoded, without its CRs */
  char decoded[SETTINGS_SIZE];
  size_t length;
} settings_reader_t;

void settings_reader_init(settings_reader_t *reader, const settings_buffer_t buffer);
/* Writes the value of key to out, empty when there is none, cut short to fit; true when found. */
bool settings_reader_get(const settings_reader_t *reader, const char *key, char *out,
                         size_t out_size);

/* Writes a serial number of 9 digits made from the time. */
void settings_generate_serial_number(char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif
