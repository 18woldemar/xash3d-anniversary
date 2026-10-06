/*
r_beams.c - ref_gl3: beams
Copyright (C) 2026 xash3d-xenon
Follows ref/gl/gl_beams.c and gl_triapi.c, Copyright (C) 2009 Uncle Mike

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
Temporary beams (the engine's pool) and beam entities (the server's env_beam, lasers) as ref_gl draws them:
one triangle API primitive per beam, with ref_gl's own TriColor4f (colour premultiplied by alpha, alpha 1)
and TriBrightness, and screen directions in viewport pixels. Beam noise uses the engine's random numbers.
*/

#include "r_local.h"
#include "customentity.h"

#define NOISE_DIVISIONS 64 // many tripmines crash at 128

typedef struct
{
	vec3_t pos;
	float  texcoord; // v
	float  width;
} beamseg_t;

static float  rgNoise[NOISE_DIVISIONS + 1]; // shared by all beams
static vec4_t gl3_beam_rgba; // gl_triapi.c ds.triRGBA

/*
==============================================================

TRIANGLE API AS REF_GL USES IT

==============================================================
*/
static void R_BeamRenderMode( int mode )
{
	R_GL3TriRenderMode( mode );
	R_GL3EffectFog( mode != kRenderGlow && mode != kRenderTransAdd );
}

// beams draw kRenderNormal or kRenderTransAdd, which both premultiply (ref_gl's kRenderTransAlpha does not)
static void R_BeamColor4f( float r, float g, float b, float a )
{
	R_GL3TriColor4f( r * a, g * a, b * a, 1.0f );
	Vector4Set( gl3_beam_rgba, r, g, b, a );
}

static void R_BeamBrightness( float brightness )
{
	const float a = gl3_beam_rgba[3] * brightness;

	R_GL3TriColor4f( gl3_beam_rgba[0] * a, gl3_beam_rgba[1] * a, gl3_beam_rgba[2] * a, 1.0f );
}

/*
==============================================================

FRACTAL NOISE

==============================================================
*/
// power of 2 wavelength
static void FracNoise( float *noise, int divs )
{
	int div2 = divs >> 1;

	if( divs < 2 )
		return;

	// normalized to +/- scale
	noise[div2] = ( noise[0] + noise[divs] ) * 0.5f + divs * gEngfuncs.COM_RandomFloat( -0.125f, 0.125f );

	if( div2 > 1 )
	{
		FracNoise( &noise[div2], div2 );
		FracNoise( noise, div2 );
	}
}

static void SineNoise( float *noise, int divs )
{
	float freq = 0;
	float step = M_PI_F / (float)divs;

	for( int i = 0; i < divs; i++ )
	{
		noise[i] = sin( freq );
		freq += step;
	}
}

/*
==============================================================

BEAM MATHLIB

==============================================================
*/
static void R_BeamComputePerpendicular( const vec3_t vecBeamDelta, vec3_t pPerp )
{
	vec3_t vecBeamCenter; // direction of the centre of the beam

	VectorNormalize2( vecBeamDelta, vecBeamCenter );
	CrossProduct( gl3_ri.vforward, vecBeamCenter, pPerp );
	VectorNormalize( pPerp );
}

// perpendicular to the beam and to the view direction: fattens the beam
static void R_BeamComputeNormal( const vec3_t vStartPos, const vec3_t vNextPos, vec3_t pNormal )
{
	vec3_t vTangentY, vDirToBeam;

	VectorSubtract( vStartPos, vNextPos, vTangentY );
	VectorSubtract( vStartPos, gl3_ri.rvp.vieworigin, vDirToBeam );
	CrossProduct( vTangentY, vDirToBeam, pNormal );
	VectorNormalizeFast( pNormal );
}

// true when the beam's box is outside the PVS or the view
static qboolean R_BeamCull( const vec3_t start, const vec3_t end, qboolean pvsOnly )
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
			maxs[i] += 1.0f;
	}

	if( gEngfuncs.Mod_BoxVisible( mins, maxs, R_GL3GetCurrentVis( )))
	{
		if( pvsOnly || !R_GL3CullBox( mins, maxs, 0 ))
			return false;
	}

	return true;
}

/*
==============================================================

BEAM DRAW METHODS

==============================================================
*/
static void R_BeamVertex( float s, float t, const vec3_t v )
{
	R_GL3TriTexCoord2f( s, t );
	R_GL3TriVertex3fv( v );
}

