/*
r_device.c - ref_gl3: the context, clear and present, and the scene target
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

#include "r_local.h"

#define GL3_DEFINE( type, name ) type p##name;
GL3_FUNCTIONS( GL3_DEFINE )
#undef GL3_DEFINE

/*
==================
R_GL3LoadFunctions

Every entry point the renderer uses, through the engine. A context without one of them is not a 3.3 core
context, and the renderer does not start on it.
==================
*/
qboolean R_GL3LoadFunctions( void )
{
	qboolean ok = true;

#define GL3_LOAD( type, name ) \
	if(( p##name = (type)gEngfuncs.GL_GetProcAddress( #name )) == NULL ) \
	{ \
		gEngfuncs.Con_Printf( S_ERROR "ref_gl3: no %s\n", #name ); \
		ok = false; \
	}
	GL3_FUNCTIONS( GL3_LOAD )
#undef GL3_LOAD

	return ok;
}

/*
==================
R_GL3CreateDevice

The engine makes the window and the 3.3 core context GL_SetupAttributes asked for (r_main.c); this takes the
entry points and sets what never changes. The default framebuffer has no samples of its own: multisampling,
like the software style's 16 bits, lives in the scene target below.
==================
*/
qboolean R_GL3CreateDevice( int width, int height )
{
	GLint major = 0, minor = 0;

	if( !R_GL3LoadFunctions( ))
		return false;

	glGetIntegerv( GL_MAJOR_VERSION, &major );
	glGetIntegerv( GL_MINOR_VERSION, &minor );
	if( major < 3 || ( major == 3 && minor < 3 ))
	{
		gEngfuncs.Con_Printf( S_ERROR "ref_gl3: OpenGL %d.%d, 3.3 is needed\n", major, minor );
		return false;
	}

	gEngfuncs.Con_Printf( "ref_gl3: %s, %s, OpenGL %s, GLSL %s, %dx%d\n", glGetString( GL_VENDOR ), glGetString( GL_RENDERER ),
		glGetString( GL_VERSION ), glGetString( GL_SHADING_LANGUAGE_VERSION ), width, height );

	// texel rows are uploaded one by one and read back for screenshots at any width
	glPixelStorei( GL_UNPACK_ALIGNMENT, 1 );
	glPixelStorei( GL_PACK_ALIGNMENT, 1 );
	glFrontFace( GL_CCW );
	return true;
}

void R_GL3DestroyDevice( void )
{
}

void R_GL3Clear( byte r, byte g, byte b )
{
	glClearColor( r / 255.0f, g / 255.0f, b / 255.0f, 1.0f );
	glClearDepth( 1.0 );
	glDepthMask( GL_TRUE );
	glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT );
}

void R_GL3Present( void )
{
	gEngfuncs.GL_SwapBuffers();
}

/*
==============================================================================

THE SCENE TARGET

The 3D part of a frame (GL_BackendStartFrame to GL_BackendEndFrame) can go through a framebuffer of its own:
multisampled for r_msaa, or 16 bit (RGB565, the span renderer's viddef_t) for the software style. At the end
of the 3D part it is resolved into the scene texture, which the 2D part draws over the whole back buffer
first. Without either, the 3D part draws straight into the back buffer.

==============================================================================
*/
static struct
{
	int      samples;  // what the target is made for: 0, 2, 4 or 8
	qboolean enabled;  // the target exists
	qboolean soft;     // the 16 bit one
	GLuint   fbo;      // where the scene draws
	GLuint   color, depth; // its renderbuffers
	GLuint   resolve_fbo;  // the scene texture's framebuffer
	GLuint   scene_gl;
	int      scene;    // the scene texture's slot, which owns it
	int      width, height;
	qboolean active;   // the 3D part draws into the target now
	qboolean pending;  // the engine opened the 3D part; the target binds at its first draw
	qboolean resolved; // the scene texture holds this frame's 3D part
} gl3_scene;

static void R_GL3FreeSceneTarget( void )
{
	R_GL3EndTiledScene();

	if( gl3_scene.scene )
		R_GL3FreeTexture( gl3_scene.scene );
	if( gl3_scene.fbo )
		glDeleteFramebuffers( 1, &gl3_scene.fbo );
	if( gl3_scene.resolve_fbo )
		glDeleteFramebuffers( 1, &gl3_scene.resolve_fbo );
	if( gl3_scene.color )
		glDeleteRenderbuffers( 1, &gl3_scene.color );
	if( gl3_scene.depth )
		glDeleteRenderbuffers( 1, &gl3_scene.depth );

	memset( &gl3_scene, 0, sizeof( gl3_scene ));
}

/*
==================
R_GL3SetMSAA

0 samples leaves the 3D part in the back buffer, unless the style is the software one, whose 16 bit target
it needs anyway. Only between frames.
==================
*/
qboolean R_GL3SetMSAA( int samples )
{
	const int width = gpGlobals->width, height = gpGlobals->height;
	const qboolean soft = R_GL3Style( ) == GL3_STYLE_SOFTWARE;
	GLint max_samples = 0;
	GLenum format = GL_RGB8; // no alpha: the 2D part draws the scene with the alpha test on

	R_GL3FreeSceneTarget();

	if( soft )
	{
		// the span renderer had no multisampling, and its output was 16 bit
		samples = 0;
		format = GL_RGB565;
	}
	else
	{
		if( samples != 2 && samples != 4 && samples != 8 )
			return samples == 0;

		glGetIntegerv( GL_MAX_SAMPLES, &max_samples );
		samples = Q_min( samples, max_samples );
	}

	glGenRenderbuffers( 1, &gl3_scene.color );
	glBindRenderbuffer( GL_RENDERBUFFER, gl3_scene.color );
	glRenderbufferStorageMultisample( GL_RENDERBUFFER, samples, format, width, height );

	glGenRenderbuffers( 1, &gl3_scene.depth );
	glBindRenderbuffer( GL_RENDERBUFFER, gl3_scene.depth );
	glRenderbufferStorageMultisample( GL_RENDERBUFFER, samples, GL_DEPTH24_STENCIL8, width, height );
	glBindRenderbuffer( GL_RENDERBUFFER, 0 );

	glGenFramebuffers( 1, &gl3_scene.fbo );
	glBindFramebuffer( GL_FRAMEBUFFER, gl3_scene.fbo );
	glFramebufferRenderbuffer( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, gl3_scene.color );
	glFramebufferRenderbuffer( GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, gl3_scene.depth );

	if( glCheckFramebufferStatus( GL_FRAMEBUFFER ) != GL_FRAMEBUFFER_COMPLETE )
	{
		glBindFramebuffer( GL_FRAMEBUFFER, 0 );
		gEngfuncs.Con_Printf( S_ERROR "ref_gl3: no %s scene target\n", soft ? "16 bit" : "multisampled" );
		R_GL3FreeSceneTarget();
		return false;
	}

	// the texture the 2D part draws; the slot owns the texture object
	glGenTextures( 1, &gl3_scene.scene_gl );
	glBindTexture( GL_TEXTURE_2D, gl3_scene.scene_gl );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGB8, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0 );
	glBindTexture( GL_TEXTURE_2D, 0 );
	R_GL3ForgetBindings(); // the unit cache does not know what was just bound

	glGenFramebuffers( 1, &gl3_scene.resolve_fbo );
	glBindFramebuffer( GL_FRAMEBUFFER, gl3_scene.resolve_fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gl3_scene.scene_gl, 0 );
	glBindFramebuffer( GL_FRAMEBUFFER, 0 );

	gl3_scene.scene = R_GL3WrapTexture( "*scene", gl3_scene.scene_gl, width, height, TF_NEAREST | TF_CLAMP | TF_NOMIPMAP );
	gl3_scene.samples = samples;
	gl3_scene.soft = soft;
	gl3_scene.width = width;
	gl3_scene.height = height;
	gl3_scene.enabled = true;

	if( soft )
		gEngfuncs.Con_Printf( "ref_gl3: the software style's 16 bit scene target, %dx%d\n", width, height );
	else gEngfuncs.Con_Printf( "ref_gl3: %dx multisampled scene target, %dx%d\n", samples, width, height );
	return true;
}

// GL_BackendStartFrame: the engine's 3D views follow. The target binds at the first draw of the scene
// (R_GL3EnsureTiledScene), so that the frame's own off-screen passes - the flashlight's shadow map - come first.
void R_GL3BeginTiledScene( void )
{
	if( !gl3_scene.enabled || gl3_scene.active )
		return;

	gl3_scene.pending = true;
}

void R_GL3WaitForGPU( void )
{
	// OpenGL orders a frame's passes by itself
}

void R_GL3EnsureTiledScene( void )
{
	if( !gl3_scene.pending || gl3_scene.active )
		return;

	gl3_scene.pending = false;

	R_GL3DrawReset();
	glBindFramebuffer( GL_FRAMEBUFFER, gl3_scene.fbo );
	glViewport( 0, 0, gl3_scene.width, gl3_scene.height );
	glClearColor( 0.0f, 0.0f, 0.0f, 1.0f );
	glClearDepth( 1.0 );
	glDepthMask( GL_TRUE );
	glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT );
	gl3_scene.active = true;
}

