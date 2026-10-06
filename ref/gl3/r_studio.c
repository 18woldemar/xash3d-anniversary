/*
r_studio.c - ref_gl3: studio models, the engine_studio_api_t side
Copyright (C) 2026 xash3d-xenon
Follows ref/gl/gl_studio.c, Copyright (C) 2010 Uncle Mike

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
In Half-Life the client (hlsdk CStudioModelRenderer) draws every studio model of the 3D view through
engine_studio_api_t: it animates the bones straight into gl3_studio.bonestransform and calls back here for
culling, lighting and StudioDrawPoints. The builtin renderer (R_StudioDrawModel, R_StudioDrawPlayer) is
ref_gl's, for views without the world (the menu's player preview) and r_studio_builtin_renderer 1.
Not ported: the flipped view model (cl_righthand does not exist in this tree), bone weights
(STUDIO_HAS_BONEINFO: no Half-Life model has them and the engine does not byte-swap mstudioboneinfo_t),
shadows (r_shadows 0) and the builtin r_drawentities 6 and 7 views.
*/

#include "r_local.h"
#include "r_studioint.h"
#include "triangleapi.h"

#define EVENT_CLIENT  5000 // lower numbers are server-side studio events
#define SHADE_LAMBERT 1.4953241f

typedef struct
{
	char     name[MAX_OSPATH];
	char     modelname[MAX_OSPATH];
	model_t *model;
} gl3_player_model_t;

gl3_studio_t gl3_studio;

// what only this file uses (the rest of ref_gl's g_studio and m_p* globals)
static struct
{
	double   time;
	double   frametime;
	qboolean interpolate;
	int      models_drawn;
	dlight_t *elights;

	player_info_t *playerinfo; // builtin player: its gait state
	float          gaitmovement;

	// builtin renderer: the last model's bones, for MOVETYPE_FOLLOW and weapon models
	matrix3x4 cached_bonestransform[MAXSTUDIOBONES];
	matrix3x4 cached_lighttransform[MAXSTUDIOBONES];
	char      cached_bonenames[MAXSTUDIOBONES][32];
	int       cached_numbones;

	gl3_player_model_t player_models[MAX_CLIENTS];
	r_studio_interface_t *draw;
	cvar_t *builtin_renderer;
} gl3_st;

static const vec3_t gl3_hullcolor[8] =
{
	{ 1.0f, 1.0f, 1.0f },
	{ 1.0f, 0.5f, 0.5f },
	{ 0.5f, 1.0f, 0.5f },
	{ 1.0f, 1.0f, 0.5f },
	{ 0.5f, 0.5f, 1.0f },
	{ 1.0f, 0.5f, 1.0f },
	{ 0.5f, 1.0f, 1.0f },
	{ 1.0f, 1.0f, 1.0f },
};

static qboolean R_GL3ViewIsLocalPlayer( void )
{
	return gp_cl->viewentity == gp_cl->playernum + 1;
}

// gl_local.h LightToTexGamma, ScreenGammaTable and LinearGammaTable
static uint R_GL3GammaTable( const uint16_t *table, uint b )
{
	if( b >= 1024 )
		return 0;

	return FBitSet( gp_host->features, ENGINE_LINEAR_GAMMA_SPACE ) ? b : table[b];
}

void R_GL3StudioInit( void )
{
	Matrix3x4_LoadIdentity( gl3_studio.rotationmatrix );
	gl3_studio.framecount = 0;
	gl3_studio.doremap = false;
	gl3_studio.lightgammatable = (const uint16_t *)ENGINE_GET_PARM( PARM_GET_LIGHTGAMMATABLE_PTR );
	gl3_studio.screengammatable = (const uint16_t *)ENGINE_GET_PARM( PARM_GET_SCREENGAMMATABLE_PTR );
	gl3_studio.lineargammatable = (const uint16_t *)ENGINE_GET_PARM( PARM_GET_LINEARGAMMATABLE_PTR );
	gl3_st.interpolate = true;
	gl3_st.elights = (dlight_t *)ENGINE_GET_PARM( PARM_GET_ELIGHTS_PTR );
	gl3_st.builtin_renderer = gEngfuncs.Cvar_Get( "r_studio_builtin_renderer", "0", 0, "use built-in studio model renderer instead of the one provided by client library (debugging)" );
}

// the clock of the model being drawn: the client's, or real time in the menu
static void R_StudioSetupTimings( void )
{
	if( FBitSet( gl3_ri.rvp.flags, RF_DRAW_WORLD ))
	{
		gl3_st.time = gp_cl->time;
		gl3_st.frametime = gp_cl->time - gp_cl->oldtime;
	}
	else
	{
		gl3_st.time = gp_host->realtime;
		gl3_st.frametime = gp_host->frametime;
	}
}

// gl_cull.c R_CullModel: the first person view model is never culled
qboolean R_GL3CullModel( const cl_entity_t *e, const vec3_t absmin, const vec3_t absmax )
{
	if( e == gl3_tr.viewent )
	{
		if( ENGINE_GET_PARM( PARM_DEV_OVERVIEW ))
			return true;

		return FBitSet( gl3_ri.rvp.flags, RF_DRAW_CUBEMAP ) || ENGINE_GET_PARM( PARM_THIRDPERSON ) || !R_GL3ViewIsLocalPlayer();
	}

	return R_GL3CullBox( absmin, absmax, 0 );
}

// the model box around the current sequence box; bbox NULL: false when the model is culled
static qboolean R_StudioComputeBBox( vec3_t bbox[8] )
{
	cl_entity_t *e = gl3_ri.currententity;
	studiohdr_t *hdr = gl3_studio.header;
	mstudioseqdesc_t *pseqdesc;
	vec3_t mins, maxs, studio_mins, studio_maxs;

	if( !hdr )
		return false;

	if( !VectorIsNull( gl3_ri.currentmodel->mins ) && !VectorIsNull( gl3_ri.currentmodel->maxs ))
	{
		VectorCopy( gl3_ri.currentmodel->mins, mins );
		VectorCopy( gl3_ri.currentmodel->maxs, maxs );
	}
	else ClearBounds( mins, maxs );

	if( e->curstate.sequence < 0 || e->curstate.sequence >= hdr->numseq )
		e->curstate.sequence = 0;

	pseqdesc = (mstudioseqdesc_t *)((byte *)hdr + hdr->seqindex) + e->curstate.sequence;
	AddPointToBounds( pseqdesc->bbmin, mins, maxs );
	AddPointToBounds( pseqdesc->bbmax, mins, maxs );
	ClearBounds( studio_mins, studio_maxs );

	for( int i = 0; i < 8; i++ )
	{
		vec3_t p1, p2;

		p1[0] = ( i & 1 ) ? mins[0] : maxs[0];
		p1[1] = ( i & 2 ) ? mins[1] : maxs[1];
		p1[2] = ( i & 4 ) ? mins[2] : maxs[2];

		Matrix3x4_VectorTransform( gl3_studio.rotationmatrix, p1, p2 );
		AddPointToBounds( p2, studio_mins, studio_maxs );
		if( bbox ) VectorCopy( p2, bbox[i] );
	}

	return bbox || !R_GL3CullModel( e, studio_mins, studio_maxs );
}

/*
==============================================================================

ENGINE_STUDIO_API_T

==============================================================================
*/
static cl_entity_t *pfnGetCurrentEntity( void )
{
	return gl3_ri.currententity;
}

static player_info_t *pfnPlayerInfo( int index )
{
	if( !FBitSet( gl3_ri.rvp.flags, RF_DRAW_WORLD ))
		index = -1;

	return gEngfuncs.pfnPlayerInfo( index );
}

static entity_state_t *R_StudioGetPlayerState( int index )
{
	if( !FBitSet( gl3_ri.rvp.flags, RF_DRAW_WORLD ) && gl3_ri.currententity )
		return &gl3_ri.currententity->curstate;

	return gEngfuncs.pfnGetPlayerState( index );
}

static void pfnGetEngineTimes( int *framecount, double *current, double *old )
{
	if( framecount ) *framecount = gl3_tr.realframecount;
	if( current ) *current = gp_cl->time;
	if( old ) *old = gp_cl->oldtime;
}

static void pfnGetViewInfo( float *origin, float *upv, float *rightv, float *forwardv )
{
	if( origin ) VectorCopy( gl3_ri.rvp.vieworigin, origin );
	if( forwardv ) VectorCopy( gl3_ri.vforward, forwardv );
	if( rightv ) VectorCopy( gl3_ri.vright, rightv );
	if( upv ) VectorCopy( gl3_ri.vup, upv );
}

static void pfnGetModelCounters( int **s, int **a )
{
	*s = &gl3_studio.framecount;
	*a = &gl3_st.models_drawn;
}

static float ****pfnStudioGetBoneTransform( void )
{
	return (float ****)gl3_studio.bonestransform;
}

static float ****pfnStudioGetLightTransform( void )
{
	return (float ****)gl3_studio.lighttransform;
}

static float ***pfnStudioGetRotationMatrix( void )
{
	return (float ***)gl3_studio.rotationmatrix;
}

