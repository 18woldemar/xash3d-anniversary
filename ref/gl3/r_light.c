/*
r_light.c - ref_gl3: lightmaps
Copyright (C) 2026 xash3d-xenon
Lightmap building, light style updates and dynamic light blocks follow ref/gl/gl_rsurf.c,
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
Luxels are ref_gl's: styles summed, scaled, through the light gamma table. Pages are 1024 luxels square and
kept in RAM; each page has two textures. A light style change rebuilds its faces into the RAM page and
leaves a dirty rectangle for both textures. Each frame the texture the previous frame did not use takes its
rectangles and becomes the one to draw with: a lock waits for the GPU to finish with a texture, and the
idle one is finished.

Faces lit by dynamic lights are rebuilt every frame into a 512-luxel page of their own (three textures in
turn) and drawn from vertices with lightmap coordinates moved to that page, as ref_gl's R_BlendLightmaps
does with its dlight block.
*/

#include "r_local.h"
#include "atlas.h"

#define GL3_MAX_PAGES        16
#define GL3_PAGE_RECTS       64
#define GL3_DLIGHT_SIZE      512
#define GL3_DLIGHT_RING      3
#define GL3_MAX_LUXELS       ( 256 * 256 ) // one face

typedef struct
{
	short x, y, w, h;
} gl3_rect_t;

typedef struct
{
	IDirect3DTexture9 *d3d;
	gl3_rect_t          rects[GL3_PAGE_RECTS];
	int                numrects;
	qboolean           whole; // too many rectangles: the whole page
} gl3_pagecopy_t;

typedef struct
{
	uint32_t     *luxels; // GL3_LIGHTMAP_SIZE squared, native 0xAARRGGBB
	gl3_pagecopy_t copies[2];
	int           current;
} gl3_page_t;

static cvar_t *gl_overbright;
static float   gl3_overbright_applied;

static struct
{
	poolhandle_t      pool;
	gl3_page_t         pages[GL3_MAX_PAGES];
	int               numpages;
	atlas_t           atlas;
	const uint16_t   *lightgammatable;

	// dynamic lights
	atlas_t            datlas;
	uint32_t           dluxels[GL3_DLIGHT_SIZE * GL3_DLIGHT_SIZE];
	IDirect3DTexture9 *dtextures[GL3_DLIGHT_RING];
	int                dcurrent;
	qboolean           dused;
	qboolean           dfull;
} gl3_lm;

static uint r_blocklights[GL3_MAX_LUXELS * 3];

/*
==============================================================================

BUILDING

==============================================================================
*/
static qboolean R_GL3LitWater( void )
{
	return FBitSet( gl3_tr.world->flags, FWORLD_HAS_LITWATER ) ? true : false;
}

static int R_GL3LitWaterMinlight( void )
{
	if( R_GL3LitWater() && gl3_tr.world->litwater_minlight >= 0 )
		return gl3_tr.world->litwater_minlight;
	return 0;
}

static float R_GL3LitWaterScale( void )
{
	if( R_GL3LitWater() && gl3_tr.world->litwater_scale >= 0.0f )
		return gl3_tr.world->litwater_scale;
	return 1.0f;
}

// a face has a lightmap unless it is tiled, or water in a map without lit water
qboolean R_GL3SurfaceHasLightmap( const msurface_t *surf )
{
	if( FBitSet( surf->flags, SURF_DRAWTILED ))
		return false;

	if( FBitSet( surf->flags, SURF_DRAWTURB ) && !R_GL3LitWater( ))
		return false;

	return true;
}