/*
==================
R_GL3EndTiledScene

GL_BackendEndFrame, and an error before it frees anything the frame uses: the target is resolved into the
scene texture, and drawing goes back to the back buffer.
==================
*/
void R_GL3EndTiledScene( void )
{
	gl3_scene.pending = false;

	if( !gl3_scene.active )
		return;

	R_GL3DrawReset();
	gl3_scene.active = false;

	glBindFramebuffer( GL_READ_FRAMEBUFFER, gl3_scene.fbo );
	glBindFramebuffer( GL_DRAW_FRAMEBUFFER, gl3_scene.resolve_fbo );
	glBlitFramebuffer( 0, 0, gl3_scene.width, gl3_scene.height, 0, 0, gl3_scene.width, gl3_scene.height, GL_COLOR_BUFFER_BIT, GL_NEAREST );
	glBindFramebuffer( GL_FRAMEBUFFER, 0 );
	gl3_scene.resolved = true;
}

// true while the 3D part draws into the target: an off-screen pass must not bind its own framebuffer then
qboolean R_GL3TiledSceneActive( void )
{
	return gl3_scene.active;
}

// true when the frame goes through the target at all
qboolean R_GL3TiledSceneWanted( void )
{
	return gl3_scene.enabled;
}

// after a pass of its own (the shadow map), the frame draws where it drew before
void R_GL3RestoreBackBuffer( void )
{
	glBindFramebuffer( GL_FRAMEBUFFER, gl3_scene.active ? gl3_scene.fbo : 0 );
}

// the scene texture's slot for the 2D part to draw first, once; 0 when this frame resolved no 3D part
int R_GL3TakeTiledScene( void )
{
	const qboolean resolved = gl3_scene.resolved;

	gl3_scene.resolved = false;
	return resolved ? gl3_scene.scene : 0;
}