static void R_StudioSetupModel( int bodypart, void **ppbodypart, void **ppsubmodel )
{
	studiohdr_t *hdr = gl3_studio.header;
	int index;

	if( bodypart > hdr->numbodyparts )
		bodypart = 0; // gl_studio.c's off-by-one, kept

	gl3_studio.bodypart = (mstudiobodyparts_t *)((byte *)hdr + hdr->bodypartindex) + bodypart;
	index = ( gl3_ri.currententity->curstate.body / gl3_studio.bodypart->base ) % gl3_studio.bodypart->nummodels;
	gl3_studio.submodel = (mstudiomodel_t *)((byte *)hdr + gl3_studio.bodypart->modelindex) + index;

	if( ppbodypart ) *ppbodypart = gl3_studio.bodypart;
	if( ppsubmodel ) *ppsubmodel = gl3_studio.submodel;
}

static int R_StudioCheckBBox( void )
{
	if( !gl3_ri.currententity || !gl3_ri.currentmodel )
		return false;

	return R_StudioComputeBBox( NULL );
}

static void pfnStudioDynamicLight( cl_entity_t *ent, alight_t *plight )
{
	R_EntityDynamicLight( ent, plight, FBitSet( gl3_ri.rvp.flags, RF_DRAW_WORLD ), gl3_st.time, gl3_studio.lightspot, gl3_studio.lightvec );
}

// the strongest elights near the model (at most GL3_MAX_LOCALLIGHTS)
static void R_StudioEntityLight( alight_t *lightinfo )
{
	float lstrength[GL3_MAX_LOCALLIGHTS];
	cl_entity_t *ent = gl3_ri.currententity;
	float dist2 = 1000000.0f;
	vec3_t origin;

	gl3_studio.numlocallights = 0;

	if( !ent || !r_dynamic->value )
		return;

	for( int i = 0; i < GL3_MAX_LOCALLIGHTS; i++ )
		lstrength[i] = 0;

	Matrix3x4_OriginFromMatrix( gl3_studio.rotationmatrix, origin );

	for( int lnum = 0; lnum < MAX_ELIGHTS; lnum++ )
	{
		dlight_t *el = &gl3_st.elights[lnum];
		vec3_t mid;
		float f, r2, minstrength;
		int k;

		if( el->die < gl3_st.time || el->radius <= 0.0f )
			continue;

		// lights keyed to this entity follow it (or its attachment)
		if(( el->key & 0xFFF ) == ent->index )
		{
			int att = ( el->key >> 12 ) & 0xF;

			if( att ) VectorCopy( ent->attachment[att], el->origin );
			else VectorCopy( ent->origin, el->origin );
		}

		VectorSubtract( origin, el->origin, mid );
		f = DotProduct( mid, mid );
		r2 = el->radius * el->radius;
		minstrength = f > r2 ? r2 / f : 1.0f;

		if( minstrength <= 0.05f )
			continue;

		if( gl3_studio.numlocallights >= GL3_MAX_LOCALLIGHTS )
		{
			k = -1;
			for( int j = 0; j < gl3_studio.numlocallights; j++ )
			{
				if( lstrength[j] < dist2 && lstrength[j] < minstrength )
				{
					dist2 = lstrength[j];
					k = j;
				}
			}
		}
		else k = gl3_studio.numlocallights;

		if( k == -1 )
			continue;

		gl3_studio.locallightcolor[k][0] = R_GL3GammaTable( gl3_studio.lineargammatable, el->color.r << 2 );
		gl3_studio.locallightcolor[k][1] = R_GL3GammaTable( gl3_studio.lineargammatable, el->color.g << 2 );
		gl3_studio.locallightcolor[k][2] = R_GL3GammaTable( gl3_studio.lineargammatable, el->color.b << 2 );
		gl3_studio.locallightR2[k] = r2;
		gl3_studio.locallight[k] = el;
		lstrength[k] = minstrength;

		if( k >= gl3_studio.numlocallights )
			gl3_studio.numlocallights = k + 1;
	}
}

static void R_StudioSetupLighting( alight_t *plight )
{
	float scale = 1.0f;

	if( !gl3_studio.header || !plight )
		return;

	if( gl3_ri.currententity )
		scale = gl3_ri.currententity->curstate.scale;

	gl3_studio.ambientlight = plight->ambientlight;
	gl3_studio.shadelight = plight->shadelight;
	VectorCopy( plight->plightvec, gl3_studio.lightvec );

	for( int i = 0; i < gl3_studio.header->numbones; i++ )
	{
		Matrix3x4_VectorIRotate( gl3_studio.lighttransform[i], plight->plightvec, gl3_studio.blightvec[i] );
		if( scale > 1.0f ) VectorNormalize( gl3_studio.blightvec[i] ); // in case the model is scaled
	}

	VectorCopy( plight->color, gl3_studio.lightcolor );
}

// gl_studio.c R_StudioLighting: brightness 0..1 of a normal (bone -1: world space)
static float R_StudioLighting( int bone, int flags, const vec3_t normal )
{
	float illum = gl3_studio.ambientlight;

	if( FBitSet( flags, STUDIO_NF_FULLBRIGHT ))
		return 1.0f;

	if( FBitSet( flags, STUDIO_NF_FLATSHADE ))
	{
		illum += gl3_studio.shadelight * 0.8f;
	}
	else
	{
		const float r = SHADE_LAMBERT; // above 1: this is ref_gl's hemispherical branch
		const float *lightvec = bone != -1 ? gl3_studio.blightvec[bone] : gl3_studio.lightvec;
		float lightcos = DotProduct( normal, lightvec ); // -1 colinear, 1 opposite

		if( lightcos > 1.0f ) lightcos = 1.0f;

		illum += gl3_studio.shadelight;
		lightcos = ( lightcos + ( r - 1.0f )) / r;
		if( lightcos > 0.0f )
			illum -= gl3_studio.shadelight * lightcos;

		illum = Q_max( illum, 0.0f );
	}

	illum = Q_min( illum, 255.0f );
	return R_GL3GammaTable( gl3_studio.lightgammatable, (uint)( illum * 4 )) / 1023.0f;
}

static void R_StudioSetupSkin( void *ptexturehdr, int index )
{
	studiohdr_t *hdr = (studiohdr_t *)ptexturehdr;
	mstudiotexture_t *ptexture = NULL;

	if( FBitSet( gl3_studio.forcefaceflags, STUDIO_NF_CHROME ) || !hdr )
		return;

	// the client may not have called StudioSetRemapColors, and an entity may have no remap info at all
	if( gl3_studio.doremap )
	{
		struct remap_info_s *info = gEngfuncs.CL_GetRemapInfoForEntity( gl3_ri.currententity );

		if( info )
			ptexture = info->ptexture;
	}
	if( !ptexture )
		ptexture = (mstudiotexture_t *)((byte *)hdr + hdr->textureindex);

	if( r_lightmap->value && !r_fullbright->value )
		R_GL3Bind( gl3_white_texture );
	else R_GL3Bind( ptexture[index].index );
}

static void R_StudioSetRenderamt( int iRenderamt )
{
	if( !gl3_ri.currententity )
		return;

	gl3_ri.currententity->curstate.renderamt = iRenderamt;
	gl3_tr.blend = R_GL3FxBlend( gl3_ri.currententity ) / 255.0f;
}

static void R_StudioSetCullState( int iCull )
{
	// does nothing in ref_gl either
}

// a prefab the client never calls: a sprite quad in black
static void R_StudioRenderShadow( int iSprite, float *p1, float *p2, float *p3, float *p4 )
{
	if( !p1 || !p2 || !p3 || !p4 || !R_GL3SpriteTexture( R_GL3ModelHandle( iSprite ), 0 ))
		return;

	R_GL3TriRenderMode( kRenderTransAlpha );
	R_GL3TriColor4f( 0.0f, 0.0f, 0.0f, 1.0f );
	R_GL3TriBegin( TRI_QUADS );
	R_GL3TriTexCoord2f( 0.0f, 0.0f );
	R_GL3TriVertex3fv( p1 );
	R_GL3TriTexCoord2f( 0.0f, 1.0f );
	R_GL3TriVertex3fv( p2 );
	R_GL3TriTexCoord2f( 1.0f, 1.0f );
	R_GL3TriVertex3fv( p3 );
	R_GL3TriTexCoord2f( 1.0f, 0.0f );
	R_GL3TriVertex3fv( p4 );
	R_GL3TriEnd();
	R_GL3TriRenderMode( kRenderNormal );
}

// r_studiomesh.c draws the submodel (state notes in r_local.h)
static void R_StudioDrawPoints( void )
{
	if( gl3_studio.header )
		R_GL3StudioDrawPoints();
}

// the debug views (r_drawentities 2..5) draw with the triangle API, untextured
static void R_StudioDebugBox( const vec3_t p[8], int bone, const vec3_t color, float alpha )
{
	R_GL3TriBegin( TRI_QUADS );

	for( int j = 0; j < 6; j++ )
	{
		vec3_t normal;
		float lv;

		VectorClear( normal );
		normal[j % 3] = j < 3 ? 1.0f : -1.0f;
		lv = R_StudioLighting( bone, 0, normal ) * alpha; // gl_triapi.c TriBrightness

		R_GL3TriColor4f( color[0] * lv, color[1] * lv, color[2] * lv, 1.0f );
		for( int k = 0; k < 4; k++ )
			R_GL3TriVertex3fv( p[boxpnt[j][k]] );
	}

	R_GL3TriEnd();
}

