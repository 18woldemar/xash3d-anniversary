/*
r_decal.c - ref_gl3: decals on brush surfaces
Copyright (C) 2026 xash3d-xenon
Decal placement, clipping, pool and save lists follow ref/gl/gl_decals.c, Copyright (C) 2010 Uncle Mike

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
A decal is clipped to its face once, in the face's texture space, and kept as a polygon with lightmap
coordinates. r_world.c draws the polygons after the face, with the face's lightmap: ref_gl blends decals
over the texture before its lightmap pass multiplies both, which comes out the same.
*/

#include "r_local.h"

#define DECAL_OVERLAP_DISTANCE 2
#define DECAL_DISTANCE         4  // too big values produce more clipped polygons
#define MAX_DECALCLIPVERT      32 // produced vertexes of fragmented decal
#define MAX_OVERLAP_DECALS     6
#define MIN_DECAL_SCALE        0.01f
#define MAX_DECAL_SCALE        16.0f

// clip edges
#define LEFT_EDGE   0
#define RIGHT_EDGE  1
#define TOP_EDGE    2
#define BOTTOM_EDGE 3

// how a new decal goes in
typedef struct
{
	vec3_t   m_Position; // world coordinates of the decal center
	model_t *m_pModel;   // the model the decal is going to be applied in
	int      m_iTexture;
	int      m_Size;     // in world coords
	int      m_Flags;
	int      m_Entity;
	float    m_scale;
	int      m_decalWidth;
	int      m_decalHeight;
	vec3_t   m_Basis[3];
} decalinfo_t;

static float   gl3_clipverts[MAX_DECALCLIPVERT][VERTEXSIZE];
static float   gl3_clipverts2[MAX_DECALCLIPVERT][VERTEXSIZE];
static decal_t gl3_decals[MAX_RENDER_DECALS];
static int     gl3_decal_count;

void R_GL3ClearDecals( void )
{
	// the polygons come from r_temppool, which lives until the renderer shuts down, so a map change that
	// only forgot the pointers (as ref_gl does) left the previous map's decals in memory for good. The
	// surfaces they hang on belong to the old map and may already be freed, so nothing is unlinked here.
	for( int i = 0; i < MAX_RENDER_DECALS; i++ )
	{
		if( gl3_decals[i].polys )
			Mem_Free( gl3_decals[i].polys );
	}

	memset( gl3_decals, 0, sizeof( gl3_decals ));
	gl3_decal_count = 0;
}

// unlink the decal from the surface it is on
static void R_GL3DecalUnlink( decal_t *pdecal )
{
	if( pdecal->psurface )
	{
		if( pdecal->psurface->pdecals == pdecal )
		{
			pdecal->psurface->pdecals = pdecal->pnext;
		}
		else
		{
			decal_t *tmp = pdecal->psurface->pdecals;

			if( !tmp )
				gEngfuncs.Host_Error( "%s: bad decal list\n", __func__ );

			for( ; tmp->pnext; tmp = tmp->pnext )
			{
				if( tmp->pnext == pdecal )
				{
					tmp->pnext = pdecal->pnext;
					break;
				}
			}
		}
	}

	if( pdecal->polys )
		Mem_Free( pdecal->polys );

	pdecal->psurface = NULL;
	pdecal->polys = NULL;
}

// the next pool entry; a decal over several faces uses one entry per face
static decal_t *R_GL3DecalAlloc( decal_t *pdecal )
{
	int limit = MAX_RENDER_DECALS;

	if( r_decals->value < limit )
		limit = (int)r_decals->value;

	if( !limit )
		return NULL;

	if( !pdecal )
	{
		int count = 0;

		do
		{
			if( gl3_decal_count >= limit )
				gl3_decal_count = 0;

			pdecal = &gl3_decals[gl3_decal_count];
			gl3_decal_count++;
			count++;
		} while( FBitSet( pdecal->flags, FDECAL_PERMANENT ) && count < limit );
	}

	R_GL3DecalUnlink( pdecal );
	return pdecal;
}

static void R_GL3DecalDimensions( int texture, int *width, int *height )
{
	const gl3_texture_t *tex = R_GL3GetTexture( texture );

	// gl_draw.c R_GetTextureParms
	*width = Q_max( 1, tex->srcWidth );
	*height = Q_max( 1, tex->srcHeight );
}

