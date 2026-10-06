/*
r_draw.c - ref_gl3: 2D drawing, render modes and the immediate-mode triangle API
Copyright (C) 2026 18woldemar
Render mode and triangle API semantics follow ref/gl/gl_backend.c, gl_draw.c and gl_triapi.c,
Copyright (C) 2010 Uncle Mike

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
The engine draws 2D one quad at a time. Quads and triangle API primitives become triangles in one batch that
is drawn when the texture or a render state changes, and at the end of the frame. The batch keeps the states
it was made with; state calls only record what the next geometry wants.
draw.glsl multiplies the texture by the vertex colour, and untextured draws use *white.
*/

#include "r_local.h"
#include "draw.glsl.h"

typedef struct
{
	float    position[3];
	uint32_t color; // 0xAARRGGBB, read as GL_BGRA
	float    texcoord[2];
} gl3_vertex_t;

#define GL3_MAX_BATCH 8192 // vertices, a multiple of 3
#define GL3_MAX_PRIM  4096 // vertices between Begin and End

static gl3_program_t *gl3_draw_prog;
static gl3_program_t *gl3_warpscreen_prog;
static gl3_program_t *gl3_warpwave_prog;
static GLuint gl3_draw_vao, gl3_draw_vbo;
static int gl3_draw_warpscreen; // the scene quad under water: 0 none, 1 the software grid, 2 our waves
static qboolean gl3_draw_program; // the draw program, its buffers and its registers are set
static float    gl3_transform[16]; // rows of the object-to-clip matrix
static float    gl3_draw_eyeplane[4];
static float    gl3_draw_fog[4];   // colour, density (0: no fog)

static gl3_vertex_t gl3_batch[GL3_MAX_BATCH];
static int         gl3_batch_count;
static int         gl3_batch_texture;
static gl3_states_t gl3_applied;
gl3_states_t        gl3_wanted;

static gl3_vertex_t gl3_prim[GL3_MAX_PRIM];
static int         gl3_prim_count;
static int         gl3_prim_mode = -1;
static qboolean    gl3_prim_overflow;

static uint32_t gl3_color = 0xffffffff;
static float    gl3_texcoord[2];
static int      gl3_texture; // the triangle API's texture (GL_Bind)

static qboolean gl3_2d;
static float    gl3_2d_offset[2];

static int      gl3_draws, gl3_vertices; // this frame

#define GL3_ARGB( a, r, g, b ) (((uint32_t)( a ) << 24 ) | ((uint32_t)( r ) << 16 ) | ((uint32_t)( g ) << 8 ) | (uint32_t)( b ))

static byte R_GL3ColorByte( float f )
{
	return (byte)( bound( 0.0f, f, 1.0f ) * 255.0f + 0.5f );
}

void R_GL3ApplyStates( void )
{
	const gl3_states_t *w = &gl3_wanted;
	gl3_states_t *a = &gl3_applied;

	if( w->blend != a->blend )
	{
		if( w->blend ) glEnable( GL_BLEND );
		else glDisable( GL_BLEND );
	}
	if( w->src != a->src || w->dst != a->dst )
		glBlendFunc( w->src, w->dst );
	if( w->alphatest != a->alphatest || w->alpharef != a->alpharef )
	{
		const float ref[4] = { w->alphatest ? (float)w->alpharef : -1.0f, 0.0f, 0.0f, 0.0f };

		R_GL3SetPSConstants( GL3_PS_ALPHAREF, ref, 1 );
	}
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

	*a = *w;
}

static void R_GL3UseDrawProgram( void )
{
	if( gl3_draw_program )
		return;

	R_GL3UseProgram( gl3_draw_warpscreen == 2 ? gl3_warpwave_prog :
		gl3_draw_warpscreen == 1 ? gl3_warpscreen_prog : gl3_draw_prog );
	glBindVertexArray( gl3_draw_vao );
	glBindBuffer( GL_ARRAY_BUFFER, gl3_draw_vbo );
	R_GL3SetVSConstants( 0, gl3_transform, 4 );
	R_GL3SetVSConstants( 4, gl3_draw_eyeplane, 1 );
	R_GL3SetPSConstants( 0, gl3_draw_fog, 1 );
	gl3_draw_program = true;
}

// count vertices of the given primitive from data, through the stream buffer
static void R_GL3DrawStream( GLenum mode, const gl3_vertex_t *data, int count )
{
	const GLsizeiptr bytes = count * sizeof( *data );

	// a new store each time: the driver hands out fresh memory instead of waiting for the last draw
	glBufferData( GL_ARRAY_BUFFER, bytes, NULL, GL_STREAM_DRAW );
	glBufferSubData( GL_ARRAY_BUFFER, 0, bytes, data );
	R_GL3CommitConstants();
	glDrawArrays( mode, 0, count );
}

