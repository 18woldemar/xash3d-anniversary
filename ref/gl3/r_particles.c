/*
r_particles.c - ref_gl3: particles and tracers
Copyright (C) 2026 xash3d-xenon
Follows ref/gl/gl_rpart.c, Copyright (C) 2010 Uncle Mike

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
Every particle and tracer is a quad straight in the triangle batch (the pool holds thousands). Drawing also
runs their physics: the engine's CL_ThinkParticle for particles, the tracers' movement here.
*/

#include "r_local.h"
#include "r_efx.h"

static const float gTracerSize[11] = { 1.5f, 0.5f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
static color24 gTracerColors[] =
{
	{ 255, 255, 255 }, // white
	{ 255, 0, 0 },     // red
	{ 0, 255, 0 },     // green
	{ 0, 0, 255 },     // blue
	{ 0, 0, 0 },       // the default, from the tracer cvars
	{ 255, 167, 17 },  // yellow-orange sparks
	{ 255, 130, 90 },  // yellowish streaks (garg)
	{ 55, 60, 144 },   // blue egon streak
	{ 255, 130, 90 },  // more yellowish streaks (garg)
	{ 255, 140, 90 },  // more yellowish streaks (garg)
	{ 200, 130, 90 },  // more red streaks (garg)
	{ 255, 120, 70 },  // darker red streaks (garg)
};

/*
================
CL_DrawParticles
================
*/
void R_GL3DrawParticles( double frametime, particle_t *cl_active_particles, float partsize )
{

	// u runs down and v to the left, as in ref_gl (the dot in the texture is off centre)
	static const float st[8] = { 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f };
	const color24 *palette;

	if( !cl_active_particles )
		return;

	palette = (const color24 *)ENGINE_GET_PARM( PARM_GET_PALETTE_PTR );

	gl3_wanted.blend = TRUE;
	gl3_wanted.src = D3DBLEND_SRCALPHA;
	gl3_wanted.dst = D3DBLEND_INVSRCALPHA;
	gl3_wanted.alphatest = FALSE;
	gl3_wanted.zwrite = FALSE;
	R_GL3Bind( gl3_particle_texture );

	for( particle_t *p = cl_active_particles; p; p = p->next )
	{
		if(( p->type != pt_blob ) || ( p->unused == 255 ))
		{
			float size = partsize;
			vec3_t right, up, v[4];
			color24 color;
			int alpha;

			// bigger with the distance, so particles do not disappear
			size += ( p->org[0] - gl3_ri.rvp.vieworigin[0] ) * gl3_ri.vforward[0];
			size += ( p->org[1] - gl3_ri.rvp.vieworigin[1] ) * gl3_ri.vforward[1];
			size += ( p->org[2] - gl3_ri.rvp.vieworigin[2] ) * gl3_ri.vforward[2];

			if( size < 20.0f ) size = partsize;
			else size = partsize + size * 0.002f;

			VectorScale( gl3_ri.vright, size, right );
			VectorScale( gl3_ri.vup, size, up );

			p->color = bound( 0, p->color, 255 );
			color = palette[p->color];

			alpha = (int)( 255 * ( p->die - gp_cl->time ) * 16.0f );
			if( alpha > 255 || p->type == pt_static )
				alpha = 255;

			R_GL3TriColor4ub( color.r, color.g, color.b, (byte)alpha );

			for( int i = 0; i < 3; i++ )
			{
				v[0][i] = p->org[i] - right[i] + up[i];
				v[1][i] = p->org[i] + right[i] + up[i];
				v[2][i] = p->org[i] + right[i] - up[i];
				v[3][i] = p->org[i] - right[i] - up[i];
			}
			R_GL3TriQuad( v, st );
		}

		gEngfuncs.CL_ThinkParticle( frametime, p );
	}

	gl3_wanted.zwrite = TRUE;
}

// true when the tracer's box is outside the view
static qboolean CL_CullTracer( particle_t *p, const vec3_t start, const vec3_t end )
{
	vec3_t mins, maxs;

	for( int i = 0; i < 3; i++ )
	{
		if( start[i] < end[i] )
		{
			mins[i] = start[i];
			maxs[i] = end[i];
		}
		else
		{
			mins[i] = end[i];
			maxs[i] = start[i];
		}

		// not zero sized
		if( mins[i] == maxs[i] )
			maxs[i] += gTracerSize[p->type] * 2.0f;
	}

	return R_GL3CullBox( mins, maxs, 0 );
}

/*
================
CL_DrawTracers
================
*/
void R_GL3DrawTracers( double frametime, particle_t *cl_active_tracers )
{
	static const float st[8] = { 0.0f, 0.8f, 1.0f, 0.8f, 1.0f, 0.0f, 0.0f, 0.0f };
	float gravity, scale;

	// the default colour follows the cvars
	if( FBitSet( tracerred->flags | tracergreen->flags | tracerblue->flags | traceralpha->flags, FCVAR_CHANGED ))
	{
		color24 *customColors = &gTracerColors[TRACER_COLORINDEX_DEFAULT];

		customColors->r = (byte)( tracerred->value * traceralpha->value * 255 );
		customColors->g = (byte)( tracergreen->value * traceralpha->value * 255 );
		customColors->b = (byte)( tracerblue->value * traceralpha->value * 255 );
		ClearBits( tracerred->flags, FCVAR_CHANGED );
		ClearBits( tracergreen->flags, FCVAR_CHANGED );
		ClearBits( tracerblue->flags, FCVAR_CHANGED );
		ClearBits( traceralpha->flags, FCVAR_CHANGED );
	}

	if( !cl_active_tracers )
		return;

	if( !R_GL3SpriteTexture( gEngfuncs.GetDefaultSprite( REF_DOT_SPRITE ), 0 ))
		return;

	gl3_wanted.blend = TRUE;
	gl3_wanted.src = D3DBLEND_SRCALPHA;
	gl3_wanted.dst = D3DBLEND_ONE;
	gl3_wanted.alphatest = FALSE;
	gl3_wanted.zwrite = FALSE;

	gravity = frametime * gp_movevars->gravity;
	scale = 1.0 - ( frametime * 0.9 );
	if( scale < 0.0f ) scale = 0.0f;

	for( particle_t *p = cl_active_tracers; p; p = p->next )
	{
		float atten = ( p->die - gp_cl->time );
		vec3_t start, end, delta;

		if( atten > 0.1f ) atten = 0.1f;

		VectorScale( p->vel, ( p->ramp * atten ), delta );
		VectorAdd( p->org, delta, end );
		VectorCopy( p->org, start );

		if( !CL_CullTracer( p, start, end ))
		{
			vec3_t verts[4], quad[4], tmp, tmp2, normal, screen, screenLast;
			color24 color;

			// the screen direction of the tracer, in the view's pixels
			R_GL3TriWorldToScreen( start, screen );
			R_GL3TriWorldToScreen( end, screenLast );
			VectorSubtract( screen, screenLast, tmp );
			tmp[2] = 0;
			VectorNormalize( tmp );

			// the world-space normal to it (normal is -y, x)
			VectorScale( gl3_ri.vup, tmp[0] * gTracerSize[p->type], normal );
			VectorScale( gl3_ri.vright, -tmp[1] * gTracerSize[p->type], tmp2 );
			VectorSubtract( normal, tmp2, normal );

			VectorSubtract( start, normal, verts[0] );
			VectorAdd( start, normal, verts[1] );
			VectorAdd( verts[0], delta, verts[2] );
			VectorAdd( verts[1], delta, verts[3] );

			if( p->color < 0 || p->color >= (int)ARRAYSIZE( gTracerColors ))
				p->color = TRACER_COLORINDEX_DEFAULT;

			color = gTracerColors[p->color];
			R_GL3TriColor4ub( color.r, color.g, color.b, (byte)p->unused );

			VectorCopy( verts[2], quad[0] );
			VectorCopy( verts[3], quad[1] );
			VectorCopy( verts[1], quad[2] );
			VectorCopy( verts[0], quad[3] );
			R_GL3TriQuad( quad, st );
		}

		VectorMA( p->org, frametime, p->vel, p->org );

		if( p->type == pt_grav )
		{
			p->vel[0] *= scale;
			p->vel[1] *= scale;
			p->vel[2] -= gravity;

			p->unused = (short)( 255 * ( p->die - gp_cl->time ) * 2 );
			if( p->unused > 255 ) p->unused = 255;
		}
		else if( p->type == pt_slowgrav )
		{
			p->vel[2] = gravity * 0.05f; // an assignment in ref_gl too
		}
	}

	gl3_wanted.zwrite = TRUE;
}
