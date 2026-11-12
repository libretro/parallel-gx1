/*
** Copyright (c) 2013-2015 The Khronos Group Inc.
** SPDX-License-Identifier: MIT
*/

#include "Common/GL/GLExtensions/gl_common.h"

typedef void(APIENTRYP PFNDOLBINDVERTEXARRAYPROC)(GLuint array);
typedef void(APIENTRYP PFNDOLDELETEVERTEXARRAYSPROC)(GLsizei n, const GLuint* arrays);
typedef void(APIENTRYP PFNDOLGENVERTEXARRAYSPROC)(GLsizei n, GLuint* arrays);

extern PFNDOLBINDVERTEXARRAYPROC dolBindVertexArray;
extern PFNDOLDELETEVERTEXARRAYSPROC dolDeleteVertexArrays;
extern PFNDOLGENVERTEXARRAYSPROC dolGenVertexArrays;

#define glBindVertexArray dolBindVertexArray
#define glDeleteVertexArrays dolDeleteVertexArrays
#define glGenVertexArrays dolGenVertexArrays
