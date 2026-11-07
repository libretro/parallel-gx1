/* Copyright 2026 Dolphin Emulator Project
 * SPDX-License-Identifier: GPL-2.0-or-later */

/* An mGBA VFile over a libretro-common filestream, so the GBA core opens
 * its ROM, BIOS and saves through the frontend's VFS like everything
 * else. A read-only map of a mapped file lends the mapping itself: the
 * ROM and the BIOS are read where they lie. A writable map is a buffer
 * of its own, read in once and written back on sync. */

#ifndef GBA_VFILE_H
#define GBA_VFILE_H

#ifdef __cplusplus
extern "C" {
#endif

struct VFile;

/* writable: open for reading and writing, made if it is not there yet.
 * Returns NULL when the file cannot be opened. */
struct VFile* gba_vfile_open(const char* path, int writable);

#ifdef __cplusplus
}
#endif

#endif
