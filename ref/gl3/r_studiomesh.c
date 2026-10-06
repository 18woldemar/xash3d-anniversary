/*
r_studiomesh.c - ref_gl3: drawing studio submodels on the GPU
Copyright (C) 2026 xash3d-xenon
Mesh order, render states and lighting follow ref/gl/gl_studio.c R_StudioDrawPoints, Copyright (C) 2010 Uncle Mike

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
ref_gl transforms and lights every vertex on the CPU each frame. Here a submodel is turned once into a
static vertex buffer (bone-space position and normal, texels, a bone slot) with ref_gl's triangle order,
and studio.hlsl does the rest with the bone matrices as constants: skinning, ref_gl's per-vertex lighting
(the light gamma table is a texture the vertex shader reads), chrome and local lights. Stock models use one
bone per vertex and at most 64 bones per submodel, so each submodel maps its bones to 64 slots.
*/

#include "r_local.h"
#include "xenonshaders/studio_VS.h"
#include "xenonshaders/studiospot_VS.h"
#include "xenonshaders/studiospot_PS.h"
#include "xenonshaders/studioshadow_VS.h"
#include "xenonshaders/shadow_PS.h"
#include "xenonshaders/studio_PS.h"

#define GL3_STUDIO_BONES   64
#define GL3_MAX_SHADOW_CASTERS 12 // models in the flashlight's map; the cone holds few
#define GL3_STUDIO_HASH    256
#define GL3_GAMMA_ROWS     3 // light, linear, screen

typedef struct
{
	float position[3];
	float normal[3];
	float st[2];
	float bone;
} gl3_studiovertex_t;

typedef struct
{
	int first, count; // indices
} gl3_studiomeshrange_t;

typedef struct gl3_studiosub_s
{
	const studiohdr_t      *header;
	const mstudiomodel_t   *submodel;
	IDirect3DVertexBuffer9 *vb;
	IDirect3DIndexBuffer9  *ib;
	int                     numverts;
	int                     nummeshes;
	gl3_studiomeshrange_t   *meshes;
	int                     numbones;
	byte                    bones[GL3_STUDIO_BONES]; // slot -> model bone
	struct gl3_studiosub_s  *next;
} gl3_studiosub_t;

