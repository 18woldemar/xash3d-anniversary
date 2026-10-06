/*
r_world.c - ref_gl3: world and brush surfaces
Copyright (C) 2026 xash3d-xenon
Surface building, visibility, culling and texture animation follow ref/gl/gl_rsurf.c and gl_cull.c,
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
At map load every face of the brush models gets its polygon (ref_gl's glpoly2_t, which is also the vertex
layout here: position, texture st, lightmap st) and all polygons go into one static vertex buffer. Water
faces keep the 64-unit pieces the engine had subdivided at load and warp in the vertex shader.

A frame walks the BSP as ref_gl does and collects the visible faces in texture chains. Each chain becomes
one or more draws (one per animation frame, lightmap page and wave direction): fan indices in RAM, copied
once per frame into one of three index buffers. The console locks a buffer only when the GPU is done with
it, and the one used two frames ago is. That counts views, not displayed frames: the game draws one view
per frame today, and anything that draws a second one (a mirror through rvp.nextView, the menu's player
model through pfnRenderScene, neither of which the console build reaches) has to take the rings with it,
here and in the dynamic lightmap pages.
*/

#include "r_local.h"
#include "xenonshaders/world_VS.h"
#include "xenonshaders/warp_VS.h"
#include "xenonshaders/warpsw_VS.h"
#include "xenonshaders/world_PS.h"
#include "xenonshaders/warpsw_PS.h"
#include "xenonshaders/warpswspot_VS.h"
#include "xenonshaders/warpswspot_PS.h"
#include "xenonshaders/worldspot_VS.h"
#include "xenonshaders/worldspot_PS.h"
#include "xenonshaders/shadow_VS.h"
#include "xenonshaders/shadow_PS.h"

#define GL3_WORLD_MAX_INDICES ( 1 << 18 )
#define GL3_WORLD_IB_RING     3
#define GL3_WORLD_MAX_DRAWS   4096
#define GL3_MAX_CHAIN         16384  // faces of one texture chain split by animation frame
#define GL3_WORLD_MAX_OBJECTS 1024   // entity matrices in a frame
#define GL3_OBJECT_SIZE       40     // object-to-clip rows, the eye plane, the object-to-flashlight rows,
                                    // then the lamp in this object's own space
#define GL3_SUBDIVIDE_SIZE    64
#define GL3_MAX_DYNAMIC_VERTS 65532 // dynamic-lit faces and decals; DrawPrimitiveUP takes fewer than 65535
#define GL3_MAX_WATERALPHA    256
#define GL3_POLYOFFSET_BMODELS 2.0f // the reference's gl_polyoffset_bmodels
#define GL3_POLYOFFSET_DECALS  4.0f // the reference's gl_polyoffset
#define GL3_MAX_DECAL_SURFS    4096
#define GL3_WORLD_LIMIT        262144.0f // a brush entity this far out is a broken number, not a place

typedef struct
{
	msurface_t *surf;
	int         page;
	float       lm[3];
} gl3_decalsurf_t;

typedef struct
{
	int                object;     // index of its object-to-clip matrix
	int                key;        // R_GL3SurfaceKey
	qboolean           reverse;    // turbulent back faces
	gl3_states_t        states;
	float              color[4];
	float              fog[8];     // pixel shader c1, c2
	int                texture;
	int                lightmap;   // page, GL3_LIGHTMAP_DYNAMIC, or -1 for none
	qboolean           dynamic;    // lightmap coordinates on this frame's dlight page
	qboolean           up;         // vertices in dynverts, not indices
	qboolean           decal;
	qboolean           warp;
	float              offset[2];  // scrolling textures
	float              waveheight;
	int                first, count; // indices, or dynverts
} gl3_worlddraw_t;