// the general beam: a strip along the segments
static void R_DrawSegs( vec3_t source, vec3_t delta, float width, float scale, float freq, float speed, int segments, int flags )
{
	int noiseIndex = 0, noiseStep, total_segs, segs_drawn = 0;
	float length, flMaxWidth, div, vStep, vLast, brightness = 1.0f;
	vec3_t perp1, vLastNormal;
	beamseg_t curSeg;

	if( segments < 2 )
		return;

	length = VectorLength( delta );
	flMaxWidth = width * 0.5f;
	div = 1.0f / ( segments - 1 );

	if( length * div < flMaxWidth * 1.414f )
	{
		// too many segments would overlap
		segments = (int)( length / ( flMaxWidth * 1.414f )) + 1;
		if( segments < 2 ) segments = 2;
	}

	if( segments > NOISE_DIVISIONS )
		segments = NOISE_DIVISIONS;

	div = 1.0f / ( segments - 1 );
	length *= 0.01f;
	vStep = length * div; // texture length texels per space pixel

	// scroll speed 3.5: initial texture position, scrolls 3.5/sec (1.0 is the entire texture)
	vLast = fmod( freq * speed, 1.0f );

	if( FBitSet( flags, FBEAM_SINENOISE ))
	{
		if( segments < 16 )
		{
			segments = 16;
			div = 1.0f / ( segments - 1 );
		}
		scale *= 100.0f;
		length = segments * 0.1f;
	}
	else scale *= length * 2.0f;

	// resamples the noise waveform (generated in powers of 2)
	noiseStep = (int)((float)( NOISE_DIVISIONS - 1 ) * div * 65536.0f );

	if( FBitSet( flags, FBEAM_SHADEIN ))
		brightness = 0;

	R_BeamComputePerpendicular( delta, perp1 );

	total_segs = segments;
	VectorClear( vLastNormal );
	memset( &curSeg, 0, sizeof( curSeg ));

	for( int i = 0; i < segments; i++ )
	{
		const float fraction = i * div;
		beamseg_t nextSeg;
		vec3_t vPoint1, vPoint2;

		VectorMA( source, fraction, delta, nextSeg.pos );

		// distort using noise
		if( scale != 0 )
		{
			const float factor = rgNoise[noiseIndex >> 16] * scale;

			if( FBitSet( flags, FBEAM_SINENOISE ))
			{
				float s, c;

				SinCos( fraction * M_PI_F * length + freq, &s, &c );
				VectorMA( nextSeg.pos, ( factor * s ), gl3_ri.vup, nextSeg.pos );

				// rotated along the perpendicular a bit, so the bolt does not look diagonal
				VectorMA( nextSeg.pos, ( factor * c ), gl3_ri.vright, nextSeg.pos );
			}
			else VectorMA( nextSeg.pos, factor, perp1, nextSeg.pos );
		}

		nextSeg.width = width * 2.0f;
		nextSeg.texcoord = vLast;

		if( segs_drawn > 0 )
		{
			vec3_t vNormal, vAveNormal;

			R_BeamComputeNormal( curSeg.pos, nextSeg.pos, vNormal );

			if( segs_drawn > 1 )
			{
				// averaged with the previous normal
				VectorAdd( vNormal, vLastNormal, vAveNormal );
				VectorScale( vAveNormal, 0.5f, vAveNormal );
				VectorNormalizeFast( vAveNormal );
			}
			else VectorCopy( vNormal, vAveNormal );

			VectorCopy( vNormal, vLastNormal );

			VectorMA( curSeg.pos, ( curSeg.width * 0.5f ), vAveNormal, vPoint1 );
			VectorMA( curSeg.pos, ( -curSeg.width * 0.5f ), vAveNormal, vPoint2 );

			R_BeamBrightness( brightness );
			R_BeamVertex( 0.0f, curSeg.texcoord, vPoint1 );
			R_BeamVertex( 1.0f, curSeg.texcoord, vPoint2 );
		}

		curSeg = nextSeg;
		segs_drawn++;

		if( FBitSet( flags, FBEAM_SHADEIN ) && FBitSet( flags, FBEAM_SHADEOUT ))
			brightness = fraction < 0.5f ? fraction : ( 1.0f - fraction );
		else if( FBitSet( flags, FBEAM_SHADEIN ))
			brightness = fraction;
		else if( FBitSet( flags, FBEAM_SHADEOUT ))
			brightness = 1.0f - fraction;

		if( segs_drawn == total_segs )
		{
			// the last segment
			VectorMA( curSeg.pos, ( curSeg.width * 0.5f ), vLastNormal, vPoint1 );
			VectorMA( curSeg.pos, ( -curSeg.width * 0.5f ), vLastNormal, vPoint2 );

			R_BeamBrightness( brightness );
			R_BeamVertex( 0.0f, curSeg.texcoord, vPoint1 );
			R_BeamVertex( 1.0f, curSeg.texcoord, vPoint2 );
		}

		vLast += vStep; // texture scroll, v axis only
		noiseIndex += noiseStep;
	}
}