static void R_GL3DecalComputeBasis( const msurface_t *surf, vec3_t basis[3] )
{
	vec3_t normal;

	if( FBitSet( surf->flags, SURF_PLANEBACK ))
		VectorNegate( surf->plane->normal, normal );
	else VectorCopy( surf->plane->normal, normal );

	VectorNormalize2( normal, basis[2] );
	VectorNormalize2( surf->texinfo->vecs[0], basis[0] );
	VectorNormalize2( surf->texinfo->vecs[1], basis[1] );
}

static void R_GL3DecalTextureSpaceBasis( decal_t *pDecal, const msurface_t *surf, int texture, vec3_t basis[3], float scale[2] )
{
	int width, height;

	R_GL3DecalComputeBasis( surf, basis );
	R_GL3DecalDimensions( texture, &width, &height );

	// scale is inverse: world space to decal u/v space [0,1]
	scale[0] = (float)pDecal->scale / width;
	scale[1] = (float)pDecal->scale / height;

	VectorScale( basis[0], scale[0], basis[0] );
	VectorScale( basis[1], scale[1], basis[1] );
}

static void R_GL3DecalClipSetup( decal_t *pDecal, const msurface_t *surf, int texture, vec3_t basis[3], float scale[2] )
{
	R_GL3DecalTextureSpaceBasis( pDecal, surf, texture, basis, scale );
	pDecal->dx = DotProduct( pDecal->position, basis[0] );
	pDecal->dy = DotProduct( pDecal->position, basis[1] );
}

static qboolean R_GL3ClipInside( const float *vert, int edge )
{
	switch( edge )
	{
	case LEFT_EDGE: return vert[3] > 0.0f;
	case RIGHT_EDGE: return vert[3] < 1.0f;
	case TOP_EDGE: return vert[4] > 0.0f;
	case BOTTOM_EDGE: return vert[4] < 1.0f;
	}
	return false;
}

// gl_decals.c R_ClipIntersect, lightmap coordinates included (they are computed again after clipping)
static void R_GL3ClipIntersect( const float *one, const float *two, float *out, int edge )
{
	float t;

	if( edge < TOP_EDGE )
	{
		if( edge == LEFT_EDGE )
		{
			t = ( one[3] - 0.0f ) / ( one[3] - two[3] );
			out[3] = out[5] = 0.0f;
		}
		else
		{
			t = ( one[3] - 1.0f ) / ( one[3] - two[3] );
			out[3] = out[5] = 1.0f;
		}

		out[4] = one[4] + ( two[4] - one[4] ) * t;
		out[6] = one[6] + ( two[6] - one[6] ) * t;
	}
	else
	{
		if( edge == TOP_EDGE )
		{
			t = ( one[4] - 0.0f ) / ( one[4] - two[4] );
			out[4] = out[6] = 0.0f;
		}
		else
		{
			t = ( one[4] - 1.0f ) / ( one[4] - two[4] );
			out[4] = out[6] = 1.0f;
		}

		out[3] = one[3] + ( two[3] - one[3] ) * t;
		out[5] = one[5] + ( two[5] - one[5] ) * t;
	}

	VectorLerp( one, t, two, out );
}

static int R_GL3SHClip( const float *vert, int vertCount, float *out, int edge )
{
	const float *s = &vert[( vertCount - 1 ) * VERTEXSIZE];
	int outCount = 0;

	for( int j = 0; j < vertCount; j++ )
	{
		const float *p = &vert[j * VERTEXSIZE];

		if( outCount + 2 > MAX_DECALCLIPVERT )
			break;

		if( R_GL3ClipInside( p, edge ))
		{
			if( !R_GL3ClipInside( s, edge ))
			{
				R_GL3ClipIntersect( s, p, out, edge );
				out += VERTEXSIZE;
				outCount++;
			}

			memcpy( out, p, sizeof( float ) * VERTEXSIZE );
			out += VERTEXSIZE;
			outCount++;
		}
		else if( R_GL3ClipInside( s, edge ))
		{
			R_GL3ClipIntersect( p, s, out, edge );
			out += VERTEXSIZE;
			outCount++;
		}

		s = p;
	}

	return outCount;
}

