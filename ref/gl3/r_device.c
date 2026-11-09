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
#include "r_gl.h"
#include "flashlight.glsl.h"

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
	R_GL3ClearBuffers( GL3_CLEAR_COLOR | GL3_CLEAR_DEPTH | GL3_CLEAR_STENCIL );
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
	R_GL3EndSceneTarget();

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
// (R_GL3EnsureSceneTarget), so that the frame's own off-screen passes - the flashlight's shadow map - come first.
void R_GL3BeginSceneTarget( void )
{
	if( !gl3_scene.enabled || gl3_scene.active )
		return;

	gl3_scene.pending = true;
}

void R_GL3EnsureSceneTarget( void )
{
	if( !gl3_scene.pending || gl3_scene.active )
		return;

	gl3_scene.pending = false;

	R_GL3DrawReset();
	glBindFramebuffer( GL_FRAMEBUFFER, gl3_scene.fbo );
	glViewport( 0, 0, gl3_scene.width, gl3_scene.height );
	glClearColor( 0.0f, 0.0f, 0.0f, 1.0f );
	R_GL3ClearBuffers( GL3_CLEAR_COLOR | GL3_CLEAR_DEPTH | GL3_CLEAR_STENCIL );
	gl3_scene.active = true;
}

/*
==================
R_GL3EndSceneTarget

GL_BackendEndFrame, and an error before it frees anything the frame uses: the target is resolved into the
scene texture, and drawing goes back to the back buffer.
==================
*/
void R_GL3EndSceneTarget( void )
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
qboolean R_GL3SceneTargetActive( void )
{
	return gl3_scene.active;
}

static struct
{
	GLuint fbo, depth;
} gl3_shadow;

// after a pass of its own (the shadow map), the frame draws where it drew before
static void R_GL3RestoreBackBuffer( void )
{
	glBindFramebuffer( GL_FRAMEBUFFER, gl3_scene.active ? gl3_scene.fbo : 0 );
}

void R_GL3BackendFreeShadowMap( void )
{
	if( gl3_shadow.fbo )
		glDeleteFramebuffers( 1, &gl3_shadow.fbo );
	if( gl3_shadow.depth )
		glDeleteRenderbuffers( 1, &gl3_shadow.depth );
	memset( &gl3_shadow, 0, sizeof( gl3_shadow ));
}

gl3_handle_t R_GL3BackendCreateShadowMap( int size )
{
	GLuint color = 0;
	GLenum status;

	glGenTextures( 1, &color );
	glBindTexture( GL_TEXTURE_2D, color );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_R32F, size, size, 0, GL_RED, GL_FLOAT, NULL );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0 );
	R_GL3ForgetBindings();

	glGenRenderbuffers( 1, &gl3_shadow.depth );
	glBindRenderbuffer( GL_RENDERBUFFER, gl3_shadow.depth );
	glRenderbufferStorageMultisample( GL_RENDERBUFFER, 0, GL_DEPTH_COMPONENT24, size, size );
	glBindRenderbuffer( GL_RENDERBUFFER, 0 );

	glGenFramebuffers( 1, &gl3_shadow.fbo );
	glBindFramebuffer( GL_FRAMEBUFFER, gl3_shadow.fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0 );
	glFramebufferRenderbuffer( GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, gl3_shadow.depth );
	status = glCheckFramebufferStatus( GL_FRAMEBUFFER );
	R_GL3RestoreBackBuffer();

	if( status != GL_FRAMEBUFFER_COMPLETE )
	{
		glDeleteTextures( 1, &color );
		R_GL3BackendFreeShadowMap();
		return 0;
	}

	return color;
}

void R_GL3BackendBeginShadowPass( int size )
{
	glBindFramebuffer( GL_FRAMEBUFFER, gl3_shadow.fbo );
	glViewport( 0, 0, size, size );
	glDepthRange( 0.0, 1.0 );
	glClearColor( 1.0f, 1.0f, 1.0f, 1.0f );
}

void R_GL3BackendEndShadowPass( void )
{
	R_GL3RestoreBackBuffer();
}

qboolean R_GL3BackendReadScreen( byte *rgba, int width, int height )
{
	glBindFramebuffer( GL_READ_FRAMEBUFFER, 0 );
	glReadPixels( 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba );
	return true;
}

