/* Copyright 2008 Dolphin Emulator Project
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <string.h>

#include "VideoCommon/IndexGenerator.h"

#define RESTART 0xFFFFu

/* GX primitive numbers */
enum
{
   PRIM_QUADS = 0,
   PRIM_QUADS_2,
   PRIM_TRIANGLES,
   PRIM_TRIANGLE_STRIP,
   PRIM_TRIANGLE_FAN,
   PRIM_LINES,
   PRIM_LINE_STRIP,
   PRIM_POINTS
};

#define TRI(p, a, b, c)                                                        \
   do                                                                         \
   {                                                                          \
      *(p)++ = (uint16_t)(a);                                                 \
      *(p)++ = (uint16_t)(b);                                                 \
      *(p)++ = (uint16_t)(c);                                                 \
   } while (0)

/* Without primitive restart: separate triangles */

static uint16_t *list_tri(uint16_t *p, uint32_t n, uint32_t index)
{
   uint32_t i;
   for (i = 2; i < n; i += 3)
      TRI(p, index + i - 2, index + i - 1, index + i);
   return p;
}

static uint16_t *strip_tri(uint16_t *p, uint32_t n, uint32_t index)
{
   uint32_t i;
   uint32_t wind = 0;
   for (i = 2; i < n; ++i)
   {
      TRI(p, index + i - 2, index + i - (wind ^ 1), index + i - wind);
      wind ^= 1;
   }
   return p;
}

static uint16_t *fan_tri(uint16_t *p, uint32_t n, uint32_t index)
{
   uint32_t i;
   for (i = 2; i < n; ++i)
      TRI(p, index, index + i - 1, index + i);
   return p;
}

/* 0---1
 * |\  |  012, 023; three vertices left over make a triangle
 * 3---2 */
static uint16_t *quads_tri(uint16_t *p, uint32_t n, uint32_t index)
{
   uint32_t i;
   for (i = 3; i < n; i += 4)
   {
      TRI(p, index + i - 3, index + i - 2, index + i - 1);
      TRI(p, index + i - 3, index + i - 1, index + i);
   }
   if (i == n)
      TRI(p, index + n - 3, index + n - 2, index + n - 1);
   return p;
}

/* With primitive restart: strips ended by the restart index */

static uint16_t *list_pr(uint16_t *p, uint32_t n, uint32_t index)
{
   uint32_t i;
   for (i = 2; i < n; i += 3)
   {
      TRI(p, index + i - 2, index + i - 1, index + i);
      *p++ = RESTART;
   }
   return p;
}

static uint16_t *strip_pr(uint16_t *p, uint32_t n, uint32_t index)
{
   uint32_t i;
   for (i = 0; i < n; ++i)
      *p++ = (uint16_t)(index + i);
   *p++ = RESTART;
   return p;
}

/*   2---3
 *  / \ / \     012, 023, 034 rotated to 120, 302, 034:
 * 1---0---4    the strip 12034, six indices for three triangles */
static uint16_t *fan_pr(uint16_t *p, uint32_t n, uint32_t index)
{
   uint32_t i = 2;
   for (; i + 3 <= n; i += 3)
   {
      *p++ = (uint16_t)(index + i - 1);
      *p++ = (uint16_t)(index + i);
      *p++ = (uint16_t)index;
      *p++ = (uint16_t)(index + i + 1);
      *p++ = (uint16_t)(index + i + 2);
      *p++ = RESTART;
   }
   for (; i + 2 <= n; i += 2)
   {
      *p++ = (uint16_t)(index + i - 1);
      *p++ = (uint16_t)(index + i);
      *p++ = (uint16_t)index;
      *p++ = (uint16_t)(index + i + 1);
      *p++ = RESTART;
   }
   for (; i < n; ++i)
   {
      TRI(p, index, index + i - 1, index + i);
      *p++ = RESTART;
   }
   return p;
}

/* each quad the strip 1203 */
static uint16_t *quads_pr(uint16_t *p, uint32_t n, uint32_t index)
{
   uint32_t i;
   for (i = 3; i < n; i += 4)
   {
      *p++ = (uint16_t)(index + i - 2);
      *p++ = (uint16_t)(index + i - 1);
      *p++ = (uint16_t)(index + i - 3);
      *p++ = (uint16_t)(index + i);
      *p++ = RESTART;
   }
   if (i == n)
   {
      TRI(p, index + n - 3, index + n - 2, index + n - 1);
      *p++ = RESTART;
   }
   return p;
}

/* Lines and points as the rasterizer draws them; strips become lists */

static uint16_t *lines(uint16_t *p, uint32_t n, uint32_t index)
{
   uint32_t i;
   for (i = 1; i < n; i += 2)
   {
      *p++ = (uint16_t)(index + i - 1);
      *p++ = (uint16_t)(index + i);
   }
   return p;
}