// the face's polygon clipped to the decal's [0,1] square
static float *R_GL3DecalVertsClip( decal_t *pDecal, const msurface_t *surf, int texture, int *pVertCount )
{
	float scale[2];
	vec3_t basis[3];
	const float *v = surf->polys->verts[0];
	float *out = gl3_clipverts[0];
	int count = Q_min( surf->polys->numverts, MAX_DECALCLIPVERT );

	R_GL3DecalClipSetup( pDecal, surf, texture, basis, scale );

	for( int i = 0; i < count; i++, v += VERTEXSIZE, out += VERTEXSIZE )
	{
		VectorCopy( v, out );
		out[3] = DotProduct( out, basis[0] ) - pDecal->dx + 0.5f;
		out[4] = DotProduct( out, basis[1] ) - pDecal->dy + 0.5f;
		out[5] = out[6] = 0.0f;
	}

	count = R_GL3SHClip( gl3_clipverts[0], count, gl3_clipverts2[0], LEFT_EDGE );
	count = R_GL3SHClip( gl3_clipverts2[0], count, gl3_clipverts[0], RIGHT_EDGE );
	count = R_GL3SHClip( gl3_clipverts[0], count, gl3_clipverts2[0], TOP_EDGE );
	count = R_GL3SHClip( gl3_clipverts2[0], count, gl3_clipverts[0], BOTTOM_EDGE );

	*pVertCount = count;
	return gl3_clipverts[0];
}

// the decal's vertices on its face, clipped once and kept
const float *R_GL3DecalVerts( decal_t *pDecal, msurface_t *surf, int *outCount )
{
	float *v;
	int count;

	if( pDecal->polys )
	{
		*outCount = pDecal->polys->numverts;
		return pDecal->polys->verts[0];
	}

	v = R_GL3DecalVertsClip( pDecal, surf, pDecal->texture, &count );
	for( int j = 0; j < count; j++ )
		R_GL3LightmapCoord( v + j * VERTEXSIZE, surf, (float)gEngfuncs.Mod_SampleSizeForFace( surf ), v + j * VERTEXSIZE + 5 );

	*outCount = count;
	return v;
}

// gl_decals.c R_DecalIntersect: the smallest decal this one would cover
static decal_t *R_GL3DecalIntersect( decalinfo_t *decalinfo, msurface_t *surf, int *pcount )
{
	decal_t *plast = NULL;
	float lastArea = 2;
	int mapSize[2];
	vec3_t decalExtents[2];

	*pcount = 0;

	R_GL3DecalDimensions( decalinfo->m_iTexture, &mapSize[0], &mapSize[1] );
	VectorScale( decalinfo->m_Basis[0], (( mapSize[0] / decalinfo->m_scale ) * 0.5f ), decalExtents[0] );
	VectorScale( decalinfo->m_Basis[1], (( mapSize[1] / decalinfo->m_scale ) * 0.5f ), decalExtents[1] );

	for( decal_t *pDecal = surf->pdecals; pDecal; pDecal = pDecal->pnext )
	{
		vec3_t testBasis[3], testPosition[2];
		float testWorldScale[2];
		vec2_t vDecalMin, vDecalMax, vUnionMin, vUnionMax;

		// don't steal permanent decals
		if( FBitSet( pDecal->flags, FDECAL_PERMANENT ))
			continue;

		R_GL3DecalTextureSpaceBasis( pDecal, surf, pDecal->texture, testBasis, testWorldScale );

		VectorSubtract( decalinfo->m_Position, decalExtents[0], testPosition[0] );
		VectorSubtract( decalinfo->m_Position, decalExtents[1], testPosition[1] );
		vDecalMin[0] = DotProduct( testPosition[0], testBasis[0] ) - pDecal->dx + 0.5f;
		vDecalMin[1] = DotProduct( testPosition[1], testBasis[1] ) - pDecal->dy + 0.5f;

		VectorAdd( decalinfo->m_Position, decalExtents[0], testPosition[0] );
		VectorAdd( decalinfo->m_Position, decalExtents[1], testPosition[1] );
		vDecalMax[0] = DotProduct( testPosition[0], testBasis[0] ) - pDecal->dx + 0.5f;
		vDecalMax[1] = DotProduct( testPosition[1], testBasis[1] ) - pDecal->dy + 0.5f;

		vUnionMin[0] = Q_max( vDecalMin[0], 0 );
		vUnionMin[1] = Q_max( vDecalMin[1], 0 );
		vUnionMax[0] = Q_min( vDecalMax[0], 1 );
		vUnionMax[1] = Q_min( vDecalMax[1], 1 );

		if( vUnionMin[0] < 1 && vUnionMin[1] < 1 && vUnionMax[0] > 0 && vUnionMax[1] > 0 )
		{
			// ref_gl's area uses the min y for both axes
			float flArea = ( vUnionMax[0] - vUnionMin[1] ) * ( vUnionMax[1] - vUnionMin[1] );

			if( flArea > 0.6f )
			{
				*pcount += 1;

				if( !plast || flArea <= lastArea )
				{
					plast = pDecal;
					lastArea = flArea;
				}
			}
		}
	}

	return plast;
}

