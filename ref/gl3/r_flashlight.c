/*
r_flashlight.c - the enhanced style's flashlight, projected the way Half-Life 2 does it

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
Half-Life's own flashlight is a dynamic light dropped where the player's view hits something
(`CL_UpdateFlashlight`, `cl_tent.c`): a round blob on the wall that also lights whatever is behind the
wall, since a dlight has no direction at all. Half-Life 2 projects a texture from the eye instead, which
is what this does: a cone with a cookie over it.

The light sits at the eye and points where the player looks, so every surface the player can see faces the
light. That is why no surface normal is needed here, and why the projection alone looks right.
*/

#include "r_local.h"
#include "pm_defs.h"

// Half-Life 2's own numbers, from the behaviour of its flashlighteffect.cpp. The light hangs below the
// eye, aims at a point ahead of the eye - so the beam is slightly raised - and backs away from a wall it
// stands close to. Its r_flashlightoffsetx and r_flashlightoffsetz are dead there: nothing reads them,
// and the only offset in play is r_flashlightoffsety along the eye's up vector.
#define GL3_FLASHLIGHT_FOV    45.0f    // r_flashlightfov, the same across and down
#define GL3_FLASHLIGHT_RANGE  750.0f   // r_flashlightfar
#define GL3_FLASHLIGHT_NEAR   4.0f     // r_flashlightnear
#define GL3_FLASHLIGHT_DOWN   20.0f    // -r_flashlightoffsety: the lamp sits this far under the eye
#define GL3_FLASHLIGHT_SWING  12.0f    // the point behind the eye that lifts the lamp when looking down
#define GL3_FLASHLIGHT_PULL   128.0f   // a wall nearer than this pushes the light back towards the player
#define GL3_FLASHLIGHT_SMOOTH 0.2f     // the drag on that push, per frame, as HL2 has it
#define GL3_FLASHLIGHT_LINEAR 100.0f   // r_flashlightlinear: the light is white within this many units
#define GL3_FLASHLIGHT_BRIGHT 2.0f     // what HL2 multiplies the cone by when it is not rendering in HDR
#define GL3_SHADOW_SIZE       512      // shadow map, and the size shaders/flashlight.glsl filters it at
#define GL3_COOKIE_SIZE       256

static struct
{
	qboolean  active;
	qboolean  was_active;
	qboolean  updated;   // its matrix is built for the frame being drawn
	float     distmod;   // units the wall pushed the light back by, smoothed: HL2's distance modifier
	matrix4x4 matrix;  // world to the light's clip space
	vec3_t    origin;  // where the lamp ended up
	int       cookie;
	float     params[4];  // pixel shader: rgb of the light, w its linear attenuation
	float     params2[4]; // xyz the lamp, w 1 / far

	GLuint             shadow_fbo;
	GLuint             shadow_depth;
	int                shadow;   // the texture slot that owns the map's texture
	qboolean           shadowed; // this frame's map holds something
} gl3_flashlight;

static cvar_t *r_flashlight_shadows;

static cvar_t *r_flashlight;

void R_GL3InitFlashlight( void )
{
	r_flashlight = gEngfuncs.Cvar_Get( "r_flashlight", "1", FCVAR_ARCHIVE,
		"the enhanced style's projected flashlight instead of the original's dynamic light: 2 keeps it on" );
	r_flashlight_shadows = gEngfuncs.Cvar_Get( "r_flashlight_shadows", "1", FCVAR_ARCHIVE,
		"the flashlight casts shadows: the world is drawn again from the light into a 512x512 map" );
	gl3_flashlight.cookie = 0;
}

