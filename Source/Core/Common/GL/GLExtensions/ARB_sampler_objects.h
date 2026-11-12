/*
** Copyright (c) 2013-2015 The Khronos Group Inc.
** SPDX-License-Identifier: MIT
*/

#include "Common/GL/GLExtensions/gl_common.h"

typedef void(APIENTRYP PFNDOLGENSAMPLERSPROC)(GLsizei count, GLuint* samplers);
typedef void(APIENTRYP PFNDOLDELETESAMPLERSPROC)(GLsizei count, const GLuint* samplers);
typedef void(APIENTRYP PFNDOLBINDSAMPLERPROC)(GLuint unit, GLuint sampler);
typedef void(APIENTRYP PFNDOLSAMPLERPARAMETERIPROC)(GLuint sampler, GLenum pname, GLint param);
typedef void(APIENTRYP PFNDOLSAMPLERPARAMETERFPROC)(GLuint sampler, GLenum pname, GLfloat param);

extern PFNDOLBINDSAMPLERPROC dolBindSampler;
extern PFNDOLDELETESAMPLERSPROC dolDeleteSamplers;
extern PFNDOLGENSAMPLERSPROC dolGenSamplers;
extern PFNDOLSAMPLERPARAMETERFPROC dolSamplerParameterf;
extern PFNDOLSAMPLERPARAMETERIPROC dolSamplerParameteri;

#define glBindSampler dolBindSampler
#define glDeleteSamplers dolDeleteSamplers
#define glGenSamplers dolGenSamplers
#define glSamplerParameterf dolSamplerParameterf
#define glSamplerParameteri dolSamplerParameteri
