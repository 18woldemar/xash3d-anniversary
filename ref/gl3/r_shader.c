/*
r_shader.c - ref_gl3: GLSL programs and their constants
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
A shader file holds both stages, between #ifdef VERTEX_SHADER and #ifdef FRAGMENT_SHADER, and a program is
one file with a set of defines: the variants of a shader are its defines, not copies of it.
The constants are numbered registers, the way the renderer was first written: each program declares
"uniform vec4 vsc[N]" and "uniform vec4 psc[M]" and reads its values at fixed indices. The renderer writes
them into one shared set, as it would into a device's registers, and a program takes the set when it draws -
only if something changed since it last did, and only as many registers as it declares.
Attributes and samplers have fixed numbers for every program (r_local.h GL3_ATTR_*, GL3_UNIT_*).
*/

#include "r_local.h"

#define GL3_MAX_PROGRAMS 32

static gl3_program_t gl3_programs[GL3_MAX_PROGRAMS];
static int           gl3_num_programs;
static gl3_program_t *gl3_current_program;

static float  gl3_vsc[GL3_VS_CONSTANTS][4];
static float  gl3_psc[GL3_PS_CONSTANTS][4];
static uint   gl3_vsc_serial = 1, gl3_psc_serial = 1;

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
	const char *parts[4] =
	{
		"#version 330 core\n",
		stage == GL_VERTEX_SHADER ? "#define VERTEX_SHADER 1\n" : "#define FRAGMENT_SHADER 1\n",
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

void R_GL3UseProgram( gl3_program_t *p )
{
	if( p == gl3_current_program )
		return;

	glUseProgram( p ? p->prog : 0 );
	gl3_current_program = p;
}

// registers start..start+count of the vertex stage, as SetVertexShaderConstantF wrote them
void R_GL3SetVSConstants( int start, const float *data, int count )
{
	if( start < 0 || start + count > GL3_VS_CONSTANTS )
	{
		gEngfuncs.Con_Printf( S_ERROR "%s: registers %d..%d out of range\n", __func__, start, start + count - 1 );
		return;
	}

	if( !memcmp( gl3_vsc[start], data, count * sizeof( gl3_vsc[0] )))
		return;

	memcpy( gl3_vsc[start], data, count * sizeof( gl3_vsc[0] ));
	gl3_vsc_serial++;
}

void R_GL3SetPSConstants( int start, const float *data, int count )
{
	if( start < 0 || start + count > GL3_PS_CONSTANTS )
	{
		gEngfuncs.Con_Printf( S_ERROR "%s: registers %d..%d out of range\n", __func__, start, start + count - 1 );
		return;
	}

	if( !memcmp( gl3_psc[start], data, count * sizeof( gl3_psc[0] )))
		return;

	memcpy( gl3_psc[start], data, count * sizeof( gl3_psc[0] ));
	gl3_psc_serial++;
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