static void R_StudioDrawHulls( void )
{
	studiohdr_t *hdr = gl3_studio.header;
	mstudiobbox_t *pbbox = (mstudiobbox_t *)((byte *)hdr + hdr->hitboxindex);
	const float alpha = r_drawentities->value == 4 ? 0.5f : 1.0f;

	R_GL3Bind( gl3_white_texture );

	for( int i = 0; i < hdr->numhitboxes; i++ )
	{
		vec3_t tmp, p[8];

		for( int j = 0; j < 8; j++ )
		{
			tmp[0] = ( j & 1 ) ? pbbox[i].bbmin[0] : pbbox[i].bbmax[0];
			tmp[1] = ( j & 2 ) ? pbbox[i].bbmin[1] : pbbox[i].bbmax[1];
			tmp[2] = ( j & 4 ) ? pbbox[i].bbmin[2] : pbbox[i].bbmax[2];
			Matrix3x4_VectorTransform( gl3_studio.bonestransform[pbbox[i].bone], tmp, p[j] );
		}

		R_StudioDebugBox( p, pbbox[i].bone, gl3_hullcolor[pbbox[i].group % 8], alpha );
	}
}

static void R_StudioDrawAbsBBox( void )
{
	static const vec3_t color = { 0.5f, 0.5f, 1.0f };
	vec3_t p[8];

	// looks ugly, skip
	if( gl3_ri.currententity == gl3_tr.viewent || !R_StudioComputeBBox( p ))
		return;

	R_GL3Bind( gl3_white_texture );
	R_GL3TriRenderMode( kRenderTransAdd );
	R_StudioDebugBox( p, -1, color, 0.5f );
	R_GL3TriRenderMode( kRenderNormal );
}

// ponytail: lines only, the triangle API draws no points (ref_gl marks the joints with them)
static void R_StudioDrawBones( void )
{
	studiohdr_t *hdr = gl3_studio.header;
	mstudiobone_t *pbones = (mstudiobone_t *)((byte *)hdr + hdr->boneindex);

	R_GL3Bind( gl3_white_texture );
	R_GL3TriColor4f( 1.0f, 0.7f, 0.0f, 1.0f );
	R_GL3TriBegin( TRI_LINES );

	for( int i = 0; i < hdr->numbones; i++ )
	{
		vec3_t point;

		if( pbones[i].parent < 0 )
			continue;

		Matrix3x4_OriginFromMatrix( gl3_studio.bonestransform[pbones[i].parent], point );
		R_GL3TriVertex3fv( point );
		Matrix3x4_OriginFromMatrix( gl3_studio.bonestransform[i], point );
		R_GL3TriVertex3fv( point );
	}

	R_GL3TriEnd();
}

static void R_StudioSetRemapColors( int newTop, int newBottom )
{
	if( gEngfuncs.CL_EntitySetRemapColors( gl3_ri.currententity, gl3_ri.currentmodel, newTop, newBottom ))
		gl3_studio.doremap = true;
}

void R_GL3StudioResetPlayerModels( void )
{
	memset( gl3_st.player_models, 0, sizeof( gl3_st.player_models ));
}

// the model a player is drawn with: models/player/<name> for the menu, multiplayer and developer mode
static model_t *R_StudioSetupPlayerModel( int index )
{
	player_info_t *info = gEngfuncs.pfnPlayerInfo( index );
	gl3_player_model_t *state;

	if( index < 0 || index >= gp_cl->maxclients )
		return NULL;

	state = &gl3_st.player_models[index];

	if(( gpGlobals->developer || !ENGINE_GET_PARM( PARM_SINGLEPLAYER_GAME ) || !FBitSet( gl3_ri.rvp.flags, RF_DRAW_WORLD )) && info->model[0] )
	{
		if( Q_strcmp( state->name, info->model ))
		{
			Q_strncpy( state->name, info->model, sizeof( state->name ));
			Q_snprintf( state->modelname, sizeof( state->modelname ), "models/player/%s/%s.mdl", info->model, info->model );

			if( gEngfuncs.fsapi->FileExists( state->modelname, false ))
				state->model = gEngfuncs.Mod_ForName( state->modelname, false, true );
			else state->model = NULL;

			if( !state->model )
				state->model = gl3_ri.currententity->model;
		}
	}
	else
	{
		state->model = gl3_ri.currententity->model;
		state->name[0] = 0;
	}

	return state->model;
}

// gl_studio.c R_GetEntityRenderMode: a studio model mostly of additive textures sorts as additive
int R_GL3EntityRenderMode( cl_entity_t *ent )
{
	cl_entity_t *oldent = gl3_ri.currententity;
	model_t *model = NULL;
	studiohdr_t *phdr;
	mstudiotexture_t *ptexture;
	int opaque = 0, trans = 0;

	gl3_ri.currententity = ent;
	if( ent->player )
		model = R_StudioSetupPlayerModel( ent->curstate.number - 1 );
	if( !model )
		model = ent->model;
	gl3_ri.currententity = oldent;

	phdr = (studiohdr_t *)gEngfuncs.Mod_Extradata( mod_studio, model );
	if( !phdr )
	{
		if( ent->curstate.rendermode == kRenderNormal && model && model->type == mod_brush && FBitSet( model->flags, MODEL_TRANSPARENT ))
			return kRenderTransAlpha;
		return ent->curstate.rendermode;
	}

	// ponytail: counted on every call, also inside the translucent sort; cache per model if it shows up
	ptexture = (mstudiotexture_t *)((byte *)phdr + phdr->textureindex);
	for( int i = 0; i < phdr->numtextures; i++ )
	{
		// chrome with additive is a specular-like effect
		if( FBitSet( ptexture[i].flags, STUDIO_NF_ADDITIVE ) && !FBitSet( ptexture[i].flags, STUDIO_NF_CHROME ))
			trans++;
		else opaque++;
	}

	return trans > opaque ? kRenderTransAdd : ent->curstate.rendermode;
}

static void R_StudioClientEvents( void )
{
	cl_entity_t *e = gl3_ri.currententity;
	studiohdr_t *hdr = gl3_studio.header;
	mstudioseqdesc_t *pseqdesc;
	mstudioevent_t *pevent;
	float start, end;

	if( gl3_st.frametime == 0.0 )
		return; // paused

	// no attachments: all four at the model origin
	if( hdr->numattachments <= 0 )
	{
		for( int i = 0; i < 4; i++ )
			Matrix3x4_OriginFromMatrix( gl3_studio.rotationmatrix, e->attachment[i] );
	}

	if( FBitSet( e->curstate.effects, EF_MUZZLEFLASH ))
	{
		dlight_t *el = gEngfuncs.CL_AllocElight( 0 );

		ClearBits( e->curstate.effects, EF_MUZZLEFLASH );
		VectorCopy( e->attachment[0], el->origin );
		el->die = gp_cl->time + 0.05f;
		el->color.r = 255;
		el->color.g = 192;
		el->color.b = 64;
		el->decay = 320;
		el->radius = 24;
	}

	pseqdesc = (mstudioseqdesc_t *)((byte *)hdr + hdr->seqindex) + bound( 0, e->curstate.sequence, hdr->numseq - 1 );
	if( pseqdesc->numevents == 0 )
		return;

	end = R_GL3StudioEstimateFrame( e, pseqdesc, gl3_st.time );
	start = end - e->curstate.framerate * gp_host->frametime * pseqdesc->fps;
	pevent = (mstudioevent_t *)((byte *)hdr + pseqdesc->eventindex);

	if( e->latched.sequencetime == e->curstate.animtime && !FBitSet( pseqdesc->flags, STUDIO_LOOPING ))
		start = -0.01f;

	for( int i = 0; i < pseqdesc->numevents; i++ )
	{
		if( pevent[i].event < EVENT_CLIENT )
			continue;

		if( (float)pevent[i].frame > start && pevent[i].frame <= end )
			gEngfuncs.pfnStudioEvent( &pevent[i], e );
	}
}

static int R_StudioGetForceFaceFlags( void )
{
	return gl3_studio.forcefaceflags;
}

static void R_StudioSetForceFaceFlags( int flags )
{
	gl3_studio.forcefaceflags = flags;
}

static void R_StudioSetHeader( void *header )
{
	gl3_studio.header = (studiohdr_t *)header;
	gl3_studio.doremap = false;
}

// PARM_GET_STUDIO_HDR: the engine swaps client-loaded sequence group files for it (model.c)
studiohdr_t *R_GL3StudioGetHeader( void )
{
	return gl3_studio.header;
}

static void R_StudioSetRenderModel( model_t *model )
{
	gl3_ri.currentmodel = model;
}