// the scene texture's slot for the 2D part to draw first, once; 0 when this frame resolved no 3D part
int R_GL3TakeSceneTarget( void )
{
	const qboolean resolved = gl3_scene.resolved;

	gl3_scene.resolved = false;
	return resolved ? gl3_scene.scene : 0;
}

/*
==============================================================================

THE PROGRAMS

==============================================================================
*/
struct gl3_program_s
{
	char   name[32];
	GLuint prog;
	GLint  vsc, psc;             // the register arrays' locations, -1 when unused
	int    vsc_count, psc_count; // and their sizes
	uint   vsc_serial, psc_serial; // the shared registers' version it last took
};

#define GL3_MAX_PROGRAMS 32

static gl3_program_t gl3_programs[GL3_MAX_PROGRAMS];
static int           gl3_num_programs;
static gl3_program_t *gl3_current_program;

// what the shader files say about their resources, as GLSL 330 has it (the Vulkan backend has its own)
static const char gl3_glsl_prelude[] =
	"#define VS_REGISTERS( n ) uniform vec4 vsc[n];\n"
	"#define PS_REGISTERS( n ) uniform vec4 psc[n];\n"
	"#define SAMPLER( name, unit ) uniform sampler2D name;\n"
	"#define USAMPLER( name, unit ) uniform usampler2D name;\n"
	"#define ATTR( type, name, location ) in type name;\n"
	"#define VK_FIX_CLIP()\n";

static const char *gl3_attribs[GL3_ATTR_COUNT] =
{
	"a_position", "a_color", "a_texcoord", "a_lightcoord", "a_normal", "a_bone", "a_extra",
};

static const char *gl3_samplers[GL3_UNIT_COUNT] =
{
	"s_texture", "s_lightmap", "s_ripple", "s_cookie", "s_shadow", "s_gamma",
};

static GLuint R_GL3CompileStage( GLenum stage, const char *name, const char *defines, const char *source )
{
	const char *parts[5] =
	{
		"#version 330 core\n",
		stage == GL_VERTEX_SHADER ? "#define VERTEX_SHADER 1\n" : "#define FRAGMENT_SHADER 1\n",
		gl3_glsl_prelude,
		defines ? defines : "",
		source,
	};
	GLuint shader = glCreateShader( stage );
	GLint ok = 0;

	glShaderSource( shader, ARRAYSIZE( parts ), parts, NULL );
	glCompileShader( shader );
	glGetShaderiv( shader, GL_COMPILE_STATUS, &ok );

	if( !ok )
	{
		char log[4096];

		log[0] = '\0';
		glGetShaderInfoLog( shader, sizeof( log ), NULL, log );
		gEngfuncs.Con_Printf( S_ERROR "ref_gl3: %s (%s) %s shader:\n%s\n", name, defines ? defines : "",
			stage == GL_VERTEX_SHADER ? "vertex" : "fragment", log );
		glDeleteShader( shader );
		return 0;
	}

	return shader;
}

