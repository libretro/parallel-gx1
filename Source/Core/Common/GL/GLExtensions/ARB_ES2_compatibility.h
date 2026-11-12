/*
** Copyright (c) 2013-2015 The Khronos Group Inc.
** SPDX-License-Identifier: MIT
*/

#include "Common/GL/GLExtensions/gl_common.h"

typedef void(APIENTRYP PFNDOLDEPTHRANGEFPROC)(GLfloat n, GLfloat f);
typedef void(APIENTRYP PFNDOLCLEARDEPTHFPROC)(GLfloat d);

extern PFNDOLCLEARDEPTHFPROC dolClearDepthf;
extern PFNDOLDEPTHRANGEFPROC dolDepthRangef;

#define glClearDepthf dolClearDepthf
#define glDepthRangef dolDepthRangef