static void R_GL3DecalCreatePoly( decal_t *pdecal, msurface_t *surf )
{
	const float *v;
	glpoly2_t *poly;
	int lnumverts;

	if( pdecal->polys )
		return;

	v = R_GL3DecalVerts( pdecal, surf, &lnumverts );
	if( !lnumverts )
		return;

	poly = (glpoly2_t *)Mem_Calloc( r_temppool, sizeof( glpoly2_t ) + lnumverts * VERTEXSIZE * sizeof( float ));
	poly->next = NULL;
	poly->flags = surf->flags;
	poly->numverts = lnumverts;
	memcpy( poly->verts[0], v, lnumverts * VERTEXSIZE * sizeof( float ));
	pdecal->polys = poly;
}

// appended at the end: later decals draw over earlier ones
static void R_GL3AddDecalToSurface( decal_t *pdecal, msurface_t *surf )
{
	pdecal->pnext = NULL;

	if( surf->pdecals )
	{
		decal_t *pold = surf->pdecals;

		while( pold->pnext )
			pold = pold->pnext;
		pold->pnext = pdecal;
	}
	else surf->pdecals = pdecal;

	pdecal->psurface = surf;
	R_GL3DecalCreatePoly( pdecal, surf );
}

static void R_GL3DecalCreate( decalinfo_t *decalinfo, msurface_t *surf, float x, float y )
{
	decal_t *pold, *pdecal;
	int count, vertCount;

	pold = R_GL3DecalIntersect( decalinfo, surf, &count );
	if( count < MAX_OVERLAP_DECALS )
		pold = NULL;

	pdecal = R_GL3DecalAlloc( pold );
	if( !pdecal )
		return;

	pdecal->flags = decalinfo->m_Flags;
	VectorCopy( decalinfo->m_Position, pdecal->position );
	pdecal->dx = x;
	pdecal->dy = y;
	pdecal->scale = decalinfo->m_scale;
	pdecal->entityIndex = decalinfo->m_Entity;
	pdecal->texture = decalinfo->m_iTexture;

	// does it touch the face at all
	R_GL3DecalVertsClip( pdecal, surf, decalinfo->m_iTexture, &vertCount );
	if( !vertCount )
	{
		R_GL3DecalUnlink( pdecal );
		return;
	}

	R_GL3AddDecalToSurface( pdecal, surf );
}

static void R_GL3DecalSurface( msurface_t *surf, decalinfo_t *decalinfo )
{
	const mtexinfo_t *tex = surf->texinfo;
	const int state = ENGINE_GET_PARM( PARM_CONNSTATE );
	float s, t, w, h;

	// restoring: a decal from another level may already be here
	if( state == ca_connected || state == ca_validate )
	{
		for( decal_t *decal = surf->pdecals; decal; decal = decal->pnext )
		{
			if( VectorCompare( decal->position, decalinfo->m_Position ) && decal->texture == decalinfo->m_iTexture )
				return;
		}
	}

	// the decal center in the face's texture space
	s = DotProduct( decalinfo->m_Position, tex->vecs[0] ) + tex->vecs[0][3] - surf->texturemins[0];
	t = DotProduct( decalinfo->m_Position, tex->vecs[1] ) + tex->vecs[1][3] - surf->texturemins[1];

	R_GL3DecalComputeBasis( surf, decalinfo->m_Basis );

	// the decal's axis-aligned size in the face's texture space
	w = fabs( decalinfo->m_decalWidth * DotProduct( tex->vecs[0], decalinfo->m_Basis[0] ))
		+ fabs( decalinfo->m_decalHeight * DotProduct( tex->vecs[0], decalinfo->m_Basis[1] ));
	h = fabs( decalinfo->m_decalWidth * DotProduct( tex->vecs[1], decalinfo->m_Basis[0] ))
		+ fabs( decalinfo->m_decalHeight * DotProduct( tex->vecs[1], decalinfo->m_Basis[1] ));

	// upper left corner
	s -= w * 0.5f;
	t -= h * 0.5f;

	if( s <= -w || t <= -h || s > surf->extents[0] + w || t > surf->extents[1] + h )
		return;

	R_GL3DecalCreate( decalinfo, surf, s, t );
}