// ref_gl also turns fog off for additive and glow modes, and back on in RestoreRenderer
static void R_StudioSetupRenderer( int rendermode )
{
	if( rendermode > kRenderTransAdd )
		rendermode = 0;
	gl3_studio.rendermode = bound( 0, rendermode, kRenderTransAdd );
}

static void R_StudioRestoreRenderer( void )
{
	gl3_studio.doremap = false;
}

static void R_StudioSetChromeOrigin( void )
{
	VectorCopy( gl3_ri.rvp.vieworigin, gl3_studio.chrome_origin );
}

// with r_shadows 0 ref_gl only turns depth writes back on here
static void GL_StudioDrawShadow( void )
{
}

// gl_studio.c GL_StudioSetRenderMode: TransAdd draws with colour blend and no depth writes, other
// translucent modes with alpha blend
static void GL_StudioSetRenderMode( int rendermode )
{
	gl3_studio.meshmode = rendermode;
}

/*
==============================================================================

BUILTIN RENDERER

==============================================================================
*/
static void R_StudioPlayerBlend( mstudioseqdesc_t *pseqdesc, int *pBlend, float *pPitch )
{
	// calc up/down pointing
	*pBlend = ( *pPitch * 3.0f );

	if( *pBlend < pseqdesc->blendstart[0] )
	{
		*pPitch -= pseqdesc->blendstart[0] / 3.0f;
		*pBlend = 0;
	}
	else if( *pBlend > pseqdesc->blendend[0] )
	{
		*pPitch -= pseqdesc->blendend[0] / 3.0f;
		*pBlend = 255;
	}
	else
	{
		if( pseqdesc->blendend[0] - pseqdesc->blendstart[0] < 0.1f ) // catch qc error
			*pBlend = 127;
		else *pBlend = 255 * ( *pBlend - pseqdesc->blendstart[0] ) / ( pseqdesc->blendend[0] - pseqdesc->blendstart[0] );
		*pPitch = 0.0f;
	}
}

// engine hook too, but only with ENGINE_COMPUTE_STUDIO_LERP (not Half-Life)
void R_GL3StudioLerpMovement( cl_entity_t *e, double time, vec3_t origin, vec3_t angles )
{
	float f = 1.0f;

	// the limit is 1 s, twice the lag multiplayer characters are interpolated over
	if( gl3_st.interpolate && ( time < e->curstate.animtime + 1.0f ) && ( e->curstate.animtime != e->latched.prevanimtime ))
		f = ( time - e->curstate.animtime ) / ( e->curstate.animtime - e->latched.prevanimtime );

	VectorLerp( e->latched.prevorigin, f, e->curstate.origin, origin );

	if( !VectorCompareEpsilon( e->curstate.angles, e->latched.prevangles, ON_EPSILON ))
	{
		vec4_t q, q1, q2;

		AngleQuaternion( e->curstate.angles, q1, false );
		AngleQuaternion( e->latched.prevangles, q2, false );
		QuaternionSlerp( q2, q1, f, q );
		QuaternionAngle( q, angles );
	}
	else VectorCopy( e->curstate.angles, angles );
}

static void R_StudioSetUpTransform( cl_entity_t *e )
{
	vec3_t origin = Vec3( e->origin );
	vec3_t angles = Vec3( e->angles );

	if( e->curstate.movetype == MOVETYPE_STEP && !FBitSet( gp_host->features, ENGINE_COMPUTE_STUDIO_LERP ))
		R_GL3StudioLerpMovement( e, gl3_st.time, origin, angles );

	if( !FBitSet( gp_host->features, ENGINE_COMPENSATE_QUAKE_BUG ))
		angles[PITCH] = -angles[PITCH]; // stupid quake bug

	// don't rotate clients, only aim
	if( e->player )
		angles[PITCH] = 0.0f;

	Matrix3x4_CreateFromEntity( gl3_studio.rotationmatrix, angles, origin, 1.0f );
}

// engine hook: CL_ResetLatchedVars takes the previous frame from it
float R_GL3StudioEstimateFrame( cl_entity_t *e, mstudioseqdesc_t *pseqdesc, double time )
{
	double dfdt, f;

	if( gl3_st.interpolate && time >= e->curstate.animtime )
		dfdt = ( time - e->curstate.animtime ) * e->curstate.framerate * pseqdesc->fps;
	else dfdt = 0.0;

	if( pseqdesc->numframes <= 1 )
		f = 0.0;
	else f = ( e->curstate.frame * ( pseqdesc->numframes - 1 )) / 256.0f;

	f += dfdt;

	if( FBitSet( pseqdesc->flags, STUDIO_LOOPING ))
	{
		if( pseqdesc->numframes > 1 )
			f -= (int)( f / ( pseqdesc->numframes - 1 )) * ( pseqdesc->numframes - 1 );
		if( f < 0 )
			f += ( pseqdesc->numframes - 1 );
	}
	else
	{
		if( f >= pseqdesc->numframes - 1.001 )
			f = pseqdesc->numframes - 1.001;
		if( f < 0.0 )
			f = 0.0;
	}

	return f;
}

static float R_StudioEstimateInterpolant( cl_entity_t *e )
{
	float dadt = 1.0f;

	if( gl3_st.interpolate && ( e->curstate.animtime >= e->latched.prevanimtime + 0.01f ))
	{
		dadt = ( gl3_st.time - e->curstate.animtime ) / 0.1f;
		if( dadt > 2.0f )
			dadt = 2.0f;
	}

	return dadt;
}

// renderfx on the root bones (vertex transforms only)
static void R_StudioFxTransform( cl_entity_t *ent, matrix3x4 transform )
{
	switch( ent->curstate.renderfx )
	{
	case kRenderFxDistort:
	case kRenderFxHologram:
		if( !gEngfuncs.COM_RandomLong( 0, 49 ))
		{
			int axis = gEngfuncs.COM_RandomLong( 0, 1 );

			if( axis == 1 ) axis = 2; // x or z
			VectorScale( transform[axis], gEngfuncs.COM_RandomFloat( 1.0f, 1.484f ), transform[axis] );
		}
		else if( !gEngfuncs.COM_RandomLong( 0, 49 ))
		{
			float offset;

			gEngfuncs.COM_RandomLong( 0, 1 ); // an unused axis, kept for the random sequence
			offset = gEngfuncs.COM_RandomFloat( -10.0f, 10.0f );
			transform[gEngfuncs.COM_RandomLong( 0, 2 )][3] += offset;
		}
		break;
	case kRenderFxExplode:
	{
		float scale = 1.0f + ( gl3_st.time - ent->curstate.animtime ) * 10.0f;

		if( scale > 2.0f )
			scale = 2.0f; // don't blow up more than 200%

		transform[0][1] *= scale;
		transform[1][1] *= scale;
		transform[2][1] *= scale;
		break;
	}
	}
}

static void R_StudioCalcBoneAdj( float dadt, float *adj, const byte *pcontroller1, const byte *pcontroller2, byte mouthopen )
{
	studiohdr_t *hdr = gl3_studio.header;
	mstudiobonecontroller_t *pbonecontroller = (mstudiobonecontroller_t *)((byte *)hdr + hdr->bonecontrollerindex);

	for( int j = 0; j < hdr->numbonecontrollers; j++ )
	{
		const mstudiobonecontroller_t *c = &pbonecontroller[j];
		float value = 0.0f;
		int i = c->index;

		if( i == STUDIO_MOUTH )
		{
			// mouth hardcoded at controller 4
			value = bound( 0.0f, (float)mouthopen / 64.0f, 1.0f );
			value = ( 1.0f - value ) * c->start + value * c->end;
		}
		else if( i < 4 )
		{
			if( FBitSet( c->type, STUDIO_RLOOP ))
			{
				// 360 degree wrapping
				if( abs( pcontroller1[i] - pcontroller2[i] ) > 128 )
				{
					int a = ( pcontroller1[i] + 128 ) % 256;
					int b = ( pcontroller2[i] + 128 ) % 256;
					value = (( a * dadt ) + ( b * ( 1.0f - dadt )) - 128 ) * ( 360.0f / 256.0f ) + c->start;
				}
				else value = ( pcontroller1[i] * dadt + pcontroller2[i] * ( 1.0f - dadt )) * ( 360.0f / 256.0f ) + c->start;
			}
			else
			{
				value = bound( 0.0f, ( pcontroller1[i] * dadt + pcontroller2[i] * ( 1.0f - dadt )) / 255.0f, 1.0f );
				value = ( 1.0f - value ) * c->start + value * c->end;
			}
		}

		switch( c->type & STUDIO_TYPES )
		{
		case STUDIO_XR:
		case STUDIO_YR:
		case STUDIO_ZR:
			adj[j] = DEG2RAD( value );
			break;
		case STUDIO_X:
		case STUDIO_Y:
		case STUDIO_Z:
			adj[j] = value;
			break;
		}
	}
}