// the point in the view's pixels and the screen-space line normal to the previous point, in world space
static void R_BeamScreenNormal( const vec3_t point, vec3_t screen, const vec3_t screenLast, float sign, vec3_t normal )
{
	vec3_t tmp;

	R_GL3TriWorldToScreen( point, screen );
	VectorSubtract( screen, screenLast, tmp );
	tmp[2] = 0; // screen space
	VectorNormalize( tmp );

	// normal is -y, x
	VectorScale( gl3_ri.vup, sign * tmp[0], normal );
	VectorMA( normal, tmp[1], gl3_ri.vright, normal );
}

static void R_DrawTorus( vec3_t source, vec3_t delta, float width, float scale, float freq, float speed, int segments )
{
	vec3_t screenLast = { 0.0f, 0.0f, 0.0f };
	int noiseIndex = 0, noiseStep;
	float length, div, vStep, vLast;

	if( segments < 2 )
		return;

	if( segments > NOISE_DIVISIONS )
		segments = NOISE_DIVISIONS;

	length = VectorLength( delta ) * 0.01f;
	if( length < 0.5f ) length = 0.5f; // keep the noise and texture of short beams

	div = 1.0f / ( segments - 1 );
	vStep = length * div;
	vLast = fmod( freq * speed, 1.0f );
	scale = scale * length;
	noiseStep = (int)((float)( NOISE_DIVISIONS - 1 ) * div * 65536.0f );

	for( int i = 0; i < segments; i++ )
	{
		const float fraction = i * div;
		vec3_t point, screen, normal, last1, last2;
		float s, c;

		SinCos( fraction * M_PI2_F, &s, &c );
		point[0] = s * freq * delta[2] + source[0];
		point[1] = c * freq * delta[2] + source[1];
		point[2] = source[2];

		// distort using noise
		if( scale != 0 && ( noiseIndex >> 16 ) < NOISE_DIVISIONS )
		{
			float factor = rgNoise[noiseIndex >> 16] * scale;

			VectorMA( point, factor, gl3_ri.vup, point );
			factor = rgNoise[noiseIndex >> 16] * scale * cos( fraction * M_PI_F * 3 + freq );
			VectorMA( point, factor, gl3_ri.vright, point );
		}

		R_BeamScreenNormal( point, screen, screenLast, -1.0f, normal );

		if( i != 0 )
		{
			VectorMA( point, width, normal, last1 );
			VectorMA( point, -width, normal, last2 );

			vLast += vStep;
			R_BeamVertex( 1.0f, vLast, last2 );
			R_BeamVertex( 0.0f, vLast, last1 );
		}

		VectorCopy( screen, screenLast );
		noiseIndex += noiseStep;
	}
}

static void R_DrawDisk( vec3_t source, vec3_t delta, float width, float scale, float freq, float speed, int segments )
{
	float length, div, vStep, vLast, w;

	if( segments < 2 )
		return;

	if( segments > NOISE_DIVISIONS )
		segments = NOISE_DIVISIONS;

	length = VectorLength( delta ) * 0.01f;
	if( length < 0.5f ) length = 0.5f;

	div = 1.0f / ( segments - 1 );
	vStep = length * div;
	vLast = fmod( freq * speed, 1.0f );
	w = fmod( freq, width * 0.1f ) * delta[2];

	// the degenerate triangles stay on the edge
	for( int i = 0; i < segments; i++ )
	{
		const float fraction = i * div;
		vec3_t point;
		float s, c;

		R_BeamBrightness( 1.0f );
		R_BeamVertex( 1.0f, vLast, source );

		SinCos( fraction * M_PI2_F, &s, &c );
		point[0] = s * w + source[0];
		point[1] = c * w + source[1];
		point[2] = source[2];

		R_BeamBrightness( 1.0f );
		R_BeamVertex( 0.0f, vLast, point );

		vLast += vStep;
	}
}

