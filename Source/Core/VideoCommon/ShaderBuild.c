/* Copyright 2026 Dolphin Emulator Project
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <stdarg.h>
#include <string.h>

#include "VideoCommon/ShaderArena.h"
#include "VideoCommon/ShaderBuild.h"

#define SB_MAX_ARGS 8

void shader_build_init(struct shader_build *sb, struct rslc_builder *b, unsigned api,
      unsigned gl, bool compute)
{
   unsigned k;
   memset(sb, 0, sizeof(*sb));
   sb->b       = b;
   sb->api     = api;
   sb->gl      = gl;
   sb->compute = compute;
   for (k = 1; k <= 4; k++)
   {
      sb->f[k] = rslc_b_vec(b, RSLC_BASE_FLOAT, k);
      sb->i[k] = rslc_b_vec(b, RSLC_BASE_INT, k);
      sb->u[k] = rslc_b_vec(b, RSLC_BASE_UINT, k);
   }
   sb->bl = rslc_b_vec(b, RSLC_BASE_BOOL, 1);
   sb->v  = rslc_b_void(b);
}

bool shader_build_compile(shader_build_fn fn, const void *arg, enum rslc_stage stage,
      unsigned api, unsigned gl, struct rslc_opts *opts, struct rslc_result *result)
{
   for (;;)
   {
      struct shader_build  sb;
      struct rslc_builder *b;
      bool                 built;
      opts->err[0] = '\0';
      shader_arena_use(opts);
      b = rslc_b_begin(opts, stage);
      if (!b)
         return false;
      shader_build_init(&sb, b, api, gl, stage == RSLC_STAGE_COMPUTE);
      built = fn(&sb, arg);
      if (rslc_b_end(b, result))
      {
         if (built)
            return true;
         shader_arena_blob_release(result->blob);
         return false;
      }
      if (!shader_arena_grow(opts->err))
         return false;
   }
}

/* OpenGL without the layouts its GLSL lacks binds by name after linking. */
#define SB_GL_NO(sb, flag) ((sb)->api == SHADER_BUILD_OPENGL && !((sb)->gl & (flag)))

uint32_t sb_attribute_location(const struct shader_build *sb, unsigned x)
{
   return SB_GL_NO(sb, SHADER_BUILD_GL_LOCATIONS) ? RSLC_NO_BINDING : x;
}

uint32_t sb_output_location(const struct shader_build *sb, unsigned x)
{
   return SB_GL_NO(sb, SHADER_BUILD_GL_LOCATIONS) ? RSLC_NO_BINDING : x;
}

uint32_t sb_varying_location(const struct shader_build *sb, unsigned x)
{
   return sb->api == SHADER_BUILD_OPENGL ? RSLC_NO_BINDING : x;
}

/* Vulkan: uniform blocks in set 0, from binding 0; graphics samplers in
 * set 1, texel buffers after the eight samplers, storage blocks in set 2;
 * compute packs everything into set 0. Direct3D: registers by kind, the
 * uniform blocks from 0. OpenGL: bindings by kind as given. */
void sb_ubo(const struct shader_build *sb, struct rslc_b_quals *q, unsigned x)
{
   q->storage = RSLC_B_ST_UNIFORM;
   q->std140  = true;
   if (SB_GL_NO(sb, SHADER_BUILD_GL_BINDINGS))
      return;
   q->binding = sb->api == SHADER_BUILD_OPENGL ? x : x - 1;
   if (sb->api == SHADER_BUILD_VULKAN)
      q->set = 0;
}

void sb_sampler(const struct shader_build *sb, struct rslc_b_quals *q, unsigned x)
{
   q->storage = RSLC_B_ST_UNIFORM;
   if (SB_GL_NO(sb, SHADER_BUILD_GL_BINDINGS))
      return;
   if (sb->api == SHADER_BUILD_VULKAN)
   {
      q->set     = sb->compute ? 0 : 1;
      q->binding = sb->compute ? 1 + x : x;
   }
   else
      q->binding = x;
}

void sb_texel_buffer(const struct shader_build *sb, struct rslc_b_quals *q, unsigned x)
{
   q->storage = RSLC_B_ST_UNIFORM;
   if (SB_GL_NO(sb, SHADER_BUILD_GL_BINDINGS))
      return;
   if (sb->api == SHADER_BUILD_VULKAN)
   {
      q->set     = sb->compute ? 0 : 1;
      q->binding = sb->compute ? 9 + x : x + 8;
   }
   else
      q->binding = x;
}

