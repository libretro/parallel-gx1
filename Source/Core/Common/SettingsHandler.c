/* Copyright 2012 Dolphin Emulator Project
 * SPDX-License-Identifier: GPL-2.0-or-later */

/* Thanks to Treeki for writing the original class - 29/01/2012 */

#include "Common/SettingsHandler.h"

#include <string.h>
#include <time.h>

/* Key used to encrypt/decrypt setting.txt contents */
#define INITIAL_SEED 0x73B5DBFAu

void settings_writer_init(settings_writer_t *writer)
{
  memset(writer->buffer, 0, sizeof(writer->buffer));
  writer->position = 0;
  writer->key = INITIAL_SEED;
}

static void settings_write_byte(settings_writer_t *writer, uint8_t b)
{
  if (writer->position >= sizeof(writer->buffer))
    return;

  writer->buffer[writer->position] = (uint8_t)(b ^ writer->key);
  writer->position++;
  writer->key = (writer->key >> 31) | (writer->key << 1);
}

static void settings_write_text(settings_writer_t *writer, const char *text)
{
  for (; *text; text++)
    settings_write_byte(writer, (uint8_t)*text);
}

static void settings_write_line(settings_writer_t *writer, const char *key, const char *value)
{
  const uint32_t old_position = writer->position;
  const uint32_t old_key = writer->key;
  bool has_null = false;
  uint32_t i;

  /* Encode and write the line */
  settings_write_text(writer, key);
  settings_write_byte(writer, '=');
  settings_write_text(writer, value);
  settings_write_text(writer, "\r\n");

  /* If the encoded data contains a null byte, Nintendo's decoder will stop at that null byte
   * instead of decoding all the data. To avoid this: If the data we just wrote contains
   * a null byte, add an LF right before the line to prod the values into being different,
   * just like Nintendo does. Due to the chosen key, LF itself never encodes into a null byte. */
  for (i = old_position; i < writer->position; i++)
    has_null = has_null || writer->buffer[i] == 0;
  if (has_null)
  {
    writer->key = old_key;
    writer->position = old_position;
    settings_write_byte(writer, '\n');
    settings_write_line(writer, key, value);
  }
}

void settings_writer_add(settings_writer_t *writer, const char *key, const char *value)
{
  settings_write_line(writer, key, value);
}

void settings_reader_init(settings_reader_t *reader, const settings_buffer_t buffer)
{
  uint32_t key = INITIAL_SEED;
  size_t position;

  /* The decoded data normally uses CRLF line endings, but occasionally
   * (see the comment in settings_write_line), lines can be separated by CRLFLF.
   * To handle this, we remove every CR and treat LF as the line ending.
   * (We ignore empty lines.) */
  reader->length = 0;
  for (position = 0; position < SETTINGS_SIZE; ++position)
  {
    const char c = (char)(uint8_t)(buffer[position] ^ key);
    if (c != '\x0d')
      reader->decoded[reader->length++] = c;
    key = (key >> 31) | (key << 1);
  }
}

/* The first place needle is found in the size bytes at haystack, or size */
static size_t settings_find(const char *haystack, size_t size, size_t from, const char *needle,
                            size_t needle_size)
{
  size_t i;
  for (i = from; i + needle_size <= size; i++)
  {
    if (!memcmp(haystack + i, needle, needle_size))
      return i;
  }
  return size;
}

bool settings_reader_get(const settings_reader_t *reader, const char *key, char *out,
                         size_t out_size)
{
  const size_t key_size = strlen(key);
  const char *const decoded = reader->decoded;
  const size_t length = reader->length;
  char to_find[SETTINGS_SIZE + 2];
  size_t found;
  size_t start;
  size_t end;
  size_t value_size;

  out[0] = '\0';
  if (key_size > SETTINGS_SIZE)
    return false;

  /* "\nkey=" anywhere, else "key=" at the very start */
  to_find[0] = '\n';
  memcpy(to_find + 1, key, key_size);
  to_find[key_size + 1] = '=';
  found = settings_find(decoded, length, 0, to_find, key_size + 2);
  if (found < length)
  {
    start = found + key_size + 2;
  }
  else if (settings_find(decoded, length, 0, to_find + 1, key_size + 1) == 0 &&
           key_size + 1 <= length)
  {
    start = key_size + 1;
  }
  else
  {
    return false;
  }

  /* The value runs to the next LF, or else drops the last byte, as it always has */
  end = settings_find(decoded, length, start, "\n", 1);
  if (end == length)
    end = length ? length - 1 : 0;
  value_size = end > start ? end - start : 0;
  if (value_size >= out_size)
    value_size = out_size - 1;
  memcpy(out, decoded + start, value_size);
  out[value_size] = '\0';
  return true;
}

void settings_generate_serial_number(char *out, size_t out_size)
{
  const time_t t = time(NULL);
  unsigned long value = (unsigned long)(t % 1000000000);
  char digits[10];
  size_t i;

  /* Must be 9 characters at most; otherwise the serial number will be rejected by SDK libraries,
   * as there is a check to ensure the string length is strictly lower than 10. */
  for (i = 9; i-- > 0;)
  {
    digits[i] = (char)('0' + value % 10);
    value /= 10;
  }
  digits[9] = '\0';
  if (!out_size)
    return;
  for (i = 0; i + 1 < out_size && digits[i]; i++)
    out[i] = digits[i];
  out[i] = '\0';
}
