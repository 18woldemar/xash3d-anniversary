/*
r_sky.c - ref_gl3: the sky box
Copyright (C) 2026 xash3d-xenon
Sky clipping and drawing follow ref/gl/gl_warp.c, Copyright (C) 2010 Uncle Mike

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
Sky faces are never drawn. Each visible one is clipped into the six directions of the box, which leaves a
rectangle per box side; the side is drawn over that rectangle only, at half the far clip distance, depth
tested, after the world.
*/

#include "r_local.h"

#define GL3_MAX_CLIP_VERTS 128

static const int gl3_sky_order[SKYBOX_MAX_SIDES] = { 0, 2, 1, 3, 4, 5 };

static const vec3_t gl3_sky_clip[SKYBOX_MAX_SIDES] =
{
	{  1,  1,  0 },
	{  1, -1,  0 },
	{  0, -1,  1 },
	{  0,  1,  1 },
	{  1,  0,  1 },
	{ -1,  0,  1 }
};

// 1 = s, 2 = t, 3 = distance
static const int gl3_st_to_vec[SKYBOX_MAX_SIDES][3] =
{
	{  3, -1,  2 },
	{ -3,  1,  2 },
	{  1,  3,  2 },
	{ -1, -3,  2 },
	{ -2, -1,  3 }, // 0 degrees yaw, look straight up
	{  2, -1, -3 }  // look straight down
};

// s = [0]/[2], t = [1]/[2]
static const int gl3_vec_to_st[SKYBOX_MAX_SIDES][3] =
{
	{ -2,  3,  1 },
	{  2,  3, -1 },
	{  1,  3,  2 },
	{ -1,  3, -2 },
	{ -2, -1,  3 },
	{ -2,  1, -3 }
};

static int   gl3_sky_textures[SKYBOX_MAX_SIDES];
static float gl3_sky_mins[2][SKYBOX_MAX_SIDES];
static float gl3_sky_maxs[2][SKYBOX_MAX_SIDES];

static void R_GL3SkyPolygon( int nump, const float *vecs )
{
	vec3_t v, av;
	int axis;

	VectorClear( v );
	for( int i = 0; i < nump; i++ )
		VectorAdd( vecs + i * 3, v, v );

	av[0] = fabs( v[0] );
	av[1] = fabs( v[1] );
	av[2] = fabs( v[2] );

	// the side it maps to
	if( av[0] > av[1] && av[0] > av[2] )
		axis = v[0] < 0 ? 1 : 0;
	else if( av[1] > av[2] && av[1] > av[0] )
		axis = v[1] < 0 ? 3 : 2;
	else axis = v[2] < 0 ? 5 : 4;

	for( int i = 0; i < nump; i++, vecs += 3 )
	{
		int j = gl3_vec_to_st[axis][2];
		float dv = j > 0 ? vecs[j - 1] : -vecs[-j - 1];
		float s, t;

		if( dv == 0.0f )
			continue;

		j = gl3_vec_to_st[axis][0];
		s = j < 0 ? -vecs[-j - 1] / dv : vecs[j - 1] / dv;

		j = gl3_vec_to_st[axis][1];
		t = j < 0 ? -vecs[-j - 1] / dv : vecs[j - 1] / dv;

		if( s < gl3_sky_mins[0][axis] ) gl3_sky_mins[0][axis] = s;
		if( t < gl3_sky_mins[1][axis] ) gl3_sky_mins[1][axis] = t;
		if( s > gl3_sky_maxs[0][axis] ) gl3_sky_maxs[0][axis] = s;
		if( t > gl3_sky_maxs[1][axis] ) gl3_sky_maxs[1][axis] = t;
	}
}

static void R_GL3ClipSkyPolygon( int nump, float *vecs, int stage )
{
	float dists[GL3_MAX_CLIP_VERTS + 1];
	int sides[GL3_MAX_CLIP_VERTS + 1];
	vec3_t newv[2][GL3_MAX_CLIP_VERTS + 1];
	int newc[2];
	qboolean front, back;
	float *v;
	int i;

	if( nump > GL3_MAX_CLIP_VERTS )
		gEngfuncs.Host_Error( "%s: MAX_CLIP_VERTS\n", __func__ );

	for( ;; stage++ )
	{
		const float *norm;

		if( stage == SKYBOX_MAX_SIDES )
		{
			// fully clipped
			R_GL3SkyPolygon( nump, vecs );
			return;
		}

		front = back = false;
		norm = gl3_sky_clip[stage];

		for( i = 0, v = vecs; i < nump; i++, v += 3 )
		{
			const float d = DotProduct( v, norm );

			if( d > ON_EPSILON )
			{
				front = true;
				sides[i] = SIDE_FRONT;
			}
			else if( d < -ON_EPSILON )
			{
				back = true;
				sides[i] = SIDE_BACK;
			}
			else sides[i] = SIDE_ON;

			dists[i] = d;
		}

		if( front && back )
			break;
	}

	// clip it
	sides[i] = sides[0];
	dists[i] = dists[0];
	VectorCopy( vecs, vecs + i * 3 );
	newc[0] = newc[1] = 0;

	for( i = 0, v = vecs; i < nump; i++, v += 3 )
	{
		float d;

		switch( sides[i] )
		{
		case SIDE_FRONT:
			VectorCopy( v, newv[0][newc[0]] );
			newc[0]++;
			break;
		case SIDE_BACK:
			VectorCopy( v, newv[1][newc[1]] );
			newc[1]++;
			break;
		case SIDE_ON:
			VectorCopy( v, newv[0][newc[0]] );
			newc[0]++;
			VectorCopy( v, newv[1][newc[1]] );
			newc[1]++;
			break;
		}

		if( sides[i] == SIDE_ON || sides[i + 1] == SIDE_ON || sides[i + 1] == sides[i] )
			continue;

		d = dists[i] / ( dists[i] - dists[i + 1] );
		for( int j = 0; j < 3; j++ )
		{
			const float e = v[j] + d * ( v[j + 3] - v[j] );
			newv[0][newc[0]][j] = e;
			newv[1][newc[1]][j] = e;
		}
		newc[0]++;
		newc[1]++;
	}

	R_GL3ClipSkyPolygon( newc[0], newv[0][0], stage + 1 );
	R_GL3ClipSkyPolygon( newc[1], newv[1][0], stage + 1 );
}

