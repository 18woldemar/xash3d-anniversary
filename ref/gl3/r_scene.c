/*
r_scene.c - ref_gl3: entity lists, view setup, the 3D frame
Copyright (C) 2026 xash3d-xenon
View, frustum and entity list logic follow ref/gl/gl_rmain.c and gl_frustum.c,
Copyright (C) 2010-2016 Uncle Mike

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
Matrices are ref_gl's (row-major, applied to column vectors, OpenGL clip space) so the engine and client get
what they expect from WorldToScreen and GetMatrix. Only what goes to the GPU is converted: Direct3D wants clip
z in [0, w] where OpenGL has [-w, w], which gives the same depth buffer values as glDepthRange( 0, 1 ).
*/

#include "r_local.h"
#include "entity_types.h"

gl3_view_t    gl3_ri;
gl3_globals_t gl3_tr;

static D3DVIEWPORT9 gl3_viewport;

/*
==============================================================================

FRUSTUM

==============================================================================
*/
static void R_GL3FrustumSetPlane( int side, const vec3_t normal, float dist )
{
	mplane_t *p = &gl3_ri.frustum.planes[side];

	p->type = PlaneTypeForNormal( normal );
	p->signbits = SignbitsForPlane( normal );
	VectorCopy( normal, p->normal );
	p->dist = dist;
	SetBits( gl3_ri.frustum.clipFlags, BIT( side ));
}

// no near plane, as in ref_gl's general view
static void R_GL3FrustumInit( float zfar, float fov_x, float fov_y )
{
	const float *org = gl3_ri.rvp.vieworigin;
	vec3_t normal, iforward, farpoint;
	float s, c;

	gl3_ri.frustum.clipFlags = 0;

	SinCos( DEG2RAD( fov_x ) * 0.5f, &s, &c );
	VectorMAM( s, gl3_ri.vforward, -c, gl3_ri.vright, normal );
	R_GL3FrustumSetPlane( GL3_FRUSTUM_LEFT, normal, DotProduct( org, normal ));
	VectorMAM( s, gl3_ri.vforward, c, gl3_ri.vright, normal );
	R_GL3FrustumSetPlane( GL3_FRUSTUM_RIGHT, normal, DotProduct( org, normal ));

	SinCos( DEG2RAD( fov_y ) * 0.5f, &s, &c );
	VectorMAM( s, gl3_ri.vforward, -c, gl3_ri.vup, normal );
	R_GL3FrustumSetPlane( GL3_FRUSTUM_BOTTOM, normal, DotProduct( org, normal ));
	VectorMAM( s, gl3_ri.vforward, c, gl3_ri.vup, normal );
	R_GL3FrustumSetPlane( GL3_FRUSTUM_TOP, normal, DotProduct( org, normal ));

	VectorNegate( gl3_ri.vforward, iforward );
	VectorMA( org, zfar, gl3_ri.vforward, farpoint );
	R_GL3FrustumSetPlane( GL3_FRUSTUM_FAR, iforward, DotProduct( iforward, farpoint ));
}

// true when the box is outside; clipflags 0 means all planes
qboolean R_GL3CullBox( const vec3_t mins, const vec3_t maxs, int clipflags )
{
	int flags = clipflags ? clipflags : gl3_ri.frustum.clipFlags;

	for( int i = 0; i < GL3_FRUSTUM_PLANES; i++ )
	{
		const mplane_t *p = &gl3_ri.frustum.planes[i];
		vec3_t corner;

		if( !FBitSet( flags, BIT( i )))
			continue;

		// the corner furthest along the normal
		corner[0] = FBitSet( p->signbits, 1 ) ? mins[0] : maxs[0];
		corner[1] = FBitSet( p->signbits, 2 ) ? mins[1] : maxs[1];
		corner[2] = FBitSet( p->signbits, 4 ) ? mins[2] : maxs[2];

		if( DotProduct( p->normal, corner ) < p->dist )
			return true;
	}

	return false;
}

/*
==============================================================================

ENTITY LISTS

==============================================================================
*/
cl_entity_t *R_GL3EntityByIndex( int index )
{
	if( index < 0 || (uint)index >= gl3_tr.max_entities || !gl3_tr.entities )
		return NULL;
	return &gl3_tr.entities[index];
}

static qboolean R_GL3OpaqueEntity( cl_entity_t *ent )
{
	if( R_GL3EntityRenderMode( ent ) != kRenderNormal )
		return false;

	switch( ent->curstate.renderfx )
	{
	case kRenderFxNone:
	case kRenderFxDeadPlayer:
	case kRenderFxLightMultiplier:
	case kRenderFxExplode:
		return true;
	}

	return false;
}

