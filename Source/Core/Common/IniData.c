/* Copyright 2026 Dolphin Emulator Project
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "Common/IniData.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <streams/file_stream.h>

static char ini_lower(char c)
{
   if (c >= 'A' && c <= 'Z')
      return (char)(c - 'A' + 'a');
   return c;
}

/* Whether a terminated name is those len characters, whatever the case. */
static bool ini_name_is(const char *name, const char *other, size_t len)
{
   size_t i;
   for (i = 0; i < len; i++)
   {
      if (!name[i] || ini_lower(name[i]) != ini_lower(other[i]))
         return false;
   }
   return name[len] == '\0';
}

static char *ini_dup(const char *text, size_t len)
{
   char *copy = (char*)malloc(len + 1);
   if (!copy)
      return NULL;
   if (len)
      memcpy(copy, text, len);
   copy[len] = '\0';
   return copy;
}

static bool ini_is_space(char c)
{
   return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/* The text without the whitespace at either end. */
static void ini_strip(const char **text, size_t *len)
{
   const char *t = *text;
   size_t      n = *len;
   while (n && ini_is_space(t[0]))
   {
      t++;
      n--;
   }
   while (n && ini_is_space(t[n - 1]))
      n--;
   *text = t;
   *len  = n;
}

void ini_data_init(struct ini_data *ini)
{
   ini->sections     = NULL;
   ini->num_sections = 0;
   ini->cap_sections = 0;
}

void ini_section_init(struct ini_section *section)
{
   memset(section, 0, sizeof(*section));
}

void ini_section_clear(struct ini_section *section)
{
   size_t i;
   for (i = 0; i < section->num_keys; i++)
   {
      free(section->keys[i].name);
      free(section->keys[i].value);
   }
   for (i = 0; i < section->num_lines; i++)
      free(section->lines[i]);
   free(section->keys);
   free(section->lines);
   free(section->name);
   ini_section_init(section);
}

void ini_data_free(struct ini_data *ini)
{
   size_t i;
   for (i = 0; i < ini->num_sections; i++)
   {
      ini_section_clear(ini->sections[i]);
      free(ini->sections[i]);
   }
   free(ini->sections);
   ini_data_init(ini);
}

struct ini_section *ini_data_section(const struct ini_data *ini,
      const char *name, size_t name_len)
{
   size_t i;
   for (i = 0; i < ini->num_sections; i++)
   {
      if (ini_name_is(ini->sections[i]->name, name, name_len))
         return ini->sections[i];
   }
   return NULL;
}

struct ini_section *ini_data_section_make(struct ini_data *ini,
      const char *name, size_t name_len)
{
   struct ini_section *section = ini_data_section(ini, name, name_len);
   if (section)
      return section;

   if (ini->num_sections == ini->cap_sections)
   {
      const size_t         cap   = ini->cap_sections ? ini->cap_sections * 2 : 16;
      struct ini_section **grown = (struct ini_section**)realloc(
            ini->sections, cap * sizeof(*grown));
      if (!grown)
         return NULL;
      ini->sections     = grown;
      ini->cap_sections = cap;
   }

   section = (struct ini_section*)calloc(1, sizeof(*section));
   if (!section)
      return NULL;
   section->name = ini_dup(name, name_len);
   if (!section->name)
   {
      free(section);
      return NULL;
   }
   ini->sections[ini->num_sections++] = section;
   return section;
}

static struct ini_key *ini_section_key(const struct ini_section *section,
      const char *key, size_t key_len)
{
   size_t i;
   for (i = 0; i < section->num_keys; i++)
   {
      if (ini_name_is(section->keys[i].name, key, key_len))
         return &section->keys[i];
   }
   return NULL;
}

const char *ini_section_get(const struct ini_section *section,
      const char *key, size_t key_len)
{
   const struct ini_key *found = ini_section_key(section, key, key_len);
   return found ? found->value : NULL;
}

bool ini_section_set(struct ini_section *section,
      const char *key, size_t key_len,
      const char *value, size_t value_len)
{
   struct ini_key *found = ini_section_key(section, key, key_len);
   char           *copy  = ini_dup(value, value_len);
   if (!copy)
      return false;

   if (found)
   {
      free(found->value);
      found->value = copy;
      return true;
   }

   if (section->num_keys == section->cap_keys)
   {
      const size_t    cap   = section->cap_keys ? section->cap_keys * 2 : 16;
      struct ini_key *grown = (struct ini_key*)realloc(
            section->keys, cap * sizeof(*grown));
      if (!grown)
      {
         free(copy);
         return false;
      }
      section->keys     = grown;
      section->cap_keys = cap;
   }

   found       = &section->keys[section->num_keys];
   found->name = ini_dup(key, key_len);
   if (!found->name)
   {
      free(copy);
      return false;
   }
   found->value = copy;
   section->num_keys++;
   return true;
}

bool ini_section_delete(struct ini_section *section,
      const char *key, size_t key_len)
{
   struct ini_key *found = ini_section_key(section, key, key_len);
   size_t          at;
   if (!found)
      return false;

   at = (size_t)(found - section->keys);
   free(found->name);
   free(found->value);
   memmove(found, found + 1,
         (section->num_keys - at - 1) * sizeof(*found));
   section->num_keys--;
   return true;
}

static bool ini_section_add_line(struct ini_section *section,
      const char *line, size_t len)
{
   char *copy;
   if (section->num_lines == section->cap_lines)
   {
      const size_t cap   = section->cap_lines ? section->cap_lines * 2 : 16;
      char       **grown = (char**)realloc(section->lines, cap * sizeof(*grown));
      if (!grown)
         return false;
      section->lines     = grown;
      section->cap_lines = cap;
   }
   copy = ini_dup(line, len);
   if (!copy)
      return false;
   section->lines[section->num_lines++] = copy;
   return true;
}

/* One line of a section, without its line ending. */
static void ini_section_read_line(struct ini_section *section,
      const char *line, size_t len)
{
   const char *equals    = NULL;
   const char *key       = NULL;
   const char *value     = NULL;
   size_t      key_len   = 0;
   size_t      value_len = 0;

   /* a comment is no key: it is kept among the lines */
   if (line[0] != '#')
      equals = (const char*)memchr(line, '=', len);

   if (equals)
   {
      key       = line;
      key_len   = (size_t)(equals - line);
      value     = equals + 1;
      value_len = len - key_len - 1;
      ini_strip(&key, &key_len);
      ini_strip(&value, &value_len);
      /* "value" is value */
      if (value_len && value[0] == '"' && value[value_len - 1] == '"')
      {
         /* a lone quote is both ends: nothing is left of it */
         value_len = value_len >= 2 ? value_len - 2 : 0;
         value++;
      }
   }

   /* Lines starting with '$', '*' or '+' are kept verbatim, as are those
    * that give no key and no value. */
   if ((!key_len && !value_len) || line[0] == '$' || line[0] == '+' || line[0] == '*')
      ini_section_add_line(section, line, len);
   else
      ini_section_set(section, key, key_len, value, value_len);
}

bool ini_data_load(struct ini_data *ini, const char *path, bool keep)
{
   void               *buf     = NULL;
   int64_t             size    = 0;
   const char         *text;
   const char         *end;
   struct ini_section *section = NULL;

   if (!keep)
      ini_data_free(ini);

   if (!filestream_read_file(path, &buf, &size))
      return false;
   if (!buf)
      return true;

   text = (const char*)buf;
   end  = text + (size > 0 ? (size_t)size : 0);

   /* Skips the UTF-8 BOM at the start of files. Notepad likes to add this. */
   if (end - text >= 3 && !memcmp(text, "\xEF\xBB\xBF", 3))
      text += 3;

   while (text < end)
   {
      const char *newline = (const char*)memchr(text, '\n', (size_t)(end - text));
      const char *next    = newline ? newline + 1 : end;
      size_t      len     = (size_t)((newline ? newline : end) - text);

      /* the line ending is "\n" or "\r\n" */
      if (len && text[len - 1] == '\r')
         len--;

      if (len)
      {
         if (text[0] == '[')
         {
            const char *close = (const char*)memchr(text, ']', len);
            if (close)
               section = ini_data_section_make(ini, text + 1, (size_t)(close - text - 1));
         }
         else if (section)
            ini_section_read_line(section, text, len);
      }
      text = next;
   }

   free(buf);
   return true;
}

/* Text being put together for a file. */
struct ini_text
{
   char  *data;
   size_t len;
   size_t cap;
   bool   failed;
};

static void ini_text_add(struct ini_text *out, const char *text, size_t len)
{
   if (out->failed)
      return;
   if (out->len + len > out->cap)
   {
      size_t cap = out->cap ? out->cap * 2 : 4096;
      char  *grown;
      while (cap < out->len + len)
         cap *= 2;
      grown = (char*)realloc(out->data, cap);
      if (!grown)
      {
         out->failed = true;
         return;
      }
      out->data = grown;
      out->cap  = cap;
   }
   memcpy(out->data + out->len, text, len);
   out->len += len;
}

bool ini_data_save(const struct ini_data *ini, const char *path)
{
   struct ini_text out;
   size_t          i;
   size_t          j;
   bool            saved;

   out.data   = NULL;
   out.len    = 0;
   out.cap    = 0;
   out.failed = false;

   for (i = 0; i < ini->num_sections; i++)
   {
      const struct ini_section *section = ini->sections[i];

      if (section->num_keys || section->num_lines)
      {
         ini_text_add(&out, "[", 1);
         ini_text_add(&out, section->name, strlen(section->name));
         ini_text_add(&out, "]\n", 2);
      }

      if (!section->num_keys)
      {
         for (j = 0; j < section->num_lines; j++)
         {
            ini_text_add(&out, section->lines[j], strlen(section->lines[j]));
            ini_text_add(&out, "\n", 1);
         }
      }
      else
      {
         for (j = 0; j < section->num_keys; j++)
         {
            const struct ini_key *key = &section->keys[j];
            ini_text_add(&out, key->name, strlen(key->name));
            ini_text_add(&out, " = ", 3);
            ini_text_add(&out, key->value, strlen(key->value));
            ini_text_add(&out, "\n", 1);
         }
      }
   }

   if (out.failed)
      saved = false;
   else if (filestream_matches_buf(path, out.data ? out.data : "", out.len))
      saved = true;
   else
      saved = filestream_write_file_atomic(path, out.data ? out.data : "", (int64_t)out.len);

   free(out.data);
   return saved;
}