void R_GL3ClearSkyBox( void )
{
	for( int i = 0; i < SKYBOX_MAX_SIDES; i++ )
	{
		gl3_sky_mins[0][i] = gl3_sky_mins[1][i] = 9999999.0f;
		gl3_sky_maxs[0][i] = gl3_sky_maxs[1][i] = -9999999.0f;
	}
}

// room for the wrap-around vertex the clipper adds
void R_GL3AddSkyBoxSurface( const msurface_t *fa )
{
	vec3_t verts[GL3_MAX_CLIP_VERTS + 1];

	for( const glpoly2_t *p = fa->polys; p; p = p->next )
	{
		if( p->numverts > GL3_MAX_CLIP_VERTS )
			continue;

		for( int i = 0; i < p->numverts; i++ )
			VectorSubtract( p->verts[i], gl3_ri.rvp.vieworigin, verts[i] );
		R_GL3ClipSkyPolygon( p->numverts, verts[0], 0 );
	}
}

static void R_GL3SkyVertex( float s, float t, int axis )
{
	const int farclip = (int)gl3_ri.farClip;
	vec3_t b, v;

	b[0] = s * ( farclip >> 1 );
	b[1] = t * ( farclip >> 1 );
	b[2] = farclip >> 1;

	for( int j = 0; j < 3; j++ )
	{
		const int k = gl3_st_to_vec[axis][j];
		v[j] = ( k < 0 ? -b[-k - 1] : b[k - 1] ) + gl3_ri.rvp.vieworigin[j];
	}

	// clamped to the edge against the bilinear seam
	s = bound( 0.0f, ( s + 1.0f ) * 0.5f, 1.0f );
	t = bound( 0.0f, ( t + 1.0f ) * 0.5f, 1.0f );

	R_GL3TriTexCoord2f( s, 1.0f - t );
	R_GL3TriVertex3fv( v );
}

// gl_warp.c R_DrawSkyBox, with the world matrix loaded
void R_GL3DrawSkyBox( void )
{
	R_GL3TriRenderMode( kRenderNormal );
	gl3_wanted.alphatest = FALSE;
	R_GL3TriColor4ub( 255, 255, 255, 255 );

	for( int i = 0; i < SKYBOX_MAX_SIDES; i++ )
	{
		if( gl3_sky_mins[0][i] >= gl3_sky_maxs[0][i] || gl3_sky_mins[1][i] >= gl3_sky_maxs[1][i] )
			continue;

		// no sky: the gray stub
		R_GL3Bind( gl3_sky_textures[gl3_sky_order[i]] ? gl3_sky_textures[gl3_sky_order[i]] : R_GL3FindTexture( REF_GRAY_TEXTURE ));
		R_GL3TriBegin( TRI_QUADS );
		R_GL3SkyVertex( gl3_sky_mins[0][i], gl3_sky_mins[1][i], i );
		R_GL3SkyVertex( gl3_sky_mins[0][i], gl3_sky_maxs[1][i], i );
		R_GL3SkyVertex( gl3_sky_maxs[0][i], gl3_sky_maxs[1][i], i );
		R_GL3SkyVertex( gl3_sky_maxs[0][i], gl3_sky_mins[1][i], i );
		R_GL3TriEnd();
	}

	R_GL3DrawReset();
}

// the engine loaded new sides (NULL: unload); it sets FWORLD_CUSTOM_SKYBOX, which ref_gl clears here
void R_GL3SetupSky( int *textures )
{
	for( int i = 0; i < SKYBOX_MAX_SIDES; i++ )
	{
		if( gl3_sky_textures[i] )
			R_GL3FreeTexture( gl3_sky_textures[i] );
	}

	memset( gl3_sky_textures, 0, sizeof( gl3_sky_textures ));
	if( gl3_tr.world )
		ClearBits( gl3_tr.world->flags, FWORLD_CUSTOM_SKYBOX );

	if( !textures )
		return;

	for( int i = 0; i < SKYBOX_MAX_SIDES; i++ )
		gl3_sky_textures[i] = textures[i];
}