static void R_GL3DecalNodeSurfaces( model_t *model, mnode_t *node, decalinfo_t *decalinfo )
{
	const int first = node_firstsurface( node, model );
	const int count = node_numsurfaces( node, model );
	msurface_t *surf = model->surfaces + first;

	for( int i = 0; i < count; i++, surf++ )
	{
		// never on water, sky or scrolling faces
		if( FBitSet( surf->flags, SURF_DRAWTURB | SURF_DRAWSKY | SURF_CONVEYOR ))
			continue;

		// ponytail: ref_gl puts decals on '{' faces too when it has a stencil buffer and masks them there
		// (against whatever texture is bound); left out, as ref_gl does without stencil
		if( FBitSet( surf->flags, SURF_TRANSPARENT ))
			continue;

		R_GL3DecalSurface( surf, decalinfo );
	}
}

static void R_GL3DecalNode( model_t *model, mnode_t *node, decalinfo_t *decalinfo )
{
	while( node->contents >= 0 )
	{
		const mplane_t *splitplane = node->plane;
		const float dist = DotProduct( decalinfo->m_Position, splitplane->normal ) - splitplane->dist;

		if( dist > decalinfo->m_Size )
		{
			node = node_child( node, 0, model );
		}
		else if( dist < -decalinfo->m_Size )
		{
			node = node_child( node, 1, model );
		}
		else
		{
			if( dist < DECAL_DISTANCE && dist > -DECAL_DISTANCE )
				R_GL3DecalNodeSurfaces( model, node, decalinfo );

			R_GL3DecalNode( model, node_child( node, 0, model ), decalinfo );
			node = node_child( node, 1, model );
		}
	}
}

// a decal centered at pos on the entity's (or the world's) faces
void R_GL3DecalShoot( int textureIndex, int entityIndex, int modelIndex, vec3_t pos, int flags, float scale )
{
	cl_entity_t *ent = NULL;
	model_t *model;
	decalinfo_t decalInfo;
	int width, height;

	if( textureIndex <= 0 || textureIndex >= GL3_MAX_TEXTURES )
	{
		gEngfuncs.Con_Printf( S_ERROR "Decal has invalid texture!\n" );
		return;
	}

	if( entityIndex > 0 )
	{
		ent = R_GL3EntityByIndex( entityIndex );

		if( modelIndex > 0 )
			model = R_GL3ModelHandle( modelIndex );
		else if( ent != NULL )
			model = R_GL3ModelHandle( ent->curstate.modelindex );
		else return;
	}
	else if( modelIndex > 0 )
		model = R_GL3ModelHandle( modelIndex );
	else model = R_GL3ModelHandle( 1 );

	if( !model )
		return;

	if( model->type != mod_brush )
	{
		gEngfuncs.Con_Reportf( S_ERROR "Decals must hit mod_brush!\n" );
		return;
	}

	decalInfo.m_pModel = model;

	// first placement moves the position into the entity's space; later restores keep it there
	if( ent && !FBitSet( flags, FDECAL_LOCAL_SPACE ))
	{
		if( !VectorIsNull( ent->angles ))
		{
			matrix4x4 matrix;

			Matrix4x4_CreateFromEntity( matrix, ent->angles, ent->origin, 1.0f );
			Matrix4x4_VectorITransform( matrix, pos, decalInfo.m_Position );
		}
		else VectorSubtract( pos, ent->origin, decalInfo.m_Position );

		SetBits( flags, FDECAL_LOCAL_SPACE );
	}
	else VectorCopy( pos, decalInfo.m_Position );

	// models that exist only in world space move with the landmark on transitions
	if( !FBitSet( model->flags, MODEL_HAS_ORIGIN ))
		SetBits( flags, FDECAL_USE_LANDMARK );

	decalInfo.m_iTexture = textureIndex;
	decalInfo.m_Entity = entityIndex;
	decalInfo.m_Flags = flags;

	R_GL3DecalDimensions( textureIndex, &width, &height );
	decalInfo.m_Size = Q_max( width >> 1, height >> 1 );
	decalInfo.m_scale = bound( MIN_DECAL_SCALE, scale, MAX_DECAL_SCALE );
	decalInfo.m_decalWidth = (int)( width / decalInfo.m_scale );
	decalInfo.m_decalHeight = (int)( height / decalInfo.m_scale );

	R_GL3DecalNode( model, &model->nodes[model->hulls[0].firstclipnode], &decalInfo );
}