/*
==================
R_GL3CreateProgram

One shader file with its defines ("#define NAME 1\n" lines), linked; NULL with the compiler's words in the
console when it fails.
==================
*/
gl3_program_t *R_GL3CreateProgram( const char *name, const char *source, const char *defines )
{
	gl3_program_t *p;
	GLuint vs, fs;
	GLint ok = 0;

	if( gl3_num_programs >= GL3_MAX_PROGRAMS )
	{
		gEngfuncs.Con_Printf( S_ERROR "ref_gl3: more than %d programs\n", GL3_MAX_PROGRAMS );
		return NULL;
	}

	vs = R_GL3CompileStage( GL_VERTEX_SHADER, name, defines, source );
	fs = R_GL3CompileStage( GL_FRAGMENT_SHADER, name, defines, source );
	if( !vs || !fs )
	{
		if( vs ) glDeleteShader( vs );
		if( fs ) glDeleteShader( fs );
		return NULL;
	}

	p = &gl3_programs[gl3_num_programs];
	memset( p, 0, sizeof( *p ));
	p->prog = glCreateProgram();
	glAttachShader( p->prog, vs );
	glAttachShader( p->prog, fs );

	for( int i = 0; i < GL3_ATTR_COUNT; i++ )
		glBindAttribLocation( p->prog, i, gl3_attribs[i] );
	glBindFragDataLocation( p->prog, 0, "o_color" );

	glLinkProgram( p->prog );
	glDeleteShader( vs );
	glDeleteShader( fs );
	glGetProgramiv( p->prog, GL_LINK_STATUS, &ok );

	if( !ok )
	{
		char log[4096];

		log[0] = '\0';
		glGetProgramInfoLog( p->prog, sizeof( log ), NULL, log );
		gEngfuncs.Con_Printf( S_ERROR "ref_gl3: %s (%s) does not link:\n%s\n", name, defines ? defines : "", log );
		glDeleteProgram( p->prog );
		return NULL;
	}

	Q_strncpy( p->name, name, sizeof( p->name ));
	p->vsc = glGetUniformLocation( p->prog, "vsc" );
	p->psc = glGetUniformLocation( p->prog, "psc" );

	// how many registers each array holds: the linker trims an array to the highest index the code reads
	{
		GLint count = 0, size = 0;
		GLenum type;
		char uname[64];

		glGetProgramiv( p->prog, GL_ACTIVE_UNIFORMS, &count );
		for( GLuint i = 0; i < (GLuint)count; i++ )
		{
			pglGetActiveUniform( p->prog, i, sizeof( uname ), NULL, &size, &type, uname );
			if( !Q_strncmp( uname, "vsc", 3 ))
				p->vsc_count = size;
			else if( !Q_strncmp( uname, "psc", 3 ))
				p->psc_count = size;
		}
	}

	glUseProgram( p->prog );
	for( int i = 0; i < GL3_UNIT_COUNT; i++ )
	{
		GLint loc = glGetUniformLocation( p->prog, gl3_samplers[i] );

		if( loc >= 0 )
			glUniform1i( loc, i );
	}
	glUseProgram( gl3_current_program ? gl3_current_program->prog : 0 );

	gl3_num_programs++;
	return p;
}

// a program with the flashlight's cone: its filter goes in front of the shader file
gl3_program_t *R_GL3CreateSpotProgram( const char *name, const char *source, const char *defines )
{
	char prefix[8192];

	Q_snprintf( prefix, sizeof( prefix ), "%s#define SPOT 1\n%s", defines ? defines : "", flashlight_glsl );
	return R_GL3CreateProgram( name, source, prefix );
}

void R_GL3UseProgram( gl3_program_t *p )
{
	if( p == gl3_current_program )
		return;

	glUseProgram( p ? p->prog : 0 );
	gl3_current_program = p;
}

// right before a draw: the current program takes the registers that changed since it last drew
void R_GL3CommitConstants( void )
{
	gl3_program_t *p = gl3_current_program;

	if( !p )
		return;

	if( p->vsc >= 0 && p->vsc_serial != gl3_vsc_serial )
	{
		glUniform4fv( p->vsc, p->vsc_count, gl3_vsc[0] );
		p->vsc_serial = gl3_vsc_serial;
	}

	if( p->psc >= 0 && p->psc_serial != gl3_psc_serial )
	{
		glUniform4fv( p->psc, p->psc_count, gl3_psc[0] );
		p->psc_serial = gl3_psc_serial;
	}
}

void R_GL3ShutdownPrograms( void )
{
	R_GL3UseProgram( NULL );

	for( int i = 0; i < gl3_num_programs; i++ )
		glDeleteProgram( gl3_programs[i].prog );

	memset( gl3_programs, 0, sizeof( gl3_programs ));
	gl3_num_programs = 0;
}

/*
==============================================================================

THE TEXTURES

==============================================================================
*/
// what each texture unit holds: GL3_UNIT_*
static GLuint gl3_bound[GL3_UNIT_COUNT];
static int    gl3_active_unit;

static void R_GL3ActiveUnit( int unit )
{
	if( unit == gl3_active_unit )
		return;

	glActiveTexture( GL_TEXTURE0 + unit );
	gl3_active_unit = unit;
}

void R_GL3BackendBindTexture( int unit, gl3_handle_t texture )
{
	if( gl3_bound[unit] == texture )
		return;

	R_GL3ActiveUnit( unit );
	glBindTexture( GL_TEXTURE_2D, texture );
	gl3_bound[unit] = texture;
}

// a deleted texture name may come back from glGenTextures for another texture: no unit may still claim it
void R_GL3UnbindTexture( gl3_handle_t texture )
{
	if( !texture )
		return;

	for( int i = 0; i < GL3_UNIT_COUNT; i++ )
	{
		if( gl3_bound[i] == texture )
			R_GL3BackendBindTexture( i, 0 );
	}
}