// gl_rmain.c CL_FxBlend (the int conversions truncate as there)
int R_GL3FxBlend( cl_entity_t *e )
{
	const double time = gp_cl->time;
	float offset = (int)e->index * 363.0f; // de-syncs the effects of different entities
	int blend;

	switch( e->curstate.renderfx )
	{
	case kRenderFxPulseSlowWide:
		blend = e->curstate.renderamt + 0x40 * sin( time * 2 + offset );
		break;
	case kRenderFxPulseFastWide:
		blend = e->curstate.renderamt + 0x40 * sin( time * 8 + offset );
		break;
	case kRenderFxPulseSlow:
		blend = e->curstate.renderamt + 0x10 * sin( time * 2 + offset );
		break;
	case kRenderFxPulseFast:
		blend = e->curstate.renderamt + 0x10 * sin( time * 8 + offset );
		break;
	case kRenderFxFadeSlow:
		if( !FBitSet( gl3_ri.rvp.flags, RF_DRAW_CUBEMAP ))
			e->curstate.renderamt = Q_max( 0, e->curstate.renderamt - 1 );
		blend = e->curstate.renderamt;
		break;
	case kRenderFxFadeFast:
		if( !FBitSet( gl3_ri.rvp.flags, RF_DRAW_CUBEMAP ))
			e->curstate.renderamt = e->curstate.renderamt > 3 ? e->curstate.renderamt - 4 : 0;
		blend = e->curstate.renderamt;
		break;
	case kRenderFxSolidSlow:
		if( !FBitSet( gl3_ri.rvp.flags, RF_DRAW_CUBEMAP ))
			e->curstate.renderamt = Q_min( 255, e->curstate.renderamt + 1 );
		blend = e->curstate.renderamt;
		break;
	case kRenderFxSolidFast:
		if( !FBitSet( gl3_ri.rvp.flags, RF_DRAW_CUBEMAP ))
			e->curstate.renderamt = e->curstate.renderamt < 252 ? e->curstate.renderamt + 4 : 255;
		blend = e->curstate.renderamt;
		break;
	case kRenderFxStrobeSlow:
		blend = (int)( 20 * sin( time * 4 + offset )) < 0 ? 0 : e->curstate.renderamt;
		break;
	case kRenderFxStrobeFast:
		blend = (int)( 20 * sin( time * 16 + offset )) < 0 ? 0 : e->curstate.renderamt;
		break;
	case kRenderFxStrobeFaster:
		blend = (int)( 20 * sin( time * 36 + offset )) < 0 ? 0 : e->curstate.renderamt;
		break;
	case kRenderFxFlickerSlow:
		blend = (int)( 20 * ( sin( time * 2 ) + sin( time * 17 + offset ))) < 0 ? 0 : e->curstate.renderamt;
		break;
	case kRenderFxFlickerFast:
		blend = (int)( 20 * ( sin( time * 16 ) + sin( time * 23 + offset ))) < 0 ? 0 : e->curstate.renderamt;
		break;
	case kRenderFxHologram:
	case kRenderFxDistort:
	{
		vec3_t delta;
		float dist;

		VectorSubtract( e->origin, gl3_ri.rvp.vieworigin, delta );
		dist = e->curstate.renderfx == kRenderFxDistort ? 1.0f : DotProduct( delta, gl3_ri.vforward );

		if( dist <= 0 )
		{
			blend = 0;
			break;
		}

		e->curstate.renderamt = 180;
		if( dist <= 100 )
			blend = e->curstate.renderamt;
		else blend = (int)(( 1.0f - ( dist - 100 ) * ( 1.0f / 400.0f )) * e->curstate.renderamt );
		blend += gEngfuncs.COM_RandomLong( -32, 31 );
		break;
	}
	default:
		blend = e->curstate.renderamt;
		break;
	}

	return bound( 0, blend, 255 );
}

void R_GL3ClearScene( void )
{
	gl3_tr.draw_list->num_solid = 0;
	gl3_tr.draw_list->num_trans = 0;
	gl3_tr.draw_list->num_beams = 0;

	if( gEngfuncs.drawFuncs->R_ClearScene != NULL )
		gEngfuncs.drawFuncs->R_ClearScene();
}

qboolean R_GL3AddEntity( cl_entity_t *clent, int type )
{
	gl3_drawlist_t *list = gl3_tr.draw_list;

	if( !r_drawentities->value )
		return false;

	if( FBitSet( clent->curstate.effects, EF_NODRAW ))
		return false;

	if( clent->curstate.rendermode != kRenderNormal && R_GL3FxBlend( clent ) <= 0 )
		return true; // invisible

	if( type == ET_BEAM )
	{
		if( list->num_beams >= MAX_VISIBLE_PACKET )
		{
			gEngfuncs.Con_Printf( S_ERROR "Too many beams %d!\n", list->num_beams );
			return false;
		}
		list->beams[list->num_beams++] = clent;
	}
	else if( R_GL3OpaqueEntity( clent ))
	{
		if( list->num_solid >= MAX_VISIBLE_PACKET )
			return false;
		list->solid[list->num_solid++] = clent;
	}
	else
	{
		if( list->num_trans >= MAX_VISIBLE_PACKET )
			return false;
		list->trans[list->num_trans++] = clent;
	}

	return true;
}