static void R_DrawCylinder( vec3_t source, vec3_t delta, float width, float scale, float freq, float speed, int segments )
{
	float length, div, vStep, vLast;

	if( segments < 2 )
		return;

	if( segments > NOISE_DIVISIONS )
		segments = NOISE_DIVISIONS;

	length = VectorLength( delta ) * 0.01f;
	if( length < 0.5f ) length = 0.5f;

	div = 1.0f / ( segments - 1 );
	vStep = length * div;
	vLast = fmod( freq * speed, 1.0f );

	for( int i = 0; i < segments; i++ )
	{
		const float fraction = i * div;
		vec3_t point;
		float s, c;

		SinCos( fraction * M_PI2_F, &s, &c );

		point[0] = s * freq * delta[2] + source[0];
		point[1] = c * freq * delta[2] + source[1];
		point[2] = source[2] + width;

		R_BeamBrightness( 0.0f );
		R_BeamVertex( 1.0f, vLast, point );

		point[0] = s * freq * ( delta[2] + width ) + source[0];
		point[1] = c * freq * ( delta[2] + width ) + source[1];
		point[2] = source[2] - width;

		R_BeamBrightness( 1.0f );
		R_BeamVertex( 0.0f, vLast, point );

		vLast += vStep;
	}
}

// a trail of particles behind the start entity
static void R_DrawBeamFollow( BEAM *pbeam, float frametime )
{
	particle_t *particles, *pnew = NULL;
	vec3_t delta, screen, screenLast, normal, last1, last2;
	float div = 0.0f, fraction;

	gEngfuncs.R_FreeDeadParticles( &pbeam->particles );
	particles = pbeam->particles;

	if( FBitSet( pbeam->flags, FBEAM_STARTENTITY ))
	{
		if( particles )
		{
			VectorSubtract( particles->org, pbeam->source, delta );
			div = VectorLength( delta );

			if( div >= 32 )
				pnew = gEngfuncs.CL_AllocParticleFast();
		}
		else pnew = gEngfuncs.CL_AllocParticleFast();
	}

	if( pnew )
	{
		VectorCopy( pbeam->source, pnew->org );
		pnew->die = gp_cl->time + pbeam->amplitude;
		VectorClear( pnew->vel );

		pnew->next = particles;
		pbeam->particles = pnew;
		particles = pnew;
	}

	if( !particles )
		return;

	if( !pnew && div != 0 )
	{
		VectorCopy( pbeam->source, delta );
		R_GL3TriWorldToScreen( pbeam->source, screenLast );
		R_BeamScreenNormal( particles->org, screen, screenLast, 1.0f, normal );
	}
	else if( particles->next )
	{
		VectorCopy( particles->org, delta );
		R_GL3TriWorldToScreen( particles->org, screenLast );
		R_BeamScreenNormal( particles->next->org, screen, screenLast, 1.0f, normal );
		particles = particles->next;
	}
	else return;

	// ref_gl's UNDONE: screen and screenLast should be extrapolated for the first segment
	VectorMA( delta, pbeam->width, normal, last1 );
	VectorMA( delta, -pbeam->width, normal, last2 );

	div = 1.0f / pbeam->amplitude;
	fraction = ( pbeam->die - gp_cl->time ) * div;

	// ponytail: one primitive holds 4096 vertices, so a trail longer than 1024 particles loses its tail
	while( particles )
	{
		R_BeamBrightness( fraction );
		R_BeamVertex( 1.0f, 1.0f, last2 );
		R_BeamVertex( 0.0f, 1.0f, last1 );

		R_BeamScreenNormal( particles->org, screen, screenLast, 1.0f, normal );
		VectorMA( particles->org, pbeam->width, normal, last1 );
		VectorMA( particles->org, -pbeam->width, normal, last2 );

		fraction = particles->next ? ( particles->die - gp_cl->time ) * div : 0.0f;

		R_BeamBrightness( fraction );
		R_BeamVertex( 0.0f, 0.0f, last1 );
		R_BeamVertex( 1.0f, 0.0f, last2 );

		VectorCopy( screen, screenLast );
		particles = particles->next;
	}

	// the trail drifts with its velocity
	for( particles = pbeam->particles; particles; particles = particles->next )
		VectorMA( particles->org, frametime, particles->vel, particles->org );
}