/*
==================
R_GL3FlashlightCookie

The shape of the beam, drawn here so that no game's art has to be shipped: a bright core, a soft ring around it
and black at the rim. A gfx/flashlight.tga of the game's own takes its place.
==================
*/
int R_GL3FlashlightCookie( void )
{
	byte *rgba;
	GLuint glnum = 0;

	if( gl3_flashlight.cookie )
		return gl3_flashlight.cookie;

	gl3_flashlight.cookie = R_GL3LoadTexture( "gfx/flashlight.tga", NULL, 0, TF_CLAMP | TF_NOMIPMAP );
	if( gl3_flashlight.cookie )
		return gl3_flashlight.cookie;

	rgba = (byte *)Mem_Malloc( r_temppool, GL3_COOKIE_SIZE * GL3_COOKIE_SIZE * 4 );

	for( int y = 0; y < GL3_COOKIE_SIZE; y++ )
	{
		for( int x = 0; x < GL3_COOKIE_SIZE; x++ )
		{
			const float u = ( x + 0.5f ) / GL3_COOKIE_SIZE * 2.0f - 1.0f;
			const float v = ( y + 0.5f ) / GL3_COOKIE_SIZE * 2.0f - 1.0f;
			const float r = sqrt( u * u + v * v );
			const float edge = bound( 0.0f, ( 1.0f - r ) / 0.3f, 1.0f ); // the rim fades over the last 30 %
			const float ring = 0.75f + 0.25f * ( 1.0f - bound( 0.0f, r / 0.55f, 1.0f )); // and the core is the brightest
			const byte value = (byte)( edge * edge * ( 3.0f - 2.0f * edge ) * ring * 255.0f );
			byte *out = rgba + ( y * GL3_COOKIE_SIZE + x ) * 4;

			out[0] = out[1] = out[2] = value;
			out[3] = 255;
		}
	}

	glGenTextures( 1, &glnum );
	glBindTexture( GL_TEXTURE_2D, glnum );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, GL3_COOKIE_SIZE, GL3_COOKIE_SIZE, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0 );
	R_GL3ForgetBindings();
	Mem_Free( rgba );

	gl3_flashlight.cookie = R_GL3WrapTexture( "*flashcookie", glnum, GL3_COOKIE_SIZE, GL3_COOKIE_SIZE, TF_CLAMP | TF_NOMIPMAP );
	return gl3_flashlight.cookie;
}

qboolean R_GL3FlashlightActive( void )
{
	return gl3_flashlight.active;
}

qboolean R_GL3FlashlightShadowed( void )
{
	return gl3_flashlight.shadowed;
}

int R_GL3FlashlightShadowMap( void )
{
	return gl3_flashlight.shadow;
}

static qboolean R_GL3FlashlightMakeShadowMap( void )
{
	GLuint color = 0;

	if( gl3_flashlight.shadow )
		return true;

	glGenTextures( 1, &color );
	glBindTexture( GL_TEXTURE_2D, color );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_R32F, GL3_SHADOW_SIZE, GL3_SHADOW_SIZE, 0, GL_RED, GL_FLOAT, NULL );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0 );
	R_GL3ForgetBindings();

	glGenRenderbuffers( 1, &gl3_flashlight.shadow_depth );
	glBindRenderbuffer( GL_RENDERBUFFER, gl3_flashlight.shadow_depth );
	glRenderbufferStorageMultisample( GL_RENDERBUFFER, 0, GL_DEPTH_COMPONENT24, GL3_SHADOW_SIZE, GL3_SHADOW_SIZE );
	glBindRenderbuffer( GL_RENDERBUFFER, 0 );

	glGenFramebuffers( 1, &gl3_flashlight.shadow_fbo );
	glBindFramebuffer( GL_FRAMEBUFFER, gl3_flashlight.shadow_fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0 );
	glFramebufferRenderbuffer( GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, gl3_flashlight.shadow_depth );

	gl3_flashlight.shadow = R_GL3WrapTexture( "*flashshadow", color, GL3_SHADOW_SIZE, GL3_SHADOW_SIZE,
		TF_NEAREST | TF_CLAMP | TF_NOMIPMAP );

	if( glCheckFramebufferStatus( GL_FRAMEBUFFER ) != GL_FRAMEBUFFER_COMPLETE || !gl3_flashlight.shadow )
	{
		gEngfuncs.Con_Printf( S_ERROR "ref_gl3: no flashlight shadow map\n" );
		R_GL3RestoreBackBuffer();
		R_GL3FreeFlashlight();
		gEngfuncs.Cvar_Set( "r_flashlight_shadows", "0" );
		return false;
	}

	R_GL3RestoreBackBuffer();
	return true;
}

void R_GL3FreeFlashlight( void )
{
	if( gl3_flashlight.shadow )
		R_GL3FreeTexture( gl3_flashlight.shadow );
	if( gl3_flashlight.cookie )
		R_GL3FreeTexture( gl3_flashlight.cookie );
	if( gl3_flashlight.shadow_fbo )
		glDeleteFramebuffers( 1, &gl3_flashlight.shadow_fbo );
	if( gl3_flashlight.shadow_depth )
		glDeleteRenderbuffers( 1, &gl3_flashlight.shadow_depth );

	gl3_flashlight.shadow = 0;
	gl3_flashlight.cookie = 0;
	gl3_flashlight.shadow_fbo = 0;
	gl3_flashlight.shadow_depth = 0;
}