// something bound textures behind the cache's back (a framebuffer's setup): every unit binds anew
void R_GL3ForgetBindings( void )
{
	for( int i = 0; i < GL3_UNIT_COUNT; i++ )
		gl3_bound[i] = (GLuint)-1;
	gl3_active_unit = -1;
}

gl3_handle_t R_GL3BackendCreateTexture( int width, int height, int levels )
{
	GLuint texture = 0;

	glGenTextures( 1, &texture );
	R_GL3BackendBindTexture( 0, texture );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, levels - 1 );

	// every level's storage first: the rows go in with glTexSubImage2D
	for( int level = 0; level < levels; level++ )
	{
		glTexImage2D( GL_TEXTURE_2D, level, GL_RGBA8, Q_max( 1, width >> level ), Q_max( 1, height >> level ),
			0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, NULL );
	}

	return texture;
}

void R_GL3BackendDeleteTexture( gl3_handle_t texture )
{
	GLuint name = texture;

	R_GL3UnbindTexture( texture );
	glDeleteTextures( 1, &name );
}

static struct
{
	gl3_handle_t texture;
	int          level, width, height;
	uint32_t    *rows;
	size_t       capacity;
} gl3_lock;

uint32_t *R_GL3BackendLockLevel( gl3_handle_t texture, int level, int width, int height )
{
	const size_t bytes = (size_t)width * height * sizeof( uint32_t );

	if( bytes > gl3_lock.capacity )
	{
		gl3_lock.rows = (uint32_t *)Mem_Realloc( r_temppool, gl3_lock.rows, bytes );
		gl3_lock.capacity = bytes;
	}

	gl3_lock.texture = texture;
	gl3_lock.level = level;
	gl3_lock.width = width;
	gl3_lock.height = height;
	return gl3_lock.rows;
}

// rows are 0xAARRGGBB: as GL_BGRA with GL_UNSIGNED_INT_8_8_8_8_REV that reads the same on either byte order
void R_GL3BackendUnlockLevel( void )
{
	R_GL3BackendBindTexture( 0, gl3_lock.texture );
	glTexSubImage2D( GL_TEXTURE_2D, gl3_lock.level, 0, 0, gl3_lock.width, gl3_lock.height, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, gl3_lock.rows );
}

// sampler state lives on the texture object: the texture is bound to this unit by now
void R_GL3BackendSetSampler( int unit, gl3_handle_t texture, int levels, qboolean filtered, qboolean clamp, int mipcap, int anisotropy )
{
	const GLenum mag = filtered ? GL_LINEAR : GL_NEAREST;
	const GLenum min = levels > 1 ? ( filtered ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST ) : mag;
	const GLenum wrap = clamp ? GL_CLAMP_TO_EDGE : GL_REPEAT;

	R_GL3ActiveUnit( unit );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mag );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, min );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap );
	glTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_MAX_LOD, (float)mipcap );

	if( R_GL3BackendMaxAnisotropy( ) > 0.0f )
		glTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, (float)Q_max( 1, anisotropy ));
}

// anisotropic filtering is an extension in 3.3, present in every driver that matters
float R_GL3BackendMaxAnisotropy( void )
{
	static float maximum = -1.0f;
	GLint count = 0;

	if( maximum >= 0.0f )
		return maximum;

	maximum = 0.0f;
	glGetIntegerv( GL_NUM_EXTENSIONS, &count );

	for( int i = 0; i < count; i++ )
	{
		const char *ext = (const char *)glGetStringi( GL_EXTENSIONS, i );

		if( ext && ( !Q_strcmp( ext, "GL_EXT_texture_filter_anisotropic" ) || !Q_strcmp( ext, "GL_ARB_texture_filter_anisotropic" )))
		{
			glGetFloatv( GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &maximum );
			break;
		}
	}

	return maximum;
}

