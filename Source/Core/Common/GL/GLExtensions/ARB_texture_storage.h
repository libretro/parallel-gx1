/*
** Copyright (c) 2013-2015 The Khronos Group Inc.
** SPDX-License-Identifier: MIT
*/

#include "Common/GL/GLExtensions/gl_common.h"

#define GL_TEXTURE_IMMUTABLE_FORMAT 0x912F

typedef void(APIENTRYP PFNDOLTEXSTORAGE2DPROC)(GLenum target, GLsizei levels, GLenum internalformat,
                                               GLsizei width, GLsizei height);
typedef void(APIENTRYP PFNDOLTEXSTORAGE3DPROC)(GLenum target, GLsizei levels, GLenum internalformat,
                                               GLsizei width, GLsizei height, GLsizei depth);

extern PFNDOLTEXSTORAGE2DPROC dolTexStorage2D;
extern PFNDOLTEXSTORAGE3DPROC dolTexStorage3D;

#define glTexStorage2D dolTexStorage2D
#define glTexStorage3D dolTexStorage3D