static void R_DrawRing( vec3_t source, vec3_t delta, float width, float amplitude, float freq, float speed, int segments )
{
	vec3_t screenLast = { 0.0f, 0.0f, 0.0f };
	vec3_t center, xaxis, yaxis, mins, maxs;
	int noiseIndex = 0, noiseStep, j;
	float length, div, vStep, vLast, scale, radius;

	if( segments < 2 )
		return;

	segments = (int)( segments * M_PI_F );

	if( segments > NOISE_DIVISIONS * 8 )
		segments = NOISE_DIVISIONS * 8;

	length = VectorLength( delta ) * 0.01f * M_PI_F;
	if( length < 0.5f ) length = 0.5f;

	div = 1.0f / ( segments - 1 );
	vStep = length * div / 8.0f;
	vLast = fmod( freq * speed, 1.0f );
	scale = amplitude * length / 8.0f;
	noiseStep = (int)((float)( NOISE_DIVISIONS - 1 ) * div * 65536.0f ) * 8;

	// the beam's delta itself is halved, as in ref_gl
	VectorScale( delta, 0.5f, delta );
	VectorAdd( source, delta, center );
	VectorCopy( delta, xaxis );
	radius = VectorLength( xaxis );

	// cull the box around the ring
	VectorSet( mins, radius, radius, scale );
	VectorAdd( center, mins, maxs );
	VectorSubtract( center, mins, mins );

	if( !WORLDMODEL )
		return;

	if( !gEngfuncs.Mod_BoxVisible( mins, maxs, R_GL3GetCurrentVis( )) || R_GL3CullBox( mins, maxs, 0 ))
		return;

	VectorSet( yaxis, xaxis[1], -xaxis[0], 0.0f );
	VectorNormalize( yaxis );
	VectorScale( yaxis, radius, yaxis );

	j = segments / 8;

	for( int i = 0; i < segments + 1; i++ )
	{
		const float fraction = i * div;
		vec3_t point, screen, normal, last1, last2;
		float x, y, factor;

		SinCos( fraction * M_PI2_F, &x, &y );
		VectorMAMAM( x, xaxis, y, yaxis, 1.0f, center, point );

		// distort using noise
		factor = rgNoise[( noiseIndex >> 16 ) & ( NOISE_DIVISIONS - 1 )] * scale;
		VectorMA( point, factor, gl3_ri.vup, point );

		factor = rgNoise[( noiseIndex >> 16 ) & ( NOISE_DIVISIONS - 1 )] * scale;
		factor *= cos( fraction * M_PI_F * 24 + freq );
		VectorMA( point, factor, gl3_ri.vright, point );

		R_BeamScreenNormal( point, screen, screenLast, 1.0f, normal );

		if( i != 0 )
		{
			VectorMA( point, width, normal, last1 );
			VectorMA( point, -width, normal, last2 );

			vLast += vStep;
			R_BeamVertex( 1.0f, vLast, last2 );
			R_BeamVertex( 0.0f, vLast, last1 );
		}

		VectorCopy( screen, screenLast );
		noiseIndex += noiseStep;
		j--;

		if( j == 0 && amplitude != 0 )
		{
			j = segments / 8;
			FracNoise( rgNoise, NOISE_DIVISIONS );
		}
	}
}

/*
==============================================================

BEAMS

==============================================================
*/
// an attachment, the local player's view origin or the entity's origin
static qboolean R_BeamComputePoint( int beamEnt, vec3_t pt )
{
	cl_entity_t *ent = gEngfuncs.R_BeamGetEntity( beamEnt );
	const int attach = beamEnt < 0 ? BEAMENT_ATTACHMENT( -beamEnt ) : BEAMENT_ATTACHMENT( beamEnt );

	if( !ent )
	{
		gEngfuncs.Con_DPrintf( S_ERROR "%s: invalid entity %i\n", __func__, BEAMENT_ENTITY( beamEnt ));
		VectorClear( pt );
		return false;
	}

	if( attach > 0 )
		VectorCopy( ent->attachment[attach - 1], pt );
	else if( ent->index == ( gp_cl->playernum + 1 ))
		VectorCopy( gp_cl->simorg, pt );
	else VectorCopy( ent->origin, pt );

	return true;
}

