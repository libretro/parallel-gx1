/*
** Copyright (c) 2013-2015 The Khronos Group Inc.
** SPDX-License-Identifier: MIT
*/

#include "Common/GL/GLExtensions/gl_common.h"

typedef void(APIENTRYP PFNDOLBINDFRAMEBUFFERPROC)(GLenum target, GLuint framebuffer);
typedef void(APIENTRYP PFNDOLBLITFRAMEBUFFERPROC)(GLint srcX0, GLint srcY0, GLint srcX1,
                                                  GLint srcY1, GLint dstX0, GLint dstY0,
                                                  GLint dstX1, GLint dstY1, GLbitfield mask,
                                                  GLenum filter);
typedef GLenum(APIENTRYP PFNDOLCHECKFRAMEBUFFERSTATUSPROC)(GLenum target);
typedef void(APIENTRYP PFNDOLDELETEFRAMEBUFFERSPROC)(GLsizei n, const GLuint* framebuffers);
typedef void(APIENTRYP PFNDOLFRAMEBUFFERTEXTURELAYERPROC)(GLenum target, GLenum attachment,
                                                          GLuint texture, GLint level, GLint layer);
typedef void(APIENTRYP PFNDOLGENFRAMEBUFFERSPROC)(GLsizei n, GLuint* framebuffers);

extern PFNDOLBINDFRAMEBUFFERPROC dolBindFramebuffer;
extern PFNDOLBLITFRAMEBUFFERPROC dolBlitFramebuffer;
extern PFNDOLCHECKFRAMEBUFFERSTATUSPROC dolCheckFramebufferStatus;
extern PFNDOLDELETEFRAMEBUFFERSPROC dolDeleteFramebuffers;
extern PFNDOLFRAMEBUFFERTEXTURELAYERPROC dolFramebufferTextureLayer;
extern PFNDOLGENFRAMEBUFFERSPROC dolGenFramebuffers;

#define glBindFramebuffer dolBindFramebuffer
#define glBlitFramebuffer dolBlitFramebuffer
#define glCheckFramebufferStatus dolCheckFramebufferStatus
#define glDeleteFramebuffers dolDeleteFramebuffers
#define glFramebufferTextureLayer dolFramebufferTextureLayer
#define glGenFramebuffers dolGenFramebuffers