// gl_rsurf.c R_AddDynamicLights
static void R_GL3AddDynamicLights( const msurface_t *surf, float sample_size, int smax, int tmax )
{
	const mextrasurf_t *info = surf->info;
	int sample_frac = 1;

	if( !surf->dlightbits )
		return;

	if( FBitSet( surf->texinfo->flags, TEX_WORLD_LUXELS ))
	{
		if( surf->texinfo->faceinfo )
			sample_frac = surf->texinfo->faceinfo->texture_step;
		else if( FBitSet( surf->texinfo->flags, TEX_EXTRA_LIGHTMAP ))
			sample_frac = LM_SAMPLE_EXTRASIZE;
		else sample_frac = LM_SAMPLE_SIZE;
	}

	for( int lnum = 0; lnum < MAX_DLIGHTS; lnum++ )
	{
		const dlight_t *dl = &gp_dlights[lnum];
		vec3_t impact, origin_l;
		float rad, dist, minlight, sl, tl, half;
		int s0, s1, t0, t1;

		if( !FBitSet( surf->dlightbits, BIT( lnum )))
			continue;

		// the light in the brush model's space
		if( !gl3_tr.modelviewIdentity )
			Matrix4x4_VectorITransform( gl3_ri.objectMatrix, dl->origin, origin_l );
		else VectorCopy( dl->origin, origin_l );

		rad = dl->radius;
		dist = PlaneDiff( origin_l, surf->plane );
		rad -= fabs( dist );

		// rad is now the highest intensity on the plane
		minlight = dl->minlight;
		if( rad < minlight )
			continue;

		minlight = rad - minlight;

		if( surf->plane->type < 3 )
		{
			VectorCopy( origin_l, impact );
			impact[surf->plane->type] -= dist;
		}
		else VectorMA( origin_l, -dist, surf->plane->normal, impact );

		sl = DotProduct( impact, info->lmvecs[0] ) + info->lmvecs[0][3] - info->lightmapmins[0];
		tl = DotProduct( impact, info->lmvecs[1] ) + info->lmvecs[1][3] - info->lightmapmins[1];

		// a luxel passes only when both distances are under minlight
		half = ( minlight + 1.0f ) / ( sample_size * sample_frac );
		s0 = Q_max( 0, (int)( sl / sample_size - half ));
		s1 = Q_min( smax - 1, (int)( sl / sample_size + half ));
		t0 = Q_max( 0, (int)( tl / sample_size - half ));
		t1 = Q_min( tmax - 1, (int)( tl / sample_size + half ));

		for( int t = t0; t <= t1; t++ )
		{
			int td = (int)(( tl - sample_size * t ) * sample_frac );

			if( td < 0 )
				td = -td;

			for( int s = s0; s <= s1; s++ )
			{
				int sd = (int)(( sl - sample_size * s ) * sample_frac );
				float d;

				if( sd < 0 )
					sd = -sd;

				d = sd > td ? sd + ( td >> 1 ) : td + ( sd >> 1 );

				if( d < minlight )
				{
					uint *bl = &r_blocklights[( s + t * smax ) * 3];

					bl[0] += ((int)(( rad - d ) * 256 ) * dl->color.r ) / 256;
					bl[1] += ((int)(( rad - d ) * 256 ) * dl->color.g ) / 256;
					bl[2] += ((int)(( rad - d ) * 256 ) * dl->color.b ) / 256;
				}
			}
		}
	}
}