void R_GL3PushScene( void )
{
	if( ++gl3_tr.draw_stack_pos >= GL3_MAX_DRAW_STACK )
		gEngfuncs.Host_Error( "draw stack overflow\n" );
	gl3_tr.draw_list = &gl3_tr.draw_stack[gl3_tr.draw_stack_pos];
}

void R_GL3PopScene( void )
{
	if( --gl3_tr.draw_stack_pos < 0 )
		gEngfuncs.Host_Error( "draw stack underflow\n" );
	gl3_tr.draw_list = &gl3_tr.draw_stack[gl3_tr.draw_stack_pos];
}

void R_GL3ProcessEntData( qboolean allocate, cl_entity_t *entities, unsigned int max_entities )
{
	if( !allocate )
	{
		gl3_tr.draw_list->num_solid = 0;
		gl3_tr.draw_list->num_trans = 0;
		gl3_tr.draw_list->num_beams = 0;
		gl3_tr.max_entities = 0;
		gl3_tr.entities = NULL;
	}
	else
	{
		gl3_tr.max_entities = max_entities;
		gl3_tr.entities = entities;
	}

	if( gEngfuncs.drawFuncs->R_ProcessEntData )
		gEngfuncs.drawFuncs->R_ProcessEntData( allocate );
}

// gl_rmain.c R_TransEntityCompare: far to near, then by render mode; alpha-tested brushes first
static float R_GL3SortDistance( cl_entity_t *ent, int rendermode )
{
	vec3_t org, delta;

	if( ent->model->type == mod_brush && rendermode == kRenderTransAlpha )
		return 1000000000.0f;

	VectorAverage( ent->model->mins, ent->model->maxs, org );
	VectorAdd( ent->origin, org, org );
	VectorSubtract( gl3_ri.rvp.vieworigin, org, delta );
	return DotProduct( delta, delta );
}

static int R_GL3RankForRenderMode( int rendermode )
{
	switch( rendermode )
	{
	case kRenderTransTexture: return 1;
	case kRenderTransAdd: return 2;
	case kRenderGlow: return 3;
	}
	return 0;
}

typedef struct
{
	cl_entity_t *ent;
	uint         index; // in the list: equal entities keep their order, as glibc's merge sort does
} gl3_sortent_t;

static int R_GL3TransEntityCompare( const void *a, const void *b )
{
	const gl3_sortent_t *s1 = (const gl3_sortent_t *)a;
	const gl3_sortent_t *s2 = (const gl3_sortent_t *)b;
	cl_entity_t *ent1 = s1->ent;
	cl_entity_t *ent2 = s2->ent;
	int mode1 = R_GL3EntityRenderMode( ent1 );
	int mode2 = R_GL3EntityRenderMode( ent2 );
	float dist1 = R_GL3SortDistance( ent1, mode1 );
	float dist2 = R_GL3SortDistance( ent2, mode2 );
	int rank1, rank2;

	if( dist1 > dist2 ) return -1;
	if( dist1 < dist2 ) return 1;

	rank1 = R_GL3RankForRenderMode( mode1 );
	rank2 = R_GL3RankForRenderMode( mode2 );
	if( rank1 != rank2 )
		return rank1 > rank2 ? 1 : -1;

	return s1->index < s2->index ? -1 : ( s1->index > s2->index ? 1 : 0 );
}

static void R_GL3SortTransEntities( void )
{
	static gl3_sortent_t sorted[MAX_VISIBLE_PACKET];
	gl3_drawlist_t *list = gl3_tr.draw_list;

	for( uint i = 0; i < list->num_trans; i++ )
	{
		sorted[i].ent = list->trans[i];
		sorted[i].index = i;
	}

	qsort( sorted, list->num_trans, sizeof( *sorted ), R_GL3TransEntityCompare );

	for( uint i = 0; i < list->num_trans; i++ )
		list->trans[i] = sorted[i].ent;
}

/*
==============================================================================

MATRICES

==============================================================================
*/
// the object-to-clip matrix for the GPU: clip z moved from [-w, w] to [0, w]
void R_GL3ClipMatrix( const matrix4x4 m, float rows[16] )
{
	for( int j = 0; j < 4; j++ )
	{
		rows[j] = m[0][j];
		rows[4 + j] = m[1][j];
		rows[8 + j] = 0.5f * ( m[2][j] + m[3][j] );
		rows[12 + j] = m[3][j];
	}
}

