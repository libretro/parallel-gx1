/* Copyright 2026 Dolphin Emulator Project
 * SPDX-License-Identifier: GPL-2.0-or-later */

/* See GBAVFile.h. */

#include <stdlib.h>
#include <string.h>

#include <mgba-util/vfs.h>
#include <streams/file_stream.h>

#include "Core/HW/GBAVFile.h"

/* A writable map: its own buffer, until it is unmapped. */
struct gba_vfile_map
{
   struct gba_vfile_map *next;
   size_t                size;
   /* the octets follow */
};

struct gba_vfile
{
   struct VFile          d;
   RFILE                *file;
   const unsigned char  *map;      /* the whole file, lent, or NULL */
   int64_t               map_size;
   struct gba_vfile_map *maps;     /* writable maps outstanding */
};

static bool gba_vfile_close(struct VFile *vf)
{
   struct gba_vfile *f = (struct gba_vfile*)vf;
   int ok              = filestream_close(f->file) == 0;

   while (f->maps)
   {
      struct gba_vfile_map *next = f->maps->next;
      free(f->maps);
      f->maps = next;
   }
   free(f);
   return ok != 0;
}

static off_t gba_vfile_seek(struct VFile *vf, off_t offset, int whence)
{
   struct gba_vfile *f = (struct gba_vfile*)vf;
   int mode;

   switch (whence)
   {
      case SEEK_CUR:
         mode = RETRO_VFS_SEEK_POSITION_CURRENT;
         break;
      case SEEK_END:
         mode = RETRO_VFS_SEEK_POSITION_END;
         break;
      default:
         mode = RETRO_VFS_SEEK_POSITION_START;
         break;
   }
   if (filestream_seek(f->file, (int64_t)offset, mode) < 0)
      return -1;
   return (off_t)filestream_tell(f->file);
}

static ssize_t gba_vfile_read(struct VFile *vf, void *buffer, size_t size)
{
   struct gba_vfile *f = (struct gba_vfile*)vf;
   return (ssize_t)filestream_read(f->file, buffer, (int64_t)size);
}

static ssize_t gba_vfile_write(struct VFile *vf, const void *buffer, size_t size)
{
   struct gba_vfile *f = (struct gba_vfile*)vf;
   return (ssize_t)filestream_write(f->file, buffer, (int64_t)size);
}

static ssize_t gba_vfile_size(struct VFile *vf)
{
   struct gba_vfile *f = (struct gba_vfile*)vf;
   return (ssize_t)filestream_get_size(f->file);
}

static void *gba_vfile_map(struct VFile *vf, size_t size, int flags)
{
   struct gba_vfile *f = (struct gba_vfile*)vf;
   struct gba_vfile_map *m;
   unsigned char *data;
   int64_t have;

   if (!size)
      return NULL;
   /* Read-only, and all there: the mapping, lent. mGBA writes to a
    * read-only map nowhere; a ROM it patches is copied first. */
   if (!(flags & MAP_WRITE) && f->map && (int64_t)size <= f->map_size)
      return (void*)f->map;

   m = (struct gba_vfile_map*)malloc(sizeof(*m) + size);
   if (!m)
      return NULL;
   data = (unsigned char*)(m + 1);
   have = filestream_get_size(f->file);
   if (have < 0)
      have = 0;
   if ((uint64_t)have > size)
      have = (int64_t)size;
   if (have && (filestream_seek(f->file, 0, RETRO_VFS_SEEK_POSITION_START) < 0
            || filestream_read(f->file, data, have) != have))
   {
      free(m);
      return NULL;
   }
   /* what lies past the end of the file reads as zeroes, as a mapping
    * of a file grown to the size would */
   memset(data + have, 0, size - (size_t)have);
   m->size = size;
   m->next = f->maps;
   f->maps = m;
   return data;
}

static void gba_vfile_unmap(struct VFile *vf, void *memory, size_t size)
{
   struct gba_vfile *f         = (struct gba_vfile*)vf;
   struct gba_vfile_map **link = &f->maps;

   (void)size;
   while (*link)
   {
      if ((void*)(*link + 1) == memory)
      {
         struct gba_vfile_map *m = *link;
         *link = m->next;
         free(m);
         return;
      }
      link = &(*link)->next;
   }
   /* the lent mapping: nothing to give back */
}

static void gba_vfile_truncate(struct VFile *vf, size_t size)
{
   struct gba_vfile *f = (struct gba_vfile*)vf;
   filestream_truncate(f->file, (int64_t)size);
}

/* What was written to a map, written to the file. */
static bool gba_vfile_sync(struct VFile *vf, void *buffer, size_t size)
{
   struct gba_vfile *f = (struct gba_vfile*)vf;

   if (buffer && size)
   {
      if (filestream_seek(f->file, 0, RETRO_VFS_SEEK_POSITION_START) < 0
            || filestream_write(f->file, buffer, (int64_t)size) != (int64_t)size)
         return false;
   }
   return filestream_flush(f->file) == 0;
}

struct VFile *gba_vfile_open(const char *path, int writable)
{
   struct gba_vfile *f;
   RFILE *file;
   unsigned mode;

   if (!path || !*path)
      return NULL;
   if (!writable)
      mode = RETRO_VFS_FILE_ACCESS_READ;
   else if (filestream_exists(path))
      mode = RETRO_VFS_FILE_ACCESS_READ_WRITE | RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING;
   else
      mode = RETRO_VFS_FILE_ACCESS_READ_WRITE;
   file = filestream_open(path, mode, writable ? RETRO_VFS_FILE_ACCESS_HINT_NONE
         : RETRO_VFS_FILE_ACCESS_HINT_FREQUENT_ACCESS);
   if (!file)
      return NULL;

   f = (struct gba_vfile*)calloc(1, sizeof(*f));
   if (!f)
   {
      filestream_close(file);
      return NULL;
   }
   f->file       = file;
   f->map        = writable ? NULL : filestream_get_mapped_ptr(file, &f->map_size);
   f->d.close    = gba_vfile_close;
   f->d.seek     = gba_vfile_seek;
   f->d.read     = gba_vfile_read;
   f->d.readline = VFileReadline;
   f->d.write    = gba_vfile_write;
   f->d.map      = gba_vfile_map;
   f->d.unmap    = gba_vfile_unmap;
   f->d.truncate = gba_vfile_truncate;
   f->d.size     = gba_vfile_size;
   f->d.sync     = gba_vfile_sync;
   return &f->d;
}