// gl_rsurf.c R_BuildLightMap, into native 0xFFRRGGBB luxels
static void R_GL3BuildLightMap( const msurface_t *surf, uint32_t *dest, int stride, qboolean dynamic )
{
	const mextrasurf_t *info = surf->info;
	const qboolean turb = FBitSet( surf->flags, SURF_DRAWTURB ) ? true : false;
	const qboolean linear_gamma = FBitSet( gp_host->features, ENGINE_LINEAR_GAMMA_SPACE ) ? true : false;
	const int litwater_minlight = R_GL3LitWaterMinlight();
	const float litwater_scale = R_GL3LitWaterScale();
	const int sample_size = gEngfuncs.Mod_SampleSizeForFace( surf );
	const int smax = ( info->lightextents[0] / sample_size ) + 1;
	const int tmax = ( info->lightextents[1] / sample_size ) + 1;
	const int size = smax * tmax;
	// with gl_overbright ref_gl keeps the lightmap unscaled and brightens it in the blend (R_GL3LightmapScale)
	const int lightscale = gl_overbright->value ? 256 : (int)(( pow( 2.0f, 1.0f / v_lightgamma->value ) * 256 ) + 0.5 );
	int map;

	if( size > GL3_MAX_LUXELS )
	{
		gEngfuncs.Con_Printf( S_ERROR "%s: face lightmap too large (%dx%d)\n", __func__, smax, tmax );
		return;
	}

	if( surf->samples && surf->styles[0] < 255 )
	{
		for( map = 0; map < MAXLIGHTMAPS && surf->styles[map] < 255; map++ )
		{
			const uint scale = g_lightstylevalue[surf->styles[map]];
			const color24 *lm = &surf->samples[map * size];

			for( int i = 0; i < size; i++ )
			{
				// the first style sets, the others add
				if( map == 0 )
				{
					r_blocklights[i * 3 + 0] = lm[i].r * scale;
					r_blocklights[i * 3 + 1] = lm[i].g * scale;
					r_blocklights[i * 3 + 2] = lm[i].b * scale;
				}
				else
				{
					r_blocklights[i * 3 + 0] += lm[i].r * scale;
					r_blocklights[i * 3 + 1] += lm[i].g * scale;
					r_blocklights[i * 3 + 2] += lm[i].b * scale;
				}
			}
		}
	}
	else memset( r_blocklights, 0, sizeof( uint ) * size * 3 );

	if( surf->dlightframe == gl3_tr.framecount && dynamic )
		R_GL3AddDynamicLights( surf, (float)sample_size, smax, tmax );

	for( int t = 0; t < tmax; t++ )
	{
		for( int s = 0; s < smax; s++ )
		{
			const uint *bl = &r_blocklights[( s + t * smax ) * 3];
			uint32_t luxel = 0xff000000u;

			for( int i = 0; i < 3; i++ )
			{
				int v = bl[i] * lightscale >> 14;

				// water that the level designer did not light gets a floor
				if( turb )
					v = Q_max( Q_rint( v * litwater_scale ), litwater_minlight );

				if( v > 1023 )
					v = 1023;

				v = linear_gamma ? v >> 2 : gl3_lm.lightgammatable[v] >> 2;
				luxel |= (uint32_t)v << ( 16 - i * 8 );
			}

			dest[t * stride + s] = luxel;
		}
	}
}

static int R_GL3FaceLuxels( const msurface_t *surf, int *smax, int *tmax )
{
	const int sample_size = gEngfuncs.Mod_SampleSizeForFace( surf );

	*smax = ( surf->info->lightextents[0] / sample_size ) + 1;
	*tmax = ( surf->info->lightextents[1] / sample_size ) + 1;
	return sample_size;
}

/*
==============================================================================

PAGES

==============================================================================
*/
static void R_GL3Dirty( gl3_pagecopy_t *copy, int x, int y, int w, int h )
{
	gl3_rect_t *r;

	if( copy->whole )
		return;

	if( copy->numrects == GL3_PAGE_RECTS )
	{
		copy->whole = true;
		copy->numrects = 0;
		return;
	}

	r = &copy->rects[copy->numrects++];
	r->x = x;
	r->y = y;
	r->w = w;
	r->h = h;
}

static void R_GL3DirtyPage( int page, int x, int y, int w, int h )
{
	R_GL3Dirty( &gl3_lm.pages[page].copies[0], x, y, w, h );
	R_GL3Dirty( &gl3_lm.pages[page].copies[1], x, y, w, h );
}

static void R_GL3WriteRows( IDirect3DTexture9 *d3d, const uint32_t *luxels, int size, const gl3_rect_t *rects, int numrects, qboolean whole )
{
	D3DLOCKED_RECT lr;

	R_GL3UnbindD3D( d3d );
	if( FAILED( d3d->LockRect( 0, &lr, NULL, D3DLOCK_NOSYSLOCK )))
	{
		gEngfuncs.Con_Printf( S_ERROR "%s: can't lock a lightmap\n", __func__ );
		return;
	}

	if( whole )
	{
		for( int y = 0; y < size; y++ )
			XMemCpyStreaming_WriteCombined((byte *)lr.pBits + y * lr.Pitch, luxels + y * size, size * sizeof( *luxels ));
	}
	else
	{
		for( int i = 0; i < numrects; i++ )
		{
			const gl3_rect_t *r = &rects[i];

			for( int y = r->y; y < r->y + r->h; y++ )
				XMemCpyStreaming_WriteCombined((byte *)lr.pBits + y * lr.Pitch + r->x * sizeof( *luxels ), luxels + y * size + r->x, r->w * sizeof( *luxels ));
		}
	}

	d3d->UnlockRect( 0 );
}

