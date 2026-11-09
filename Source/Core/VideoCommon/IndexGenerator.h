/* Copyright 2008 Dolphin Emulator Project
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef VIDEOCOMMON_INDEX_GENERATOR_H
#define VIDEOCOMMON_INDEX_GENERATOR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Writes the indices of each GX primitive as triangles, lines or points
 * straight into the buffer it is started on (a backend's mapped index
 * buffer, or a CPU one for culled batches). */

typedef uint16_t *(*index_generator_fn)(uint16_t *out, uint32_t num_verts, uint32_t index);

struct index_generator
{
   uint16_t *current;
   uint16_t *base;
   uint32_t base_index;
   int vs_expand; /* lines and points take four indices each */
   index_generator_fn table[8]; /* by GX primitive, 0 to 7 */
};

/* The functions for the backend's primitive restart and the
 * line/point expansion in the vertex shader, again on config change. */
void index_generator_init(struct index_generator *g, int primitive_restart, int vs_expand);
void index_generator_start(struct index_generator *g, uint16_t *index_ptr);
void index_generator_add(struct index_generator *g, unsigned primitive, uint32_t num_vertices);
void index_generator_add_external(struct index_generator *g, const uint16_t *indices,
      uint32_t num_indices, uint32_t num_vertices);
/* Vertices a primitive can still add before the indices run out. */
uint32_t index_generator_remaining(const struct index_generator *g, unsigned primitive);

#define index_generator_num_verts(g) ((g)->base_index)
#define index_generator_index_len(g) ((uint32_t)((g)->current - (g)->base))

#ifdef __cplusplus
}
#endif

#endif
