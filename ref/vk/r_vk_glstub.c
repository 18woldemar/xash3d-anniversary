/*
r_vk_glstub.c - ref_vk: GL entry points that do nothing
Copyright (C) 2026 18woldemar

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

/*
The shared core still calls GL in the files whose drawing has not moved to the backend yet. Here those calls
do nothing and answer as a working driver would, so the core starts up and walks its frames. This file shrinks
stage by stage and is deleted when the last GL call has left the core.
*/

#include "r_local.h"
#include "r_gl.h"
#include "r_vk.h"

// ponytail: one no-op under every signature, fine on the calling conventions we build for; temporary, see above
static intptr_t R_VKGLNothing( void )
{
	return 1; // what the Create calls need: a name that is not 0
}

#define GL3_DEFINE( type, name ) type p##name = (type)(void *)R_VKGLNothing;
GL3_FUNCTIONS( GL3_DEFINE )
#undef GL3_DEFINE

// far above the backend's own texture numbers
static GLuint vk_gl_names = 1u << 20;

static void APIENTRY R_VKGLGen( GLsizei n, GLuint *names )
{
	while( n-- > 0 )
		*names++ = ++vk_gl_names;
}

static void APIENTRY R_VKGLGetObject( GLuint object, GLenum pname, GLint *value )
{
	*value = pname != GL_ACTIVE_UNIFORMS; // compiled and linked, and no uniforms to list
}

static void APIENTRY R_VKGLGetInteger( GLenum pname, GLint *value )
{
	*value = pname == GL_NUM_EXTENSIONS ? 0 : 16384;
}

static void APIENTRY R_VKGLGetFloat( GLenum pname, GLfloat *value )
{
	*value = 1.0f;
}

static const GLubyte *APIENTRY R_VKGLGetString( GLenum name )
{
	return (const GLubyte *)"";
}

static GLenum APIENTRY R_VKGLGetError( void )
{
	return GL_NO_ERROR;
}

static GLenum APIENTRY R_VKGLFramebufferStatus( GLenum target )
{
	return GL_FRAMEBUFFER_COMPLETE;
}

void R_VKInstallGLStubs( void )
{
	pglGenTextures = R_VKGLGen;
	pglGenBuffers = R_VKGLGen;
	pglGenVertexArrays = R_VKGLGen;
	pglGenFramebuffers = R_VKGLGen;
	pglGenRenderbuffers = R_VKGLGen;
	pglGetShaderiv = R_VKGLGetObject;
	pglGetProgramiv = R_VKGLGetObject;
	pglGetIntegerv = R_VKGLGetInteger;
	pglGetFloatv = R_VKGLGetFloat;
	pglGetString = R_VKGLGetString;
	pglGetError = R_VKGLGetError;
	pglCheckFramebufferStatus = R_VKGLFramebufferStatus;
}