/*
==============================================================================

STATES AND THE STREAM

==============================================================================
*/
void R_GL3BackendApplyStates( const gl3_states_t *w, const gl3_states_t *a )
{
	if( w->blend != a->blend )
	{
		if( w->blend ) glEnable( GL_BLEND );
		else glDisable( GL_BLEND );
	}
	if( w->src != a->src || w->dst != a->dst )
		glBlendFunc( w->src, w->dst );
	if( w->zwrite != a->zwrite )
		glDepthMask( w->zwrite ? GL_TRUE : GL_FALSE );
	if( w->ztest != a->ztest )
	{
		if( w->ztest ) glEnable( GL_DEPTH_TEST );
		else glDisable( GL_DEPTH_TEST );
	}
	if( w->zfunc != a->zfunc )
		glDepthFunc( w->zfunc );
	if( w->cull != a->cull )
	{
		if( w->cull == GL3_CULL_NONE )
			glDisable( GL_CULL_FACE );
		else
		{
			glEnable( GL_CULL_FACE );
			glCullFace( w->cull );
		}
	}
	if( w->depthbias != a->depthbias || w->slopebias != a->slopebias )
	{
		// the bias is kept in the depth buffer's own units (glPolygonOffset units / 2^24)
		if( w->depthbias == 0.0f && w->slopebias == 0.0f )
			glDisable( GL_POLYGON_OFFSET_FILL );
		else
		{
			glEnable( GL_POLYGON_OFFSET_FILL );
			glPolygonOffset( w->slopebias, w->depthbias * 16777216.0f );
		}
	}
}

void R_GL3BackendClearBuffers( uint mask )
{
	glDepthMask( GL_TRUE );
	glClearDepth( 1.0 );
	glClearStencil( 0 );
	glClear( mask );
}

void R_GL3BackendViewport( int x, int y, int width, int height )
{
	glViewport( x, y, width, height );
}

void R_GL3BackendDepthRange( float minz, float maxz )
{
	glDepthRange( minz, maxz );
}

static GLuint gl3_draw_vao, gl3_draw_vbo;

void R_GL3BackendInitDraw( void )
{
	glGenVertexArrays( 1, &gl3_draw_vao );
	glGenBuffers( 1, &gl3_draw_vbo );
	glBindVertexArray( gl3_draw_vao );
	glBindBuffer( GL_ARRAY_BUFFER, gl3_draw_vbo );
	glEnableVertexAttribArray( GL3_ATTR_POSITION );
	glVertexAttribPointer( GL3_ATTR_POSITION, 3, GL_FLOAT, GL_FALSE, sizeof( gl3_vertex_t ), (void *)offsetof( gl3_vertex_t, position ));
	glEnableVertexAttribArray( GL3_ATTR_COLOR );
	glVertexAttribPointer( GL3_ATTR_COLOR, GL_BGRA, GL_UNSIGNED_BYTE, GL_TRUE, sizeof( gl3_vertex_t ), (void *)offsetof( gl3_vertex_t, color ));
	glEnableVertexAttribArray( GL3_ATTR_TEXCOORD );
	glVertexAttribPointer( GL3_ATTR_TEXCOORD, 2, GL_FLOAT, GL_FALSE, sizeof( gl3_vertex_t ), (void *)offsetof( gl3_vertex_t, texcoord ));
}

void R_GL3BackendShutdownDraw( void )
{
	if( gl3_draw_vbo )
		glDeleteBuffers( 1, &gl3_draw_vbo );
	if( gl3_draw_vao )
		glDeleteVertexArrays( 1, &gl3_draw_vao );
	gl3_draw_vbo = gl3_draw_vao = 0;
}

void R_GL3BackendUseDrawLayout( void )
{
	glBindVertexArray( gl3_draw_vao );
	glBindBuffer( GL_ARRAY_BUFFER, gl3_draw_vbo );
}

// count vertices of the given primitive from data, through the stream buffer
void R_GL3BackendDrawStream( int primitive, const gl3_vertex_t *vertices, int count )
{
	const GLsizeiptr bytes = count * sizeof( *vertices );

	// a new store each time: the driver hands out fresh memory instead of waiting for the last draw
	glBufferData( GL_ARRAY_BUFFER, bytes, NULL, GL_STREAM_DRAW );
	glBufferSubData( GL_ARRAY_BUFFER, 0, bytes, vertices );
	R_GL3CommitConstants();
	glDrawArrays( primitive, 0, count );
}

/*
==============================================================================

LIGHTMAPS AND THE WORLD

==============================================================================
*/
gl3_handle_t R_GL3BackendCreateLightmap( int size )
{
	GLuint tex = 0;

	glGenTextures( 1, &tex );
	glBindTexture( GL_TEXTURE_2D, tex );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGB8, size, size, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, NULL );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0 );
	R_GL3ForgetBindings();
	return tex;
}