static qboolean R_BeamRecomputeEndpoints( BEAM *pbeam )
{
	if( FBitSet( pbeam->flags, FBEAM_STARTENTITY ))
	{
		cl_entity_t *start = gEngfuncs.R_BeamGetEntity( pbeam->startEntity );

		if( R_BeamComputePoint( pbeam->startEntity, pbeam->source ))
		{
			if( !pbeam->pFollowModel )
				pbeam->pFollowModel = start->model;
			SetBits( pbeam->flags, FBEAM_STARTVISIBLE );
		}
		else if( !FBitSet( pbeam->flags, FBEAM_FOREVER ))
		{
			ClearBits( pbeam->flags, FBEAM_STARTENTITY );
		}
	}

	if( FBitSet( pbeam->flags, FBEAM_ENDENTITY ))
	{
		cl_entity_t *end = gEngfuncs.R_BeamGetEntity( pbeam->endEntity );

		if( R_BeamComputePoint( pbeam->endEntity, pbeam->target ))
		{
			if( !pbeam->pFollowModel )
				pbeam->pFollowModel = end->model;
			SetBits( pbeam->flags, FBEAM_ENDVISIBLE );
		}
		else if( !FBitSet( pbeam->flags, FBEAM_FOREVER ))
		{
			ClearBits( pbeam->flags, FBEAM_ENDENTITY );
			pbeam->die = gp_cl->time;
			return false;
		}
		else return false;
	}

	if( FBitSet( pbeam->flags, FBEAM_STARTENTITY ) && !FBitSet( pbeam->flags, FBEAM_STARTVISIBLE ))
		return false;

	return true;
}

