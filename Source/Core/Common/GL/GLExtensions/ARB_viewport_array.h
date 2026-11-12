/*
** Copyright (c) 2013-2015 The Khronos Group Inc.
** SPDX-License-Identifier: MIT
*/

#include "Common/GL/GLExtensions/gl_common.h"

typedef void(APIENTRYP PFNDOLVIEWPORTINDEXEDFPROC)(GLuint index, GLfloat x, GLfloat y, GLfloat w,
                                                   GLfloat h);

extern PFNDOLVIEWPORTINDEXEDFPROC dolViewportIndexedf;

#define glViewportIndexedf dolViewportIndexedf