void R_GL3BackendWriteRows( gl3_handle_t texture, const uint32_t *luxels, int size, const gl3_rect_t *rects, int numrects, qboolean whole )
{
	glBindTexture( GL_TEXTURE_2D, texture );
	glPixelStorei( GL_UNPACK_ROW_LENGTH, size );

	if( whole )
	{
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, size, size, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, luxels );
	}
	else
	{
		for( int i = 0; i < numrects; i++ )
		{
			const gl3_rect_t *r = &rects[i];

			glTexSubImage2D( GL_TEXTURE_2D, 0, r->x, r->y, r->w, r->h, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, luxels + r->y * size + r->x );
		}
	}

	glPixelStorei( GL_UNPACK_ROW_LENGTH, 0 );
	R_GL3ForgetBindings();
}

static GLuint gl3_world_vao, gl3_world_vbo, gl3_world_ibo;
static GLuint gl3_world_dyn_vao, gl3_world_dyn_vbo;

static void R_GL3WorldVertexLayout( GLuint vbo )
{
	const GLsizei stride = sizeof( float ) * VERTEXSIZE;

	glBindBuffer( GL_ARRAY_BUFFER, vbo );
	glEnableVertexAttribArray( GL3_ATTR_POSITION );
	glVertexAttribPointer( GL3_ATTR_POSITION, 3, GL_FLOAT, GL_FALSE, stride, (void *)0 );
	glEnableVertexAttribArray( GL3_ATTR_TEXCOORD );
	glVertexAttribPointer( GL3_ATTR_TEXCOORD, 2, GL_FLOAT, GL_FALSE, stride, (void *)( 3 * sizeof( float )));
	glEnableVertexAttribArray( GL3_ATTR_LIGHTCOORD );
	glVertexAttribPointer( GL3_ATTR_LIGHTCOORD, 2, GL_FLOAT, GL_FALSE, stride, (void *)( 5 * sizeof( float )));
}

void R_GL3BackendInitWorld( void )
{
	glGenVertexArrays( 1, &gl3_world_vao );
	glGenVertexArrays( 1, &gl3_world_dyn_vao );
	glGenBuffers( 1, &gl3_world_vbo );
	glGenBuffers( 1, &gl3_world_ibo );
	glGenBuffers( 1, &gl3_world_dyn_vbo );

	glBindVertexArray( gl3_world_vao );
	glBindBuffer( GL_ELEMENT_ARRAY_BUFFER, gl3_world_ibo );
	R_GL3WorldVertexLayout( gl3_world_vbo );
	glBindVertexArray( gl3_world_dyn_vao );
	R_GL3WorldVertexLayout( gl3_world_dyn_vbo );
	glBindVertexArray( 0 );
}

void R_GL3BackendShutdownWorld( void )
{
	glBindVertexArray( 0 );
	glDeleteVertexArrays( 1, &gl3_world_vao );
	glDeleteVertexArrays( 1, &gl3_world_dyn_vao );
	glDeleteBuffers( 1, &gl3_world_vbo );
	glDeleteBuffers( 1, &gl3_world_ibo );
	glDeleteBuffers( 1, &gl3_world_dyn_vbo );
	gl3_world_vao = gl3_world_dyn_vao = gl3_world_vbo = gl3_world_ibo = gl3_world_dyn_vbo = 0;
}

void R_GL3BackendWorldVerticesBegin( size_t bytes )
{
	glBindBuffer( GL_ARRAY_BUFFER, gl3_world_vbo );
	glBufferData( GL_ARRAY_BUFFER, bytes, NULL, GL_STATIC_DRAW );
}

void R_GL3BackendWorldVerticesWrite( size_t offset, const void *data, size_t bytes )
{
	glBufferSubData( GL_ARRAY_BUFFER, offset, bytes, data );
}

void R_GL3BackendWorldVerticesEnd( void )
{
}

void R_GL3BackendWorldStream( const uint32_t *indices, int numindices, const void *dynverts, size_t dynbytes )
{
	// a new store each time: the driver hands out fresh memory instead of waiting for the last view
	glBindVertexArray( gl3_world_vao );
	glBufferData( GL_ELEMENT_ARRAY_BUFFER, numindices * sizeof( *indices ), indices, GL_STREAM_DRAW );
	glBindBuffer( GL_ARRAY_BUFFER, gl3_world_dyn_vbo );
	glBufferData( GL_ARRAY_BUFFER, dynbytes, dynverts, GL_STREAM_DRAW );
}