static IDirect3DTexture9 *R_GL3CreateLightmapTexture( int size )
{
	IDirect3DTexture9 *d3d = NULL;

	if( FAILED( gl3_device->CreateTexture( size, size, 1, 0, D3DFMT_LIN_X8R8G8B8, D3DPOOL_MANAGED, &d3d, NULL )))
		gEngfuncs.Host_Error( "ref_gl3: can't create a %dx%d lightmap\n", size, size );
	return d3d;
}

static void R_GL3NewPage( void )
{
	gl3_page_t *page;

	if( gl3_lm.numpages == GL3_MAX_PAGES )
		gEngfuncs.Host_Error( "%s: too many lightmap pages\n", __func__ );

	page = &gl3_lm.pages[gl3_lm.numpages++];
	if( !page->luxels )
		page->luxels = (uint32_t *)Mem_Malloc( gl3_lm.pool, GL3_LIGHTMAP_SIZE * GL3_LIGHTMAP_SIZE * sizeof( *page->luxels ));

	// unused luxels stay dark, like the reused buffer of ref_gl never reaches a face
	memset( page->luxels, 0, GL3_LIGHTMAP_SIZE * GL3_LIGHTMAP_SIZE * sizeof( *page->luxels ));

	for( int i = 0; i < 2; i++ )
	{
		if( !page->copies[i].d3d )
			page->copies[i].d3d = R_GL3CreateLightmapTexture( GL3_LIGHTMAP_SIZE );
		page->copies[i].numrects = 0;
		page->copies[i].whole = true;
	}

	page->current = 0;
	Atlas_Init( &gl3_lm.atlas, GL3_LIGHTMAP_SIZE );
}

void R_GL3LightmapsNewMap( void )
{
	gl3_lm.numpages = 0;
	gl3_lm.lightgammatable = (const uint16_t *)ENGINE_GET_PARM( PARM_GET_LIGHTGAMMATABLE_PTR );
	gl3_lm.dcurrent = 0;
	gl3_lm.dfull = false; // each map says once that its dynamic lights outgrew the page
}

// gl_rsurf.c GL_CreateSurfaceLightmap; faces without light data still get a (dark) block
void R_GL3CreateSurfaceLightmap( msurface_t *surf, model_t *mod )
{
	int smax, tmax;
	gl3_page_t *page;

	if( !mod->lightdata || !R_GL3SurfaceHasLightmap( surf ))
		return;

	R_GL3FaceLuxels( surf, &smax, &tmax );

	if( gl3_lm.numpages == 0 || !Atlas_AllocBlock( &gl3_lm.atlas, smax, tmax, &surf->light_s, &surf->light_t ))
	{
		R_GL3NewPage();
		if( !Atlas_AllocBlock( &gl3_lm.atlas, smax, tmax, &surf->light_s, &surf->light_t ))
			gEngfuncs.Host_Error( "%s: full\n", __func__ );
	}

	surf->lightmaptexturenum = gl3_lm.numpages - 1;
	page = &gl3_lm.pages[surf->lightmaptexturenum];

	R_UpdateSurfaceCachedLight( surf );
	R_GL3BuildLightMap( surf, page->luxels + surf->light_t * GL3_LIGHTMAP_SIZE + surf->light_s, GL3_LIGHTMAP_SIZE, false );
}