/*
==============================================================================

SAVE LISTS AND REMOVAL

==============================================================================
*/
static int R_GL3DecalListAdd( decallist_t *pList, int count )
{
	const decallist_t *pdecal = pList + count;

	for( int i = 0; i < count; i++ )
	{
		if( !Q_strcmp( pdecal->name, pList[i].name ) && pdecal->entityIndex == pList[i].entityIndex )
		{
			vec3_t tmp;

			VectorSubtract( pdecal->position, pList[i].position, tmp );
			if( VectorLength( tmp ) < DECAL_OVERLAP_DISTANCE )
				return count; // merged
		}
	}

	return count + 1;
}

static int R_GL3DecalDepthCompare( const void *a, const void *b )
{
	const decallist_t *elem1 = (const decallist_t *)a;
	const decallist_t *elem2 = (const decallist_t *)b;

	return elem1->depth > elem2->depth ? 1 : ( elem1->depth < elem2->depth ? -1 : 0 );
}

// the decals a save game keeps, lowest depth first so they are applied again in order
int R_GL3CreateDecalList( decallist_t *pList )
{
	int total = 0;

	if( WORLDMODEL )
	{
		for( int i = 0; i < MAX_RENDER_DECALS; i++ )
		{
			decal_t *decal = &gl3_decals[i];
			decallist_t *entry = &pList[total];
			int depth = 0;

			if( !decal->psurface || FBitSet( decal->flags, FDECAL_DONTSAVE ))
				continue;

			for( decal_t *d = decal->psurface->pdecals; d && d != decal; d = d->pnext )
				depth++;

			entry->depth = depth;
			entry->flags = decal->flags;
			entry->scale = decal->scale;
			VectorCopy( decal->position, entry->position );
			entry->entityIndex = decal->entityIndex;

			if( FBitSet( decal->psurface->flags, SURF_PLANEBACK ))
				VectorNegate( decal->psurface->plane->normal, entry->impactPlaneNormal );
			else VectorCopy( decal->psurface->plane->normal, entry->impactPlaneNormal );

			COM_FileBase( R_GL3TextureName( decal->texture ), entry->name, sizeof( entry->name ));
			total = R_GL3DecalListAdd( pList, total );
		}

		if( gEngfuncs.drawFuncs->R_CreateStudioDecalList )
			total += gEngfuncs.drawFuncs->R_CreateStudioDecalList( pList, total );
	}

	qsort( pList, total, sizeof( decallist_t ), R_GL3DecalDepthCompare );
	return total;
}

// all decals with this texture (0: all), permanent ones stay
void R_GL3DecalRemoveAll( int textureIndex )
{
	if( textureIndex < 0 || textureIndex >= GL3_MAX_TEXTURES )
		return;

	for( int i = 0; i < gl3_decal_count; i++ )
	{
		decal_t *pdecal = &gl3_decals[i];

		if( FBitSet( pdecal->flags, FDECAL_PERMANENT ))
			continue;

		if( !textureIndex || pdecal->texture == textureIndex )
			R_GL3DecalUnlink( pdecal );
	}
}

void R_GL3ClearAllDecals( void )
{
	for( int i = 0; i < MAX_RENDER_DECALS; i++ )
		R_GL3DecalUnlink( &gl3_decals[i] );

	if( gEngfuncs.drawFuncs->R_ClearStudioDecals )
		gEngfuncs.drawFuncs->R_ClearStudioDecals();
}