static const D3DVERTEXELEMENT9 gl3_studio_elements[] =
{
	{ 0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
	{ 0, 12, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL, 0 },
	{ 0, 24, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
	{ 0, 32, D3DDECLTYPE_FLOAT1, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1 },
	D3DDECL_END()
};

static struct
{
	IDirect3DVertexShader9      *vs, *spot_vs, *shadow_vs;
	IDirect3DPixelShader9       *ps, *spot_ps, *shadow_ps;

	// what cast into the flashlight's map: the bones of the models drawn last frame. The map is filled
	// before the frame's models are posed, and posing them twice moves their animation on, so the
	// shadows follow the picture by one frame instead.
	struct
	{
		gl3_studiosub_t *sub;
		int             numbones;
		float           palette[GL3_STUDIO_BONES * 12];
	} casts[2][GL3_MAX_SHADOW_CASTERS];
	int  numcasts[2];
	int  castlist; // the list this frame fills; the shadow pass drains it before the swap
	IDirect3DVertexDeclaration9 *decl;
	IDirect3DTexture9           *gamma;       // the three gamma tables for the vertex shader
	qboolean                     gamma_dirty;
	poolhandle_t                 pool;
	gl3_studiosub_t              *hash[GL3_STUDIO_HASH];
	qboolean                     warned_bones;
} gl3_sm;

/*
==============================================================================

BUILDING

==============================================================================
*/
static uint R_GL3StudioHash( const mstudiomodel_t *submodel )
{
	return ((uintptr_t)submodel >> 4 ) & ( GL3_STUDIO_HASH - 1 );
}

static void R_GL3ReleaseSub( gl3_studiosub_t *sub )
{
	R_GL3DrawReset();
	gl3_device->SetStreamSource( 0, NULL, 0, 0 );
	gl3_device->SetIndices( NULL );

	if( sub->vb ) sub->vb->Release();
	if( sub->ib ) sub->ib->Release();
	if( sub->meshes ) Mem_Free( sub->meshes );
	Mem_Free( sub );
}

// the model is going away: its GPU copies too, and nothing may keep casting from them
void R_GL3StudioFreeMeshes( const studiohdr_t *header )
{
	R_GL3StudioForgetShadowCasters();

	for( int i = 0; i < GL3_STUDIO_HASH; i++ )
	{
		gl3_studiosub_t **link = &gl3_sm.hash[i];

		while( *link )
		{
			gl3_studiosub_t *sub = *link;

			if( sub->header == header || !header )
			{
				*link = sub->next;
				R_GL3ReleaseSub( sub );
			}
			else link = &sub->next;
		}
	}
}

static int R_GL3BoneSlot( gl3_studiosub_t *sub, int bone )
{
	for( int i = 0; i < sub->numbones; i++ )
	{
		if( sub->bones[i] == bone )
			return i;
	}

	if( sub->numbones == GL3_STUDIO_BONES )
		return -1;

	sub->bones[sub->numbones] = bone;
	return sub->numbones++;
}

// one vertex per triangle command entry and GL's strip and fan order, as ref_gl's R_StudioBuildIndices
static gl3_studiosub_t *R_GL3BuildSub( const studiohdr_t *header, const mstudiomodel_t *submodel )
{
	const byte *pvertbone = (const byte *)header + submodel->vertinfoindex;
	const vec3_t *pstudioverts = (const vec3_t *)((const byte *)header + submodel->vertindex);
	const vec3_t *pstudionorms = (const vec3_t *)((const byte *)header + submodel->normindex);
	const mstudiomesh_t *pmesh = (const mstudiomesh_t *)((const byte *)header + submodel->meshindex);
	gl3_studiosub_t *sub;
	gl3_studiovertex_t *verts;
	word *indices;
	int numverts = 0, numindices = 0;
	void *dst;

	sub = (gl3_studiosub_t *)Mem_Calloc( gl3_sm.pool, sizeof( *sub ));
	sub->header = header;
	sub->submodel = submodel;
	sub->nummeshes = submodel->nummesh;
	sub->meshes = (gl3_studiomeshrange_t *)Mem_Calloc( gl3_sm.pool, sizeof( *sub->meshes ) * Q_max( 1, submodel->nummesh ));

	// sizes first
	for( int j = 0; j < submodel->nummesh; j++ )
	{
		const short *cmd = (const short *)((const byte *)header + pmesh[j].triindex);
		int n;

		while(( n = *cmd++ ) != 0 )
		{
			n = abs( n );
			numverts += n;
			numindices += Q_max( 0, n - 2 ) * 3;
			cmd += n * 4;
		}
	}

	if( numverts == 0 || numverts > 65535 )
	{
		if( numverts )
			gEngfuncs.Con_Printf( S_ERROR "ref_gl3: %s submodel %s has %d vertices\n", header->name, submodel->name, numverts );
		return sub; // nothing to draw
	}

	verts = (gl3_studiovertex_t *)Mem_Malloc( r_temppool, sizeof( *verts ) * numverts );
	indices = (word *)Mem_Malloc( r_temppool, sizeof( *indices ) * numindices );
	numverts = numindices = 0;

	for( int j = 0; j < submodel->nummesh; j++ )
	{
		const short *cmd = (const short *)((const byte *)header + pmesh[j].triindex);
		int n;

		sub->meshes[j].first = numindices;

		while(( n = *cmd++ ) != 0 )
		{
			const qboolean strip = n > 0;
			const int first = numverts;

			n = abs( n );
			for( int k = 0; k < n; k++, cmd += 4 )
			{
				gl3_studiovertex_t *v = &verts[numverts];
				int slot = R_GL3BoneSlot( sub, pvertbone[cmd[0]] );

				if( slot < 0 )
				{
					if( !gl3_sm.warned_bones )
						gEngfuncs.Con_Printf( S_ERROR "ref_gl3: %s uses more than %d bones in one submodel\n", header->name, GL3_STUDIO_BONES );
					gl3_sm.warned_bones = true;
					slot = 0;
				}

				VectorCopy( pstudioverts[cmd[0]], v->position );
				VectorCopy( pstudionorms[cmd[1]], v->normal );
				v->st[0] = cmd[2];
				v->st[1] = cmd[3];
				v->bone = (float)slot;

				if( k >= 2 )
				{
					const int last = numverts;

					if( !strip )
					{
						indices[numindices++] = first;
						indices[numindices++] = last - 1;
					}
					else if( k & 1 )
					{
						indices[numindices++] = last - 1;
						indices[numindices++] = last - 2;
					}
					else
					{
						indices[numindices++] = last - 2;
						indices[numindices++] = last - 1;
					}
					indices[numindices++] = last;
				}

				numverts++;
			}
		}

		sub->meshes[j].count = numindices - sub->meshes[j].first;
	}

	if( FAILED( gl3_device->CreateVertexBuffer( numverts * sizeof( *verts ), 0, 0, D3DPOOL_MANAGED, &sub->vb, NULL ))
		|| FAILED( gl3_device->CreateIndexBuffer( numindices * sizeof( *indices ), 0, D3DFMT_INDEX16, D3DPOOL_MANAGED, &sub->ib, NULL )))
	{
		gEngfuncs.Con_Printf( S_ERROR "ref_gl3: can't create buffers for %s\n", header->name );
	}
	else
	{
		if( SUCCEEDED( sub->vb->Lock( 0, 0, &dst, 0 )))
		{
			XMemCpyStreaming_WriteCombined( dst, verts, numverts * sizeof( *verts ));
			sub->vb->Unlock();

			if( SUCCEEDED( sub->ib->Lock( 0, 0, &dst, 0 )))
			{
				XMemCpyStreaming_WriteCombined( dst, indices, numindices * sizeof( *indices ));
				sub->ib->Unlock();
				sub->numverts = numverts; // both buffers hold the submodel now; the drawing tests this
			}
		}
	}

	Mem_Free( verts );
	Mem_Free( indices );
	return sub;
}

static gl3_studiosub_t *R_GL3StudioSub( const studiohdr_t *header, const mstudiomodel_t *submodel )
{
	const uint h = R_GL3StudioHash( submodel );
	gl3_studiosub_t *sub;

	for( sub = gl3_sm.hash[h]; sub; sub = sub->next )
	{
		if( sub->submodel == submodel && sub->header == header )
			return sub;
	}

	sub = R_GL3BuildSub( header, submodel );
	sub->next = gl3_sm.hash[h];
	gl3_sm.hash[h] = sub;
	return sub;
}

/*
==============================================================================

GAMMA TABLES

==============================================================================
*/
static uint16_t R_GL3TableValue( const uint16_t *table, int i )
{
	return table ? table[i] : (uint16_t)i;
}

// r = value >> 2, g = value & 3: exact in any 8-bit format
static void R_GL3UploadGammaTables( void )
{
	const uint16_t *tables[GL3_GAMMA_ROWS] =
	{
		(const uint16_t *)ENGINE_GET_PARM( PARM_GET_LIGHTGAMMATABLE_PTR ),
		(const uint16_t *)ENGINE_GET_PARM( PARM_GET_LINEARGAMMATABLE_PTR ),
		(const uint16_t *)ENGINE_GET_PARM( PARM_GET_SCREENGAMMATABLE_PTR ),
	};
	uint32_t row[1024];
	D3DLOCKED_RECT lr;

	gl3_device->SetTexture( D3DVERTEXTEXTURESAMPLER0, NULL );
	if( FAILED( gl3_sm.gamma->LockRect( 0, &lr, NULL, D3DLOCK_NOSYSLOCK )))
		return;

	for( int y = 0; y < GL3_GAMMA_ROWS; y++ )
	{
		for( int x = 0; x < 1024; x++ )
		{
			const uint v = R_GL3TableValue( tables[y], x );
			row[x] = 0xff000000u | ( v >> 2 ) << 16 | ( v & 3 ) << 8;
		}
		XMemCpyStreaming_WriteCombined((byte *)lr.pBits + y * lr.Pitch, row, sizeof( row ));
	}

	gl3_sm.gamma->UnlockRect( 0 );
	gl3_sm.gamma_dirty = false;
}

void R_GL3StudioGammaChanged( void )
{
	gl3_sm.gamma_dirty = true;
}

// at the start of a frame: the table stays bound across frames, and a tiled frame must not lock it after use
void R_GL3StudioBeginFrame( void )
{
	if( gl3_sm.gamma_dirty )
		R_GL3UploadGammaTables();
}

/*
==============================================================================

DRAWING

==============================================================================
*/
// gl_studio.c GL_StudioSetRenderMode and the per-mesh flags
static void R_GL3StudioStates( gl3_states_t *st, int meshflags, qboolean *fog )
{
	const int entmode = gl3_ri.currententity->curstate.rendermode;
	const qboolean opaque = entmode == kRenderNormal;

	R_GL3DefaultStates( st );
	*fog = gl3_studio.rendermode != kRenderTransAdd && gl3_studio.rendermode != kRenderGlow;

	switch( gl3_studio.meshmode )
	{
	case kRenderNormal:
		break;
	case kRenderTransColor:
		st->blend = TRUE;
		st->src = D3DBLEND_SRCALPHA;
		st->dst = D3DBLEND_INVSRCALPHA;
		break;
	case kRenderTransAdd:
		st->blend = TRUE;
		st->src = D3DBLEND_ONE;
		st->dst = D3DBLEND_ONE;
		st->zwrite = FALSE;
		break;
	default:
		st->blend = TRUE;
		st->src = D3DBLEND_SRCALPHA;
		st->dst = D3DBLEND_INVSRCALPHA;
		break;
	}

	if( FBitSet( meshflags, STUDIO_NF_MASKED ))
	{
		st->alphatest = TRUE;
		st->alpharef = 127; // GL_GREATER 0.5
		st->zwrite = TRUE;
	}
	else if( FBitSet( meshflags, STUDIO_NF_ADDITIVE ))
	{
		if( opaque )
		{
			st->blend = TRUE;
			st->src = D3DBLEND_ONE;
			st->dst = D3DBLEND_ONE;
			st->zwrite = FALSE;
			*fog = false;
		}
		else
		{
			st->src = D3DBLEND_SRCALPHA;
			st->dst = D3DBLEND_ONE;
		}
	}
}

static int R_GL3StudioMeshTexture( const mstudiotexture_t *ptexture, int index )
{
	const mstudiotexture_t *remap = NULL;

	if( r_lightmap->value && !r_fullbright->value )
		return gl3_white_texture;

	// remapped player colours (gl_studio.c R_StudioSetupSkin)
	if( gl3_studio.doremap )
	{
		struct remap_info_s *info = gEngfuncs.CL_GetRemapInfoForEntity( gl3_ri.currententity );
		if( info )
			remap = info->ptexture;
	}

	return ( remap ? remap : ptexture )[index].index;
}

/*
==================
R_GL3StudioDrawShadowCasters

The models of the last frame into the flashlight's map: bones already posed, no lighting, no textures,
both sides casting. Then this frame's list starts empty.
==================
*/
void R_GL3StudioDrawShadowCasters( void )
{
	const int list = gl3_sm.castlist; // what last frame's models filled; this frame fills the other one
	gl3_states_t states;
	float rows[16];

	if( !gl3_sm.numcasts[list] )
	{
		gl3_sm.castlist = list ^ 1;
		gl3_sm.numcasts[gl3_sm.castlist] = 0;
		return;
	}

	R_GL3DrawReset();
	gl3_device->SetVertexDeclaration( gl3_sm.decl );
	gl3_device->SetVertexShader( gl3_sm.shadow_vs );
	gl3_device->SetPixelShader( gl3_sm.shadow_ps );

	R_GL3FlashlightRows( rows );
	gl3_device->SetVertexShaderConstantF( 212, rows, 4 );

	R_GL3DefaultStates( &states );
	states.cull = D3DCULL_NONE;
	gl3_wanted = states;
	R_GL3ApplyStates();

	for( int i = 0; i < gl3_sm.numcasts[list]; i++ )
	{
		gl3_studiosub_t *sub = gl3_sm.casts[list][i].sub;

		if( !sub || !sub->vb || !sub->ib || !sub->numverts )
			continue;

		gl3_device->SetStreamSource( 0, sub->vb, 0, sizeof( gl3_studiovertex_t ));
		gl3_device->SetIndices( sub->ib );
		gl3_device->SetVertexShaderConstantF( 20, gl3_sm.casts[list][i].palette, gl3_sm.casts[list][i].numbones * 3 );

		for( int j = 0; j < sub->nummeshes; j++ )
		{
			if( sub->meshes[j].count <= 0 )
				continue;

			gl3_device->DrawIndexedPrimitive( D3DPT_TRIANGLELIST, 0, 0, sub->numverts,
				sub->meshes[j].first, sub->meshes[j].count / 3 );
			R_GL3CountDraw( sub->meshes[j].count );
		}
	}

	R_GL3DrawReset();

	// this frame fills the other list
	gl3_sm.castlist = list ^ 1;
	gl3_sm.numcasts[gl3_sm.castlist] = 0;
}

void R_GL3StudioForgetShadowCasters( void )
{
	gl3_sm.numcasts[0] = gl3_sm.numcasts[1] = 0;
}

void R_GL3StudioDrawPoints( void )
{
	const studiohdr_t *header = gl3_studio.header;
	const mstudiomodel_t *submodel = gl3_studio.submodel;
	const cl_entity_t *e = gl3_ri.currententity;
	const mstudiotexture_t *ptexture;
	const mstudiomesh_t *pmesh;
	const short *pskinref;
	const float blend = gl3_tr.blend;
	float palette[GL3_STUDIO_BONES * 12];
	float constants[4 * 8]; // c11..c18: the local lights' origins, then their colours
	float rows[16];
	gl3_studiosub_t *sub;
	qboolean viewmodel;

	if( !header || !submodel || !e )
		return;

	// ponytail: the glow shell (multiplayer only) needs smoothed normals and the chrome sprite; not drawn yet
	if( FBitSet( gl3_studio.forcefaceflags, STUDIO_NF_CHROME ))
		return;

	sub = R_GL3StudioSub( header, submodel );
	if( !sub->numverts ) // a buffer of this submodel could not be made or filled
		return;

	ptexture = (const mstudiotexture_t *)((const byte *)header + header->textureindex);
	pmesh = (const mstudiomesh_t *)((const byte *)header + submodel->meshindex);
	pskinref = (const short *)((const byte *)header + header->skinindex);
	if( e->curstate.skin > 0 && e->curstate.skin < header->numskinfamilies )
		pskinref += e->curstate.skin * header->numskinref;

	for( int i = 0; i < sub->numbones; i++ )
		memcpy( palette + i * 12, gl3_studio.bonestransform[sub->bones[i]], sizeof( float ) * 12 );

	const qboolean flashlight = R_GL3FlashlightActive( );

	// Remember this pose for the next frame's shadow map, before anything else is set. Only when a map
	// is going to be built: the list is drained by that pass alone, and a full list that nothing drains
	// would hold the poses of whatever frame happened to fill it.
	if( R_GL3FlashlightShadowsWanted( ) && e != gl3_tr.viewent && gl3_sm.numcasts[gl3_sm.castlist] < GL3_MAX_SHADOW_CASTERS )
	{
		const int n = gl3_sm.numcasts[gl3_sm.castlist]++;

		gl3_sm.casts[gl3_sm.castlist][n].sub = sub;
		gl3_sm.casts[gl3_sm.castlist][n].numbones = sub->numbones;
		memcpy( gl3_sm.casts[gl3_sm.castlist][n].palette, palette, sizeof( float ) * sub->numbones * 12 );
	}

	R_GL3DrawReset();
	gl3_device->SetVertexDeclaration( gl3_sm.decl );
	gl3_device->SetVertexShader( flashlight ? gl3_sm.spot_vs : gl3_sm.vs );
	gl3_device->SetPixelShader( flashlight ? gl3_sm.spot_ps : gl3_sm.ps );
	gl3_device->SetStreamSource( 0, sub->vb, 0, sizeof( gl3_studiovertex_t ));
	gl3_device->SetIndices( sub->ib );
	gl3_device->SetTexture( D3DVERTEXTEXTURESAMPLER0, gl3_sm.gamma );
	gl3_device->SetSamplerState( D3DVERTEXTEXTURESAMPLER0, D3DSAMP_MINFILTER, D3DTEXF_POINT );
	gl3_device->SetSamplerState( D3DVERTEXTEXTURESAMPLER0, D3DSAMP_MAGFILTER, D3DTEXF_POINT );
	gl3_device->SetSamplerState( D3DVERTEXTEXTURESAMPLER0, D3DSAMP_MIPFILTER, D3DTEXF_NONE );
	gl3_device->SetSamplerState( D3DVERTEXTEXTURESAMPLER0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP );
	gl3_device->SetSamplerState( D3DVERTEXTEXTURESAMPLER0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP );

	R_GL3ClipMatrix( gl3_ri.worldviewProjectionMatrix, rows );
	gl3_device->SetVertexShaderConstantF( 0, rows, 4 );
	gl3_device->SetVertexShaderConstantF( 4, gl3_ri.worldviewMatrix[2], 1 );
	gl3_device->SetVertexShaderConstantF( 20, palette, sub->numbones * 3 );

	if( flashlight )
	{
		float spotrows[16];

		R_GL3FlashlightRows( spotrows );
		gl3_device->SetVertexShaderConstantF( 212, spotrows, 4 );
		gl3_device->SetPixelShaderConstantF( 2, R_GL3FlashlightParams( ), 1 );
		gl3_device->SetPixelShaderConstantF( 3, R_GL3FlashlightLamp( ), 1 );
		R_GL3BindCookie( R_GL3FlashlightCookie( ));
		R_GL3BindShadowMap( R_GL3FlashlightShadowed( ) ? R_GL3FlashlightShadowMap( ) : gl3_white_texture );
	}

	// local lights, in world space (ref_gl works in bone space; lengths and angles are the same)
	memset( constants, 0, sizeof( constants ));
	for( int i = 0; i < gl3_studio.numlocallights; i++ )
	{
		const dlight_t *el = gl3_studio.locallight[i];

		VectorCopy( el->origin, constants + i * 4 );
		constants[i * 4 + 3] = gl3_studio.locallightR2[i];
		constants[16 + i * 4] = (float)gl3_studio.locallightcolor[i][0];
		constants[16 + i * 4 + 1] = (float)gl3_studio.locallightcolor[i][1];
		constants[16 + i * 4 + 2] = (float)gl3_studio.locallightcolor[i][2];
	}
	gl3_device->SetVertexShaderConstantF( 11, constants, 8 );

	// the view model sits in the front 30% of the depth range, so it does not poke into walls
	viewmodel = e == gl3_tr.viewent;
	if( viewmodel )
		R_GL3SetDepthRange( 0.0f, 0.3f );

	for( int j = 0; j < submodel->nummesh; j++ )
	{
		const mstudiotexture_t *tex = &ptexture[pskinref[pmesh[j].skinref]];
		const int flags = tex->flags | gl3_studio.forcefaceflags;
		const qboolean masked_opaque = FBitSet( flags, STUDIO_NF_MASKED ) && e->curstate.rendermode == kRenderNormal;
		const float meshblend = masked_opaque ? 1.0f : blend;
		const qboolean transadd = gl3_studio.rendermode == kRenderTransAdd;
		float vs[24], fog[4];
		qboolean fogged;
		gl3_states_t st;

		if( sub->meshes[j].count <= 0 )
			continue;

		R_GL3StudioStates( &st, flags, &fogged );
		gl3_wanted = st;
		R_GL3ApplyStates();

		// c5..c10 of studio.hlsl
		vs[0] = gl3_studio.ambientlight;
		vs[1] = gl3_studio.shadelight;
		vs[2] = FBitSet( flags, STUDIO_NF_FLATSHADE ) ? 1.0f : 0.0f;
		vs[3] = FBitSet( flags, STUDIO_NF_FULLBRIGHT ) ? 1.0f : 0.0f;
		VectorCopy( gl3_studio.lightcolor, vs + 4 );
		vs[7] = (float)(int)( meshblend * 255.0f ) / 255.0f;
		VectorCopy( gl3_studio.lightvec, vs + 8 );
		vs[11] = FBitSet( flags, STUDIO_NF_CHROME ) ? 1.0f : 0.0f;
		VectorCopy( gl3_studio.chrome_origin, vs + 12 );
		vs[15] = transadd ? 1.0f : 0.0f;
		VectorCopy( gl3_ri.vright, vs + 16 );
		vs[19] = meshblend;
		vs[20] = 1.0f / (float)tex->width;
		vs[21] = 1.0f / (float)tex->height;
		vs[22] = 0.0f;
		vs[23] = gl3_studio.numlocallights > 0 ? 1.0f : 0.0f;
		gl3_device->SetVertexShaderConstantF( 5, vs, 6 );

		// studio models fog with the plain colour (ref_gl resets it after the world)
		if( fogged && gl3_ri.fogEnabled )
		{
			VectorCopy( gl3_ri.fogColor, fog );
			fog[3] = gl3_ri.fogEndInv;
		}
		else Vector4Set( fog, 0.0f, 0.0f, 0.0f, 0.0f );
		gl3_device->SetPixelShaderConstantF( 1, fog, 1 );

		R_GL3BindTexture( R_GL3StudioMeshTexture( ptexture, pskinref[pmesh[j].skinref] ));
		gl3_device->DrawIndexedPrimitive( D3DPT_TRIANGLELIST, 0, 0, sub->numverts, sub->meshes[j].first, sub->meshes[j].count / 3 );
		R_GL3CountDraw( sub->meshes[j].count );
	}

	if( viewmodel )
		R_GL3SetDepthRange( 0.0f, 1.0f );

	// ref_gl's RestoreRenderer and DrawShadow leave blending off and depth writes on
	R_GL3DefaultStates( &gl3_wanted );
	R_GL3DrawReset();
	gl3_device->SetStreamSource( 0, NULL, 0, 0 );
	gl3_device->SetIndices( NULL );
}

/*
==============================================================================

INIT

==============================================================================
*/
qboolean R_GL3InitStudioMeshes( void )
{
	if( FAILED( gl3_device->CreateVertexShader( g_studioshadow_VS, &gl3_sm.shadow_vs ))
		|| FAILED( gl3_device->CreatePixelShader( g_shadow_PS, &gl3_sm.shadow_ps ))
		|| FAILED( gl3_device->CreateVertexShader( g_studiospot_VS, &gl3_sm.spot_vs ))
		|| FAILED( gl3_device->CreatePixelShader( g_studiospot_PS, &gl3_sm.spot_ps ))
		|| FAILED( gl3_device->CreateVertexShader( g_studio_VS, &gl3_sm.vs ))
		|| FAILED( gl3_device->CreatePixelShader( g_studio_PS, &gl3_sm.ps ))
		|| FAILED( gl3_device->CreateVertexDeclaration( gl3_studio_elements, &gl3_sm.decl ))
		|| FAILED( gl3_device->CreateTexture( 1024, GL3_GAMMA_ROWS, 1, 0, D3DFMT_LIN_A8R8G8B8, D3DPOOL_MANAGED, &gl3_sm.gamma, NULL )))
	{
		gEngfuncs.Con_Printf( S_ERROR "ref_gl3: can't create the studio shaders\n" );
		return false;
	}

	gl3_sm.pool = Mem_AllocPool( "ref_gl3 studio meshes" );
	gl3_sm.gamma_dirty = true;
	return true;
}

void R_GL3ShutdownStudioMeshes( void )
{
	R_GL3StudioForgetShadowCasters();

	if( !gl3_device )
		return;

	R_GL3StudioFreeMeshes( NULL );
	R_GL3DrawReset();
	gl3_device->SetVertexShader( NULL );
	gl3_device->SetPixelShader( NULL );
	gl3_device->SetVertexDeclaration( NULL );
	gl3_device->SetTexture( D3DVERTEXTEXTURESAMPLER0, NULL );

	if( gl3_sm.gamma ) gl3_sm.gamma->Release();
	if( gl3_sm.decl ) gl3_sm.decl->Release();
	if( gl3_sm.shadow_ps ) gl3_sm.shadow_ps->Release();
	if( gl3_sm.shadow_vs ) gl3_sm.shadow_vs->Release();
	if( gl3_sm.spot_ps ) gl3_sm.spot_ps->Release();
	if( gl3_sm.spot_vs ) gl3_sm.spot_vs->Release();
	if( gl3_sm.ps ) gl3_sm.ps->Release();
	if( gl3_sm.vs ) gl3_sm.vs->Release();
	if( gl3_sm.pool ) Mem_FreePool( &gl3_sm.pool );
	memset( &gl3_sm, 0, sizeof( gl3_sm ));
}