/*
==================
R_GL3FlashlightShadow

The world from the light, into a 512x512 map, before the scene target binds. Only what the main view collected is
in there, which is what the beam can reach anyway, and the whole pass is one shader.
==================
*/
void R_GL3FlashlightShadow( int first, int last )
{
	const float range[4] = { 1.0f / GL3_FLASHLIGHT_RANGE, 0.0f, 0.0f, 0.0f };
	gl3_states_t states;

	gl3_flashlight.shadowed = false;

	if( !gl3_flashlight.active || !r_flashlight_shadows->value )
		return;

	R_GL3DrawReset();
	if( !R_GL3FlashlightMakeShadowMap( ))
		return;

	glBindFramebuffer( GL_FRAMEBUFFER, gl3_flashlight.shadow_fbo );
	glViewport( 0, 0, GL3_SHADOW_SIZE, GL3_SHADOW_SIZE );
	glDepthRange( 0.0, 1.0 );
	glClearColor( 1.0f, 1.0f, 1.0f, 1.0f );
	R_GL3ClearBuffers( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );
	R_GL3SetPSConstants( 0, range, 1 );

	// both sides cast: a one-sided pass would let the light through the back of thin geometry
	R_GL3DefaultStates( &states );
	states.cull = GL3_CULL_NONE;
	states.zwrite = true;
	states.ztest = true;
	gl3_wanted = states;
	R_GL3ApplyStates();

	R_GL3WorldShadowRange( first, last );

	// and the models, which are the shadows a player actually sees
	R_GL3StudioDrawShadowCasters();

	R_GL3RestoreBackBuffer();
	R_GL3RestoreViewViewport(); // the view's own, which this pass replaced with the map's
	R_GL3DrawReset();
	gl3_flashlight.shadowed = true;
}

// a new frame: the first view of it places the lamp
void R_GL3FlashlightNewFrame( void )
{
	gl3_flashlight.updated = false;
}

const float *R_GL3FlashlightParams( void )
{
	return gl3_flashlight.params;
}

// whether a map will be built at all this frame: the models only keep their poses for one that is
qboolean R_GL3FlashlightShadowsWanted( void )
{
	return gl3_flashlight.active && r_flashlight_shadows->value != 0.0f;
}

// the lamp itself and 1 / its range: the shaders measure the distance from it, as HL2's model path does
const float *R_GL3FlashlightLamp( void )
{
	return gl3_flashlight.params2;
}

/*
==================
R_GL3FlashlightOrigin

Where Half-Life 2 hangs its flashlight: up from the eye first (unless something is in the way), then
forward and to the left. A wall ahead pushes the light back towards the player, which widens the cone
over the wall, and that push is smoothed frame to frame rather than snapped - which is why switching the
light on settles over a few frames instead of appearing finished.
==================
*/
static void R_GL3FlashlightOrigin( vec3_t origin, vec3_t dir )
{
	const float *eye = gl3_ri.rvp.vieworigin;
	vec3_t start, target, ahead, behind;
	float down = GL3_FLASHLIGHT_DOWN;
	pmtrace_t tr;
	float dist, want;

	// looking down lifts the lamp back towards the eye, by how much the point behind the eye rose
	VectorMA( eye, -GL3_FLASHLIGHT_SWING, gl3_ri.vforward, start );
	if( start[2] > eye[2] )
		down -= start[2] - eye[2];

	VectorMA( eye, -down, gl3_ri.vup, start );

	// no room under the eye: the lamp goes back to it
	tr = gEngfuncs.CL_TraceLine( (float *)eye, start, PM_STUDIO_IGNORE );
	if( tr.fraction < 1.0f )
		VectorCopy( eye, start );

	// the beam aims at a point ahead of the eye, not of the lamp, which raises it a little
	VectorMA( eye, GL3_FLASHLIGHT_RANGE, gl3_ri.vforward, target );
	VectorSubtract( target, start, dir );
	VectorNormalize( dir );

	// a wall within the cutoff pushes the lamp back by what is left of it
	VectorMA( start, GL3_FLASHLIGHT_RANGE, dir, ahead );
	tr = gEngfuncs.CL_TraceLine( start, ahead, PM_STUDIO_IGNORE );
	dist = tr.fraction * GL3_FLASHLIGHT_RANGE;
	want = dist < GL3_FLASHLIGHT_PULL ? GL3_FLASHLIGHT_PULL - dist : 0.0f;

	if( !gl3_flashlight.was_active )
		gl3_flashlight.distmod = 1.0f; // HL2 starts it one unit back and lets the traces settle it

	gl3_flashlight.distmod += ( want - gl3_flashlight.distmod ) * GL3_FLASHLIGHT_SMOOTH;

	// and never through whatever stands behind the player
	VectorMA( start, -gl3_flashlight.distmod, dir, behind );
	tr = gEngfuncs.CL_TraceLine( start, behind, PM_STUDIO_IGNORE );
	if( tr.fraction < 1.0f )
		gl3_flashlight.distmod = tr.fraction * gl3_flashlight.distmod - 0.1f;

	VectorMA( start, -gl3_flashlight.distmod, dir, origin );
}