// gamma changed: same blocks, new values
void R_GL3RebuildLightmaps( void )
{
	if( !ENGINE_GET_PARM( PARM_CLIENT_ACTIVE ) || !WORLDMODEL || !gl3_lm.numpages )
		return;

	gl3_lm.lightgammatable = (const uint16_t *)ENGINE_GET_PARM( PARM_GET_LIGHTGAMMATABLE_PTR );
	CL_RunLightStyles( (lightstyle_t *)ENGINE_GET_PARM( PARM_GET_LIGHTSTYLES_PTR ));

	for( int i = 0; i < gp_cl->nummodels; i++ )
	{
		model_t *m = gp_cl->models[i + 1];

		if( !m || m->type != mod_brush || m->name[0] == '*' || !m->lightdata )
			continue;

		for( int j = 0; j < m->numsurfaces; j++ )
		{
			msurface_t *surf = &m->surfaces[j];
			gl3_page_t *page;

			if( !R_GL3SurfaceHasLightmap( surf ))
				continue;

			// a face without a page of its own would write its luxels over page 0
			if( surf->lightmaptexturenum < 0 || surf->lightmaptexturenum >= gl3_lm.numpages )
				continue;

			page = &gl3_lm.pages[surf->lightmaptexturenum];
			R_UpdateSurfaceCachedLight( surf );
			R_GL3BuildLightMap( surf, page->luxels + surf->light_t * GL3_LIGHTMAP_SIZE + surf->light_s, GL3_LIGHTMAP_SIZE, false );
		}
	}

	for( int i = 0; i < gl3_lm.numpages; i++ )
	{
		for( int j = 0; j < 2; j++ )
		{
			gl3_lm.pages[i].copies[j].whole = true;
			gl3_lm.pages[i].copies[j].numrects = 0;
		}
	}
}

// gl_rsurf.c R_CheckLightMap: style changes go into the page; true when dynamic lights touch the face
qboolean R_GL3CheckLightmap( msurface_t *fa )
{
	if( !r_dynamic->value || !gl3_lm.numpages )
		return false;

	// a face the renderer never gave a page (a brush model without light data, or one precached after
	// the map loaded) would write outside the pages below
	if( fa->lightmaptexturenum < 0 || fa->lightmaptexturenum >= gl3_lm.numpages )
		return false;

	if( fa->dlightframe == gl3_tr.framecount )
		return true;

	for( int maps = 0; maps < MAXLIGHTMAPS && fa->styles[maps] != 255; maps++ )
	{
		gl3_page_t *page = &gl3_lm.pages[fa->lightmaptexturenum];
		uint32_t *dest = page->luxels + fa->light_t * GL3_LIGHTMAP_SIZE + fa->light_s;
		int smax, tmax;

		if( g_lightstylevalue[fa->styles[maps]] == fa->cached_light[maps] )
			continue;

		R_GL3FaceLuxels( fa, &smax, &tmax );

		if( smax < 132 && tmax < 132 )
		{
			// the static page takes light styles only; a face with dynamic light on it was answered above
			R_GL3BuildLightMap( fa, dest, GL3_LIGHTMAP_SIZE, false );
		}
		else
		{
			// ref_gl's update buffer is 132 luxels wide and uploads white for larger faces
			smax = Q_min( smax, 132 );
			tmax = Q_min( tmax, 132 );
			for( int t = 0; t < tmax; t++ )
			{
				for( int s = 0; s < smax; s++ )
					dest[t * GL3_LIGHTMAP_SIZE + s] = 0xffffffffu;
			}
		}

		R_UpdateSurfaceCachedLight( fa );
		R_GL3DirtyPage( fa->lightmaptexturenum, fa->light_s, fa->light_t, smax, tmax );
		return false;
	}

	return false;
}

/*
==============================================================================

DYNAMIC LIGHTS

==============================================================================
*/
void R_GL3DynamicBegin( void )
{
	Atlas_Init( &gl3_lm.datlas, GL3_DLIGHT_SIZE );
	gl3_lm.dused = false;
}

// builds the face into this frame's dlight page; false when it is full
qboolean R_GL3DynamicLightmap( msurface_t *fa, float *scale, float offset[2] )
{
	int smax, tmax;
	mextrasurf_t *info = fa->info;

	R_GL3FaceLuxels( fa, &smax, &tmax );

	if( !Atlas_AllocBlock( &gl3_lm.datlas, smax, tmax, &info->dlight_s, &info->dlight_t ))
	{
		if( !gl3_lm.dfull )
			gEngfuncs.Con_Reportf( "ref_gl3: dynamic light page full, some faces keep static light\n" );
		gl3_lm.dfull = true;
		return false;
	}

	R_GL3BuildLightMap( fa, gl3_lm.dluxels + info->dlight_t * GL3_DLIGHT_SIZE + info->dlight_s, GL3_DLIGHT_SIZE, true );
	gl3_lm.dused = true;

	// static page coordinates moved to the dlight page (both keep the half-luxel inset)
	*scale = (float)GL3_LIGHTMAP_SIZE / GL3_DLIGHT_SIZE;
	offset[0] = -( fa->light_s - info->dlight_s ) / (float)GL3_DLIGHT_SIZE;
	offset[1] = -( fa->light_t - info->dlight_t ) / (float)GL3_DLIGHT_SIZE;
	return true;
}