static const D3DVERTEXELEMENT9 gl3_world_elements[] =
{
	{ 0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
	{ 0, 12, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
	{ 0, 20, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1 },
	D3DDECL_END()
};

static struct
{
	IDirect3DVertexShader9      *vs, *warp_vs, *warpsw_vs, *warpswspot_vs, *spot_vs, *shadow_vs;
	IDirect3DPixelShader9       *ps, *warpsw_ps, *warpswspot_ps, *spot_ps, *shadow_ps;
	IDirect3DVertexDeclaration9 *decl;
	IDirect3DIndexBuffer9       *ib[GL3_WORLD_IB_RING];
	int                          ib_current;
	qboolean                     ib_valid; // this frame's indices reached the buffer
	IDirect3DVertexBuffer9      *vb;
	int                          numverts;

	uint32_t       indices[GL3_WORLD_MAX_INDICES];
	int            numindices;
	qboolean       overflow;
	gl3_worlddraw_t draws[GL3_WORLD_MAX_DRAWS];
	int            numdraws;
	float          objects[GL3_WORLD_MAX_OBJECTS][GL3_OBJECT_SIZE];
	qboolean       fog;    // of the draws being added
	int            numobjects;
	int            object; // of the draws being added, -1 until the first
	int            object_first_draw;
	gl3_states_t    state;  // of the draws being added
	float          color[4];
	qboolean       untextured;

	texture_t     *wateralpha[GL3_MAX_WATERALPHA]; // chains kept for the translucent water pass
	int            numwateralpha;

	gl3_decalsurf_t decalsurfs[GL3_MAX_DECAL_SURFS]; // faces whose decals are drawn after them all
	int            numdecalsurfs;

	msurface_t    *chain_surfs[GL3_MAX_CHAIN];
	int            chain_keys[GL3_MAX_CHAIN];
	float          chain_lm[GL3_MAX_CHAIN][3]; // dynamic light page: scale, offset

	float          dynverts[GL3_MAX_DYNAMIC_VERTS][VERTEXSIZE];
	int            numdynverts;

	msurface_t    *skychain;
	int            skytexturenum;
} gl3_world;

static cvar_t *r_xenon_swwater;

/*
==================
R_GL3Cap

A limit of this renderer was reached and geometry was dropped. Each one says so once in a map, with the
place to stand to see it again: a picture that is missing or wrong in a busy scene is otherwise silent.
==================
*/
enum
{
	GL3_CAP_CHAIN, GL3_CAP_DECALSURFS, GL3_CAP_DECALLIST, GL3_CAP_DYNVERTS, GL3_CAP_OBJECTS, GL3_CAP_WATERALPHA, GL3_CAP_DRAWLIST, GL3_CAP_WORLDASENTITY, GL3_CAP_ENTITYNAN, GL3_NUM_CAPS
};

static uint gl3_caps_reported;

static void R_GL3Cap( int cap )
{
	static const char *const names[GL3_NUM_CAPS] =
	{
		"more faces of one texture than the chain holds",
		"more faces with decals than the frame holds",
		"more decals on one face than the frame holds",
		"more dynamic-lit vertices than the frame holds",
		"more entity matrices than the frame holds",
		"more translucent water textures than the frame holds",
		"a draw that does not describe geometry of this frame", // reported by R_GL3CheckDraws itself
		"an entity whose model is a whole map, not an inline brush model",
		"a brush entity whose place is not a number", // reported above with its numbers
	};

	if( FBitSet( gl3_caps_reported, BIT( cap )))
		return;

	SetBits( gl3_caps_reported, BIT( cap ));
	gEngfuncs.Con_Printf( S_WARN "ref_gl3: %s, at %.0f %.0f %.0f\n", names[cap],
		gl3_ri.rvp.vieworigin[0], gl3_ri.rvp.vieworigin[1], gl3_ri.rvp.vieworigin[2] );
}

// the water of the span renderer: its field has to step even before a water surface is drawn
qboolean R_GL3RipplesWanted( void )
{
	return r_xenon_swwater && r_xenon_swwater->value != 0.0f;
}

/*
==============================================================================

SURFACE BUILDING

==============================================================================
*/
void R_GL3LightmapCoord( const vec3_t v, const msurface_t *surf, float sample_size, vec2_t coords )
{
	const mextrasurf_t *info = surf->info;
	float s, t;

	s = DotProduct( v, info->lmvecs[0] ) + info->lmvecs[0][3] - info->lightmapmins[0];
	s += surf->light_s * sample_size;
	s += sample_size * 0.5f;
	s /= GL3_LIGHTMAP_SIZE * sample_size;

	t = DotProduct( v, info->lmvecs[1] ) + info->lmvecs[1][3] - info->lightmapmins[1];
	t += surf->light_t * sample_size;
	t += sample_size * 0.5f;
	t /= GL3_LIGHTMAP_SIZE * sample_size;

	Vector2Set( coords, s, t );
}

// water keeps raw st: the warp divides by the 64-unit cell size
static void R_GL3TextureCoord( const vec3_t v, const msurface_t *surf, vec2_t coords )
{
	const mtexinfo_t *info = surf->texinfo;
	float s = DotProduct( v, info->vecs[0] );
	float t = DotProduct( v, info->vecs[1] );

	if( !FBitSet( surf->flags, SURF_DRAWTURB ))
	{
		s = ( s + info->vecs[0][3] ) / info->texture->width;
		t = ( t + info->vecs[1][3] ) / info->texture->height;
	}

	Vector2Set( coords, s, t );
}

static void R_GL3EdgePosition( const model_t *mod, const msurface_t *fa, int i, vec3_t vec )
{
	const int lindex = mod->surfedges[fa->firstedge + i];

	if( FBitSet( mod->flags, MODEL_QBSP2 ))
	{
		const medge32_t *pedges = mod->edges32;
		VectorCopy( mod->vertexes[lindex > 0 ? pedges[lindex].v[0] : pedges[-lindex].v[1]].position, vec );
	}
	else
	{
		const medge16_t *pedges = mod->edges16;
		VectorCopy( mod->vertexes[lindex > 0 ? pedges[lindex].v[0] : pedges[-lindex].v[1]].position, vec );
	}
}

static glpoly2_t *R_GL3AllocPoly( model_t *mod, int numverts )
{
	// glpoly2_t already holds one vertex in C++, which leaves room to spare
	return (glpoly2_t *)Mem_Calloc( mod->mempool, sizeof( glpoly2_t ) + numverts * VERTEXSIZE * sizeof( float ));
}

static void R_GL3SubdividePolygon( model_t *mod, msurface_t *warpface, int numverts, float *verts )
{
	vec3_t front[GL3_SUBDIVIDE_SIZE], back[GL3_SUBDIVIDE_SIZE];
	float dist[GL3_SUBDIVIDE_SIZE];
	vec3_t mins, maxs;
	glpoly2_t *poly;
	float *v;
	int i, j;

	if( numverts > GL3_SUBDIVIDE_SIZE - 4 )
		gEngfuncs.Host_Error( "%s: too many vertexes on face ( %i )\n", __func__, numverts );

	ClearBounds( mins, maxs );
	for( i = 0, v = verts; i < numverts; i++, v += 3 )
		AddPointToBounds( v, mins, maxs );

	for( i = 0; i < 3; i++ )
	{
		float m = ( mins[i] + maxs[i] ) * 0.5f;
		int f = 0, b = 0;

		m = GL3_SUBDIVIDE_SIZE * floor( m / GL3_SUBDIVIDE_SIZE + 0.5f );
		if( maxs[i] - m < 8 ) continue;
		if( m - mins[i] < 8 ) continue;

		// cut it
		v = verts + i;
		for( j = 0; j < numverts; j++, v += 3 )
			dist[j] = *v - m;

		// wrap cases
		dist[j] = dist[0];
		v -= i;
		VectorCopy( verts, v );

		v = verts;
		for( j = 0; j < numverts; j++, v += 3 )
		{
			if( dist[j] >= 0 )
			{
				VectorCopy( v, front[f] );
				f++;
			}

			if( dist[j] <= 0 )
			{
				VectorCopy( v, back[b] );
				b++;
			}

			if( dist[j] == 0 || dist[j + 1] == 0 )
				continue;

			if(( dist[j] > 0 ) != ( dist[j + 1] > 0 ))
			{
				// clip point
				float frac = dist[j] / ( dist[j] - dist[j + 1] );
				for( int k = 0; k < 3; k++ )
					front[f][k] = back[b][k] = v[k] + frac * ( v[3 + k] - v[k] );
				f++;
				b++;
			}
		}

		R_GL3SubdividePolygon( mod, warpface, f, front[0] );
		R_GL3SubdividePolygon( mod, warpface, b, back[0] );
		return;
	}

	// the new piece goes first: ref_gl draws them in this order
	poly = R_GL3AllocPoly( mod, numverts );
	poly->next = warpface->polys;
	poly->flags = warpface->flags;
	warpface->polys = poly;
	poly->numverts = numverts;

	for( i = 0; i < numverts; i++, verts += 3 )
	{
		VectorCopy( verts, poly->verts[i] );
		R_GL3TextureCoord( verts, warpface, &poly->verts[i][3] );
	}
}

// the engine's call for water faces while it loads a map (before lighting data is there)
void R_GL3SubdivideSurface( model_t *mod, msurface_t *fa )
{
	vec3_t verts[GL3_SUBDIVIDE_SIZE];

	for( int i = 0; i < fa->numedges; i++ )
		R_GL3EdgePosition( mod, fa, i, verts[i] );

	R_GL3SubdividePolygon( mod, fa, fa->numedges, verts[0] );
}

void R_GL3BuildWaterLightmapCoords( model_t *mod, msurface_t *fa )
{
	float sample_size;

	if( !fa->texinfo || !fa->texinfo->texture )
		return;

	sample_size = gEngfuncs.Mod_SampleSizeForFace( fa );

	for( glpoly2_t *poly = fa->polys; poly; poly = poly->next )
	{
		for( int i = 0; i < poly->numverts; i++ )
		{
			vec3_t v;
			VectorCopy( poly->verts[i], v );
			R_GL3LightmapCoord( v, fa, sample_size, &poly->verts[i][5] );
		}
	}
}

// gl_rsurf.c GL_BuildPolygonFromSurface with gl_keeptjunctions 1
static void R_GL3BuildPolygon( model_t *mod, msurface_t *fa )
{
	float sample_size;
	glpoly2_t *poly;

	if( !fa->texinfo || !fa->texinfo->texture )
		return;

	if( FBitSet( fa->flags, SURF_CONVEYOR ) && fa->texinfo->texture->gl_texturenum != 0 )
	{
		// scrolling speed is relative to the BSP texture size
		gl3_texture_t *glt = R_GL3GetTexture( fa->texinfo->texture->gl_texturenum );
		glt->srcWidth = fa->texinfo->texture->width;
		glt->srcHeight = fa->texinfo->texture->height;
	}

	sample_size = gEngfuncs.Mod_SampleSizeForFace( fa );

	// map changes rebuild in place (the memory comes from the model's pool)
	poly = fa->polys;
	fa->polys = NULL;
	poly = (glpoly2_t *)Mem_Realloc( mod->mempool, poly, sizeof( glpoly2_t ) + fa->numedges * VERTEXSIZE * sizeof( float ));
	poly->next = NULL;
	poly->flags = fa->flags;
	poly->numverts = fa->numedges;
	fa->polys = poly;

	for( int i = 0; i < fa->numedges; i++ )
	{
		R_GL3EdgePosition( mod, fa, i, poly->verts[i] );
		R_GL3TextureCoord( poly->verts[i], fa, &poly->verts[i][3] );
		R_GL3LightmapCoord( poly->verts[i], fa, sample_size, &poly->verts[i][5] );
	}
}

static int R_GL3SurfaceVertices( const msurface_t *fa )
{
	int count = 0;

	for( const glpoly2_t *p = fa->polys; p; p = p->next )
		count += p->numverts;

	return count;
}

static void R_GL3FreeWorldBuffer( void )
{
	if( !gl3_world.vb )
		return;

	R_GL3DrawReset();
	gl3_device->SetStreamSource( 0, NULL, 0, 0 );
	gl3_world.vb->Release();
	gl3_world.vb = NULL;
	gl3_world.numverts = 0;
}

static qboolean R_GL3BrushModelForMap( const model_t *m )
{
	return m && m->type == mod_brush && m->name[0] != '*';
}

void R_GL3WorldNewMap( void )
{
	model_t *world = WORLDMODEL;
	int total = 0, filled = 0;
	byte *dst;

	R_GL3FreeWorldBuffer();
	gl3_caps_reported = 0;
	gl3_world.overflow = false;
	gl3_world.skychain = NULL;
	gl3_world.skytexturenum = -1;

	// clear out efrags in case the level hasn't been reloaded
	for( int i = 0; i < world->numleafs; i++ )
		world->leafs[i + 1].efrags = NULL;

	for( int i = 0; i < world->numtextures; i++ )
	{
		texture_t *tx = world->textures[i];

		if( !tx )
			continue;

		if( !Q_strncmp( tx->name, "sky", 3 ) && tx->width == tx->height * 2 )
			gl3_world.skytexturenum = i;
		tx->texturechain = NULL;
	}

	CL_RunLightStyles( (lightstyle_t *)ENGINE_GET_PARM( PARM_GET_LIGHTSTYLES_PTR ));
#if XASH_XENON
	Xenon_Phase( "new map: lightmaps" );
#endif
	R_GL3LightmapsNewMap();
#if XASH_XENON
	Xenon_Phase( "new map: surfaces" );
#endif

	for( int i = 0; i < gp_cl->nummodels; i++ )
	{
		model_t *m = gp_cl->models[i + 1];

		if( !R_GL3BrushModelForMap( m ))
			continue;

		for( int j = 0; j < m->numsurfaces; j++ )
		{
			msurface_t *surf = &m->surfaces[j];

			surf->pdecals = NULL;
			surf->visframe = 0;
			R_GL3CreateSurfaceLightmap( surf, m );

			if( FBitSet( surf->flags, SURF_DRAWTURB ))
				R_GL3BuildWaterLightmapCoords( m, surf );
			else R_GL3BuildPolygon( m, surf );

			surf->info->firstvertex = total;
			surf->info->numverts = R_GL3SurfaceVertices( surf );
			total += surf->info->numverts;
		}

		for( int j = 0; j < m->numleafs; j++ )
			m->leafs[j + 1].visframe = 0;
		for( int j = 0; j < m->numnodes; j++ )
			m->nodes[j].visframe = 0;
	}

	if( total == 0 )
		return;

	if( FAILED( gl3_device->CreateVertexBuffer( total * sizeof( float ) * VERTEXSIZE, 0, 0, D3DPOOL_MANAGED, &gl3_world.vb, NULL )))
	{
		gEngfuncs.Host_Error( "%s: can't create a vertex buffer of %d vertices\n", __func__, total );
		return;
	}

	// the polygon vertex is the buffer's vertex; locked memory only takes the streaming copy
	if( FAILED( gl3_world.vb->Lock( 0, 0, (void **)&dst, 0 )))
	{
		gEngfuncs.Host_Error( "%s: can't lock the world vertex buffer\n", __func__ );
		return;
	}
	for( int i = 0; i < gp_cl->nummodels; i++ )
	{
		model_t *m = gp_cl->models[i + 1];

		if( !R_GL3BrushModelForMap( m ))
			continue;

		for( int j = 0; j < m->numsurfaces; j++ )
		{
			for( glpoly2_t *p = m->surfaces[j].polys; p; p = p->next )
			{
				const size_t bytes = p->numverts * VERTEXSIZE * sizeof( float );
				XMemCpyStreaming_WriteCombined( dst + filled, p->verts[0], bytes );
				filled += bytes;
			}
		}
	}
	gl3_world.vb->Unlock();

	gl3_world.numverts = total;
	gEngfuncs.Con_Reportf( "ref_gl3: world vertex buffer %d vertices, %d KB\n", total, filled >> 10 );
}

/*
==============================================================================

VISIBILITY

==============================================================================
*/
static void R_GL3MarkLeaves( void )
{
	model_t *world = WORLDMODEL;
	qboolean novis = false, force = false;
	vec3_t test;

	if( gl3_tr.fResetVis )
	{
		gl3_tr.fResetVis = false;
		gl3_ri.viewleaf = NULL;
	}

	VectorCopy( gl3_ri.rvp.vieworigin, test );

	if( gl3_ri.viewleaf != NULL )
	{
		mleaf_t *leaf;

		// merge two leafs that can be a crossed-line contents
		test[2] += gl3_ri.viewleaf->contents == CONTENTS_EMPTY ? -16.0f : 16.0f;
		leaf = gEngfuncs.Mod_PointInLeaf( test, world->nodes, world );

		if( leaf->contents != CONTENTS_SOLID && gl3_ri.viewleaf != leaf )
			force = true;
	}

	if( gl3_ri.viewleaf == gl3_ri.oldviewleaf && gl3_ri.viewleaf != NULL && !force )
		return;

	gl3_ri.oldviewleaf = gl3_ri.viewleaf;
	gl3_tr.visframecount++;

	if( !gl3_ri.viewleaf || !world->visdata )
		novis = true;

	gEngfuncs.R_FatPVS( gl3_ri.rvp.vieworigin, r_pvs_radius->value, gl3_ri.visbytes, false, novis );
	if( force && !novis )
		gEngfuncs.R_FatPVS( test, r_pvs_radius->value, gl3_ri.visbytes, true, novis );
	gl3_tr.visValid = true;

	for( int i = 0; i < world->numleafs; i++ )
	{
		mnode_t *node;

		if( !CHECKVISBIT( gl3_ri.visbytes, i ))
			continue;

		for( node = (mnode_t *)&world->leafs[i + 1]; node; node = node->parent )
		{
			if( node->visframe == gl3_tr.visframecount )
				break;
			node->visframe = gl3_tr.visframecount;
		}
	}
}

// gl_cull.c R_CullSurface with GL_FRONT face culling
int R_GL3CullSurface( const msurface_t *surf, uint clipflags )
{
	const cl_entity_t *e = gl3_ri.currententity;
	float dist;

	if( surf->visframe != gl3_tr.framecount && e == R_GL3EntityByIndex( 0 ))
		return GL3_CULL_VISFRAME;

	if( !surf->texinfo || !surf->texinfo->texture )
		return GL3_CULL_OTHER;

	if( !VectorIsNull( surf->plane->normal ))
	{
		dist = PlaneDiff( gl3_tr.modelorg, surf->plane );
		if( FBitSet( surf->flags, SURF_PLANEBACK ))
			dist = -dist;

		if( dist <= BACKFACE_EPSILON )
			return GL3_CULL_BACKSIDE;
	}

	// only static entities can be culled by frustum
	if( clipflags && VectorIsNull( e->origin ) && VectorIsNull( e->angles ) && R_GL3CullBox( surf->info->mins, surf->info->maxs, clipflags ))
		return GL3_CULL_FRUSTUM;

	return GL3_CULL_VISIBLE;
}

static void R_GL3RecursiveWorldNode( mnode_t *node, uint clipflags )
{
	model_t *world = WORLDMODEL;

	while( node->contents != CONTENTS_SOLID && node->visframe == gl3_tr.visframecount )
	{
		int side, first, count;

		for( int i = 0; clipflags && i < GL3_FRUSTUM_PLANES; i++ )
		{
			int clipped;

			if( !FBitSet( clipflags, BIT( i )))
				continue;

			clipped = BOX_ON_PLANE_SIDE( node->minmaxs, node->minmaxs + 3, &gl3_ri.frustum.planes[i] );
			if( clipped == 2 )
				return;
			if( clipped == 1 )
				ClearBits( clipflags, BIT( i ));
		}

		if( node->contents < 0 )
		{
			mleaf_t *leaf = (mleaf_t *)node;
			msurface_t **mark = leaf->firstmarksurface;

			for( int i = 0; i < leaf->nummarksurfaces; i++ )
				mark[i]->visframe = gl3_tr.framecount;

			// static entities in this leaf
			if( leaf->efrags )
				gEngfuncs.R_StoreEfrags( &leaf->efrags, gl3_tr.realframecount );
			return;
		}

		// front side first
		side = PlaneDiff( gl3_tr.modelorg, node->plane ) >= 0.0f ? 0 : 1;
		R_GL3RecursiveWorldNode( node_child( node, side, world ), clipflags );

		first = node_firstsurface( node, world );
		count = node_numsurfaces( node, world );

		for( int i = first; i < first + count; i++ )
		{
			msurface_t *surf = &world->surfaces[i];

			if( R_GL3CullSurface( surf, clipflags ))
				continue;

			if( FBitSet( surf->flags, SURF_DRAWSKY ))
			{
				surf->texturechain = gl3_world.skychain;
				gl3_world.skychain = surf;
			}
			else
			{
				surf->texturechain = surf->texinfo->texture->texturechain;
				surf->texinfo->texture->texturechain = surf;
			}
		}

		node = node_child( node, !side, world );
	}
}

/*
==============================================================================

DRAW LISTS

==============================================================================
*/
// gl_rsurf.c R_TextureAnimation
texture_t *R_GL3TextureAnimation( msurface_t *s )
{
	texture_t *base = s->texinfo->texture;
	int relative, count = 0;

	if( gl3_ri.currententity && gl3_ri.currententity->curstate.frame && base->alternate_anims )
		base = base->alternate_anims;

	if( !base->anim_total )
		return base;

	if( base->name[0] == '-' )
	{
		int tx = (int)(( s->texturemins[0] + ( base->width << 16 )) / base->width ) % MOD_FRAMES;
		int ty = (int)(( s->texturemins[1] + ( base->height << 16 )) / base->height ) % MOD_FRAMES;

		relative = rtable[tx][ty] % base->anim_total;
	}
	else
	{
		// Quake textures animate at 10 frames per second
		int speed = FBitSet( R_GL3GetTexture( base->gl_texturenum )->flags, TF_QUAKEPAL ) ? 10 : 20;
		relative = (int)( gp_cl->time * speed ) % base->anim_total;
	}

	while( base->anim_min > relative || base->anim_max <= relative )
	{
		base = base->anim_next;

		if( !base || ++count > MOD_FRAMES )
			return s->texinfo->texture;
	}

	return base;
}

// gl_rsurf.c DrawGLPoly: the offset of a scrolling texture
static void R_GL3ConveyorOffset( const cl_entity_t *e, int texture, float *offset )
{
	float speed, rate, sy, cy, s, t;

	if( e == R_GL3EntityByIndex( 0 ) && FBitSet( gp_host->features, ENGINE_QUAKE_COMPATIBLE ))
	{
		speed = -35.0f;
	}
	else
	{
		speed = ( e->curstate.rendercolor.g << 8 | e->curstate.rendercolor.b ) / 16.0f;
		if( e->curstate.rendercolor.r )
			speed = -speed;
	}

	// a face whose texture is missing draws with *white, whose width is 0
	rate = fabs( speed ) / (float)Q_max( 1, R_GL3GetTexture( texture )->srcWidth );
	SinCos(( speed >= 0 ? 180.0f : 0.0f ) * ( M_PI_F / 180.0f ), &sy, &cy );
	s = gp_cl->time * cy * rate;
	t = gp_cl->time * sy * rate;

	// positive, then into [0,1)
	if( s < 0.0f ) s += 1.0f + -(int)s;
	if( t < 0.0f ) t += 1.0f + -(int)t;
	offset[0] = s - (int)s;
	offset[1] = t - (int)t;
}

static void R_GL3EmitSurface( const msurface_t *s, qboolean reverse )
{
	int base = s->info->firstvertex;

	for( const glpoly2_t *p = s->polys; p; p = p->next )
	{
		const int triangles = p->numverts - 2;
		uint32_t *out;

		if( triangles <= 0 )
		{
			base += p->numverts;
			continue;
		}

		if( gl3_world.numindices + triangles * 3 > GL3_WORLD_MAX_INDICES )
		{
			if( !gl3_world.overflow )
				gEngfuncs.Con_Printf( S_ERROR "ref_gl3: more than %d world indices in a frame\n", GL3_WORLD_MAX_INDICES );
			gl3_world.overflow = true;
			return;
		}

		out = &gl3_world.indices[gl3_world.numindices];
		for( int i = 1; i <= triangles; i++, out += 3 )
		{
			out[0] = base;
			out[1] = base + ( reverse ? i + 1 : i );
			out[2] = base + ( reverse ? i : i + 1 );
		}

		gl3_world.numindices += triangles * 3;
		base += p->numverts;
	}
}

// gl_rsurf.c R_HasLightmap. Blended entities never show one: ref_gl's lightmap pass tests depth for equality
// and they write none.
static qboolean R_GL3EntityHasLightmap( const cl_entity_t *e )
{
	if( r_fullbright->value || !WORLDMODEL->lightdata )
		return false;

	if( FBitSet( e->curstate.effects, EF_FULLBRIGHT ))
		return false;

	switch( e->curstate.rendermode )
	{
	case kRenderNormal:
	case kRenderTransAlpha:
		return true;
	}

	return false;
}

// key bits: texture 0-15, lightmap page + 1 at 16-23, wave direction 24
static int R_GL3SurfaceKey( msurface_t *s, int index, qboolean lightmaps )
{
	int key = R_GL3TextureAnimation( s )->gl_texturenum & 0xffff;
	int page = -1;

	// ref_gl checks light styles for every face it draws a lightmap pass for
	if( s->polys && R_GL3SurfaceHasLightmap( s ))
	{
		const qboolean dynamic = R_GL3CheckLightmap( s );

		if( lightmaps )
		{
			page = s->lightmaptexturenum;
			if( dynamic && R_GL3DynamicLightmap( s, &gl3_world.chain_lm[index][0], &gl3_world.chain_lm[index][1] ))
				page = GL3_LIGHTMAP_DYNAMIC;
		}
	}

	key |= ( page + 1 ) << 16;

	// the wave goes down when the water is above the view
	if( FBitSet( s->flags, SURF_DRAWTURB ) && s->polys && s->polys->verts[0][2] >= gl3_ri.rvp.vieworigin[2] )
		key |= 1 << 24;

	return key;
}

// a dynamic-lit face as triangles, lightmap coordinates moved to this frame's dlight page
static void R_GL3EmitDynamicSurface( const msurface_t *s, const float *lm, qboolean reverse )
{
	for( const glpoly2_t *p = s->polys; p; p = p->next )
	{
		for( int i = 1; i + 1 < p->numverts; i++ )
		{
			const int corner[3] = { 0, reverse ? i + 1 : i, reverse ? i : i + 1 };

			if( gl3_world.numdynverts + 3 > GL3_MAX_DYNAMIC_VERTS )
			{
				R_GL3Cap( GL3_CAP_DYNVERTS );
				return;
			}

			for( int j = 0; j < 3; j++ )
			{
				float *v = gl3_world.dynverts[gl3_world.numdynverts++];

				memcpy( v, p->verts[corner[j]], sizeof( float ) * 5 );
				v[5] = p->verts[corner[j]][5] * lm[0] + lm[1];
				v[6] = p->verts[corner[j]][6] * lm[0] + lm[2];
			}
		}
	}
}

static void R_GL3Emit( const gl3_worlddraw_t *d, int index, qboolean reverse )
{
	if( d->dynamic )
		R_GL3EmitDynamicSurface( gl3_world.chain_surfs[index], gl3_world.chain_lm[index], reverse );
	else R_GL3EmitSurface( gl3_world.chain_surfs[index], reverse );
}

static void R_GL3EndDraw( gl3_worlddraw_t *d )
{
	d->count = ( d->up ? gl3_world.numdynverts : gl3_world.numindices ) - d->first;
}

static const float gl3_static_lm[3] = { 1.0f, 0.0f, 0.0f };

static void R_GL3SetDrawFogClass( gl3_worlddraw_t *d, int fogclass );
static int R_GL3FogClass( const msurface_t *s, int page, qboolean decal );

// the face's lightmap page and coordinate move, from its key
static void R_GL3RememberDecals( msurface_t *s, int key, const float *lm )
{
	gl3_decalsurf_t *ds;

	if( !s->pdecals )
		return;

	if( gl3_world.numdecalsurfs >= GL3_MAX_DECAL_SURFS )
	{
		R_GL3Cap( GL3_CAP_DECALSURFS );
		return;
	}

	ds = &gl3_world.decalsurfs[gl3_world.numdecalsurfs++];
	ds->surf = s;
	ds->page = (( key >> 16 ) & 0xff ) - 1;
	memcpy( ds->lm, ds->page == GL3_LIGHTMAP_DYNAMIC ? lm : gl3_static_lm, sizeof( ds->lm ));
}

// gl_decals.c DrawSurfaceDecals: the face's decals in order, blended, with its lightmap
static void R_GL3EmitDecals( msurface_t *s, int page, const float *lm, const gl3_states_t *st, const float color[4], qboolean reverse )
{
	decal_t *list[256];
	int count = 0;

	for( decal_t *p = s->pdecals; p; p = p->pnext )
	{
		if( count == (int)ARRAYSIZE( list ))
		{
			R_GL3Cap( GL3_CAP_DECALLIST );
			break;
		}

		if( p->texture )
			list[count++] = p;
	}

	for( int n = 0; n < count; n++ )
	{
		decal_t *p = list[reverse ? count - 1 - n : n];
		const qboolean premultiplied = FBitSet( R_GL3GetTexture( p->texture )->flags, TF_PREMULTIPLIED ) ? true : false;
		const int key = p->texture | (( page + 1 ) << 16 ) | ( premultiplied << 25 );
		gl3_states_t decal_states = *st;
		gl3_worlddraw_t *d = NULL;
		const float *v;
		int numverts;

		v = R_GL3DecalVerts( p, s, &numverts );
		if( numverts < 3 )
			continue;

		decal_states.src = premultiplied ? D3DBLEND_ONE : D3DBLEND_SRCALPHA;
		decal_states.dst = D3DBLEND_INVSRCALPHA;

		// the previous decal draw goes on when nothing differs
		if( gl3_world.numdraws > gl3_world.object_first_draw )
		{
			d = &gl3_world.draws[gl3_world.numdraws - 1];
			if( d->key != key || !d->decal || memcmp( &d->states, &decal_states, sizeof( decal_states )) || memcmp( d->color, color, sizeof( d->color )))
				d = NULL;
		}

		if( !d )
		{
			if( gl3_world.numdraws >= GL3_WORLD_MAX_DRAWS || gl3_world.object < 0 )
				return;

			d = &gl3_world.draws[gl3_world.numdraws++];
			memset( d, 0, sizeof( *d ));
			d->object = gl3_world.object;
			d->key = key;
			d->states = decal_states;
			Vector4Copy( color, d->color );
			d->texture = p->texture;
			d->lightmap = page;
			d->dynamic = page == GL3_LIGHTMAP_DYNAMIC;
			d->up = true;
			d->decal = true;
			d->first = gl3_world.numdynverts;
			R_GL3SetDrawFogClass( d, R_GL3FogClass( s, page, true ));
		}

		for( int i = 1; i + 1 < numverts; i++ )
		{
			const int corner[3] = { 0, i, i + 1 };

			if( gl3_world.numdynverts + 3 > GL3_MAX_DYNAMIC_VERTS )
			{
				R_GL3Cap( GL3_CAP_DYNVERTS );
				break;
			}

			for( int j = 0; j < 3; j++ )
			{
				float *out = gl3_world.dynverts[gl3_world.numdynverts++];
				const float *in = v + corner[j] * VERTEXSIZE;

				memcpy( out, in, sizeof( float ) * 5 );
				out[5] = in[5] * lm[0] + lm[1];
				out[6] = in[6] * lm[0] + lm[2];
			}
		}

		R_GL3EndDraw( d );
	}
}

static void R_GL3DecalStates( gl3_states_t *st, qboolean single )
{
	*st = gl3_world.state;
	st->slopebias = -1.0f;
	st->depthbias = -GL3_POLYOFFSET_DECALS / 16777216.0f;

	switch( gl3_ri.currententity->curstate.rendermode )
	{
	case kRenderNormal:
	case kRenderTransAlpha:
		st->blend = TRUE;
		st->zwrite = FALSE;
		if( single )
			st->alphatest = FALSE;
		break;
	case kRenderTransTexture:
	case kRenderTransAdd:
		st->cull = D3DCULL_NONE;
		break;
	}
}

// the decals of the faces collected since the last call (ref_gl's DrawDecalsBatch)
static void R_GL3AddDecalBatch( void )
{
	gl3_states_t st;

	if( !gl3_world.numdecalsurfs )
		return;

	R_GL3DecalStates( &st, false );
	for( int i = 0; i < gl3_world.numdecalsurfs; i++ )
	{
		const gl3_decalsurf_t *ds = &gl3_world.decalsurfs[i];
		R_GL3EmitDecals( ds->surf, ds->page, ds->lm, &st, gl3_world.color, false );
	}

	gl3_world.numdecalsurfs = 0;
}

enum
{
	GL3_FOG_NONE = 0,
	GL3_FOG_LIT,       // texture and lightmap terms fogged with the pass-corrected colour
	GL3_FOG_CORRECTED, // texture term with the pass-corrected colour
	GL3_FOG_PLAIN,     // texture term with the fog colour
};

void R_GL3WorldSetFog( qboolean fog )
{
	gl3_world.fog = fog;
}

// gl_rsurf.c GL_SetupFogColorForSurfaces: each of the two passes fogs with sqrt of the colour
static void R_GL3SetDrawFogClass( gl3_worlddraw_t *d, int fogclass )
{
	const float fogalpha = gl3_ri.fogEndInv;
	vec3_t corrected;

	memset( d->fog, 0, sizeof( d->fog ));
	if( !gl3_world.fog || fogclass == GL3_FOG_NONE )
		return;

	for( int i = 0; i < 3; i++ )
		corrected[i] = sqrt( gl3_ri.fogColor[i] );

	d->fog[3] = fogalpha;
	if( fogclass == GL3_FOG_PLAIN )
	{
		VectorCopy( gl3_ri.fogColor, d->fog );
		return;
	}

	VectorCopy( corrected, d->fog );
	if( fogclass == GL3_FOG_LIT )
	{
		VectorCopy( corrected, d->fog + 4 );
		d->fog[7] = 1.0f;
	}
}

// which fog colour ref_gl would have for this face in this render mode
static int R_GL3FogClass( const msurface_t *s, int page, qboolean decal )
{
	switch( gl3_ri.currententity->curstate.rendermode )
	{
	case kRenderTransAdd:
		return GL3_FOG_NONE;
	case kRenderTransTexture:
		return GL3_FOG_PLAIN;
	}

	if( page >= 0 )
		return GL3_FOG_LIT;

	if( !decal && FBitSet( s->flags, SURF_DRAWTURB | SURF_DRAWTILED ))
		return GL3_FOG_PLAIN;

	return GL3_FOG_CORRECTED;
}

// the draw state for the draws added from now on: the entity's render mode (the world's by default)
void R_GL3WorldSetState( const gl3_states_t *states, const float color[4] )
{
	gl3_world.state = *states;
	Vector4Copy( color, gl3_world.color );
}

// a new draw for faces with this key, using the current object and state
static gl3_worlddraw_t *R_GL3StartDraw( int key, const msurface_t *s, qboolean reverse )
{
	cl_entity_t *e = gl3_ri.currententity;
	gl3_worlddraw_t *d;

	if( gl3_world.object < 0 )
	{
		if( gl3_world.numobjects >= GL3_WORLD_MAX_OBJECTS )
		{
			R_GL3Cap( GL3_CAP_OBJECTS );
			return NULL;
		}
		gl3_world.object = gl3_world.numobjects++;
		R_GL3ClipMatrix( gl3_ri.objectClipMatrix, gl3_world.objects[gl3_world.object] );
		Vector4Copy( gl3_ri.modelviewMatrix[2], gl3_world.objects[gl3_world.object] + 16 );
		if( R_GL3FlashlightActive( ))
		{
			float *lamp = gl3_world.objects[gl3_world.object] + 36;

			R_GL3FlashlightObject( gl3_ri.objectMatrix, gl3_world.objects[gl3_world.object] + 20 );

			// The shader measures the distance to the lamp from the position the vertex shader hands it,
			// and for a brush entity that position is in the model's own space: a door or a lift would
			// otherwise be lit as if the lamp stood where the world's origin is. r_light.c moves dlights
			// the same way.
			Matrix4x4_VectorITransform( gl3_ri.objectMatrix, R_GL3FlashlightLamp( ), lamp );
			lamp[3] = R_GL3FlashlightLamp( )[3];
		}
	}

	if( gl3_world.numdraws >= GL3_WORLD_MAX_DRAWS )
	{
		if( !gl3_world.overflow )
			gEngfuncs.Con_Printf( S_ERROR "ref_gl3: more than %d world draws in a frame\n", GL3_WORLD_MAX_DRAWS );
		gl3_world.overflow = true;
		return NULL;
	}

	d = &gl3_world.draws[gl3_world.numdraws++];
	d->object = gl3_world.object;
	d->key = key;
	d->reverse = reverse;
	d->states = gl3_world.state;
	Vector4Copy( gl3_world.color, d->color );
	d->texture = gl3_world.untextured ? gl3_white_texture : key & 0xffff;
	d->lightmap = (( key >> 16 ) & 0xff ) - 1;
	d->dynamic = d->lightmap == GL3_LIGHTMAP_DYNAMIC;
	d->up = d->dynamic;
	d->decal = false;
	d->warp = FBitSet( s->flags, SURF_DRAWTURB ) ? true : false;
	d->waveheight = d->warp ? ( FBitSet( key, 1 << 24 ) ? -e->curstate.scale : e->curstate.scale ) : 0.0f;
	d->first = d->up ? gl3_world.numdynverts : gl3_world.numindices;
	d->count = 0;

	if( FBitSet( s->flags, SURF_CONVEYOR ))
		R_GL3ConveyorOffset( e, key & 0xffff, d->offset );
	else d->offset[0] = d->offset[1] = 0.0f;

	R_GL3SetDrawFogClass( d, R_GL3FogClass( s, d->lightmap, false ));
	return d;
}

// the draws added from now on use the current object matrix
void R_GL3WorldNewObject( void )
{
	gl3_world.object = -1;
	gl3_world.object_first_draw = gl3_world.numdraws;
}

// a chain of faces with one BSP texture becomes a draw per animation frame and wave direction
void R_GL3AddChain( msurface_t *chain, qboolean reverse )
{
	const qboolean lightmaps = R_GL3EntityHasLightmap( gl3_ri.currententity );
	int count = 0;

	for( msurface_t *s = chain; s; s = s->texturechain, count++ )
	{
		if( count == GL3_MAX_CHAIN )
		{
			R_GL3Cap( GL3_CAP_CHAIN );
			break;
		}

		gl3_world.chain_surfs[count] = s;
		gl3_world.chain_keys[count] = R_GL3SurfaceKey( s, count, lightmaps );

		if( gl3_ri.currententity->curstate.rendermode == kRenderNormal )
			R_GL3RememberDecals( s, gl3_world.chain_keys[count], gl3_world.chain_lm[count] );
	}

	for( int first = 0; first < count; first++ )
	{
		const int key = gl3_world.chain_keys[first];
		gl3_worlddraw_t *d;

		if( key < 0 )
			continue; // already drawn

		d = R_GL3StartDraw( key, gl3_world.chain_surfs[first], reverse );
		if( !d )
			return;

		for( int i = first; i < count; i++ )
		{
			if( gl3_world.chain_keys[i] != key )
				continue;

			R_GL3Emit( d, i, reverse );
			gl3_world.chain_keys[i] = -1;
		}

		R_GL3EndDraw( d );
		if( d->count == 0 )
			gl3_world.numdraws--;
	}
}

// one face after another, in this order (sorted translucent faces); neighbours with one key share a draw
static int R_GL3AddSurfaceInOrder( msurface_t *s, qboolean reverse, qboolean lightmaps )
{
	gl3_worlddraw_t *d = NULL;
	int key;

	gl3_world.chain_surfs[0] = s;
	key = R_GL3SurfaceKey( s, 0, lightmaps );

	if( gl3_world.numdraws > gl3_world.object_first_draw )
	{
		d = &gl3_world.draws[gl3_world.numdraws - 1];
		if( d->key != key || d->reverse != reverse || d->decal )
			d = NULL;
	}

	if( !d && ( d = R_GL3StartDraw( key, s, reverse )) == NULL )
		return key;

	R_GL3Emit( d, 0, reverse );
	R_GL3EndDraw( d );
	if( d->count == 0 )
		gl3_world.numdraws--;
	return key;
}

void R_GL3WorldBeginFrame( void )
{
	gl3_world.numindices = 0;
	gl3_world.numdraws = 0;
	gl3_world.numobjects = 0;
	gl3_world.object = -1;
	gl3_world.object_first_draw = 0;
	gl3_world.numdynverts = 0;
	gl3_world.numwateralpha = 0;
	gl3_world.numdecalsurfs = 0;
	R_GL3DynamicBegin();
}

int R_GL3WorldDrawCount( void )
{
	return gl3_world.numdraws;
}

// what every 3D draw starts from (ref_gl's R_SetupGL state)
void R_GL3DefaultStates( gl3_states_t *st )
{
	memset( st, 0, sizeof( *st ));
	st->blend = FALSE;
	st->src = D3DBLEND_ONE;
	st->dst = D3DBLEND_ZERO;
	st->alphatest = FALSE;
	st->zwrite = TRUE;
	st->ztest = TRUE;
	st->zfunc = D3DCMP_LESSEQUAL;
	st->cull = GL3_CULL_FRONT;
	st->alpharef = 0;
}

// the visible world faces, as draws
void R_GL3CollectWorld( void )
{
	static const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	model_t *world = WORLDMODEL;
	gl3_states_t st;

	gl3_ri.currententity = R_GL3EntityByIndex( 0 );
	gl3_ri.currentmodel = world;

	R_GL3MarkLeaves();
	VectorCopy( gl3_ri.rvp.vieworigin, gl3_tr.modelorg );
	gl3_tr.blend = 1.0f;
	gl3_world.skychain = NULL;

	R_GL3LoadIdentity();
	R_GL3WorldNewObject();
	R_GL3DefaultStates( &st );
	R_GL3WorldSetState( &st, white );
	gl3_world.untextured = false;
	R_GL3RecursiveWorldNode( world->nodes, gl3_ri.frustum.clipFlags );

	R_GL3ClearSkyBox();
	for( msurface_t *s = gl3_world.skychain; s; s = s->texturechain )
		R_GL3AddSkyBoxSurface( s );

	for( int i = 0; i < world->numtextures; i++ )
	{
		texture_t *t = world->textures[i];

		if( !t || !t->texturechain )
			continue;

		if( i == gl3_world.skytexturenum )
		{
			t->texturechain = NULL;
			continue;
		}

		// translucent water goes after the entities
		if( FBitSet( t->texturechain->flags, SURF_DRAWTURB ) && gp_movevars->wateralpha < 1.0f )
		{
			if( gl3_world.numwateralpha < GL3_MAX_WATERALPHA )
			{
				gl3_world.wateralpha[gl3_world.numwateralpha++] = t;
				continue;
			}

			R_GL3Cap( GL3_CAP_WATERALPHA );
		}

		R_GL3AddChain( t->texturechain, false );
		t->texturechain = NULL;
	}

	R_GL3AddDecalBatch();
}

// gl_rsurf.c R_DrawWaterSurfaces
void R_GL3CollectWaterAlpha( void )
{
	const float color[4] = { 1.0f, 1.0f, 1.0f, gp_movevars->wateralpha };
	gl3_states_t st;

	if( !gl3_world.numwateralpha )
		return;

	gl3_ri.currententity = R_GL3EntityByIndex( 0 );
	gl3_ri.currentmodel = WORLDMODEL;
	R_GL3LoadIdentity();
	R_GL3WorldNewObject();

	R_GL3DefaultStates( &st );
	st.blend = TRUE;
	st.src = D3DBLEND_SRCALPHA;
	st.dst = D3DBLEND_INVSRCALPHA;
	st.zwrite = FALSE;
	R_GL3WorldSetState( &st, color );
	gl3_world.untextured = false;

	for( int i = 0; i < gl3_world.numwateralpha; i++ )
	{
		texture_t *t = gl3_world.wateralpha[i];

		R_GL3AddChain( t->texturechain, false );
		t->texturechain = NULL;
	}
	gl3_world.numdecalsurfs = 0; // water takes no decals

	gl3_world.numwateralpha = 0;
}

/*
==============================================================================

BRUSH ENTITIES

==============================================================================
*/
// gl_rsurf.c R_SetRenderMode
static void R_GL3BrushRenderMode( const cl_entity_t *e )
{
	float color[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	gl3_states_t st;

	R_GL3DefaultStates( &st );
	gl3_world.untextured = false;

	switch( e->curstate.rendermode )
	{
	case kRenderNormal:
		break;
	case kRenderTransColor:
		st.blend = TRUE;
		st.src = D3DBLEND_SRCALPHA;
		st.dst = D3DBLEND_INVSRCALPHA;
		Vector4Set( color, e->curstate.rendercolor.r / 255.0f, e->curstate.rendercolor.g / 255.0f, e->curstate.rendercolor.b / 255.0f, e->curstate.renderamt / 255.0f );
		gl3_world.untextured = true;
		break;
	case kRenderTransAdd:
		st.blend = TRUE;
		st.src = D3DBLEND_ONE;
		st.dst = D3DBLEND_ONE;
		st.zwrite = FALSE;
		Vector4Set( color, gl3_tr.blend, gl3_tr.blend, gl3_tr.blend, 1.0f );
		break;
	case kRenderTransAlpha:
		st.alphatest = TRUE;
		st.alpharef = 64; // GL_GREATER 0.25
		if( FBitSet( gp_host->features, ENGINE_QUAKE_COMPATIBLE ))
		{
			st.blend = TRUE;
			st.src = D3DBLEND_SRCALPHA;
			st.dst = D3DBLEND_INVSRCALPHA;
			color[3] = gl3_tr.blend;
		}
		break;
	default:
		st.blend = TRUE;
		st.src = D3DBLEND_SRCALPHA;
		st.dst = D3DBLEND_INVSRCALPHA;
		st.zwrite = FALSE;
		color[3] = gl3_tr.blend;
		break;
	}

	// bmodels a little closer than the world they sit in, against flickering
	st.slopebias = -0.5f;
	st.depthbias = -GL3_POLYOFFSET_BMODELS / 16777216.0f;
	R_GL3WorldSetState( &st, color );
}

// gl_rsurf.c R_SurfaceCompare: far faces first
static int R_GL3SurfaceCompare( const void *a, const void *b )
{
	const msurface_t *surf1 = ((const sortedface_t *)a)->surf;
	const msurface_t *surf2 = ((const sortedface_t *)b)->surf;
	vec3_t org1, org2;
	float len1, len2;

	VectorAdd( gl3_ri.currententity->origin, surf1->info->origin, org1 );
	VectorAdd( gl3_ri.currententity->origin, surf2->info->origin, org2 );

	len1 = DotProduct( org1, gl3_ri.vforward ) - gl3_ri.viewplanedist;
	len2 = DotProduct( org2, gl3_ri.vforward ) - gl3_ri.viewplanedist;

	return len1 > len2 ? -1 : ( len1 < len2 ? 1 : 0 );
}

// gl_rsurf.c R_DrawBrushModel: the entity's faces as draws
void R_GL3CollectBrushModel( cl_entity_t *e )
{
	model_t *clmodel = e->model;
	int old_rendermode = e->curstate.rendermode;
	vec3_t mins, maxs;
	qboolean rotated;
	qboolean lightmaps;
	gl3_states_t decal_states;
	int num_sorted = 0;

	if( !FBitSet( gl3_ri.rvp.flags, RF_DRAW_WORLD ) || !gl3_world.vb )
		return;

	// A brush entity carries an inline model of the map ("*12"). An entity that somehow carries the map
	// itself would draw every face of it a second time, through the entity's own origin: the whole level
	// laid over itself a few units away. Nothing should ever send that, which is why it says so.
	if( !clmodel || clmodel->name[0] != '*' )
	{
		R_GL3Cap( GL3_CAP_WORLDASENTITY );
		return;
	}

	// An inline brush model holds its faces in world coordinates, so the entity's own matrix is usually
	// the identity and the geometry sits where the level designer put it. A single frame of a number that
	// is not a number - this port has had one before, from a sine it could not reduce - turns that matrix
	// into anything, and the entity's faces are drawn wherever it points: a piece of the map lying on the
	// ground or stretched over the screen, for one frame, in a scene nobody can reproduce afterwards.
	if( VectorIsNAN( e->origin ) || VectorIsNAN( e->angles )
		|| fabs( e->origin[0] ) > GL3_WORLD_LIMIT || fabs( e->origin[1] ) > GL3_WORLD_LIMIT || fabs( e->origin[2] ) > GL3_WORLD_LIMIT )
	{
		if( !FBitSet( gl3_caps_reported, BIT( GL3_CAP_ENTITYNAN )))
		{
			SetBits( gl3_caps_reported, BIT( GL3_CAP_ENTITYNAN ));
			gEngfuncs.Con_Printf( S_ERROR "ref_gl3: %s (entity %d) is at %g %g %g, angles %g %g %g, and is not drawn\n",
				clmodel->name, e->index, e->origin[0], e->origin[1], e->origin[2],
				e->angles[0], e->angles[1], e->angles[2] );
		}
		return;
	}

	if( !VectorIsNull( e->angles ))
	{
		for( int i = 0; i < 3; i++ )
		{
			mins[i] = e->origin[i] - clmodel->radius;
			maxs[i] = e->origin[i] + clmodel->radius;
		}
		rotated = true;
	}
	else
	{
		VectorAdd( e->origin, clmodel->mins, mins );
		VectorAdd( e->origin, clmodel->maxs, maxs );
		rotated = false;
	}

	if( R_GL3CullBox( mins, maxs, 0 ))
		return;

	gl3_ri.currententity = e;
	gl3_ri.currentmodel = clmodel;

	if( rotated )
	{
		R_GL3RotateForEntity( e );
		Matrix4x4_VectorITransform( gl3_ri.objectMatrix, gl3_ri.rvp.vieworigin, gl3_tr.modelorg );
	}
	else
	{
		R_GL3TranslateForEntity( e );
		VectorSubtract( gl3_ri.rvp.vieworigin, e->origin, gl3_tr.modelorg );
	}
	R_GL3WorldNewObject();

	if( FBitSet( gp_host->features, ENGINE_QUAKE_COMPATIBLE ) && FBitSet( clmodel->flags, MODEL_TRANSPARENT ))
		e->curstate.rendermode = kRenderTransAlpha;

	e->visframe = gl3_tr.realframecount;
	R_PushDlightsForBmodel( clmodel, gl3_tr.dlightframecount, gl3_ri.objectMatrix );
	R_GL3BrushRenderMode( e );
	lightmaps = R_GL3EntityHasLightmap( e );

	// gl_rsurf.c R_SortBrushModelSurfaces
	for( int i = 0; i < clmodel->nummodelsurfaces; i++ )
	{
		msurface_t *psurf = &clmodel->surfaces[clmodel->firstmodelsurface + i];
		int cull;

		if( FBitSet( psurf->flags, SURF_DRAWTURB ) && !FBitSet( gp_host->features, ENGINE_QUAKE_COMPATIBLE ))
		{
			if( psurf->plane->type != PLANE_Z && !FBitSet( e->curstate.effects, EF_WATERSIDES ))
				continue;

			if( mins[2] + 1.0f >= psurf->plane->dist )
				continue;
		}

		cull = R_GL3CullSurface( psurf, gl3_ri.frustum.clipFlags );
		if( cull >= GL3_CULL_FRUSTUM )
			continue;

		if( cull == GL3_CULL_BACKSIDE )
		{
			if( FBitSet( psurf->flags, SURF_DRAWTURB ))
			{
				// no back faces with lightmaps, against z fighting
				if( R_GL3SurfaceHasLightmap( psurf ))
					continue;
			}
			else if( !( psurf->pdecals && e->curstate.rendermode == kRenderTransTexture ))
				continue;
		}

		if( num_sorted < gpGlobals->max_surfaces )
		{
			gpGlobals->draw_surfaces[num_sorted].surf = psurf;
			gpGlobals->draw_surfaces[num_sorted].cull = cull;
			num_sorted++;
		}
	}

	if( !FBitSet( clmodel->flags, MODEL_LIQUID ) && e->curstate.rendermode == kRenderTransTexture )
		qsort( gpGlobals->draw_surfaces, num_sorted, sizeof( sortedface_t ), R_GL3SurfaceCompare );

	R_GL3DecalStates( &decal_states, true );
	gl3_world.numdecalsurfs = 0;

	for( int i = 0; i < num_sorted; i++ )
	{
		msurface_t *s = gpGlobals->draw_surfaces[i].surf;
		const qboolean backside = gpGlobals->draw_surfaces[i].cull == GL3_CULL_BACKSIDE;
		int key;

		if( FBitSet( s->flags, SURF_DRAWSKY ))
			continue;

		// turbulent back faces are drawn reversed
		key = R_GL3AddSurfaceInOrder( s, FBitSet( s->flags, SURF_DRAWTURB ) && backside, lightmaps );

		if( !s->pdecals || FBitSet( s->flags, SURF_DRAWTURB ))
			continue;

		// other render modes draw a face's decals right after it
		if( e->curstate.rendermode == kRenderNormal )
			R_GL3RememberDecals( s, key, gl3_world.chain_lm[0] );
		else R_GL3EmitDecals( s, (( key >> 16 ) & 0xff ) - 1, (( key >> 16 ) & 0xff ) - 1 == GL3_LIGHTMAP_DYNAMIC ? gl3_world.chain_lm[0] : gl3_static_lm,
			&decal_states, gl3_world.color, backside && e->curstate.rendermode == kRenderTransTexture );
	}

	R_GL3AddDecalBatch();

	e->curstate.rendermode = old_rendermode;
	R_GL3LoadIdentity();
}

/*
==============================================================================

DRAWING

==============================================================================
*/
/*
==================
R_GL3CheckDraws

Every draw says which geometry it is: a range of this frame's indices (or of its own vertices), a texture,
an object matrix. A frame where one of them says something else draws real faces of the map in the wrong
place, or the wrong faces under the right texture - which is what a player sees as a piece of the level
lying somewhere it is not. Nothing here should ever fire; it runs so that the first time one does, the log
names the draw instead of leaving a photograph of a television to argue about.
==================
*/
static void R_GL3CheckDraws( void )
{
	int end_indices = 0, end_dynverts = 0;

	for( int i = 0; i < gl3_world.numdraws; i++ )
	{
		const gl3_worlddraw_t *d = &gl3_world.draws[i];
		const int limit = d->up ? gl3_world.numdynverts : gl3_world.numindices;
		int *end = d->up ? &end_dynverts : &end_indices;
		const char *what = NULL;

		if( d->object < 0 || d->object >= gl3_world.numobjects )
			what = "an object matrix that is not in this frame";
		else if( d->first < 0 || d->count < 0 || d->first + d->count > limit )
			what = "a range outside the geometry of this frame";
		else if( d->count % 3 )
			what = "a count that is not whole triangles";
		else if( d->first < *end )
			what = "a range that overlaps the draw before it";
		else if( !d->up && d->count && gl3_world.indices[d->first] >= (uint32_t)gl3_world.numverts )
			what = "an index outside the world's vertices";

		if( what && !FBitSet( gl3_caps_reported, BIT( GL3_CAP_DRAWLIST )))
		{
			SetBits( gl3_caps_reported, BIT( GL3_CAP_DRAWLIST ));
			// the texture number is part of what may be wrong, and R_GL3GetTexture stops the host on an
			// impossible one - the report would then say nothing at all
			const char *texture = ( d->texture > 0 && d->texture < GL3_MAX_TEXTURES )
				? R_GL3GetTexture( d->texture )->name : "out of range";

			gEngfuncs.Con_Printf( S_ERROR "ref_gl3: draw %d of %d has %s: texture %d %s, %s %d+%d of %d, object %d of %d, at %.0f %.0f %.0f\n",
				i, gl3_world.numdraws, what, d->texture, texture,
				d->up ? "vertices" : "indices", d->first, d->count, limit, d->object, gl3_world.numobjects,
				gl3_ri.rvp.vieworigin[0], gl3_ri.rvp.vieworigin[1], gl3_ri.rvp.vieworigin[2] );
		}

		// detecting it only helps if the frame stops drawing it: a bad object index would otherwise still
		// reach SetVertexShaderConstantF through gl3_world.objects[]
		if( what )
			gl3_world.draws[i].count = 0;

		*end = d->first + d->count;
	}
}

void R_GL3WorldUpload( void )
{
	IDirect3DIndexBuffer9 *ib;
	void *dst;

	R_GL3CheckDraws();
	R_GL3LightmapsCommit();
	gl3_world.ib_valid = false;

	if( !gl3_world.numindices )
		return;

	gl3_world.ib_current = ( gl3_world.ib_current + 1 ) % GL3_WORLD_IB_RING;
	ib = gl3_world.ib[gl3_world.ib_current];

	R_GL3DrawReset();
	gl3_device->SetIndices( NULL );
	if( FAILED( ib->Lock( 0, 0, &dst, 0 )))
	{
		// keep the ring where it was. The buffer it holds belongs to an earlier frame, and this frame's
		// draws point into this frame's indices, so drawing from it would put other faces of the map on
		// the screen: the indexed draws are skipped instead, and the frame loses the world.
		gl3_world.ib_current = ( gl3_world.ib_current + GL3_WORLD_IB_RING - 1 ) % GL3_WORLD_IB_RING;
		if( !gl3_world.overflow )
			gEngfuncs.Con_Printf( S_ERROR "ref_gl3: can't lock the world index buffer\n" );
		gl3_world.overflow = true;
		return;
	}
	XMemCpyStreaming_WriteCombined( dst, gl3_world.indices, gl3_world.numindices * sizeof( *gl3_world.indices ));
	ib->Unlock();
	gl3_world.ib_valid = true;
}

/*
==================
R_GL3WorldShadowRange

The same geometry again, from the flashlight, into whatever render target is set: only the position
matters, so one shader and one constant per object. Sky, water and decals are skipped - the first two
cast nothing in the original either, and a decal sits on the face that already cast its shadow.
==================
*/
void R_GL3WorldShadowRange( int first, int last )
{
	int object = -1;
	qboolean streams = false;

	if( first >= last || !gl3_world.vb || !gl3_world.ib_valid )
		return;

	R_GL3DrawReset();
	gl3_device->SetVertexDeclaration( gl3_world.decl );
	gl3_device->SetVertexShader( gl3_world.shadow_vs );
	gl3_device->SetPixelShader( gl3_world.shadow_ps );

	for( int i = first; i < last; i++ )
	{
		const gl3_worlddraw_t *d = &gl3_world.draws[i];

		if( d->warp || d->decal || d->up || d->count <= 0 )
			continue;

		if( d->object != object )
		{
			object = d->object;
			gl3_device->SetVertexShaderConstantF( 8, gl3_world.objects[object] + 20, 4 );
		}

		if( !streams )
		{
			gl3_device->SetStreamSource( 0, gl3_world.vb, 0, sizeof( float ) * VERTEXSIZE );
			gl3_device->SetIndices( gl3_world.ib[gl3_world.ib_current] );
			streams = true;
		}

		gl3_device->DrawIndexedPrimitive( D3DPT_TRIANGLELIST, 0, 0, gl3_world.numverts, d->first, d->count / 3 );
		R_GL3CountDraw( d->count );
	}

	R_GL3DrawReset();
}

void R_GL3DrawWorldRange( int first, int last )
{
	IDirect3DVertexShader9 *vs = NULL;
	IDirect3DPixelShader9 *ps = NULL;
	const qboolean swwater = r_xenon_swwater->value != 0.0f;
	const qboolean flashlight = R_GL3FlashlightActive( );
	const float *color = NULL, *fog = NULL;
	float lmscale[4] = { -1.0f, 0.0f, 0.0f, 0.0f };
	int object = -1;
	qboolean streams = false; // DrawPrimitiveUP unsets them

	if( first >= last || !gl3_world.vb )
		return;

	R_GL3DrawReset();
	gl3_device->SetVertexDeclaration( gl3_world.decl );

	if( flashlight )
	{
		R_GL3BindCookie( R_GL3FlashlightCookie( ));
		R_GL3BindShadowMap( R_GL3FlashlightShadowed( ) ? R_GL3FlashlightShadowMap( ) : gl3_white_texture );
		gl3_device->SetPixelShaderConstantF( 4, R_GL3FlashlightParams( ), 1 );
	}

	for( int i = first; i < last; i++ )
	{
		const gl3_worlddraw_t *d = &gl3_world.draws[i];
		const qboolean soft = d->warp && swwater;

		// an empty draw is one R_GL3CheckDraws took apart: it must not reach gl3_world.objects[] either
		if( d->count <= 0 )
			continue;

		// the flashlight lights plain surfaces, and water through the pair that carries its own warp. The
		// sine water of the original style is left out: that style has no flashlight to light it with.
		const qboolean spot = flashlight && ( !d->warp || soft );
		IDirect3DVertexShader9 *want = d->warp
			? ( soft ? ( spot ? gl3_world.warpswspot_vs : gl3_world.warpsw_vs ) : gl3_world.warp_vs )
			: ( spot ? gl3_world.spot_vs : gl3_world.vs );
		IDirect3DPixelShader9 *want_ps = soft ? ( spot ? gl3_world.warpswspot_ps : gl3_world.warpsw_ps )
			: ( spot ? gl3_world.spot_ps : gl3_world.ps );
		float constants[8];

		if( want != vs )
		{
			vs = want;
			gl3_device->SetVertexShader( vs );
		}

		if( want_ps != ps )
		{
			ps = want_ps;
			gl3_device->SetPixelShader( ps );
		}

		// the software water reads its ripple field from sampler 2; the field itself steps once a frame
		if( soft )
			R_GL3BindRipple( R_GL3RippleTexture( ));

		if( d->object != object )
		{
			object = d->object;
			gl3_device->SetVertexShaderConstantF( 0, gl3_world.objects[object], 4 );
			gl3_device->SetVertexShaderConstantF( 6, gl3_world.objects[object] + 16, 1 );
			if( flashlight )
			{
				gl3_device->SetVertexShaderConstantF( 8, gl3_world.objects[object] + 20, 4 );
				gl3_device->SetPixelShaderConstantF( 5, gl3_world.objects[object] + 36, 1 );
			}
		}

		if( !fog || memcmp( fog, d->fog, sizeof( d->fog )))
		{
			fog = d->fog;
			gl3_device->SetPixelShaderConstantF( 1, fog, 2 );
		}

		if( !color || memcmp( color, d->color, sizeof( d->color )))
		{
			color = d->color;
			gl3_device->SetPixelShaderConstantF( 0, color, 1 );
		}

		if( memcmp( &gl3_wanted, &d->states, sizeof( d->states )))
		{
			gl3_wanted = d->states;
			R_GL3ApplyStates();
		}

		constants[0] = d->offset[0];
		constants[1] = d->offset[1];
		constants[2] = constants[3] = 0.0f;
		constants[4] = (float)gp_cl->time;
		constants[5] = d->waveheight;
		constants[6] = constants[7] = 0.0f;
		gl3_device->SetVertexShaderConstantF( 4, constants, 2 );

		// r_lightmap shows the lightmaps alone
		R_GL3BindTexture( d->lightmap >= 0 && r_lightmap->value ? gl3_white_texture : d->texture );
		R_GL3BindLightmap( R_GL3LightmapTexture( d->lightmap ));

		// overbright brightens real lightmaps only: unlit water has the white one, as ref_gl draws it unblended
		const float scale = d->lightmap >= 0 ? R_GL3LightmapScale() : 1.0f;
		if( lmscale[0] != scale )
		{
			lmscale[0] = scale;
			gl3_device->SetPixelShaderConstantF( 3, lmscale, 1 );
		}

		if( d->up )
		{
			gl3_device->DrawPrimitiveUP( D3DPT_TRIANGLELIST, d->count / 3, gl3_world.dynverts[d->first], sizeof( float ) * VERTEXSIZE );
			streams = false;
		}
		else
		{
			if( !gl3_world.ib_valid )
				continue;

			if( !streams )
			{
				gl3_device->SetStreamSource( 0, gl3_world.vb, 0, sizeof( float ) * VERTEXSIZE );
				gl3_device->SetIndices( gl3_world.ib[gl3_world.ib_current] );
				streams = true;
			}
			gl3_device->DrawIndexedPrimitive( D3DPT_TRIANGLELIST, 0, 0, gl3_world.numverts, d->first, d->count / 3 );
		}
		R_GL3CountDraw( d->count );
	}

	// back to the plain 3D state for whatever draws next
	R_GL3DefaultStates( &gl3_wanted );
	R_GL3DrawReset();
}

// gl_warp.c R_DrawSkyBox: under water the sky takes half the fog, i.e. twice the range
void R_GL3DrawSky( qboolean fog )
{
	static const float nofog[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

	if( gl3_world.skychain )
	{
		if( fog && gl3_ri.fogSkybox )
		{
			const float skyfog[4] = { gl3_ri.fogColor[0], gl3_ri.fogColor[1], gl3_ri.fogColor[2], gl3_ri.fogEndInv * 0.5f };
			R_GL3SetDrawFog( gl3_ri.worldviewMatrix[2], skyfog );
		}

		R_GL3DrawSkyBox();
		R_GL3SetDrawFog( nofog, nofog );
	}

	gl3_world.skychain = NULL;
}

/*
==============================================================================

INIT

==============================================================================
*/
qboolean R_GL3InitWorld( void )
{
	// ref_gl warps water once per vertex on the 64-unit grid the engine subdivides it into, which slides the
	// texture; the software renderer left the geometry alone and animated the texture through a ripple
	// simulation instead (r_ripple.c). Both are here, and the render style picks one.
	r_xenon_swwater = gEngfuncs.Cvar_Get( "r_xenon_swwater", "0", FCVAR_ARCHIVE,
		"animate water the way the software renderer did: the 128x128 ripple field it simulated" );

	if( FAILED( gl3_device->CreateVertexShader( g_shadow_VS, &gl3_world.shadow_vs ))
		|| FAILED( gl3_device->CreatePixelShader( g_shadow_PS, &gl3_world.shadow_ps ))
		|| FAILED( gl3_device->CreateVertexShader( g_worldspot_VS, &gl3_world.spot_vs ))
		|| FAILED( gl3_device->CreatePixelShader( g_worldspot_PS, &gl3_world.spot_ps ))
		|| FAILED( gl3_device->CreateVertexShader( g_world_VS, &gl3_world.vs ))
		|| FAILED( gl3_device->CreateVertexShader( g_warp_VS, &gl3_world.warp_vs ))
		|| FAILED( gl3_device->CreateVertexShader( g_warpsw_VS, &gl3_world.warpsw_vs ))
		|| FAILED( gl3_device->CreatePixelShader( g_world_PS, &gl3_world.ps ))
		|| FAILED( gl3_device->CreatePixelShader( g_warpsw_PS, &gl3_world.warpsw_ps ))
		|| FAILED( gl3_device->CreateVertexShader( g_warpswspot_VS, &gl3_world.warpswspot_vs ))
		|| FAILED( gl3_device->CreatePixelShader( g_warpswspot_PS, &gl3_world.warpswspot_ps ))
		|| FAILED( gl3_device->CreateVertexDeclaration( gl3_world_elements, &gl3_world.decl )))
	{
		gEngfuncs.Con_Printf( S_ERROR "ref_gl3: can't create the world shaders\n" );
		return false;
	}

	for( int i = 0; i < GL3_WORLD_IB_RING; i++ )
	{
		if( FAILED( gl3_device->CreateIndexBuffer( GL3_WORLD_MAX_INDICES * sizeof( uint32_t ), 0, D3DFMT_INDEX32, D3DPOOL_MANAGED, &gl3_world.ib[i], NULL )))
		{
			gEngfuncs.Con_Printf( S_ERROR "ref_gl3: can't create the world index buffers\n" );
			return false;
		}
	}

	gl3_world.skytexturenum = -1;
	return true;
}

void R_GL3ShutdownWorld( void )
{
	if( !gl3_device )
		return;

	R_GL3FreeWorldBuffer();
	R_GL3DrawReset();
	gl3_device->SetIndices( NULL );
	gl3_device->SetVertexShader( NULL );
	gl3_device->SetPixelShader( NULL );
	gl3_device->SetVertexDeclaration( NULL );

	for( int i = 0; i < GL3_WORLD_IB_RING; i++ )
	{
		if( gl3_world.ib[i] )
			gl3_world.ib[i]->Release();
		gl3_world.ib[i] = NULL;
	}

	if( gl3_world.decl ) gl3_world.decl->Release();
	if( gl3_world.ps ) gl3_world.ps->Release();
	if( gl3_world.shadow_ps ) gl3_world.shadow_ps->Release();
	if( gl3_world.shadow_vs ) gl3_world.shadow_vs->Release();
	if( gl3_world.spot_ps ) gl3_world.spot_ps->Release();
	if( gl3_world.spot_vs ) gl3_world.spot_vs->Release();
	if( gl3_world.warpsw_ps ) gl3_world.warpsw_ps->Release();
	if( gl3_world.warp_vs ) gl3_world.warp_vs->Release();
	if( gl3_world.warpsw_vs ) gl3_world.warpsw_vs->Release();
	if( gl3_world.warpswspot_ps ) gl3_world.warpswspot_ps->Release();
	if( gl3_world.warpswspot_vs ) gl3_world.warpswspot_vs->Release();
	gl3_world.warpswspot_ps = NULL;
	gl3_world.warpswspot_vs = NULL;
	if( gl3_world.vs ) gl3_world.vs->Release();
	gl3_world.decl = NULL;
	gl3_world.ps = NULL;
	gl3_world.shadow_ps = NULL;
	gl3_world.shadow_vs = NULL;
	gl3_world.spot_ps = NULL;
	gl3_world.spot_vs = NULL;
	gl3_world.warpsw_ps = NULL;
	gl3_world.warp_vs = NULL;
	gl3_world.warpsw_vs = NULL;
	gl3_world.vs = NULL;
}