static uint16_t *line_strip(uint16_t *p, uint32_t n, uint32_t index)
{
   uint32_t i;
   for (i = 1; i < n; ++i)
   {
      *p++ = (uint16_t)(index + i - 1);
      *p++ = (uint16_t)(index + i);
   }
   return p;
}

static uint16_t *points(uint16_t *p, uint32_t n, uint32_t index)
{
   uint32_t i;
   for (i = 0; i != n; ++i)
      *p++ = (uint16_t)(index + i);
   return p;
}

/* Expanded in the vertex shader: the vertex is index >> 2, bit 0 the side
 * of the line (or point), bit 1 its end; the two ends of a line are
 * adjacent vertices. */

static uint16_t *expand_lines(uint16_t *p, uint32_t n, uint32_t index, uint32_t step, int pr)
{
   uint32_t i;
   for (i = 1; i < n; i += step)
   {
      uint32_t p0 = (index + i - 1) << 2;
      uint32_t p1 = (index + i) << 2;
      *p++ = (uint16_t)p0;
      *p++ = (uint16_t)(p0 + 1);
      *p++ = (uint16_t)(p1 + 2);
      if (pr)
         *p++ = (uint16_t)(p1 + 3), *p++ = RESTART;
      else
         TRI(p, p0 + 1, p1 + 2, p1 + 3);
   }
   return p;
}

static uint16_t *expand_points(uint16_t *p, uint32_t n, uint32_t index, int pr)
{
   uint32_t i;
   for (i = 0; i < n; ++i)
   {
      uint32_t b = (index + i) << 2;
      *p++ = (uint16_t)b;
      *p++ = (uint16_t)(b + 1);
      *p++ = (uint16_t)(b + 2);
      if (pr)
         *p++ = (uint16_t)(b + 3), *p++ = RESTART;
      else
         TRI(p, b + 1, b + 2, b + 3);
   }
   return p;
}

static uint16_t *lines_vs(uint16_t *p, uint32_t n, uint32_t i) { return expand_lines(p, n, i, 2, 0); }
static uint16_t *lines_vs_pr(uint16_t *p, uint32_t n, uint32_t i) { return expand_lines(p, n, i, 2, 1); }
static uint16_t *strip_vs(uint16_t *p, uint32_t n, uint32_t i) { return expand_lines(p, n, i, 1, 0); }
static uint16_t *strip_vs_pr(uint16_t *p, uint32_t n, uint32_t i) { return expand_lines(p, n, i, 1, 1); }
static uint16_t *points_vs(uint16_t *p, uint32_t n, uint32_t i) { return expand_points(p, n, i, 0); }
static uint16_t *points_vs_pr(uint16_t *p, uint32_t n, uint32_t i) { return expand_points(p, n, i, 1); }

void index_generator_init(struct index_generator *g, int primitive_restart, int vs_expand)
{
   index_generator_fn *t = g->table;
   t[PRIM_QUADS] = t[PRIM_QUADS_2] = primitive_restart ? quads_pr : quads_tri;
   t[PRIM_TRIANGLES] = primitive_restart ? list_pr : list_tri;
   t[PRIM_TRIANGLE_STRIP] = primitive_restart ? strip_pr : strip_tri;
   t[PRIM_TRIANGLE_FAN] = primitive_restart ? fan_pr : fan_tri;
   if (vs_expand)
   {
      t[PRIM_LINES] = primitive_restart ? lines_vs_pr : lines_vs;
      t[PRIM_LINE_STRIP] = primitive_restart ? strip_vs_pr : strip_vs;
      t[PRIM_POINTS] = primitive_restart ? points_vs_pr : points_vs;
   }
   else
   {
      t[PRIM_LINES] = lines;
      t[PRIM_LINE_STRIP] = line_strip;
      t[PRIM_POINTS] = points;
   }
   g->vs_expand = vs_expand;
}

void index_generator_start(struct index_generator *g, uint16_t *index_ptr)
{
   g->current = index_ptr;
   g->base = index_ptr;
   g->base_index = 0;
}

void index_generator_add(struct index_generator *g, unsigned primitive, uint32_t num_vertices)
{
   g->current = g->table[primitive & 7](g->current, num_vertices, g->base_index);
   g->base_index += num_vertices;
}

void index_generator_add_external(struct index_generator *g, const uint16_t *indices,
      uint32_t num_indices, uint32_t num_vertices)
{
   memcpy(g->current, indices, sizeof(uint16_t) * num_indices);
   g->current += num_indices;
   g->base_index += num_vertices;
}

/* 0xFFFF is the restart index, but a game sends at most 0xFFFF vertices,
 * indices 0 to 0xFFFE, so none needs reserving for it. */
uint32_t index_generator_remaining(const struct index_generator *g, unsigned primitive)
{
   uint32_t max_index = 0xFFFFu;
   if (g->vs_expand && primitive >= PRIM_LINES)
      max_index >>= 2;
   return g->base_index > max_index ? 0 : max_index - g->base_index;
}