static void R_StudioCalcRotations( cl_entity_t *e, float pos[][3], vec4_t *q, mstudioseqdesc_t *pseqdesc, mstudioanim_t *panim, float f )
{
	studiohdr_t *hdr = gl3_studio.header;
	mstudiobone_t *pbone = (mstudiobone_t *)((byte *)hdr + hdr->boneindex);
	float adj[MAXSTUDIOCONTROLLERS];
	float dadt, s;
	int frame;

	// sequences changing too fast; a negative frame could crash
	if( f > pseqdesc->numframes - 1 )
		f = 0.0f;
	else if( f < -0.01f )
		f = -0.01f;

	frame = (int)f;
	dadt = R_StudioEstimateInterpolant( e );
	s = f - frame;

	R_StudioCalcBoneAdj( dadt, adj, e->curstate.controller, e->latched.prevcontroller, e->mouth.mouthopen );

	for( int i = 0; i < hdr->numbones; i++, pbone++, panim++ )
		R_StudioCalcBones( frame, s, pbone, panim, adj, pos[i], q[i] );

	if( pseqdesc->motiontype & STUDIO_X ) pos[pseqdesc->motionbone][0] = 0.0f;
	if( pseqdesc->motiontype & STUDIO_Y ) pos[pseqdesc->motionbone][1] = 0.0f;
	if( pseqdesc->motiontype & STUDIO_Z ) pos[pseqdesc->motionbone][2] = 0.0f;
}

// bone i from its quaternion and position: root bones get the entity transform and renderfx
static void R_StudioBuildBone( cl_entity_t *e, mstudiobone_t *pbones, int i, const vec4_t q, const vec3_t pos )
{
	matrix3x4 bonematrix;

	Matrix3x4_FromOriginQuat( bonematrix, q, pos );

	if( pbones[i].parent == -1 )
	{
		Matrix3x4_ConcatTransforms( gl3_studio.bonestransform[i], gl3_studio.rotationmatrix, bonematrix );
		Matrix3x4_Copy( gl3_studio.lighttransform[i], gl3_studio.bonestransform[i] );
		R_StudioFxTransform( e, gl3_studio.bonestransform[i] );
	}
	else
	{
		Matrix3x4_ConcatTransforms( gl3_studio.bonestransform[i], gl3_studio.bonestransform[pbones[i].parent], bonematrix );
		Matrix3x4_ConcatTransforms( gl3_studio.lighttransform[i], gl3_studio.lighttransform[pbones[i].parent], bonematrix );
	}
}

// bones with the name of a cached (parent) bone take its matrices
static void R_StudioMergeBones( cl_entity_t *e, model_t *m_pSubModel )
{
	static vec4_t q[MAXSTUDIOBONES];
	static float pos[MAXSTUDIOBONES][3];
	studiohdr_t *hdr = gl3_studio.header;
	mstudioseqdesc_t *pseqdesc;
	mstudiobone_t *pbones;
	mstudioanim_t *panim;
	float f;

	if( e->curstate.sequence >= hdr->numseq )
		e->curstate.sequence = 0;

	pseqdesc = (mstudioseqdesc_t *)((byte *)hdr + hdr->seqindex) + e->curstate.sequence;
	f = R_GL3StudioEstimateFrame( e, pseqdesc, gl3_st.time );
	panim = (mstudioanim_t *)gEngfuncs.R_StudioGetAnim( hdr, m_pSubModel, pseqdesc );
	R_StudioCalcRotations( e, pos, q, pseqdesc, panim, f );
	pbones = (mstudiobone_t *)((byte *)hdr + hdr->boneindex);

	for( int i = 0; i < hdr->numbones; i++ )
	{
		int j;

		for( j = 0; j < gl3_st.cached_numbones; j++ )
		{
			if( !Q_stricmp( pbones[i].name, gl3_st.cached_bonenames[j] ))
			{
				Matrix3x4_Copy( gl3_studio.bonestransform[i], gl3_st.cached_bonestransform[j] );
				Matrix3x4_Copy( gl3_studio.lighttransform[i], gl3_st.cached_lighttransform[j] );
				break;
			}
		}

		if( j >= gl3_st.cached_numbones )
			R_StudioBuildBone( e, pbones, i, q[i], pos[i] );
	}
}

// the anims of a sequence at frame f, with its blends (weights from blend0 and blend1, 0..1)
static void R_StudioBlendedRotations( cl_entity_t *e, float pos[][3], vec4_t *q, mstudioseqdesc_t *pseqdesc, float f, float blend0, float blend1 )
{
	static vec3_t pos2[MAXSTUDIOBONES], pos3[MAXSTUDIOBONES], pos4[MAXSTUDIOBONES];
	static vec4_t q2[MAXSTUDIOBONES], q3[MAXSTUDIOBONES], q4[MAXSTUDIOBONES];
	studiohdr_t *hdr = gl3_studio.header;
	mstudioanim_t *panim = (mstudioanim_t *)gEngfuncs.R_StudioGetAnim( hdr, gl3_ri.currentmodel, pseqdesc );

	R_StudioCalcRotations( e, pos, q, pseqdesc, panim, f );

	if( pseqdesc->numblends <= 1 )
		return;

	panim += hdr->numbones;
	R_StudioCalcRotations( e, pos2, q2, pseqdesc, panim, f );
	R_StudioSlerpBones( hdr->numbones, q, pos, q2, pos2, blend0 );

	if( pseqdesc->numblends != 4 )
		return;

	panim += hdr->numbones;
	R_StudioCalcRotations( e, pos3, q3, pseqdesc, panim, f );
	panim += hdr->numbones;
	R_StudioCalcRotations( e, pos4, q4, pseqdesc, panim, f );
	R_StudioSlerpBones( hdr->numbones, q3, pos3, q4, pos4, blend0 );
	R_StudioSlerpBones( hdr->numbones, q, pos, q3, pos3, blend1 );
}

static void R_StudioSetupBones( cl_entity_t *e )
{
	static vec3_t pos[MAXSTUDIOBONES], pos1b[MAXSTUDIOBONES], pos2[MAXSTUDIOBONES];
	static vec4_t q[MAXSTUDIOBONES], q1b[MAXSTUDIOBONES], q2[MAXSTUDIOBONES];
	studiohdr_t *hdr = gl3_studio.header;
	mstudioseqdesc_t *pseqdesc;
	mstudiobone_t *pbones;
	float f, dadt;

	if( e->curstate.sequence >= hdr->numseq )
		e->curstate.sequence = 0;

	pseqdesc = (mstudioseqdesc_t *)((byte *)hdr + hdr->seqindex) + e->curstate.sequence;
	f = R_GL3StudioEstimateFrame( e, pseqdesc, gl3_st.time );
	dadt = R_StudioEstimateInterpolant( e );
	R_StudioBlendedRotations( e, pos, q, pseqdesc, f,
		( e->curstate.blending[0] * dadt + e->latched.prevblending[0] * ( 1.0f - dadt )) / 255.0f,
		( e->curstate.blending[1] * dadt + e->latched.prevblending[1] * ( 1.0f - dadt )) / 255.0f );

	if( gl3_st.interpolate && e->latched.sequencetime && ( e->latched.sequencetime + 0.2f > gl3_st.time ) && ( e->latched.prevsequence < hdr->numseq ))
	{
		// blend from the last sequence
		pseqdesc = (mstudioseqdesc_t *)((byte *)hdr + hdr->seqindex) + e->latched.prevsequence;
		R_StudioBlendedRotations( e, pos1b, q1b, pseqdesc, e->latched.prevframe,
			e->latched.prevseqblending[0] / 255.0f, e->latched.prevseqblending[1] / 255.0f );
		R_StudioSlerpBones( hdr->numbones, q, pos, q1b, pos1b, 1.0f - ( gl3_st.time - e->latched.sequencetime ) / 0.2f );
	}
	else e->latched.prevframe = f;

	pbones = (mstudiobone_t *)((byte *)hdr + hdr->boneindex);

	// gait: the legs (everything outside the "Bip01 Spine" subtree) from the gait sequence
	if( gl3_st.playerinfo && gl3_st.playerinfo->gaitsequence != 0 )
	{
		qboolean copy_bones = true;
		mstudioanim_t *panim;

		if( gl3_st.playerinfo->gaitsequence >= hdr->numseq )
			gl3_st.playerinfo->gaitsequence = 0;

		pseqdesc = (mstudioseqdesc_t *)((byte *)hdr + hdr->seqindex) + gl3_st.playerinfo->gaitsequence;
		panim = (mstudioanim_t *)gEngfuncs.R_StudioGetAnim( hdr, gl3_ri.currentmodel, pseqdesc );
		R_StudioCalcRotations( e, pos2, q2, pseqdesc, panim, gl3_st.playerinfo->gaitframe );

		for( int i = 0; i < hdr->numbones; i++ )
		{
			if( !Q_strcmp( pbones[i].name, "Bip01 Spine" ))
				copy_bones = false;
			else if( !Q_strcmp( pbones[pbones[i].parent].name, "Bip01 Pelvis" ))
				copy_bones = true;

			if( !copy_bones )
				continue;

			VectorCopy( pos2[i], pos[i] );
			Vector4Copy( q2[i], q[i] );
		}
	}

	for( int i = 0; i < hdr->numbones; i++ )
		R_StudioBuildBone( e, pbones, i, q[i], pos[i] );
}