void sb_ssbo(const struct shader_build *sb, struct rslc_b_quals *q, unsigned x)
{
   q->storage = RSLC_B_ST_BUFFER;
   if (SB_GL_NO(sb, SHADER_BUILD_GL_BINDINGS))
      return;
   if (sb->api == SHADER_BUILD_VULKAN)
   {
      q->set     = 2;
      q->binding = x;
   }
   else
      q->binding = sb->api == SHADER_BUILD_D3D ? x + 2 : x;
}

void sb_image(const struct shader_build *sb, struct rslc_b_quals *q, const char *format,
      unsigned x)
{
   q->storage = RSLC_B_ST_UNIFORM;
   q->format  = format;
   if (SB_GL_NO(sb, SHADER_BUILD_GL_BINDINGS))
      return;
   if (sb->api == SHADER_BUILD_VULKAN)
   {
      q->set     = 0;
      q->binding = 11 + x;
   }
   else
      q->binding = x;
}

uint32_t sb_f(const struct shader_build *sb, double v)
{
   return rslc_b_float(sb->b, v);
}

uint32_t sb_i(const struct shader_build *sb, int32_t v)
{
   return rslc_b_int(sb->b, v);
}

uint32_t sb_u(const struct shader_build *sb, uint32_t v)
{
   return rslc_b_uint(sb->b, v);
}

uint32_t sb_ref(const struct shader_build *sb, const struct rslc_var *v)
{
   return rslc_b_ref(sb->b, v);
}

uint32_t sb_bi(const struct shader_build *sb, const char *name)
{
   return rslc_b_ref(sb->b, rslc_b_builtin_var(sb->b, name));
}

uint32_t sb_swz(const struct shader_build *sb, uint32_t e, const char *s)
{
   return rslc_b_swizzle(sb->b, e, s);
}

uint32_t sb_mem(const struct shader_build *sb, uint32_t e, uint32_t field)
{
   return rslc_b_member(sb->b, e, field);
}

uint32_t sb_idx(const struct shader_build *sb, uint32_t e, uint32_t idx)
{
   return rslc_b_index(sb->b, e, idx);
}

uint32_t sb_bin(const struct shader_build *sb, enum rslc_b_binop op, uint32_t l, uint32_t r)
{
   return rslc_b_binary(sb->b, op, l, r);
}

uint32_t sb_un(const struct shader_build *sb, enum rslc_b_unop op, uint32_t e)
{
   return rslc_b_unary(sb->b, op, e);
}

uint32_t sb_con(const struct shader_build *sb, const struct rslc_type *type, unsigned n, ...)
{
   uint32_t a[SB_MAX_ARGS];
   unsigned k;
   va_list  ap;
   if (n > SB_MAX_ARGS)
      return 0;
   va_start(ap, n);
   for (k = 0; k < n; k++)
      a[k] = va_arg(ap, uint32_t);
   va_end(ap);
   return rslc_b_construct(sb->b, type, a, n);
}

uint32_t sb_call(const struct shader_build *sb, const char *name, unsigned n, ...)
{
   uint32_t a[SB_MAX_ARGS];
   unsigned k;
   va_list  ap;
   if (n > SB_MAX_ARGS)
      return 0;
   va_start(ap, n);
   for (k = 0; k < n; k++)
      a[k] = va_arg(ap, uint32_t);
   va_end(ap);
   return rslc_b_builtin(sb->b, name, n ? a : NULL, n);
}

bool sb_set(const struct shader_build *sb, uint32_t l, uint32_t r)
{
   return rslc_b_stmt(sb->b, rslc_b_expr(sb->b, rslc_b_assign(sb->b, RSLC_B_SET, l, r)));
}

bool sb_op_set(const struct shader_build *sb, enum rslc_b_binop op, uint32_t l, uint32_t r)
{
   return rslc_b_stmt(sb->b, rslc_b_expr(sb->b, rslc_b_assign(sb->b, op, l, r)));
}

bool sb_do(const struct shader_build *sb, uint32_t e)
{
   return rslc_b_stmt(sb->b, rslc_b_expr(sb->b, e));
}