// the current object's matrices from gl3_ri.objectMatrix; the triangle API draws with them from now on
static void R_GL3LoadObject( void )
{
	matrix4x4 mvp;
	float rows[16];

	// the affine concatenation leaves the last row alone
	Matrix4x4_ConcatTransforms( gl3_ri.modelviewMatrix, gl3_ri.worldviewMatrix, gl3_ri.objectMatrix );
	Vector4Set( gl3_ri.modelviewMatrix[3], 0.0f, 0.0f, 0.0f, 1.0f );
	Matrix4x4_Concat( mvp, gl3_ri.projectionMatrix, gl3_ri.modelviewMatrix );
	Matrix4x4_Copy( gl3_ri.objectClipMatrix, mvp );
	R_GL3ClipMatrix( mvp, rows );
	R_GL3SetTransform( rows );
}

void R_GL3LoadIdentity( void )
{
	if( gl3_tr.modelviewIdentity )
		return;

	Matrix4x4_LoadIdentity( gl3_ri.objectMatrix );
	R_GL3LoadObject();
	gl3_tr.modelviewIdentity = true;
}

void R_GL3RotateForEntity( cl_entity_t *e )
{
	if( e == R_GL3EntityByIndex( 0 ))
	{
		R_GL3LoadIdentity();
		return;
	}

	Matrix4x4_CreateFromEntity( gl3_ri.objectMatrix, e->angles, e->origin, 1.0f );
	R_GL3LoadObject();
	gl3_tr.modelviewIdentity = false;
}

void R_GL3TranslateForEntity( cl_entity_t *e )
{
	if( e == R_GL3EntityByIndex( 0 ))
	{
		R_GL3LoadIdentity();
		return;
	}

	Matrix4x4_CreateFromEntity( gl3_ri.objectMatrix, vec3_origin, e->origin, 1.0f );
	R_GL3LoadObject();
	gl3_tr.modelviewIdentity = false;
}

static float R_GL3FarClip( void )
{
	if( WORLDMODEL && FBitSet( gl3_ri.rvp.flags, RF_DRAW_WORLD ))
		return gp_movevars->zmax * 1.73f;
	return 2048.0f;
}

static void R_GL3SetupMatrices( void )
{
	const float znear = 4.0f;
	float zfar, xmax, ymax;

	gl3_ri.farClip = R_GL3FarClip();
	zfar = Q_max( 256.0f, gl3_ri.farClip );
	ymax = znear * tan( gl3_ri.rvp.fov_y * M_PI_F / 360.0f );
	xmax = znear * tan( gl3_ri.rvp.fov_x * M_PI_F / 360.0f );
	Matrix4x4_CreateProjection( gl3_ri.projectionMatrix, xmax, -xmax, ymax, -ymax, znear, zfar );

	Matrix4x4_CreateModelview( gl3_ri.worldviewMatrix );
	Matrix4x4_ConcatRotate( gl3_ri.worldviewMatrix, -gl3_ri.rvp.viewangles[2], 1, 0, 0 );
	Matrix4x4_ConcatRotate( gl3_ri.worldviewMatrix, -gl3_ri.rvp.viewangles[0], 0, 1, 0 );
	Matrix4x4_ConcatRotate( gl3_ri.worldviewMatrix, -gl3_ri.rvp.viewangles[1], 0, 0, 1 );
	Matrix4x4_ConcatTranslate( gl3_ri.worldviewMatrix, -gl3_ri.rvp.vieworigin[0], -gl3_ri.rvp.vieworigin[1], -gl3_ri.rvp.vieworigin[2] );

	Matrix4x4_Concat( gl3_ri.worldviewProjectionMatrix, gl3_ri.projectionMatrix, gl3_ri.worldviewMatrix );

	gl3_tr.modelviewIdentity = false;
	R_GL3LoadIdentity();
}

/*
==============================================================================

FRAME

==============================================================================
*/
static void R_GL3SetupView( void )
{
	const int *vp = gl3_ri.rvp.viewport;

	// ref_gl rounds the corners outwards; the top-left origin needs no flip here
	gl3_viewport.X = Q_max( 0, vp[0] );
	gl3_viewport.Y = Q_max( 0, vp[1] );
	gl3_viewport.Width = Q_min( gpGlobals->width - (int)gl3_viewport.X, vp[2] );
	gl3_viewport.Height = Q_min( gpGlobals->height - (int)gl3_viewport.Y, vp[3] );
	gl3_viewport.MinZ = 0.0f;
	gl3_viewport.MaxZ = 1.0f;

	R_GL3DrawReset();
	gl3_device->SetViewport( &gl3_viewport );
	gl3_device->Clear( 0, NULL, D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0, 1.0f, 0 );

	R_GL3SetupMatrices();

	gl3_wanted.ztest = TRUE;
	gl3_wanted.zwrite = TRUE;
	gl3_wanted.cull = GL3_CULL_FRONT;
	gl3_wanted.blend = FALSE;
	gl3_wanted.alphatest = FALSE;
	R_GL3TriColor4ub( 255, 255, 255, 255 );
}