/*
==================
R_GL3FlashlightUpdate

Once a frame, before the world is collected: is the local player's flashlight on, and where does its cone
point. Other players keep the engine's dynamic light, which is what they have in the original.
==================
*/
void R_GL3FlashlightUpdate( void )
{
	const cl_entity_t *player;
	matrix4x4 view, proj;
	vec3_t origin, dir, angles;
	float tangent, znear;

	// A frame can hold more than one view. The shadow map is built for the first of them and kept, so
	// the light it was built for has to be kept as well: a second view must not move the lamp, or the
	// map and the cone that samples it would disagree. Both frame counters step per view, which is why
	// this flag is cleared where the frame itself begins.
	if( gl3_flashlight.updated )
		return;

	gl3_flashlight.updated = true;
	gl3_flashlight.was_active = gl3_flashlight.active;
	gl3_flashlight.active = false;

	if( R_GL3Style( ) != GL3_STYLE_ENHANCED || !r_flashlight->value )
		return;

	if( !FBitSet( gl3_ri.rvp.flags, RF_DRAW_WORLD ) || FBitSet( gl3_ri.rvp.flags, RF_ONLY_CLIENTDRAW ))
		return;

	// 2 keeps the cone on whatever the player carries, which is how the shot tool looks at it
	if( r_flashlight->value < 2.0f )
	{
		player = R_GL3EntityByIndex( gp_cl->playernum + 1 );
		if( !player || !FBitSet( player->curstate.effects, EF_DIMLIGHT ))
			return;
	}

	if( !R_GL3FlashlightCookie( ))
		return;

	R_GL3FlashlightOrigin( origin, dir );

	// VectorAngles measures pitch the way mathematics does, positive upwards, while the view angles this
	// renderer builds its matrices from are the engine's, positive downwards. Without the flip the cone
	// leans against the view: look up and it points down.
	VectorAngles( dir, angles );
	angles[0] = -angles[0];

	// the near plane follows the light back, the way HL2 adds the modifier to it, so a light pushed
	// against a wall does not clip the wall away
	znear = GL3_FLASHLIGHT_NEAR + gl3_flashlight.distmod;
	tangent = znear * tan( GL3_FLASHLIGHT_FOV * M_PI_F / 360.0f );
	Matrix4x4_CreateProjection( proj, tangent, -tangent, tangent, -tangent, znear, GL3_FLASHLIGHT_RANGE );

	Matrix4x4_CreateModelview( view );
	Matrix4x4_ConcatRotate( view, -angles[2], 1, 0, 0 );
	Matrix4x4_ConcatRotate( view, -angles[0], 0, 1, 0 );
	Matrix4x4_ConcatRotate( view, -angles[1], 0, 0, 1 );
	Matrix4x4_ConcatTranslate( view, -origin[0], -origin[1], -origin[2] );

	Matrix4x4_Concat( gl3_flashlight.matrix, proj, view );
	VectorCopy( origin, gl3_flashlight.origin );

	// white, at the brightness HL2 uses when it is not rendering in HDR, and its own linear attenuation
	gl3_flashlight.params[0] = gl3_flashlight.params[1] = gl3_flashlight.params[2] = GL3_FLASHLIGHT_BRIGHT;
	gl3_flashlight.params[3] = GL3_FLASHLIGHT_LINEAR;
	VectorCopy( origin, gl3_flashlight.params2 );
	gl3_flashlight.params2[3] = 1.0f / GL3_FLASHLIGHT_RANGE;
	gl3_flashlight.active = true;
}

// the light's own rows, for geometry that is already in world space
void R_GL3FlashlightRows( float rows[16] )
{
	R_GL3ClipMatrix( gl3_flashlight.matrix, rows );
}

// the object's own rows of "object to light clip", for the vertex shader
void R_GL3FlashlightObject( const matrix4x4 object, float rows[16] )
{
	matrix4x4 m;

	Matrix4x4_Concat( m, gl3_flashlight.matrix, object );
	R_GL3ClipMatrix( m, rows );
}