static void R_GL3Flush( void )
{
	if( gl3_batch_count == 0 )
		return;

	R_GL3UseDrawProgram();
	R_GL3BindTexture( gl3_batch_texture );
	R_GL3DrawStream( GL_TRIANGLES, gl3_batch, gl3_batch_count );
	gl3_draws++;
	gl3_vertices += gl3_batch_count;
	gl3_batch_count = 0;
}

// room for count vertices drawn with texture and the wanted states
static gl3_vertex_t *R_GL3Batch( int texture, int count )
{
	gl3_vertex_t *v;

	if( texture != gl3_batch_texture || memcmp( &gl3_wanted, &gl3_applied, sizeof( gl3_wanted )) || gl3_batch_count + count > GL3_MAX_BATCH )
	{
		R_GL3Flush();
		R_GL3ApplyStates();
		gl3_batch_texture = texture;
	}

	v = &gl3_batch[gl3_batch_count];
	gl3_batch_count += count;
	return v;
}

static void R_GL3SetVertex( gl3_vertex_t *v, float x, float y, float s, float t, uint32_t color )
{
	v->position[0] = x;
	v->position[1] = y;
	v->position[2] = 0.0f;
	v->color = color;
	v->texcoord[0] = s;
	v->texcoord[1] = t;
}

static void R_GL3Quad( int texture, float x, float y, float w, float h, float s1, float t1, float s2, float t2, uint32_t color )
{
	gl3_vertex_t *v = R_GL3Batch( texture, 6 );

	R_GL3SetVertex( &v[0], x, y, s1, t1, color );
	R_GL3SetVertex( &v[1], x + w, y, s2, t1, color );
	R_GL3SetVertex( &v[2], x + w, y + h, s2, t2, color );
	v[3] = v[0];
	v[4] = v[2];
	R_GL3SetVertex( &v[5], x, y + h, s1, t2, color );
}

// geometry drawn from now on goes through this matrix (four rows)
void R_GL3SetTransform( const float rows[16] )
{
	R_GL3Flush();
	memcpy( gl3_transform, rows, sizeof( gl3_transform ));
	if( gl3_draw_program )
		R_GL3SetVSConstants( 0, gl3_transform, 4 );
}

// fog of the triangle API from now on (density 0: none)
void R_GL3SetDrawFog( const float eyeplane[4], const float fog[4] )
{
	if( !memcmp( gl3_draw_eyeplane, eyeplane, sizeof( gl3_draw_eyeplane )) && !memcmp( gl3_draw_fog, fog, sizeof( gl3_draw_fog )))
		return;

	R_GL3Flush();
	memcpy( gl3_draw_eyeplane, eyeplane, sizeof( gl3_draw_eyeplane ));
	memcpy( gl3_draw_fog, fog, sizeof( gl3_draw_fog ));
	if( gl3_draw_program )
	{
		R_GL3SetVSConstants( 4, gl3_draw_eyeplane, 1 );
		R_GL3SetPSConstants( 0, gl3_draw_fog, 1 );
	}
}