// gl_rmain.c R_RecursiveFindWaterTexture: a water face near the view's leaf, for the fog colour
static gl3_texture_t *R_GL3FindWaterTexture( const mnode_t *node, const mnode_t *ignore, qboolean down )
{
	model_t *world = WORLDMODEL;

	if( node->contents == CONTENTS_SOLID )
		return NULL;

	if( node->contents < 0 )
	{
		const mleaf_t *leaf = (const mleaf_t *)node;
		msurface_t **mark = leaf->firstmarksurface;

		if( node->contents != CONTENTS_WATER && node->contents != CONTENTS_LAVA && node->contents != CONTENTS_SLIME )
			return NULL;

		for( int i = 0; i < leaf->nummarksurfaces; i++, mark++ )
		{
			if( FBitSet( (*mark)->flags, SURF_DRAWTURB ) && (*mark)->texinfo && (*mark)->texinfo->texture )
				return R_GL3GetTexture( (*mark)->texinfo->texture->gl_texturenum );
		}

		return NULL;
	}

	for( int side = 0; side < 2; side++ )
	{
		mnode_t *child = node_child( node, side, world );

		if( child && child != ignore )
		{
			gl3_texture_t *tex = R_GL3FindWaterTexture( child, node, true );
			if( tex )
				return tex;
		}
	}

	if( down || !node->parent )
		return NULL;

	return R_GL3FindWaterTexture( node->parent, node, false );
}

#define GL3_LIQUID( cnt ) ( cnt == CONTENTS_WATER || cnt == CONTENTS_SLIME || cnt == CONTENTS_LAVA )

// gl_rmain.c R_CheckFog for Half-Life: fog when the view is under a liquid, colour from its texture
void R_GL3CheckFog( void )
{
	const int waterlevel = ENGINE_GET_PARM( PARM_WATER_LEVEL );
	gl3_texture_t *tex = NULL;
	cl_entity_t *ent;
	int cnt;

	// ponytail: Quake's global fog (movevars fog_settings) is not here, Half-Life has none
	gl3_ri.fogEnabled = false;

	if( FBitSet( gl3_ri.rvp.flags, RF_ONLY_CLIENTDRAW ) || waterlevel < 3 || !FBitSet( gl3_ri.rvp.flags, RF_DRAW_WORLD ) || !gl3_ri.viewleaf )
	{
		if( gl3_ri.cached_waterlevel == 3 )
		{
			// the water level can jump from 3 to 1
			gl3_ri.cached_waterlevel = waterlevel;
			gl3_ri.cached_contents = CONTENTS_EMPTY;
		}
		return;
	}

	ent = gEngfuncs.CL_GetWaterEntity( gl3_ri.rvp.vieworigin );
	if( ent && ent->model && ent->model->type == mod_brush && ent->curstate.skin < 0 )
		cnt = ent->curstate.skin;
	else cnt = gl3_ri.viewleaf->contents;

	gl3_ri.cached_waterlevel = waterlevel;

	if( GL3_LIQUID( gl3_ri.cached_contents ) || !GL3_LIQUID( cnt ))
	{
		gl3_ri.fogEnabled = true;
		gl3_ri.fogSkybox = true;
		return;
	}

	// just went under: the colour and density of this liquid
	if( ent && ent->model && ent->model->type == mod_brush )
	{
		msurface_t *surf = &ent->model->surfaces[ent->model->firstmodelsurface];

		for( int i = 0; i < ent->model->nummodelsurfaces; i++, surf++ )
		{
			if( FBitSet( surf->flags, SURF_DRAWTURB ) && surf->texinfo && surf->texinfo->texture )
			{
				tex = R_GL3GetTexture( surf->texinfo->texture->gl_texturenum );
				gl3_ri.cached_contents = ent->curstate.skin;
				break;
			}
		}
	}
	else
	{
		tex = R_GL3FindWaterTexture( gl3_ri.viewleaf->parent, NULL, false );
		if( tex )
			gl3_ri.cached_contents = gl3_ri.viewleaf->contents;
	}

	if( !tex )
		return;

	gl3_ri.fogColor[0] = tex->fogParams[0] / 255.0f;
	gl3_ri.fogColor[1] = tex->fogParams[1] / 255.0f;
	gl3_ri.fogColor[2] = tex->fogParams[2] / 255.0f;
	// GL_RMAIN.C R_SetupFrame: GL_LINEAR, start 0, end = 1536 - 4 * density; the shaders take 1 / end
	gl3_ri.fogEndInv = 1.0f / ( 1536.0f - 4.0f * (float)tex->fogParams[3] );
	gl3_ri.fogEnabled = true;
	gl3_ri.fogSkybox = true;
}