static void R_StudioSaveBones( void )
{
	studiohdr_t *hdr = gl3_studio.header;
	mstudiobone_t *pbones = (mstudiobone_t *)((byte *)hdr + hdr->boneindex);

	gl3_st.cached_numbones = hdr->numbones;

	for( int i = 0; i < hdr->numbones; i++ )
	{
		Matrix3x4_Copy( gl3_st.cached_bonestransform[i], gl3_studio.bonestransform[i] );
		Matrix3x4_Copy( gl3_st.cached_lighttransform[i], gl3_studio.lighttransform[i] );
		Q_strncpy( gl3_st.cached_bonenames[i], pbones[i].name, 32 );
	}
}

static void R_StudioCalcAttachments( void )
{
	studiohdr_t *hdr = gl3_studio.header;
	mstudioattachment_t *pAtt = (mstudioattachment_t *)((byte *)hdr + hdr->attachmentindex);
	cl_entity_t *e = gl3_ri.currententity;

	// the entity has four (ref_gl writes up to 64)
	for( int i = 0; i < Q_min( 4, hdr->numattachments ); i++ )
		Matrix3x4_VectorTransform( gl3_studio.lighttransform[pAtt[i].bone], pAtt[i].org, e->attachment[i] );
}

// events: the attachments go to the client's copy of the entity too
static void R_StudioRunEvents( void )
{
	cl_entity_t *e = gl3_ri.currententity;
	cl_entity_t *ent = e->index > 0 ? R_GL3EntityByIndex( e->index ) : NULL;

	R_StudioCalcAttachments();
	R_StudioClientEvents();

	if( ent )
		memcpy( ent->attachment, e->attachment, sizeof( vec3_t ) * 4 );
}

static void R_StudioRenderFinal( void )
{
	int rendermode = R_StudioGetForceFaceFlags() ? kRenderTransAdd : gl3_ri.currententity->curstate.rendermode;

	R_StudioSetupRenderer( rendermode );

	if( r_drawentities->value == 2 )
	{
		R_StudioDrawBones();
	}
	else if( r_drawentities->value == 3 )
	{
		R_StudioDrawHulls();
	}
	else
	{
		for( int i = 0; i < gl3_studio.header->numbodyparts; i++ )
		{
			R_StudioSetupModel( i, NULL, NULL );
			GL_StudioSetRenderMode( rendermode );
			R_StudioDrawPoints();
			GL_StudioDrawShadow();
		}
	}

	if( r_drawentities->value == 4 )
	{
		R_GL3TriRenderMode( kRenderTransAdd );
		R_StudioDrawHulls();
		R_GL3TriRenderMode( kRenderNormal );
	}

	if( r_drawentities->value == 5 )
		R_StudioDrawAbsBBox();

	R_StudioRestoreRenderer();
}

static void R_StudioRenderModel( void )
{
	cl_entity_t *e = gl3_ri.currententity;

	R_StudioSetChromeOrigin();
	R_StudioSetForceFaceFlags( 0 );

	if( e->curstate.renderfx == kRenderFxGlowShell )
	{
		// the model, then the shell over it
		e->curstate.renderfx = kRenderFxNone;
		R_StudioRenderFinal();

		R_StudioSetForceFaceFlags( STUDIO_NF_CHROME );
		R_GL3SpriteTexture( gEngfuncs.GetDefaultSprite( REF_CHROME_SPRITE ), 0 );
		e->curstate.renderfx = kRenderFxGlowShell;
	}

	R_StudioRenderFinal();
}

static void R_StudioEstimateGait( void )
{
	cl_entity_t *e = gl3_ri.currententity;
	player_info_t *pi = gl3_st.playerinfo;
	float dt = bound( 0.0f, gl3_st.frametime, 1.0f );
	vec3_t est_velocity;

	if( dt == 0.0f || pi->renderframe == gl3_tr.realframecount )
	{
		gl3_st.gaitmovement = 0;
		return;
	}

	VectorSubtract( e->origin, pi->prevgaitorigin, est_velocity );
	VectorCopy( e->origin, pi->prevgaitorigin );
	gl3_st.gaitmovement = VectorLength( est_velocity );

	if( dt <= 0.0f || gl3_st.gaitmovement / dt < 5.0f )
	{
		gl3_st.gaitmovement = 0.0f;
		est_velocity[0] = 0.0f;
		est_velocity[1] = 0.0f;
	}

	if( est_velocity[1] == 0.0f && est_velocity[0] == 0.0f )
	{
		float flYawDiff = e->angles[YAW] - pi->gaityaw;

		flYawDiff = flYawDiff - (int)( flYawDiff / 360 ) * 360;
		if( flYawDiff > 180.0f ) flYawDiff -= 360.0f;
		if( flYawDiff < -180.0f ) flYawDiff += 360.0f;

		if( dt < 0.25f )
			flYawDiff *= dt * 4.0f;
		else flYawDiff *= dt;

		pi->gaityaw += flYawDiff;
		pi->gaityaw = pi->gaityaw - (int)( pi->gaityaw / 360 ) * 360;
		gl3_st.gaitmovement = 0.0f;
	}
	else
	{
		pi->gaityaw = ( atan2( est_velocity[1], est_velocity[0] ) * 180 / M_PI_F );
		pi->gaityaw = bound( -180.0f, pi->gaityaw, 180.0f );
	}
}

static void R_StudioSetControllers( cl_entity_t *e, byte value )
{
	for( int i = 0; i < 4; i++ )
	{
		e->curstate.controller[i] = value;
		e->latched.prevcontroller[i] = value;
	}
}

static void R_StudioProcessGait( entity_state_t *pplayer )
{
	cl_entity_t *e = gl3_ri.currententity;
	studiohdr_t *hdr = gl3_studio.header;
	player_info_t *pi = gl3_st.playerinfo;
	float dt = bound( 0.0f, gl3_st.frametime, 1.0f );
	mstudioseqdesc_t *pseqdesc;
	float flYaw;
	int iBlend;

	if( e->curstate.sequence >= hdr->numseq )
		e->curstate.sequence = 0;

	pseqdesc = (mstudioseqdesc_t *)((byte *)hdr + hdr->seqindex) + e->curstate.sequence;
	R_StudioPlayerBlend( pseqdesc, &iBlend, &e->angles[PITCH] );

	e->latched.prevangles[PITCH] = e->angles[PITCH];
	e->curstate.blending[0] = iBlend;
	e->latched.prevblending[0] = e->curstate.blending[0];
	e->latched.prevseqblending[0] = e->curstate.blending[0];
	R_StudioEstimateGait();

	// calc side to side turning
	flYaw = e->angles[YAW] - pi->gaityaw;
	flYaw = flYaw - (int)( flYaw / 360 ) * 360;
	if( flYaw < -180.0f ) flYaw = flYaw + 360.0f;
	if( flYaw > 180.0f ) flYaw = flYaw - 360.0f;

	if( flYaw > 120.0f )
	{
		pi->gaityaw = pi->gaityaw - 180.0f;
		gl3_st.gaitmovement = -gl3_st.gaitmovement;
		flYaw = flYaw - 180.0f;
	}
	else if( flYaw < -120.0f )
	{
		pi->gaityaw = pi->gaityaw + 180.0f;
		gl3_st.gaitmovement = -gl3_st.gaitmovement;
		flYaw = flYaw + 180.0f;
	}

	// adjust torso
	R_StudioSetControllers( e, ((flYaw / 4.0f) + 30.0f) / (60.0f / 255.0f));

	e->angles[YAW] = pi->gaityaw;
	if( e->angles[YAW] < -0 ) e->angles[YAW] += 360.0f;
	e->latched.prevangles[YAW] = e->angles[YAW];

	if( pplayer->gaitsequence >= hdr->numseq )
		pplayer->gaitsequence = 0;

	pseqdesc = (mstudioseqdesc_t *)((byte *)hdr + hdr->seqindex) + pplayer->gaitsequence;

	// calc gait frame
	if( pseqdesc->linearmovement[0] > 0 )
		pi->gaitframe += ( gl3_st.gaitmovement / pseqdesc->linearmovement[0] ) * pseqdesc->numframes;
	else pi->gaitframe += pseqdesc->fps * dt;

	// do modulo
	pi->gaitframe = pi->gaitframe - (int)( pi->gaitframe / pseqdesc->numframes ) * pseqdesc->numframes;
	if( pi->gaitframe < 0 ) pi->gaitframe += pseqdesc->numframes;
}

// the entity's light, elights and lighting state
static void R_StudioLightModel( alight_t *lighting, vec3_t dir )
{
	lighting->plightvec = dir;
	pfnStudioDynamicLight( gl3_ri.currententity, lighting );
	R_StudioEntityLight( lighting );
	R_StudioSetupLighting( lighting );
}

