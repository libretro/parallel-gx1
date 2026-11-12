/*
** Copyright (c) 2013-2015 The Khronos Group Inc.
** SPDX-License-Identifier: MIT
*/

#include "Common/GL/GLExtensions/gl_common.h"

typedef GLsync(APIENTRYP PFNDOLFENCESYNCPROC)(GLenum condition, GLbitfield flags);
typedef void(APIENTRYP PFNDOLDELETESYNCPROC)(GLsync sync);
typedef GLenum(APIENTRYP PFNDOLCLIENTWAITSYNCPROC)(GLsync sync, GLbitfield flags, GLuint64 timeout);

extern PFNDOLCLIENTWAITSYNCPROC dolClientWaitSync;
extern PFNDOLDELETESYNCPROC dolDeleteSync;
extern PFNDOLFENCESYNCPROC dolFenceSync;

#define glClientWaitSync dolClientWaitSync
#define glDeleteSync dolDeleteSync
#define glFenceSync dolFenceSync