// the view's viewport with another depth range (the view model), 0..1 restores it
void R_GL3SetDepthRange( float minz, float maxz )
{
	D3DVIEWPORT9 vp = gl3_viewport;

	vp.MinZ = minz;
	vp.MaxZ = maxz;
	R_GL3DrawReset();
	gl3_device->SetViewport( &vp );
}

void R_GL3RestoreViewport( void )
{
	D3DVIEWPORT9 full = { 0, 0, (DWORD)gpGlobals->width, (DWORD)gpGlobals->height, 0.0f, 1.0f };

	R_GL3DrawReset();
	gl3_device->SetViewport( &full );
}

// what an off-screen pass leaves behind: the viewport of the view being drawn, not the whole screen
void R_GL3RestoreViewViewport( void )
{
	R_GL3DrawReset();
	gl3_device->SetViewport( &gl3_viewport );
}

static void R_GL3RenderScene( void )
{
	static int trans_first[MAX_VISIBLE_PACKET];
	static float trans_blend[MAX_VISIBLE_PACKET]; // CL_FxBlend steps fades: once per entity, as in ref_gl
	const gl3_drawlist_t *list = gl3_tr.draw_list;
	const qboolean entities = !FBitSet( gl3_ri.rvp.flags, RF_ONLY_CLIENTDRAW );
	model_t *world = WORLDMODEL;
	int world_end, solid_end, trans_end, water_end;
	qboolean drawworld;
	const qboolean worldfog = gl3_ri.fogEnabled; // ref_gl fogs the world by the last frame's decision

	gl3_tr.frametime = FBitSet( gl3_ri.rvp.flags, RF_DRAW_CUBEMAP ) ? 0.0 : gp_cl->time - gp_cl->oldtime;
	gl3_tr.framecount++;

	if( world )
		gl3_tr.dlightframecount = R_PushDlights( world, gl3_tr.framecount );

	// gl_rmain.c R_RenderScene: the water field steps before anything reads it, and only outside the
	// recording - its upload locks a texture the recorded commands may already be reading
	if( R_GL3RipplesWanted( ) && !R_GL3TiledSceneActive( ))
		R_GL3AnimateRipples();

	// The angles are mended before anything is built from them: the basis, but also the view matrices and
	// the sprite code, which read gl3_ri.rvp.viewangles themselves. Usually it is the roll alone that
	// breaks, so the view only loses its tilt instead of pointing somewhere else entirely.
	if( VectorIsNAN( gl3_ri.rvp.viewangles ))
	{
		static int reported;

		if( reported++ < 8 )
		{
			gEngfuncs.Con_Printf( S_ERROR "the view angles are not a number: %g %g %g\n",
				gl3_ri.rvp.viewangles[0], gl3_ri.rvp.viewangles[1], gl3_ri.rvp.viewangles[2] );
		}

		for( int i = 0; i < 3; i++ )
		{
			if( IS_NAN( gl3_ri.rvp.viewangles[i] ))
				gl3_ri.rvp.viewangles[i] = 0.0f;
		}
	}

	// before the flashlight: it traces along this basis, and reading the previous frame's one let a
	// single frame of bad view angles live on in it, which sent the traces off with NaN coordinates
	AngleVectors( gl3_ri.rvp.viewangles, gl3_ri.vforward, gl3_ri.vright, gl3_ri.vup );

	R_GL3FlashlightUpdate();

	R_GL3FrustumInit( R_GL3FarClip(), gl3_ri.rvp.fov_x, gl3_ri.rvp.fov_y );

	gl3_ri.viewplanedist = DotProduct( gl3_ri.rvp.vieworigin, gl3_ri.vforward );
	R_GL3SortTransEntities();

	if( world && FBitSet( gl3_ri.rvp.flags, RF_DRAW_WORLD ))
	{
		gl3_ri.oldviewleaf = gl3_ri.viewleaf;
		gl3_ri.viewleaf = gEngfuncs.Mod_PointInLeaf( gl3_ri.rvp.vieworigin, world->nodes, world );
	}

	R_GL3SetupView();

	// everything drawn from the world's buffers is collected first: the index buffer is filled once
	R_GL3WorldBeginFrame();
	drawworld = world && R_GL3EntityByIndex( 0 ) && FBitSet( gl3_ri.rvp.flags, RF_DRAW_WORLD ) && !FBitSet( gl3_ri.rvp.flags, RF_ONLY_CLIENTDRAW );
	R_GL3WorldSetFog( worldfog );
	if( drawworld )
		R_GL3CollectWorld();
	world_end = R_GL3WorldDrawCount();

	R_GL3CheckFog();
	R_GL3WorldSetFog( gl3_ri.fogEnabled );

	gl3_tr.blend = 1.0f;
	for( uint i = 0; i < list->num_solid; i++ )
	{
		cl_entity_t *e = list->solid[i];

		if( e->model && e->model->type == mod_brush )
			R_GL3CollectBrushModel( e );
	}
	solid_end = R_GL3WorldDrawCount();

	for( uint i = 0; i < list->num_trans; i++ )
	{
		cl_entity_t *e = list->trans[i];

		trans_first[i] = R_GL3WorldDrawCount();
		gl3_tr.blend = trans_blend[i] = e->curstate.rendermode != kRenderNormal ? R_GL3FxBlend( e ) / 255.0f : 1.0f;
		if( gl3_tr.blend > 0.0f && e->model && e->model->type == mod_brush )
			R_GL3CollectBrushModel( e );
	}
	trans_end = R_GL3WorldDrawCount();

	if( drawworld )
		R_GL3CollectWaterAlpha();
	water_end = R_GL3WorldDrawCount();

	R_GL3WorldUpload();

	// The flashlight's map renders off screen, so it has to be done before EDRAM belongs to the bands.
	// A frame can hold more than one view - a camera or a monitor gives the client a second one - and
	// the later views run inside the recording, where switching render targets hangs the GPU. They keep
	// the map the first view filled.
	if( !R_GL3TiledSceneActive( ))
		R_GL3FlashlightShadow( 0, solid_end );
	R_GL3EnsureTiledScene();
	R_GL3LoadIdentity();

	if( drawworld )
	{
		R_GL3DrawWorldRange( 0, world_end );
		R_GL3DrawSky( worldfog );
		gEngfuncs.R_DrawWorldHull();
	}

	gEngfuncs.CL_ExtraUpdate();

	// entities in ref_gl's order (gl_rmain.c R_DrawEntitiesOnList); studio vertices are in world space
	R_GL3DrawWorldRange( world_end, solid_end );

	gl3_tr.blend = 1.0f;
	for( uint i = 0; i < list->num_solid && entities; i++ )
	{
		cl_entity_t *e = list->solid[i];

		if( e->model && e->model->type == mod_studio )
			R_GL3DrawStudioModel( e );
	}

	for( uint i = 0; i < list->num_solid && entities; i++ )
	{
		cl_entity_t *e = list->solid[i];

		if( e->model && e->model->type == mod_sprite )
			R_GL3DrawSpriteModel( e );
	}

	// solid beams (the engine retires dead beams first)
	if( entities )
	{
		R_GL3LoadIdentity();
		gEngfuncs.CL_DrawEFX( gl3_tr.frametime, false );
	}
	R_GL3EffectFog( false );

	if( FBitSet( gl3_ri.rvp.flags, RF_DRAW_WORLD ))
		gEngfuncs.pfnDrawNormalTriangles();

	for( uint i = 0; i < list->num_trans; i++ )
	{
		cl_entity_t *e = list->trans[i];

		R_GL3DrawWorldRange( trans_first[i], i + 1 < list->num_trans ? trans_first[i + 1] : trans_end );

		gl3_tr.blend = trans_blend[i];
		if( gl3_tr.blend <= 0.0f || !e->model || !entities )
			continue;

		if( e->model->type == mod_studio )
			R_GL3DrawStudioModel( e );
		else if( e->model->type == mod_sprite )
			R_GL3DrawSpriteModel( e );
	}
	R_GL3EffectFog( false );

	R_GL3LoadIdentity();
	if( FBitSet( gl3_ri.rvp.flags, RF_DRAW_WORLD ))
	{
		R_GL3TriRenderMode( kRenderNormal );
		gEngfuncs.pfnDrawTransparentTriangles();
	}

	// translucent beams, particles and tracers, unfogged; this also moves particles and retires dead ones
	if( entities )
	{
		R_GL3LoadIdentity();
		gEngfuncs.CL_DrawEFX( gl3_tr.frametime, true );
		R_GL3EffectFog( false );
	}

	// the view model draws unblended; task 3 gives it depth range 0..0.3
	gl3_wanted.blend = FALSE;
	if( entities )
		R_GL3DrawViewModel();
	gEngfuncs.CL_ExtraUpdate();

	R_GL3DrawWorldRange( trans_end, water_end );
}