// updates the beam (noise, end points, life, the hose fade) and draws it
static void R_BeamDraw( BEAM *pbeam, float frametime )
{
	model_t *model = R_GL3ModelHandle( pbeam->modelIndex );

	SetBits( pbeam->flags, FBEAM_ISACTIVE );

	if( !model || model->type != mod_sprite )
	{
		ClearBits( pbeam->flags, FBEAM_ISACTIVE ); // ignored from now on
		pbeam->die = gp_cl->time;
		return;
	}

	pbeam->freq += frametime;

	// fractal noise; a paused frame keeps the last beam's
	if( frametime != 0.0f )
	{
		rgNoise[0] = 0;
		rgNoise[NOISE_DIVISIONS] = 0;
	}

	if( pbeam->amplitude != 0 && frametime != 0.0f )
	{
		if( FBitSet( pbeam->flags, FBEAM_SINENOISE ))
			SineNoise( rgNoise, NOISE_DIVISIONS );
		else FracNoise( rgNoise, NOISE_DIVISIONS );
	}

	if( FBitSet( pbeam->flags, FBEAM_STARTENTITY | FBEAM_ENDENTITY ))
	{
		vec3_t delta;

		// the attachments must be valid
		if( !R_BeamRecomputeEndpoints( pbeam ))
		{
			ClearBits( pbeam->flags, FBEAM_ISACTIVE );
			return;
		}

		// segments from the new end points
		VectorSubtract( pbeam->target, pbeam->source, delta );
		VectorClear( pbeam->delta );

		if( VectorLength( delta ) > 0.0000001f )
			VectorCopy( delta, pbeam->delta );

		if( pbeam->amplitude >= 0.50f )
			pbeam->segments = (int)( VectorLength( pbeam->delta ) * 0.25f + 3.0f ); // one per 4 pixels
		else pbeam->segments = (int)( VectorLength( pbeam->delta ) * 0.075f + 3.0f ); // one per 16 pixels
	}

	if( pbeam->type == TE_BEAMPOINTS && R_BeamCull( pbeam->source, pbeam->target, false ))
	{
		ClearBits( pbeam->flags, FBEAM_ISACTIVE );
		return;
	}

	// short or inactive beams are not drawn
	if( !FBitSet( pbeam->flags, FBEAM_ISACTIVE ) || VectorLength( pbeam->delta ) < 0.1f )
		return;

	if( FBitSet( pbeam->flags, FBEAM_FADEIN | FBEAM_FADEOUT ))
	{
		// life cycle
		pbeam->t = pbeam->freq + ( pbeam->die - gp_cl->time );
		if( pbeam->t != 0.0f ) pbeam->t = 1.0f - pbeam->freq / pbeam->t;
	}

	if( pbeam->type == TE_BEAMHOSE )
	{
		vec3_t delta, localDir, vecProjection, tmp;
		float flDot, flFade, flDistance;

		VectorSubtract( pbeam->target, pbeam->source, delta );
		VectorNormalize( delta );

		// the player looks along it, away from the source
		flDot = DotProduct( delta, gl3_ri.vforward );
		if( flDot > 0 )
			return;

		flFade = pow( flDot, 10.0f );

		// fades when the player does not look at the source
		VectorSubtract( gl3_ri.rvp.vieworigin, pbeam->source, localDir );
		flDot = DotProduct( delta, localDir );
		VectorScale( delta, flDot, vecProjection );
		VectorSubtract( localDir, vecProjection, tmp );
		flDistance = VectorLength( tmp );

		if( flDistance > 30 )
		{
			flDistance = 1.0f - (( flDistance - 30.0f ) / 64.0f );
			if( flDistance <= 0 ) flFade = 0;
			else flFade *= pow( flDistance, 3.0f );
		}

		if( flFade < ( 1.0f / 255.0f ))
			return;

		pbeam->brightness *= flFade;
	}

	R_BeamRenderMode( FBitSet( pbeam->flags, FBEAM_SOLID ) ? kRenderNormal : kRenderTransAdd );

	if( !R_GL3SpriteTexture( model, (int)( pbeam->frame + pbeam->frameRate * gp_cl->time ) % pbeam->frameCount ))
	{
		ClearBits( pbeam->flags, FBEAM_ISACTIVE );
		return;
	}

	if( pbeam->type == TE_BEAMFOLLOW )
	{
		// Xash3D: the brightness of the head entity
		cl_entity_t *pStart = gEngfuncs.R_BeamGetEntity( pbeam->startEntity );

		if( pStart && pStart->curstate.rendermode != kRenderNormal )
			pbeam->brightness = R_GL3FxBlend( pStart ) / 255.0f;
	}

	if( FBitSet( pbeam->flags, FBEAM_FADEIN ))
		R_BeamColor4f( pbeam->r, pbeam->g, pbeam->b, pbeam->t * pbeam->brightness );
	else if( FBitSet( pbeam->flags, FBEAM_FADEOUT ))
		R_BeamColor4f( pbeam->r, pbeam->g, pbeam->b, ( 1.0f - pbeam->t ) * pbeam->brightness );
	else R_BeamColor4f( pbeam->r, pbeam->g, pbeam->b, pbeam->brightness );

	switch( pbeam->type )
	{
	case TE_BEAMTORUS:
		gl3_wanted.cull = D3DCULL_NONE;
		R_GL3TriBegin( TRI_TRIANGLE_STRIP );
		R_DrawTorus( pbeam->source, pbeam->delta, pbeam->width, pbeam->amplitude, pbeam->freq, pbeam->speed, pbeam->segments );
		R_GL3TriEnd();
		break;
	case TE_BEAMDISK:
		gl3_wanted.cull = D3DCULL_NONE;
		R_GL3TriBegin( TRI_TRIANGLE_STRIP );
		R_DrawDisk( pbeam->source, pbeam->delta, pbeam->width, pbeam->amplitude, pbeam->freq, pbeam->speed, pbeam->segments );
		R_GL3TriEnd();
		break;
	case TE_BEAMCYLINDER:
		gl3_wanted.cull = D3DCULL_NONE;
		R_GL3TriBegin( TRI_TRIANGLE_STRIP );
		R_DrawCylinder( pbeam->source, pbeam->delta, pbeam->width, pbeam->amplitude, pbeam->freq, pbeam->speed, pbeam->segments );
		R_GL3TriEnd();
		break;
	case TE_BEAMPOINTS:
	case TE_BEAMHOSE:
		R_GL3TriBegin( TRI_TRIANGLE_STRIP );
		R_DrawSegs( pbeam->source, pbeam->delta, pbeam->width, pbeam->amplitude, pbeam->freq, pbeam->speed, pbeam->segments, pbeam->flags );
		R_GL3TriEnd();
		break;
	case TE_BEAMFOLLOW:
		R_GL3TriBegin( TRI_QUADS );
		R_DrawBeamFollow( pbeam, frametime );
		R_GL3TriEnd();
		break;
	case TE_BEAMRING:
		gl3_wanted.cull = D3DCULL_NONE;
		R_GL3TriBegin( TRI_TRIANGLE_STRIP );
		R_DrawRing( pbeam->source, pbeam->delta, pbeam->width, pbeam->amplitude, pbeam->freq, pbeam->speed, pbeam->segments );
		R_GL3TriEnd();
		break;
	}

	gl3_wanted.cull = GL3_CULL_FRONT;
}

