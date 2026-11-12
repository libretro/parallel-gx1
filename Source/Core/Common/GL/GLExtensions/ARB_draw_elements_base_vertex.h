/*
** Copyright (c) 2013-2015 The Khronos Group Inc.
** SPDX-License-Identifier: MIT
*/

#include "Common/GL/GLExtensions/gl_common.h"

typedef void(APIENTRYP PFNDOLDRAWELEMENTSBASEVERTEXPROC)(GLenum mode, GLsizei count, GLenum type,
                                                         const void* indices, GLint basevertex);

extern PFNDOLDRAWELEMENTSBASEVERTEXPROC dolDrawElementsBaseVertex;

#define glDrawElementsBaseVertex dolDrawElementsBaseVertex