void R_GL3RenderFrame( const ref_viewpass_t *rvp )
{
	if( r_norefresh->value )
		return;

	gl3_ri.rvp = *rvp;
	gl3_ri.farClip = 0.0f;

	if( gEngfuncs.drawFuncs->GL_RenderFrame != NULL && gEngfuncs.drawFuncs->GL_RenderFrame( rvp ))
	{
		if( gl3_tr.viewent )
			R_GatherPlayerLight( gl3_tr.viewent );
		gl3_tr.realframecount++;
		gl3_tr.fResetVis = true;
		return;
	}

	// the view model's muzzle flashes join the lists before the translucent sort
	if( !FBitSet( gl3_ri.rvp.flags, RF_ONLY_CLIENTDRAW ))
		R_GL3RunViewmodelEvents();

	gl3_tr.realframecount++;
	R_GL3RenderScene();
}

void R_GL3NewMapScene( void )
{
	gl3_tr.world = (world_static_t *)ENGINE_GET_PARM( PARM_GET_WORLD_PTR );
	gl3_tr.viewent = (cl_entity_t *)ENGINE_GET_PARM( PARM_GET_VIEWENT_PTR );
	gl3_ri.viewleaf = gl3_ri.oldviewleaf = NULL;
	gl3_ri.fogEnabled = false;
	gl3_ri.cached_contents = CONTENTS_EMPTY;
	gl3_ri.cached_waterlevel = 0;
	gl3_tr.framecount = gl3_tr.visframecount = gl3_tr.realframecount = 1;
	gl3_tr.fResetVis = true;
	gl3_tr.visValid = false;
}