void R_GL3BackendUseWorldLayout( qboolean dynamic )
{
	glBindVertexArray( dynamic ? gl3_world_dyn_vao : gl3_world_vao );
}

void R_GL3BackendDrawWorldIndexed( int first, int count )
{
	glDrawElements( GL_TRIANGLES, count, GL_UNSIGNED_INT, (void *)( first * sizeof( uint32_t )));
}

void R_GL3BackendDrawWorldArrays( int first, int count )
{
	glDrawArrays( GL_TRIANGLES, first, count );
}

/*
==============================================================================

STUDIO MESHES

==============================================================================
*/
struct gl3_mesh_s
{
	GLuint vao, vbo, ibo;
};

gl3_mesh_t *R_GL3BackendCreateMesh( poolhandle_t pool, const gl3_studiovertex_t *vertices, int numvertices, const word *indices, int numindices )
{
	gl3_mesh_t *mesh = (gl3_mesh_t *)Mem_Calloc( pool, sizeof( *mesh ));

	glGenVertexArrays( 1, &mesh->vao );
	glGenBuffers( 1, &mesh->vbo );
	glGenBuffers( 1, &mesh->ibo );
	glBindVertexArray( mesh->vao );
	glBindBuffer( GL_ARRAY_BUFFER, mesh->vbo );
	glBufferData( GL_ARRAY_BUFFER, numvertices * sizeof( *vertices ), vertices, GL_STATIC_DRAW );
	glBindBuffer( GL_ELEMENT_ARRAY_BUFFER, mesh->ibo );
	glBufferData( GL_ELEMENT_ARRAY_BUFFER, numindices * sizeof( *indices ), indices, GL_STATIC_DRAW );
	glEnableVertexAttribArray( GL3_ATTR_POSITION );
	glVertexAttribPointer( GL3_ATTR_POSITION, 3, GL_FLOAT, GL_FALSE, sizeof( *vertices ), (void *)offsetof( gl3_studiovertex_t, position ));
	glEnableVertexAttribArray( GL3_ATTR_NORMAL );
	glVertexAttribPointer( GL3_ATTR_NORMAL, 3, GL_FLOAT, GL_FALSE, sizeof( *vertices ), (void *)offsetof( gl3_studiovertex_t, normal ));
	glEnableVertexAttribArray( GL3_ATTR_TEXCOORD );
	glVertexAttribPointer( GL3_ATTR_TEXCOORD, 2, GL_FLOAT, GL_FALSE, sizeof( *vertices ), (void *)offsetof( gl3_studiovertex_t, st ));
	glEnableVertexAttribArray( GL3_ATTR_EXTRA );
	glVertexAttribPointer( GL3_ATTR_EXTRA, 1, GL_FLOAT, GL_FALSE, sizeof( *vertices ), (void *)offsetof( gl3_studiovertex_t, bone ));
	glBindVertexArray( 0 );
	return mesh;
}

void R_GL3BackendDeleteMesh( gl3_mesh_t *mesh )
{
	glDeleteVertexArrays( 1, &mesh->vao );
	glDeleteBuffers( 1, &mesh->vbo );
	glDeleteBuffers( 1, &mesh->ibo );
	Mem_Free( mesh );
}

void R_GL3BackendUseMesh( gl3_mesh_t *mesh )
{
	glBindVertexArray( mesh ? mesh->vao : 0 );
}

void R_GL3BackendDrawMesh( int first, int count )
{
	glDrawElements( GL_TRIANGLES, count, GL_UNSIGNED_SHORT, (void *)( first * sizeof( word )));
}

gl3_handle_t R_GL3BackendCreateGamma( int width, int height )
{
	GLuint texture;

	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_R16UI, width, height, 0, GL_RED_INTEGER, GL_UNSIGNED_SHORT, NULL );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0 );
	R_GL3ForgetBindings();
	return texture;
}

void R_GL3BackendWriteGamma( gl3_handle_t texture, const uint16_t *rows, int width, int height )
{
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RED_INTEGER, GL_UNSIGNED_SHORT, rows );
	R_GL3ForgetBindings();
}