// gl_rmain.c R_AllowFog for world-space geometry of the triangle API: the plain fog colour, when allowed
void R_GL3EffectFog( qboolean allow )
{
	static const float nofog[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	float fog[4];

	if( !allow || !gl3_ri.fogEnabled )
	{
		R_GL3SetDrawFog( nofog, nofog );
		return;
	}

	VectorCopy( gl3_ri.fogColor, fog );
	fog[3] = gl3_ri.fogEndInv;
	R_GL3SetDrawFog( gl3_ri.worldviewMatrix[2], fog );
}

// someone else is about to draw: draw what is batched, set everything again next time
void R_GL3DrawReset( void )
{
	R_GL3Flush();
	R_GL3ApplyStates();
	gl3_draw_program = false;
}

static void R_GL3Load2DTransform( void )
{
	float w = (float)gpGlobals->width, h = (float)gpGlobals->height;
	float rows[16] =
	{
		2.0f / w, 0.0f, 0.0f, -1.0f + 2.0f * gl3_2d_offset[0] / w,
		0.0f, -2.0f / h, 0.0f, 1.0f - 2.0f * gl3_2d_offset[1] / h,
		0.0f, 0.0f, 0.0f, 0.0f,
		0.0f, 0.0f, 0.0f, 1.0f,
	};

	R_GL3SetTransform( rows );
}

/*
==================
R_GL3InitDraw
==================
*/
qboolean R_GL3InitDraw( void )
{
	gl3_draw_prog = R_GL3CreateProgram( "draw", draw_glsl, NULL );
	gl3_warpscreen_prog = R_GL3CreateProgram( "draw", draw_glsl, "#define WARP_SCREEN 1\n" );
	gl3_warpwave_prog = R_GL3CreateProgram( "draw", draw_glsl, "#define WARP_WAVE 1\n" );

	if( !gl3_draw_prog || !gl3_warpscreen_prog || !gl3_warpwave_prog )
	{
		gEngfuncs.Con_Printf( S_ERROR "ref_gl3: can't create the draw shaders\n" );
		return false;
	}

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

	R_GL3UseDrawProgram();

	// the context's defaults, then everything once as the engine expects at start
	gl3_applied.blend = false;
	gl3_applied.src = GL_ONE;
	gl3_applied.dst = GL_ZERO;
	gl3_applied.alphatest = false;
	gl3_applied.zwrite = true;
	gl3_applied.ztest = false;
	gl3_applied.zfunc = GL_LESS;
	gl3_applied.cull = GL3_CULL_NONE;
	gl3_applied.alpharef = 0;
	gl3_applied.depthbias = 0.0f;
	gl3_applied.slopebias = 0.0f;
	{
		const float noref[4] = { -1.0f, 0.0f, 0.0f, 0.0f };

		R_GL3SetPSConstants( GL3_PS_ALPHAREF, noref, 1 );
	}
	gl3_wanted = gl3_applied;
	gl3_wanted.ztest = true;
	gl3_wanted.zfunc = GL_LEQUAL;
	R_GL3ApplyStates();
	return true;
}

void R_GL3ShutdownDraw( void )
{
	gl3_batch_count = 0;
	gl3_draw_program = false;

	if( gl3_draw_vbo )
		glDeleteBuffers( 1, &gl3_draw_vbo );
	if( gl3_draw_vao )
		glDeleteVertexArrays( 1, &gl3_draw_vao );
	gl3_draw_vbo = gl3_draw_vao = 0;
	gl3_draw_prog = gl3_warpscreen_prog = gl3_warpwave_prog = NULL;
}

void R_GL3BeginFrameDraw( void )
{
	gl3_draws = gl3_vertices = 0;
}

void R_GL3EndFrameDraw( void )
{
	R_GL3Flush();
}

void R_GL3CountDraw( int vertices )
{
	gl3_draws++;
	gl3_vertices += vertices;
}

void R_GL3FrameStats( double present_time )
{
	// r_speeds will read these; nothing reports them yet
}

/*
==================
R_Set2DMode
==================
*/
void R_GL3Set2DMode( qboolean enable )
{
	if( enable == gl3_2d )
		return;

	R_GL3Flush();
	gl3_2d = enable;

	if( enable )
	{
		const int scene = R_GL3TakeTiledScene();

		R_GL3RestoreViewport();
		R_GL3Load2DTransform();
		gl3_wanted.ztest = false;
		gl3_wanted.zwrite = false;
		gl3_wanted.cull = GL3_CULL_NONE;
		gl3_wanted.alphatest = true;
		gl3_color = 0xffffffff;

		// the 3D part of the scene target under everything 2D, one texel per pixel - or through the
		// software style's warp buffer, when the view is under water. A framebuffer's texture has its first
		// row at the bottom, so the quad reads it upside down.
		if( scene )
		{
			const qboolean blend = gl3_wanted.blend;
			// vid.maxwarpwidth and maxwarpheight of the original's 320x200 mode, and the clock for ours
			const float warpsize[4] = { 320.0f, 200.0f, (float)gp_cl->time, 0.0f };
			const int style = R_GL3Style( );

			// R_MISC.C R_SetupFrame sets r_dowarp under water and while a screen fade runs; ours takes
			// the waves under water only, since a fade of ours has nothing to do with the view
			if( style == GL3_STYLE_SOFTWARE )
				gl3_draw_warpscreen = ( gl3_ri.cached_waterlevel >= 3 || ENGINE_GET_PARM( PARM_SCREEN_FADE ) > 0 ) ? 1 : 0;
			else if( style == GL3_STYLE_ENHANCED )
				gl3_draw_warpscreen = gl3_ri.cached_waterlevel >= 3 ? 2 : 0;
			else gl3_draw_warpscreen = 0;
			gl3_draw_program = false;
			gl3_wanted.blend = false;
			R_GL3Quad( scene, 0.0f, 0.0f, (float)gpGlobals->width, (float)gpGlobals->height, 0.0f, 1.0f, 1.0f, 0.0f, 0xffffffff );
			if( gl3_draw_warpscreen )
			{
				R_GL3UseDrawProgram();
				R_GL3SetPSConstants( 1, warpsize, 1 );
			}
			R_GL3Flush();
			gl3_draw_warpscreen = 0;
			gl3_draw_program = false;
			gl3_wanted.blend = blend;
		}
	}
	else
	{
		gl3_wanted.ztest = true;
		gl3_wanted.zwrite = true;
	}
}

void R_GL3Set2DOffset( float x, float y )
{
	gl3_2d_offset[0] = x;
	gl3_2d_offset[1] = y;

	if( gl3_2d )
	{
		R_GL3Flush();
		R_GL3Load2DTransform();
	}
}

/*
==================
GL_SetRenderMode

Everything modulates the texture by the colour (gl_backend.c GL_SetRenderMode).
==================
*/
void R_GL3SetRenderMode( int mode )
{
	switch( mode )
	{
	case kRenderTransColor:
	case kRenderTransTexture:
		gl3_wanted.blend = true;
		gl3_wanted.src = GL_SRC_ALPHA;
		gl3_wanted.dst = GL_ONE_MINUS_SRC_ALPHA;
		gl3_wanted.alphatest = false;
		break;
	case kRenderTransAlpha:
		gl3_wanted.blend = false;
		gl3_wanted.alphatest = true;
		break;
	case kRenderGlow:
	case kRenderTransAdd:
		gl3_wanted.blend = true;
		gl3_wanted.src = GL_SRC_ALPHA;
		gl3_wanted.dst = GL_ONE;
		gl3_wanted.alphatest = false;
		break;
	case kRenderScreenFadeModulate:
		gl3_wanted.blend = true;
		gl3_wanted.src = GL_ZERO;
		gl3_wanted.dst = GL_SRC_COLOR;
		gl3_wanted.alphatest = false;
		break;
	case kRenderNormal:
	default:
		gl3_wanted.blend = false;
		gl3_wanted.alphatest = false;
		break;
	}
}

// gl_triapi.c TriRenderMode: leaves alpha test alone, and controls depth writes
void R_GL3TriRenderMode( int mode )
{
	switch( mode )
	{
	case kRenderNormal:
		gl3_wanted.blend = false;
		gl3_wanted.zwrite = true;
		break;
	case kRenderTransAlpha:
		gl3_wanted.blend = true;
		gl3_wanted.src = GL_SRC_ALPHA;
		gl3_wanted.dst = GL_ONE_MINUS_SRC_ALPHA;
		gl3_wanted.zwrite = false;
		break;
	case kRenderTransColor:
	case kRenderTransTexture:
		gl3_wanted.blend = true;
		gl3_wanted.src = GL_SRC_ALPHA;
		gl3_wanted.dst = GL_ONE_MINUS_SRC_ALPHA;
		break;
	case kRenderGlow:
	case kRenderTransAdd:
		gl3_wanted.blend = true;
		gl3_wanted.src = GL_SRC_ALPHA;
		gl3_wanted.dst = GL_ONE;
		gl3_wanted.zwrite = false;
		break;
	}
}

void R_GL3DrawStretchPic( float x, float y, float w, float h, float s1, float t1, float s2, float t2, int texnum )
{
	R_GL3Quad( texnum, x, y, w, h, s1, t1, s2, t2, gl3_color );
}

// gl_context.c CL_FillRGBA: blended, untextured, and the colour stays set
void R_GL3FillRGBA( int rendermode, float x, float y, float w, float h, byte r, byte g, byte b, byte a )
{
	gl3_wanted.blend = true;
	gl3_wanted.src = GL_SRC_ALPHA;
	gl3_wanted.dst = rendermode == kRenderTransAdd ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA;
	gl3_color = GL3_ARGB( a, r, g, b );

	R_GL3Quad( gl3_white_texture, x, y, w, h, 0.0f, 0.0f, 1.0f, 1.0f, gl3_color );
	gl3_wanted.blend = false;
}

/*
==============================================================================

TRIANGLE API

Vertices collect between Begin and End and become triangles of the batch at End.

==============================================================================
*/
void R_GL3Bind( int texnum )
{
	gl3_texture = texnum;
}

void R_GL3TriBegin( int mode )
{
	gl3_prim_mode = mode;
	gl3_prim_count = 0;
}

void R_GL3TriColor4ub( byte r, byte g, byte b, byte a )
{
	gl3_color = GL3_ARGB( a, r, g, b );
}

void R_GL3TriColor4f( float r, float g, float b, float a )
{
	gl3_color = GL3_ARGB( R_GL3ColorByte( a ), R_GL3ColorByte( r ), R_GL3ColorByte( g ), R_GL3ColorByte( b ));
}

void R_GL3TriTexCoord2f( float u, float v )
{
	gl3_texcoord[0] = u;
	gl3_texcoord[1] = v;
}

void R_GL3TriVertex3f( float x, float y, float z )
{
	gl3_vertex_t *v;

	if( gl3_prim_count >= GL3_MAX_PRIM )
	{
		if( !gl3_prim_overflow )
			gEngfuncs.Con_Printf( S_ERROR "ref_gl3: more than %d vertices in one primitive, the rest is dropped\n", GL3_MAX_PRIM );
		gl3_prim_overflow = true;
		return;
	}

	v = &gl3_prim[gl3_prim_count++];
	v->position[0] = x;
	v->position[1] = y;
	v->position[2] = z;
	v->color = gl3_color;
	v->texcoord[0] = gl3_texcoord[0];
	v->texcoord[1] = gl3_texcoord[1];
}

void R_GL3TriVertex3fv( const float *v )
{
	R_GL3TriVertex3f( v[0], v[1], v[2] );
}

static void R_GL3Triangle( const gl3_vertex_t *a, const gl3_vertex_t *b, const gl3_vertex_t *c )
{
	gl3_vertex_t *v = R_GL3Batch( gl3_texture, 3 );

	v[0] = *a;
	v[1] = *b;
	v[2] = *c;
}

static void R_GL3Lines( void )
{
	int count = gl3_prim_count & ~1;

	if( count < 2 )
		return;

	R_GL3Flush();
	R_GL3ApplyStates();
	R_GL3UseDrawProgram();
	R_GL3BindTexture( gl3_texture );
	R_GL3DrawStream( GL_LINES, gl3_prim, count );
	gl3_draws++;
	gl3_vertices += count;
}

void R_GL3TriEnd( void )
{
	const gl3_vertex_t *p = gl3_prim;
	int n = gl3_prim_count, i;

	switch( gl3_prim_mode )
	{
	case TRI_TRIANGLES:
		for( i = 0; i + 2 < n; i += 3 )
			R_GL3Triangle( &p[i], &p[i + 1], &p[i + 2] );
		break;
	case TRI_QUADS:
		for( i = 0; i + 3 < n; i += 4 )
		{
			R_GL3Triangle( &p[i], &p[i + 1], &p[i + 2] );
			R_GL3Triangle( &p[i], &p[i + 2], &p[i + 3] );
		}
		break;
	case TRI_TRIANGLE_STRIP:
		for( i = 0; i + 2 < n; i++ )
		{
			if( i & 1 )
				R_GL3Triangle( &p[i + 1], &p[i], &p[i + 2] );
			else R_GL3Triangle( &p[i], &p[i + 1], &p[i + 2] );
		}
		break;
	case TRI_QUAD_STRIP:
		for( i = 0; i + 3 < n; i += 2 )
		{
			R_GL3Triangle( &p[i], &p[i + 1], &p[i + 3] );
			R_GL3Triangle( &p[i], &p[i + 3], &p[i + 2] );
		}
		break;
	case TRI_LINES:
		R_GL3Lines();
		break;
	case TRI_POINTS:
		break; // nothing in Half-Life draws points
	case TRI_TRIANGLE_FAN:
	case TRI_POLYGON:
	default:
		for( i = 1; i + 1 < n; i++ )
			R_GL3Triangle( &p[0], &p[i], &p[i + 1] );
		break;
	}

	gl3_prim_mode = -1;
	gl3_prim_count = 0;
}

// one TRI_QUADS quad with the current texture and colour, straight into the batch (no primitive limit)
void R_GL3TriQuad( const vec3_t v[4], const float st[8] )
{
	static const int order[6] = { 0, 1, 2, 0, 2, 3 };
	gl3_vertex_t *p = R_GL3Batch( gl3_texture, 6 );

	for( int i = 0; i < 6; i++ )
	{
		const int k = order[i];

		VectorCopy( v[k], p[i].position );
		p[i].color = gl3_color;
		p[i].texcoord[0] = st[k * 2];
		p[i].texcoord[1] = st[k * 2 + 1];
	}
}

void R_GL3TriCullFace( TRICULLSTYLE mode )
{
	gl3_wanted.cull = mode == TRI_FRONT ? GL3_CULL_FRONT : GL3_CULL_NONE;
}