static int R_StudioDrawPlayer( int flags, entity_state_t *pplayer )
{
	cl_entity_t *e = gl3_ri.currententity;
	int m_nPlayerIndex = pplayer->number - 1;
	alight_t lighting;
	vec3_t dir;

	if( m_nPlayerIndex < 0 || m_nPlayerIndex >= gp_cl->maxclients )
		return 0;

	gl3_ri.currentmodel = R_StudioSetupPlayerModel( m_nPlayerIndex );
	if( !gl3_ri.currentmodel )
		return 0;

	R_StudioSetHeader( gEngfuncs.Mod_Extradata( mod_studio, gl3_ri.currentmodel ));

	if( pplayer->gaitsequence )
	{
		vec3_t orig_angles = Vec3( e->angles );

		gl3_st.playerinfo = pfnPlayerInfo( m_nPlayerIndex );
		R_StudioProcessGait( pplayer );
		gl3_st.playerinfo->gaitsequence = pplayer->gaitsequence;
		gl3_st.playerinfo = NULL;

		R_StudioSetUpTransform( e );
		VectorCopy( orig_angles, e->angles );
	}
	else
	{
		R_StudioSetControllers( e, 127 );
		gl3_st.playerinfo = pfnPlayerInfo( m_nPlayerIndex );
		gl3_st.playerinfo->gaitsequence = 0;
		R_StudioSetUpTransform( e );
	}

	if( flags & STUDIO_RENDER )
	{
		if( !R_StudioCheckBBox( ))
			return 0;

		gl3_st.models_drawn++;
		gl3_studio.framecount++; // render data cache cookie

		if( gl3_studio.header->numbodyparts == 0 )
			return 1;
	}

	gl3_st.playerinfo = pfnPlayerInfo( m_nPlayerIndex );
	R_StudioSetupBones( e );
	R_StudioSaveBones();
	gl3_st.playerinfo->renderframe = gl3_tr.realframecount;
	gl3_st.playerinfo = NULL;

	if( flags & STUDIO_EVENTS )
		R_StudioRunEvents();

	if( flags & STUDIO_RENDER )
	{
		player_info_t *pi;

		// the menu shows the highest resolution multiplayer model
		if( cl_himodels->value && ( gl3_ri.currentmodel != e->model || !FBitSet( gl3_ri.rvp.flags, RF_DRAW_WORLD )))
			e->curstate.body = 255;

		if( !( !gpGlobals->developer && gp_cl->maxclients == 1 ) && ( gl3_ri.currentmodel == e->model ))
			e->curstate.body = 1; // force helmet

		R_StudioLightModel( &lighting, dir );

		pi = pfnPlayerInfo( m_nPlayerIndex );
		R_StudioSetRemapColors( bound( 0, pi->topcolor, 360 ), bound( 0, pi->bottomcolor, 360 ));
		R_StudioRenderModel();

		if( pplayer->weaponmodel )
		{
			cl_entity_t saveent = *e;
			model_t *pweaponmodel = R_GL3ModelHandle( pplayer->weaponmodel );

			gl3_studio.header = (studiohdr_t *)gEngfuncs.Mod_Extradata( mod_studio, pweaponmodel );
			R_StudioMergeBones( e, pweaponmodel );
			R_StudioSetupLighting( &lighting );
			R_StudioRenderModel();
			R_StudioCalcAttachments();
			*e = saveent;
		}
	}

	return 1;
}

static int R_StudioDrawModel( int flags )
{
	cl_entity_t *e = gl3_ri.currententity;
	alight_t lighting;
	vec3_t dir;

	if( e->curstate.renderfx == kRenderFxDeadPlayer )
	{
		entity_state_t deadplayer;
		int result;

		if( e->curstate.renderamt <= 0 || e->curstate.renderamt > gp_cl->maxclients )
			return 0;

		// draw as though it were a player, without weapon and movement
		deadplayer = *R_StudioGetPlayerState( e->curstate.renderamt - 1 );
		deadplayer.number = e->curstate.renderamt;
		deadplayer.weaponmodel = 0;
		deadplayer.gaitsequence = 0;
		deadplayer.movetype = MOVETYPE_NONE;
		VectorCopy( e->curstate.angles, deadplayer.angles );
		VectorCopy( e->curstate.origin, deadplayer.origin );

		gl3_st.interpolate = false;
		result = R_StudioDrawPlayer( flags, &deadplayer );
		gl3_st.interpolate = true;
		return result;
	}

	R_StudioSetHeader( gEngfuncs.Mod_Extradata( mod_studio, gl3_ri.currentmodel ));
	R_StudioSetUpTransform( e );

	if( flags & STUDIO_RENDER )
	{
		if( !R_StudioCheckBBox( ))
			return 0;

		gl3_st.models_drawn++;
		gl3_studio.framecount++; // render data cache cookie

		if( gl3_studio.header->numbodyparts == 0 )
			return 1;
	}

	if( e->curstate.movetype == MOVETYPE_FOLLOW )
		R_StudioMergeBones( e, gl3_ri.currentmodel );
	else R_StudioSetupBones( e );
	R_StudioSaveBones();

	if( flags & STUDIO_EVENTS )
		R_StudioRunEvents();

	if( flags & STUDIO_RENDER )
	{
		R_StudioLightModel( &lighting, dir );
		R_StudioSetRemapColors( e->curstate.colormap & 0xFF, ( e->curstate.colormap & 0xFF00 ) >> 8 );
		R_StudioRenderModel();
	}

	return 1;
}

/*
==============================================================================

SCENE

==============================================================================
*/
// the client's renderer, the builtin one for views without the world
static void R_StudioDrawModelInternal( cl_entity_t *e, int flags )
{
	if( !FBitSet( gl3_ri.rvp.flags, RF_DRAW_WORLD ))
	{
		if( e->player )
			R_StudioDrawPlayer( flags, &e->curstate );
		else R_StudioDrawModel( flags );
	}
	else if( gl3_st.builtin_renderer->value )
	{
		if( e->player )
			R_StudioDrawPlayer( flags, R_StudioGetPlayerState( e->index - 1 ));
		else R_StudioDrawModel( flags );
	}
	else
	{
		if( e->player )
			gl3_st.draw->StudioDrawPlayer( flags, R_StudioGetPlayerState( e->index - 1 ));
		else gl3_st.draw->StudioDrawModel( flags );
	}
}

static cl_entity_t *R_FindParentEntity( cl_entity_t *e, cl_entity_t **entities, uint num_entities )
{
	for( uint i = 0; i < num_entities; i++ )
	{
		if( entities[i]->index == e->curstate.aiment )
			return entities[i];
	}

	return NULL;
}

// gl_studio.c R_DrawStudioModel; the modelview must be identity (studio vertices are in world space)
void R_GL3DrawStudioModel( cl_entity_t *e )
{
	if( FBitSet( gl3_ri.rvp.flags, RF_DRAW_CUBEMAP ))
		return;

	gl3_ri.currententity = e;
	gl3_ri.currentmodel = e->model;
	R_StudioSetupTimings();

	if( !e->player && e->curstate.movetype == MOVETYPE_FOLLOW )
	{
		gl3_drawlist_t *list = gl3_tr.draw_list;
		cl_entity_t *parent = R_GL3EntityByIndex( e->curstate.aiment );

		if( !parent || !parent->model || parent->model->type != mod_studio )
			return;

		// the parent's bones (no drawing) are what the follower merges with; not in the lists: not drawn
		parent = R_FindParentEntity( e, list->solid, list->num_solid );
		if( !parent )
			parent = R_FindParentEntity( e, list->trans, list->num_trans );
		if( !parent )
			return;

		// ref_gl leaves the follower's model current here, which only the builtin renderer reads
		gl3_ri.currententity = parent;
		gl3_ri.currentmodel = parent->model;
		R_StudioDrawModelInternal( parent, 0 );
		VectorCopy( parent->curstate.origin, e->curstate.origin );
		VectorCopy( parent->origin, e->origin );
		gl3_ri.currententity = e;
		gl3_ri.currentmodel = e->model;
	}

	R_StudioDrawModelInternal( e, STUDIO_RENDER | STUDIO_EVENTS );
}

// shared conditions of the view model's events and drawing
static qboolean R_GL3ViewModelActive( void )
{
	if( !r_drawviewmodel->value || ENGINE_GET_PARM( PARM_THIRDPERSON ))
		return false;

	// camera views, cubemaps and a dead player have none
	return !FBitSet( gl3_ri.rvp.flags, RF_DRAW_CUBEMAP ) && ENGINE_GET_PARM( PARM_LOCAL_HEALTH ) > 0 && R_GL3ViewIsLocalPlayer();
}

// before the scene: muzzle flashes and sounds of the view model (temp entities join this frame's lists)
void R_GL3RunViewmodelEvents( void )
{
	cl_entity_t *view = gl3_tr.viewent;

	if( !R_GL3ViewModelActive( ))
		return;

	gl3_ri.currententity = view;
	if( !view->model || view->model->type != mod_studio )
		return;

	R_StudioSetupTimings();
	for( int i = 0; i < 4; i++ )
		VectorCopy( gp_cl->simorg, view->attachment[i] );
	gl3_ri.currentmodel = view->model;

	R_StudioDrawModelInternal( view, STUDIO_EVENTS );
}