/*
==============================================================================

FRAME

==============================================================================
*/
// before drawing: pages take their updates, the dlight page is uploaded
void R_GL3LightmapsCommit( void )
{
	for( int i = 0; i < gl3_lm.numpages; i++ )
	{
		gl3_page_t *page = &gl3_lm.pages[i];
		const int idle = page->current ^ 1;
		gl3_pagecopy_t *copy = &page->copies[idle];

		if( copy->whole || copy->numrects )
		{
			R_GL3WriteRows( copy->d3d, page->luxels, GL3_LIGHTMAP_SIZE, copy->rects, copy->numrects, copy->whole );
			copy->whole = false;
			copy->numrects = 0;
			page->current = idle;
		}
		else if( page->copies[page->current].whole || page->copies[page->current].numrects )
		{
			page->current = idle; // the idle copy is up to date
		}
	}

	if( gl3_lm.dused )
	{
		gl3_rect_t used = { 0, 0, GL3_DLIGHT_SIZE, (short)gl3_lm.datlas.max_height };

		gl3_lm.dcurrent = ( gl3_lm.dcurrent + 1 ) % GL3_DLIGHT_RING;
		R_GL3WriteRows( gl3_lm.dtextures[gl3_lm.dcurrent], gl3_lm.dluxels, GL3_DLIGHT_SIZE, &used, 1, false );
	}
}

IDirect3DTexture9 *R_GL3LightmapTexture( int page )
{
	if( page == GL3_LIGHTMAP_DYNAMIC )
		return gl3_lm.dtextures[gl3_lm.dcurrent];

	if( page < 0 || page >= gl3_lm.numpages )
		return NULL;

	return gl3_lm.pages[page].copies[gl3_lm.pages[page].current].d3d;
}

/*
==================
R_GL3LightmapScale

The world shader's factor on the lightmap. ref_gl's overbright pass blends DST_COLOR, SRC_COLOR (twice
the product) with the lightmap at 128/192, so light can reach 4/3 of the texture instead of stopping at it.
==================
*/
float R_GL3LightmapScale( void )
{
	return gl_overbright->value ? 4.0f / 3.0f : 1.0f;
}

// at the start of a frame, before anything is recorded: the lightmaps follow a gl_overbright change
void R_GL3CheckOverbright( void )
{
	if( gl_overbright->value == gl3_overbright_applied )
		return;

	gl3_overbright_applied = gl_overbright->value;
	R_GL3RebuildLightmaps();
}

void R_GL3InitLightmaps( void )
{
	gl_overbright = gEngfuncs.Cvar_Get( "gl_overbright", "0", FCVAR_ARCHIVE, "overbrights" );
	gl3_overbright_applied = gl_overbright->value;
	gl3_lm.pool = Mem_AllocPool( "ref_gl3 lightmaps" );
	for( int i = 0; i < GL3_DLIGHT_RING; i++ )
		gl3_lm.dtextures[i] = R_GL3CreateLightmapTexture( GL3_DLIGHT_SIZE );
}

void R_GL3ShutdownLightmaps( void )
{
	for( int i = 0; i < GL3_MAX_PAGES; i++ )
	{
		for( int j = 0; j < 2; j++ )
		{
			IDirect3DTexture9 *d3d = gl3_lm.pages[i].copies[j].d3d;

			if( d3d )
			{
				R_GL3UnbindD3D( d3d );
				d3d->Release();
			}
		}
	}

	for( int i = 0; i < GL3_DLIGHT_RING; i++ )
	{
		if( gl3_lm.dtextures[i] )
		{
			R_GL3UnbindD3D( gl3_lm.dtextures[i] );
			gl3_lm.dtextures[i]->Release();
		}
	}

	Mem_FreePool( &gl3_lm.pool );
	memset( &gl3_lm, 0, sizeof( gl3_lm ));
}