void R_GL3InitScene( void )
{
	gl3_tr.world = (world_static_t *)ENGINE_GET_PARM( PARM_GET_WORLD_PTR );
	gl3_tr.viewent = (cl_entity_t *)ENGINE_GET_PARM( PARM_GET_VIEWENT_PTR );
	gl3_tr.draw_list = &gl3_tr.draw_stack[0];
	gl3_tr.draw_stack_pos = 0;
}

/*
==============================================================================

ENGINE AND CLIENT QUERIES

==============================================================================
*/
// gl_rmain.c R_WorldToScreen: normalized device x and y, true when the point is behind the view
int R_GL3WorldToScreen( const vec3_t point, vec3_t screen )
{
	const matrix4x4 *m = &gl3_ri.worldviewProjectionMatrix;
	float w;

	if( !point || !screen )
		return true;

	screen[0] = (*m)[0][0] * point[0] + (*m)[0][1] * point[1] + (*m)[0][2] * point[2] + (*m)[0][3];
	screen[1] = (*m)[1][0] * point[0] + (*m)[1][1] * point[1] + (*m)[1][2] * point[2] + (*m)[1][3];
	w = (*m)[3][0] * point[0] + (*m)[3][1] * point[1] + (*m)[3][2] * point[2] + (*m)[3][3];
	screen[2] = 0.0f;

	if( w < 0.001f )
		return true;

	screen[0] /= w;
	screen[1] /= w;
	return false;
}

// gl_triapi.c TriWorldToScreen: pixels in the view's viewport, y down (the renderer's beams, tracers, glows)
int R_GL3TriWorldToScreen( const float *world, float *screen )
{
	int retval = R_GL3WorldToScreen( world, screen );

	screen[0] = 0.5f * screen[0] * (float)gl3_ri.rvp.viewport[2];
	screen[1] = -0.5f * screen[1] * (float)gl3_ri.rvp.viewport[3];
	screen[0] += 0.5f * (float)gl3_ri.rvp.viewport[2];
	screen[1] += 0.5f * (float)gl3_ri.rvp.viewport[3];

	return retval;
}

void R_GL3ScreenToWorld( const float *screen, float *point )
{
	matrix4x4 inv;
	float w;

	if( !point || !screen )
		return;

	Matrix4x4_Invert_Full( inv, gl3_ri.worldviewProjectionMatrix );
	point[0] = screen[0] * inv[0][0] + screen[1] * inv[0][1] + screen[2] * inv[0][2] + inv[0][3];
	point[1] = screen[0] * inv[1][0] + screen[1] * inv[1][1] + screen[2] * inv[1][2] + inv[1][3];
	point[2] = screen[0] * inv[2][0] + screen[1] * inv[2][1] + screen[2] * inv[2][2] + inv[2][3];
	w = screen[0] * inv[3][0] + screen[1] * inv[3][1] + screen[2] * inv[3][2] + inv[3][3];
	if( w != 0.0f )
		VectorScale( point, 1.0f / w, point );
}

// glGetFloatv for the two matrices client code asks for
void R_GL3GetMatrix( const int pname, float *matrix )
{
	switch( pname )
	{
	case 0x0BA6: // GL_MODELVIEW_MATRIX
		Matrix4x4_ToArrayFloatGL( gl3_ri.modelviewMatrix, matrix );
		break;
	case 0x0BA7: // GL_PROJECTION_MATRIX
		Matrix4x4_ToArrayFloatGL( gl3_ri.projectionMatrix, matrix );
		break;
	default:
		memset( matrix, 0, 16 * sizeof( *matrix ));
		break;
	}
}

// NULL: everything counts as visible (no PVS yet)
byte *R_GL3GetCurrentVis( void )
{
	return gl3_tr.visValid ? gl3_ri.visbytes : NULL;
}