static void R_BeamSetup( BEAM *pbeam, vec3_t start, vec3_t end, int modelIndex, float life, float width, float amplitude, float brightness, float speed )
{
	model_t *sprite = R_GL3ModelHandle( modelIndex );

	if( !sprite )
		return;

	pbeam->type = BEAM_POINTS;
	pbeam->modelIndex = modelIndex;
	pbeam->frame = 0;
	pbeam->frameRate = 0;
	pbeam->frameCount = sprite->numframes;

	VectorCopy( start, pbeam->source );
	VectorCopy( end, pbeam->target );
	VectorSubtract( end, start, pbeam->delta );

	pbeam->freq = speed * gp_cl->time;
	pbeam->die = life + gp_cl->time;
	pbeam->amplitude = amplitude;
	pbeam->brightness = brightness;
	pbeam->width = width;
	pbeam->speed = speed;

	if( amplitude >= 0.50f )
		pbeam->segments = (int)( VectorLength( pbeam->delta ) * 0.25f + 3.0f ); // one per 4 pixels
	else pbeam->segments = (int)( VectorLength( pbeam->delta ) * 0.075f + 3.0f ); // one per 16 pixels

	pbeam->pFollowModel = NULL;
	pbeam->flags = 0;
}

// a beam entity of the server: the beam lives in its entity state
static void R_BeamDrawCustomEntity( cl_entity_t *ent )
{
	const float amp = ent->curstate.body / 100.0f;
	const float blend = R_GL3FxBlend( ent ) / 255.0f;
	const int beamFlags = ent->curstate.rendermode & 0xF0;
	BEAM beam;

	memset( &beam, 0, sizeof( beam ));
	R_BeamSetup( &beam, ent->origin, ent->curstate.angles, ent->curstate.modelindex, 0, ent->curstate.scale, amp, blend, ent->curstate.animtime );
	beam.frame = (float)(int)ent->curstate.frame; // R_BeamSetAttributes takes an int
	beam.frameRate = ent->curstate.framerate;
	beam.r = ent->curstate.rendercolor.r / 255.0f;
	beam.g = ent->curstate.rendercolor.g / 255.0f;
	beam.b = ent->curstate.rendercolor.b / 255.0f;

	switch( ent->curstate.rendermode & 0x0F )
	{
	case BEAM_ENTPOINT:
		beam.type = TE_BEAMPOINTS;
		if( ent->curstate.sequence )
		{
			SetBits( beam.flags, FBEAM_STARTENTITY );
			beam.startEntity = ent->curstate.sequence;
		}
		if( ent->curstate.skin )
		{
			SetBits( beam.flags, FBEAM_ENDENTITY );
			beam.endEntity = ent->curstate.skin;
		}
		break;
	case BEAM_ENTS:
		beam.type = TE_BEAMPOINTS;
		SetBits( beam.flags, FBEAM_STARTENTITY | FBEAM_ENDENTITY );
		beam.startEntity = ent->curstate.sequence;
		beam.endEntity = ent->curstate.skin;
		break;
	case BEAM_HOSE:
		beam.type = TE_BEAMHOSE;
		break;
	}

	if( FBitSet( beamFlags, BEAM_FSINE ))
		SetBits( beam.flags, FBEAM_SINENOISE );
	if( FBitSet( beamFlags, BEAM_FSOLID ))
		SetBits( beam.flags, FBEAM_SOLID );
	if( FBitSet( beamFlags, BEAM_FSHADEIN ))
		SetBits( beam.flags, FBEAM_SHADEIN );
	if( FBitSet( beamFlags, BEAM_FSHADEOUT ))
		SetBits( beam.flags, FBEAM_SHADEOUT );

	R_BeamDraw( &beam, gl3_tr.frametime );
}

// the solid pass draws the solid beams, the translucent pass the others
static qboolean R_BeamInPass( int flags, int fTrans )
{
	return FBitSet( flags, FBEAM_SOLID ) ? !fTrans : fTrans != 0;
}

/*
==============
CL_DrawBeams
==============
*/
void R_GL3DrawBeams( int fTrans, BEAM *active_beams )
{
	const gl3_drawlist_t *list = gl3_tr.draw_list;

	gl3_wanted.zwrite = fTrans ? FALSE : TRUE;

	// server beams keep everything in their entity
	for( uint i = 0; i < list->num_beams; i++ )
	{
		if( R_BeamInPass( list->beams[i]->curstate.rendermode & 0xF0, fTrans ))
			R_BeamDrawCustomEntity( list->beams[i] );
	}

	for( BEAM *pBeam = active_beams; pBeam; pBeam = pBeam->next )
	{
		if( R_BeamInPass( pBeam->flags, fTrans ))
			R_BeamDraw( pBeam, gp_cl->time - gp_cl->oldtime );
	}

	gl3_wanted.zwrite = TRUE;
}
