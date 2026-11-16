/* Copyright 2026 Dolphin Emulator Project
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef COMMON_INI_DATA_H
#define COMMON_INI_DATA_H

#include <stddef.h>

#include <boolean.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The sections of an INI file. A section holds keys with a value each, in
 * the order they were first set, and the lines that are no key: those
 * starting with '$', '+' or '*', and those without a '='. Section and key
 * names are found whatever their case, and keep the spelling they were
 * first given. Names and values are passed with their length and need no
 * terminator; those handed back have one. */

struct ini_key
{
   char *name;
   char *value;
};

struct ini_section
{
   char           *name;
   struct ini_key *keys;
   size_t          num_keys;
   size_t          cap_keys;
   char          **lines;
   size_t          num_lines;
   size_t          cap_lines;
};

struct ini_data
{
   /* each section is its own allocation: a section found stays where it
    * is while others are made */
   struct ini_section **sections;
   size_t               num_sections;
   size_t               cap_sections;
};

void ini_data_init(struct ini_data *ini);

/* Frees every section and leaves the data empty. */
void ini_data_free(struct ini_data *ini);

/* Reads the file's sections. With keep, they extend those already held,
 * a key read replacing the value it had; without, those are freed first.
 * False if the file cannot be read. */
bool ini_data_load(struct ini_data *ini, const char *path, bool keep);

/* Writes the sections that hold anything: the keys of a section that has
 * keys, else its lines. An unchanged file is left alone, a changed one is
 * replaced whole or not at all. */
bool ini_data_save(const struct ini_data *ini, const char *path);

/* The section of that name, or NULL. */
struct ini_section *ini_data_section(const struct ini_data *ini,
      const char *name, size_t name_len);

/* The section of that name, made empty if there is none. NULL only when
 * out of memory. */
struct ini_section *ini_data_section_make(struct ini_data *ini,
      const char *name, size_t name_len);

/* A section of no file, such as one made to be read as empty: it starts
 * with ini_section_init, and ini_section_clear frees what it was given. */
void ini_section_init(struct ini_section *section);
void ini_section_clear(struct ini_section *section);

/* The key's value, or NULL if the section has no such key. */
const char *ini_section_get(const struct ini_section *section,
      const char *key, size_t key_len);

/* Gives the key that value. False only when out of memory. */
bool ini_section_set(struct ini_section *section,
      const char *key, size_t key_len,
      const char *value, size_t value_len);

/* Removes the key. False if the section has no such key. */
bool ini_section_delete(struct ini_section *section,
      const char *key, size_t key_len);

#ifdef __cplusplus
}
#endif

#endif