void R_GL3DrawViewModel( void )
{
	cl_entity_t *view = gl3_tr.viewent;

	R_GatherPlayerLight( view );

	if( !R_GL3ViewModelActive( ))
		return;

	gl3_tr.blend = R_GL3FxBlend( view ) / 255.0f;
	if( view->curstate.rendermode != kRenderNormal && gl3_tr.blend <= 0.0f )
		return; // invisible

	gl3_ri.currententity = view;
	if( !view->model )
		return;

	// ponytail: studio models only (Half-Life has no alias models); ref_gl draws this with glDepthRange( 0, 0.3 )
	// so the gun does not poke into walls: task 3 does that in R_GL3StudioDrawPoints for gl3_tr.viewent
	gl3_ri.currentmodel = view->model;
	if( view->model->type == mod_studio )
	{
		R_StudioSetupTimings();
		R_StudioDrawModelInternal( view, STUDIO_RENDER );
	}
}

/*
==============================================================================

TEXTURES

==============================================================================
*/
// gl_studio.c R_StudioLoadTexture: mstudiotexture_t::index becomes the texture number
static void R_StudioLoadTexture( model_t *mod, studiohdr_t *phdr, mstudiotexture_t *ptexture )
{
	char texname[128], name[128], mdlname[128];
	texture_t *tx = NULL;
	qboolean load_external = false;
	int flags = 0;

	if( FBitSet( ptexture->flags, STUDIO_NF_NORMALMAP ))
		SetBits( flags, TF_NORMALMAP );

	// player colour textures keep their pixels for cl_remap.c
	if( !Q_strnicmp( ptexture->name, "DM_Base", 7 ) || !Q_strnicmp( ptexture->name, "remap", 5 ))
	{
		const int size = ptexture->width * ptexture->height + 768;
		const int i = mod->numtextures;

		mod->textures = (texture_t **)Mem_Realloc( mod->mempool, mod->textures, ( i + 1 ) * sizeof( texture_t * ));
		tx = (texture_t *)Mem_Calloc( mod->mempool, sizeof( *tx ) + size );
		mod->textures[i] = tx;

		// the hue ranges; bottom colour starts at anim_max + 1
		if( !Q_strnicmp( ptexture->name, "DM_Base", 7 ))
		{
			Q_strncpy( tx->name, "DM_Base", sizeof( tx->name ));
			tx->anim_min = PLATE_HUE_START;
			tx->anim_max = PLATE_HUE_END;
			tx->anim_total = SUIT_HUE_END;
		}
		else
		{
			char val[6];

			Q_strncpy( tx->name, "DM_User", sizeof( tx->name ));
			Q_strncpy( val, ptexture->name + 7, 4 );
			tx->anim_min = bound( 0, Q_atoi( val ), 255 );
			Q_strncpy( val, ptexture->name + 11, 4 );
			tx->anim_max = bound( 0, Q_atoi( val ), 255 );
			Q_strncpy( val, ptexture->name + 15, 4 );
			tx->anim_total = bound( 0, Q_atoi( val ), 255 );
		}

		tx->width = ptexture->width;
		tx->height = ptexture->height;

		// the pixels and palette follow the structure
		memcpy( tx + 1, (byte *)phdr + ptexture->index, size );

		SetBits( ptexture->flags, STUDIO_NF_COLORMAP );
		SetBits( flags, TF_FORCE_COLOR );
		mod->numtextures++;
	}

	Q_strncpy( mdlname, mod->name, sizeof( mdlname ));
	COM_FileBase( ptexture->name, name, sizeof( name ));
	COM_StripExtension( mdlname );

	if( FBitSet( ptexture->flags, STUDIO_NF_NOMIPS ))
		SetBits( flags, TF_NOMIPMAP );

	if( FBitSet( gp_host->features, ENGINE_IMPROVED_LINETRACE ) && FBitSet( ptexture->flags, STUDIO_NF_MASKED ))
		SetBits( flags, TF_KEEP_SOURCE ); // Paranoia2 texture alpha-tracing

	// replacements from materials/ (colormaps need their palette); ref_gl's report is left out
	if( host_allow_materials->value && !FBitSet( gp_host->features, ENGINE_DISABLE_HDTEXTURES ) && !FBitSet( ptexture->flags, STUDIO_NF_COLORMAP ))
	{
		Q_snprintf( texname, sizeof( texname ), "materials/%s/%s.tga", mdlname, name );
		if( gEngfuncs.fsapi->FileExists( texname, false ))
		{
			int texnum = R_GL3LoadTexture( texname, NULL, 0, flags );

			if(( load_external = texnum != 0 ))
				ptexture->index = texnum;
		}
	}

	if( !load_external )
	{
		// the image loader reads the pixels from this pointer
		gEngfuncs.Image_SetMDLPointer( (byte *)phdr + ptexture->index );
		Q_snprintf( texname, sizeof( texname ), "#%s/%s.mdl", mdlname, name );
		ptexture->index = R_GL3LoadTexture( texname, (byte *)ptexture, sizeof( mstudiotexture_t ) + ptexture->width * ptexture->height + 768, flags );
	}

	if( !ptexture->index )
		ptexture->index = R_GL3FindTexture( REF_DEFAULT_TEXTURE );
	else if( tx )
		tx->gl_texturenum = ptexture->index; // duplicate texnum for easy access
}

void R_GL3StudioLoadTextures( model_t *mod, void *data )
{
	studiohdr_t *phdr = (studiohdr_t *)data;
	mstudiotexture_t *ptexture;

	if( !phdr || phdr->textureindex <= 0 )
		return;

	ptexture = (mstudiotexture_t *)((byte *)phdr + phdr->textureindex);
	for( int i = 0; i < phdr->numtextures; i++ )
		R_StudioLoadTexture( mod, phdr, &ptexture[i] );
}

void R_GL3StudioUnloadTextures( void *data )
{
	studiohdr_t *phdr = (studiohdr_t *)data;
	mstudiotexture_t *ptexture;
	int default_texture;

	R_GL3StudioFreeMeshes( phdr );

	if( !phdr )
		return;

	ptexture = (mstudiotexture_t *)((byte *)phdr + phdr->textureindex);
	default_texture = R_GL3FindTexture( REF_DEFAULT_TEXTURE );

	for( int i = 0; i < phdr->numtextures; i++ )
	{
		if( ptexture[i].index != default_texture )
			R_GL3FreeTexture( ptexture[i].index );
	}
}

/*
==============================================================================

INTERFACE

==============================================================================
*/
qboolean R_GL3StudioFillAPI( engine_studio_api_t *api, r_studio_interface_t *pDefaultDraw )
{
	api->GetCurrentEntity        = pfnGetCurrentEntity;
	api->PlayerInfo              = pfnPlayerInfo;
	api->GetPlayerState          = R_StudioGetPlayerState;
	api->GetTimes                = pfnGetEngineTimes;
	api->GetViewInfo             = pfnGetViewInfo;
	api->GetModelCounters        = pfnGetModelCounters;
	api->StudioGetBoneTransform  = pfnStudioGetBoneTransform;
	api->StudioGetLightTransform = pfnStudioGetLightTransform;
	api->StudioGetRotationMatrix = pfnStudioGetRotationMatrix;
	api->StudioSetupModel        = R_StudioSetupModel;
	api->StudioCheckBBox         = R_StudioCheckBBox;
	api->StudioDynamicLight      = pfnStudioDynamicLight;
	api->StudioEntityLight       = R_StudioEntityLight;
	api->StudioSetupLighting     = R_StudioSetupLighting;
	api->StudioDrawPoints        = R_StudioDrawPoints;
	api->StudioDrawHulls         = R_StudioDrawHulls;
	api->StudioDrawAbsBBox       = R_StudioDrawAbsBBox;
	api->StudioDrawBones         = R_StudioDrawBones;
	api->StudioSetupSkin         = R_StudioSetupSkin;
	api->StudioSetRemapColors    = R_StudioSetRemapColors;
	api->SetupPlayerModel        = R_StudioSetupPlayerModel;
	api->StudioClientEvents      = R_StudioClientEvents;
	api->GetForceFaceFlags       = R_StudioGetForceFaceFlags;
	api->SetForceFaceFlags       = R_StudioSetForceFaceFlags;
	api->StudioSetHeader         = R_StudioSetHeader;
	api->SetRenderModel          = R_StudioSetRenderModel;
	api->SetupRenderer           = R_StudioSetupRenderer;
	api->RestoreRenderer         = R_StudioRestoreRenderer;
	api->SetChromeOrigin         = R_StudioSetChromeOrigin;
	api->GL_StudioDrawShadow     = GL_StudioDrawShadow;
	api->GL_SetRenderMode        = GL_StudioSetRenderMode;
	api->StudioSetRenderamt      = R_StudioSetRenderamt;
	api->StudioSetCullState      = R_StudioSetCullState;
	api->StudioRenderShadow      = R_StudioRenderShadow;

	pDefaultDraw->version          = STUDIO_INTERFACE_VERSION;
	pDefaultDraw->StudioDrawModel  = R_StudioDrawModel;
	pDefaultDraw->StudioDrawPlayer = R_StudioDrawPlayer;

	return true;
}

void R_GL3StudioSetDrawInterface( r_studio_interface_t *pDraw )
{
	gl3_st.draw = pDraw;
}
