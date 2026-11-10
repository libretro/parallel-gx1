/* Copyright 2026 Dolphin Emulator Project
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef VIDEOCOMMON_SHADER_BUILD_H
#define VIDEOCOMMON_SHADER_BUILD_H

#include <stdint.h>

#include <boolean.h>
#include <gfx/shaders/rslc.h>
#include <gfx/shaders/rslc_build.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Whose resource layout a stage is built for. */
enum shader_build_api
{
   SHADER_BUILD_VULKAN = 0,
   SHADER_BUILD_D3D,
   SHADER_BUILD_OPENGL
};

/* What the OpenGL context's GLSL takes in layouts. */
#define SHADER_BUILD_GL_BINDINGS  1u   /* binding = on blocks, samplers, images */
#define SHADER_BUILD_GL_LOCATIONS 2u   /* location = on attributes and outputs */

/* A stage being built: the builder, the layout it binds its resources
 * by, and the scalar and vector types by base and width (index 1-4). */
struct shader_build
{
   struct rslc_builder    *b;
   const struct rslc_type *f[5];
   const struct rslc_type *i[5];
   const struct rslc_type *u[5];
   const struct rslc_type *bl;
   const struct rslc_type *v;
   unsigned                api;     /* enum shader_build_api */
   unsigned                gl;      /* SHADER_BUILD_GL_*, for SHADER_BUILD_OPENGL */
   bool                    compute;
};

/* Builds one stage into sb; false where something could not be built
 * (the builder holds the reason). Run again from the start when the
 * compile's memory had to grow. */
typedef bool (*shader_build_fn)(struct shader_build *sb, const void *arg);

void shader_build_init(struct shader_build *sb, struct rslc_builder *b, unsigned api,
      unsigned gl, bool compute);

/* fn's stage compiled for opts (dst, version, err) in this thread's
 * shader arena: built again from the start where the arena had to grow.
 * The result is released with shader_arena_blob_release(result->blob). */
bool shader_build_compile(shader_build_fn fn, const void *arg, enum rslc_stage stage,
      unsigned api, unsigned gl, struct rslc_opts *opts, struct rslc_result *result);

/* Resource layouts: what the shader headers' UBO_BINDING, SAMPLER_BINDING,
 * TEXEL_BUFFER_BINDING, SSBO_BINDING and IMAGE_BINDING said. */
/* A vertex attribute's, a fragment output's and a varying's location:
 * ATTRIBUTE_LOCATION, FRAGMENT_OUTPUT_LOCATION and VARYING_LOCATION. */
uint32_t sb_attribute_location(const struct shader_build *sb, unsigned x);
uint32_t sb_output_location(const struct shader_build *sb, unsigned x);
uint32_t sb_varying_location(const struct shader_build *sb, unsigned x);
void sb_ubo(const struct shader_build *sb, struct rslc_b_quals *q, unsigned x);
void sb_sampler(const struct shader_build *sb, struct rslc_b_quals *q, unsigned x);
void sb_texel_buffer(const struct shader_build *sb, struct rslc_b_quals *q, unsigned x);
void sb_ssbo(const struct shader_build *sb, struct rslc_b_quals *q, unsigned x);
void sb_image(const struct shader_build *sb, struct rslc_b_quals *q, const char *format,
      unsigned x);

/* Expressions, shorter. */
uint32_t sb_f(const struct shader_build *sb, double v);
uint32_t sb_i(const struct shader_build *sb, int32_t v);
uint32_t sb_u(const struct shader_build *sb, uint32_t v);
uint32_t sb_ref(const struct shader_build *sb, const struct rslc_var *v);
uint32_t sb_bi(const struct shader_build *sb, const char *name);
uint32_t sb_swz(const struct shader_build *sb, uint32_t e, const char *s);
uint32_t sb_mem(const struct shader_build *sb, uint32_t e, uint32_t field);
uint32_t sb_idx(const struct shader_build *sb, uint32_t e, uint32_t idx);
uint32_t sb_bin(const struct shader_build *sb, enum rslc_b_binop op, uint32_t l, uint32_t r);
uint32_t sb_un(const struct shader_build *sb, enum rslc_b_unop op, uint32_t e);
/* type(args...), name(args...): n arguments follow */
uint32_t sb_con(const struct shader_build *sb, const struct rslc_type *type, unsigned n, ...);
uint32_t sb_call(const struct shader_build *sb, const char *name, unsigned n, ...);
/* l = r;  l op= r; as a statement of the open block */
bool sb_set(const struct shader_build *sb, uint32_t l, uint32_t r);
bool sb_op_set(const struct shader_build *sb, enum rslc_b_binop op, uint32_t l, uint32_t r);
bool sb_do(const struct shader_build *sb, uint32_t e);

#ifdef __cplusplus
}
#endif

#endif
