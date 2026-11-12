/*
** Copyright (c) 2013-2015 The Khronos Group Inc.
** SPDX-License-Identifier: MIT
*/

#include "Common/GL/GLExtensions/gl_common.h"

typedef void(APIENTRYP PFNDOLBINDBUFFERRANGEPROC)(GLenum target, GLuint index, GLuint buffer,
                                                  GLintptr offset, GLsizeiptr size);
typedef void(APIENTRYP PFNDOLBINDBUFFERBASEPROC)(GLenum target, GLuint index, GLuint buffer);
typedef GLuint(APIENTRYP PFNDOLGETUNIFORMBLOCKINDEXPROC)(GLuint program,
                                                         const GLchar* uniformBlockName);
typedef void(APIENTRYP PFNDOLUNIFORMBLOCKBINDINGPROC)(GLuint program, GLuint uniformBlockIndex,
                                                      GLuint uniformBlockBinding);

extern PFNDOLBINDBUFFERBASEPROC dolBindBufferBase;
extern PFNDOLBINDBUFFERRANGEPROC dolBindBufferRange;
extern PFNDOLGETUNIFORMBLOCKINDEXPROC dolGetUniformBlockIndex;
extern PFNDOLUNIFORMBLOCKBINDINGPROC dolUniformBlockBinding;

#define glBindBufferBase dolBindBufferBase
#define glBindBufferRange dolBindBufferRange
#define glGetUniformBlockIndex dolGetUniformBlockIndex
#define glUniformBlockBinding dolUniformBlockBinding
