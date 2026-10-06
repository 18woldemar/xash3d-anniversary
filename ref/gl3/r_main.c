/*
r_main.c - ref_gl3: the renderer interface, init, frames
Copyright (C) 2026 xash3d-xenon
Based on ref/null/r_context.c, Copyright (C) 2023-2024 a1batross

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

#include "r_local.h"

// what ref/common/ref_context.c defines for other renderers (that file clashes with the engine in one xex)
DEFINE_ENGINE_SHARED_CVAR_LIST()

ref_api_t      gEngfuncs;
ref_globals_t *gpGlobals;
ref_client_t  *gp_cl;
ref_host_t    *gp_host;
struct movevars_s *gp_movevars;
uint16_t       rtable[MOD_FRAMES][MOD_FRAMES];
dlight_t      *gp_dlights;
int            g_lightstylevalue[MAX_LIGHTSTYLES];
poolhandle_t   r_temppool;

static cvar_t *r_xenon_tvgamma;
static cvar_t *r_xenon_showtex;
static float   gl3_tvgamma_applied = -1.0f;
static cvar_t *r_xenon_msaa;
static float   gl3_msaa_applied = 0.0f;
static cvar_t *r_xenon_style;
static float   gl3_style_applied = -1.0f;

int R_GL3Style( void )
{
	const int style = (int)r_xenon_style->value;

	return ( style < GL3_STYLE_ORIGINAL || style > GL3_STYLE_ENHANCED ) ? GL3_STYLE_ENHANCED : style;
}

// the cvars a style owns; what a style does not name keeps the value the player set
static void R_GL3ApplyStyle( int style )
{
	const qboolean software = style == GL3_STYLE_SOFTWARE;

	// both accelerated styles take the 4 samples the console can afford; the span renderer never had any
	gEngfuncs.Cvar_SetValue( "r_xenon_msaa", software ? 0.0f : 4.0f );

	// gl_overbright lives in the accelerated renderer only: the span renderer lights through its own ramp
	gEngfuncs.Cvar_SetValue( "gl_overbright", software ? 0.0f : 1.0f );

	// textures are point sampled in the span renderer, filtered in the other two
	gEngfuncs.Cvar_SetValue( "gl_texture_nearest", software ? 1.0f : 0.0f );

	// the ripple simulation is the one thing the span renderer did better, so our style takes it as well;
	// the original accelerated one slid the texture with a sine instead and keeps that
	gEngfuncs.Cvar_SetValue( "r_xenon_swwater", style == GL3_STYLE_ORIGINAL ? 0.0f : 1.0f );
}

// random tiling of '-' textures, as ref_gl fills it at start
void GL_InitRandomTable( void )
{
	for( int tu = 0; tu < MOD_FRAMES; tu++ )
	{
		for( int tv = 0; tv < MOD_FRAMES; tv++ )
			rtable[tu][tv] = gEngfuncs.COM_RandomLong( 0, 0x7FFF );
	}

	gEngfuncs.COM_SetRandomSeed( 0 );
}

static qboolean R_Init( void )
{
	gEngfuncs.R_Init_Video( REF_D3D ); // vid_xenon.c sets the one mode
	r_temppool = Mem_AllocPool( "ref_gl3 zone" );
	GL_InitRandomTable();
	R_GL3InitScene();
	R_GL3StudioInit();
	if( !R_GL3CreateDevice( gpGlobals->width, gpGlobals->height ))
		return false;

	R_GL3InitImages();
	if( !R_GL3InitDraw( ) || !R_GL3InitWorld( ))
		return false;
	R_GL3InitLightmaps();
	if( !R_GL3InitStudioMeshes( ))
		return false;
	r_xenon_showtex = gEngfuncs.Cvar_Get( "r_xenon_showtex", "0", 0, "draw this texture number over the frame (r_xenon_texstats list shows the numbers)" );
	r_xenon_tvgamma = gEngfuncs.Cvar_Get( "r_xenon_tvgamma", "1", FCVAR_ARCHIVE, "undo the console's sRGB to TV gamma conversion, so the picture matches a PC monitor" );
	// 4x by default: clean on a TV and 60 fps on the console (2026-09-18 notes); the shot tools pass their own value
	r_xenon_msaa = gEngfuncs.Cvar_Get( "r_xenon_msaa", "4", 0, "multisampling of the 3D view through EDRAM tiling: 0, 2 or 4 samples" );
	R_GL3InitFlashlight();
	r_xenon_style = gEngfuncs.Cvar_Get( "r_xenon_style", "2", FCVAR_ARCHIVE,
		"render style: 0 the original accelerated renderer, 1 the software one, 2 ours" );
	return true;
}

static void R_Shutdown( void )
{
	R_GL3FreeFlashlight();
	R_GL3ShutdownWorld();
	R_GL3ShutdownLightmaps();
	R_GL3ShutdownStudioMeshes();
	R_GL3ShutdownScreenShots();
	R_GL3SetMSAA( 0 );
	R_GL3ShutdownDraw();
	R_GL3ShutdownImages();
	R_GL3DestroyDevice();
	Mem_FreePool( &r_temppool );
	gEngfuncs.R_Free_Video();
}

static void R_BeginFrame( qboolean clearScene )
{
	// a frame cut short by an error: its 3D part closed, the picture dropped
	R_GL3EndTiledScene();
	R_GL3TakeTiledScene();

	if( r_xenon_style->value != gl3_style_applied )
	{
		gl3_style_applied = r_xenon_style->value;
		R_GL3ApplyStyle( R_GL3Style( ));
		gl3_msaa_applied = -1.0f; // the scene target is the style's as well: 16 bit for the software one
	}

	if( r_xenon_msaa->value != gl3_msaa_applied )
	{
		gl3_msaa_applied = r_xenon_msaa->value;
		if( !R_GL3SetMSAA( (int)gl3_msaa_applied ))
		{
			gl3_msaa_applied = 0.0f;
			gEngfuncs.Cvar_Set( "r_xenon_msaa", "0" );
		}
	}

	if( r_xenon_tvgamma->value != gl3_tvgamma_applied )
	{
		gl3_tvgamma_applied = r_xenon_tvgamma->value;
		R_GL3SetTVGamma( gl3_tvgamma_applied != 0.0f );
	}

	Xenon_Phase( "render" );
	R_GL3CheckOverbright();
	R_GL3StudioBeginFrame();
	R_GL3FlashlightNewFrame();
	R_GL3BeginFrameDraw();
	R_GL3RestoreViewport(); // an aborted frame leaves its last view's
	R_GL3Clear( 0, 0, 0 );
	R_GL3Set2DMode( true );
}

static void R_EndFrame( void )
{
	double start;

	R_GL3EndTiledScene(); // never Present inside the bracket

	if( r_xenon_showtex->value > 0.0f )
	{
		int texnum = (int)r_xenon_showtex->value;
		gl3_texture_t *tex = R_GL3GetTexture( texnum );

		R_GL3SetRenderMode( kRenderNormal );
		R_GL3TriColor4ub( 255, 255, 255, 255 );
		R_GL3DrawStretchPic( 0, 0, Q_min( tex->width, gpGlobals->width ), Q_min( tex->height, gpGlobals->height ), 0, 0,
			(float)Q_min( tex->width, gpGlobals->width ) / Q_max( 1, tex->width ), (float)Q_min( tex->height, gpGlobals->height ) / Q_max( 1, tex->height ), texnum );
	}

	R_GL3Set2DMode( false );
	R_GL3EndFrameDraw();

	start = gEngfuncs.pfnTime();
	Xenon_Phase( "Present" );
	R_GL3Present();
	Xenon_Phase( "client" );
	R_GL3FrameStats( gEngfuncs.pfnTime() - start );
}

static intptr_t RefGetParm( int parm, int arg )
{
	switch( parm )
	{
	case PARM_TEX_WIDTH:
		return R_GL3GetTexture( arg )->width;
	case PARM_TEX_HEIGHT:
		return R_GL3GetTexture( arg )->height;
	case PARM_TEX_SRC_WIDTH:
		return R_GL3GetTexture( arg )->srcWidth;
	case PARM_TEX_SRC_HEIGHT:
		return R_GL3GetTexture( arg )->srcHeight;
	case PARM_TEX_MIPCOUNT:
		return R_GL3GetTexture( arg )->numMips;
	case PARM_TEX_DEPTH:
		return 1;
	case PARM_TEX_FLAGS:
		return R_GL3GetTexture( arg )->flags;
	case PARM_TEX_FILTERING:
		return R_GL3TextureFilteringEnabled( arg );
	case PARM_GL_CONTEXT_TYPE:
		return CONTEXT_TYPE_SOFTWARE;
	case PARM_GET_STUDIO_HDR:
		return (intptr_t)R_GL3StudioGetHeader();
	default:
		return ENGINE_GET_PARM_( parm, arg );
	}
}

static void R_GetDetailScaleForTexture( int texture, float *xScale, float *yScale )
{
	gl3_texture_t *tex = R_GL3GetTexture( texture );

	if( xScale ) *xScale = tex->xscale;
	if( yScale ) *yScale = tex->yscale;
}

static void R_SetDetailScaleForTexture( int texture, float xScale, float yScale )
{
	gl3_texture_t *tex = R_GL3GetTexture( texture );

	tex->xscale = xScale;
	tex->yscale = yScale;
}

static void R_OverrideTextureSourceSize( unsigned int texnum, unsigned int srcWidth, unsigned int srcHeight )
{
	gl3_texture_t *tex = R_GL3GetTexture( texnum );

	tex->srcWidth = srcWidth;
	tex->srcHeight = srcHeight;
}

static void GL_Bind( int tmu, unsigned int texnum )
{
	R_GL3Bind( texnum );
}

static void R_TriFog( float flFogColor[3], float flStart, float flEnd, int bOn )
{
}

static void R_TriFogParams( float flDensity, int iFogSkybox )
{
}

static void R_FillTriAPI( triangleapi_t *api )
{
	api->TexCoord2f = R_GL3TriTexCoord2f;
	api->Fog = R_TriFog;
	api->ScreenToWorld = R_GL3ScreenToWorld;
	api->GetMatrix = R_GL3GetMatrix;
	api->FogParams = R_TriFogParams;
}

static void R_SimpleStub( void )
{
	;
}

static void R_SimpleStubInt( int unused )
{
	;
}

static void R_SimpleStubUInt( unsigned int unused )
{
	;
}

static void R_SimpleStubBool( qboolean unused )
{
	;
}

static void R_FillRenderAPI( render_api_t *api )
{
	;
}

static const char *R_GetConfigName( void )
{
	return NULL;
}

static qboolean R_SetDisplayTransform( ref_screen_rotation_t rotate, int x, int y, float scale_x, float scale_y )
{
	return true;
}

static void GL_SetupAttributes( int safegl )
{
	;
}

static qboolean VID_CubemapShot( const char *base, uint size, const float *vieworg, qboolean skyshot )
{
	return false;
}

static void R_SetSkyCloudsTextures( int solidskyTexture, int alphaskyTexture )
{
	;
}

// the engine's gamma, brightness or light gamma changed (true: fixed tables for env shots)
static void R_GammaChanged( qboolean do_reset_gamma )
{
	if( !do_reset_gamma )
	{
		R_GL3RebuildLightmaps();
		R_GL3StudioGammaChanged();
	}
}

#if XASH_XENON
const char *Xenon_Phase( const char *phase ); // engine/platform/xenon/sys_xenon.c: names the step of a frame
#endif

static void R_NewMap( void )
{
#if XASH_XENON
	Xenon_Phase( "new map: decals" );
#endif
	R_GL3ClearDecals();
	R_GL3StudioForgetShadowCasters();
	R_GL3StudioResetPlayerModels();
	R_GL3NewMapScene();
	R_GL3ResetRipples();
#if XASH_XENON
	Xenon_Phase( "new map: world" );
#endif
	R_GL3WorldNewMap();
#if XASH_XENON
	Xenon_Phase( "new map: done" );
#endif
}

static qboolean R_GL3LooksLikeWater( const char *name )
{
	if(( name[0] == '*' && Q_stricmp( name, REF_DEFAULT_TEXTURE )) || name[0] == '!' )
		return true;

	if( !FBitSet( gp_host->features, ENGINE_QUAKE_COMPATIBLE ))
	{
		if( !Q_strncmp( name, "water", 5 ) || !Q_strnicmp( name, "laser", 5 ))
			return true;
	}

	return false;
}

// gl_context.c Mod_BrushUnloadTextures: a map's textures go with it
static void R_GL3BrushUnloadTextures( model_t *mod )
{
	int default_texture = R_GL3FindTexture( REF_DEFAULT_TEXTURE );

	for( int i = 0; i < mod->numtextures; i++ )
	{
		texture_t *tx = mod->textures[i];

		if( !tx )
			continue;

		if( tx->gl_texturenum != default_texture && !R_GL3KeepTextureForNextMap( tx->gl_texturenum ))
			R_GL3FreeTexture( tx->gl_texturenum );

		if( !R_GL3LooksLikeWater( tx->name ))
		{
			if( !R_GL3KeepTextureForNextMap( tx->fb_texturenum ))
				R_GL3FreeTexture( tx->fb_texturenum );
			R_GL3FreeTexture( tx->dt_texturenum );
		}
	}
}

static qboolean Mod_ProcessRenderData( model_t *mod, qboolean create, const byte *buffer, size_t buffersize )
{
	if( create )
		return true;

	if( mod->type == mod_brush )
		R_GL3BrushUnloadTextures( mod );
	else if( mod->type == mod_studio )
		R_GL3StudioUnloadTextures( mod->cache.data );
	return true;
}

static void GL_OrthoBounds( const float *mins, const float *maxs )
{
	;
}

static qboolean R_SpeedsMessage( char *out, size_t size )
{
	return false;
}

static void VGUI_SetupDrawing( qboolean rect )
{
	;
}

const ref_interface_t gReffuncs =
{
	R_Init, // R_Init
	R_Shutdown, // R_Shutdown
	R_GetConfigName, // R_GetConfigName
	R_SetDisplayTransform, // R_SetDisplayTransform
	GL_SetupAttributes, // GL_SetupAttributes
	(void (*)( void ))R_SimpleStub, // GL_InitExtensions
	(void (*)( void ))R_SimpleStub, // GL_ClearExtensions
	R_GammaChanged, // R_GammaChanged
	R_BeginFrame, // R_BeginFrame
	(void (*)( void ))R_SimpleStub, // R_RenderScene
	R_EndFrame, // R_EndFrame
	R_GL3PushScene, // R_PushScene
	R_GL3PopScene, // R_PopScene
	R_GL3BeginTiledScene, // GL_BackendStartFrame
	R_GL3EndTiledScene, // GL_BackendEndFrame
	(void (*)( void ))R_SimpleStub, // R_ClearScreen
	(void (*)( qboolean allow ))R_SimpleStubBool, // R_AllowFog
	R_GL3SetRenderMode, // GL_SetRenderMode
	R_GL3AddEntity, // R_AddEntity
	R_GL3ProcessEntData, // R_ProcessEntData
	(void (*)( void ))R_SimpleStub, // R_ShowTextures
	R_GL3TextureData, // R_GetTextureOriginalBuffer
	R_GL3LoadTextureFromBuffer, // GL_LoadTextureFromBuffer
	R_GL3ProcessTexture, // GL_ProcessTexture
	R_GL3SetupSky, // R_SetupSky
	R_GL3Set2DMode, // R_Set2DMode
	R_GL3Set2DOffset, // R_Set2DOffset
	R_GL3DrawStretchPic, // R_DrawStretchPic
	R_GL3FillRGBA, // FillRGBA
	R_GL3WorldToScreen, // WorldToScreen
	R_GL3ScreenShot, // VID_ScreenShot
	VID_CubemapShot, // VID_CubemapShot
	R_LightPoint, // R_LightPoint
	R_GL3DecalShoot, // R_DecalShoot
	R_GL3DecalRemoveAll, // R_DecalRemoveAll
	R_GL3CreateDecalList, // R_CreateDecalList
	R_GL3ClearAllDecals, // R_ClearAllDecals
	R_GL3StudioEstimateFrame, // R_StudioEstimateFrame
	R_GL3StudioLerpMovement, // R_StudioLerpMovement
	R_GL3StudioFillAPI, // R_StudioFillAPI
	R_GL3StudioSetDrawInterface, // R_StudioSetDrawInterface
	R_SetSkyCloudsTextures, // R_SetSkyCloudsTextures
	R_GL3SubdivideSurface, // GL_SubdivideSurface
	CL_RunLightStyles, // CL_RunLightStyles
	Mod_ProcessRenderData, // Mod_ProcessRenderData
	R_GL3StudioLoadTextures, // Mod_StudioLoadTextures
	R_GL3DrawParticles, // CL_DrawParticles
	R_GL3DrawTracers, // CL_DrawTracers
	R_GL3DrawBeams, // CL_DrawBeams
	RefGetParm, // RefGetParm
	R_GetDetailScaleForTexture, // R_GetDetailScaleForTexture
	R_SetDetailScaleForTexture, // R_SetDetailScaleForTexture
	R_GL3CreateTexture, // GL_CreateTexture
	R_GL3FindTexture, // GL_FindTexture
	R_GL3TextureName, // GL_TextureName
	R_GL3TextureData, // GL_TextureData
	R_GL3LoadTexture, // GL_LoadTexture
	R_GL3FreeTexture, // GL_FreeTexture
	R_OverrideTextureSourceSize, // R_OverrideTextureSourceSize
	R_GL3UpdateTexture, // GL_UpdateTexture
	GL_Bind, // GL_Bind
	R_GL3RenderFrame, // GL_RenderFrame
	GL_OrthoBounds, // GL_OrthoBounds
	R_SpeedsMessage, // R_SpeedsMessage
	R_GL3GetCurrentVis, // Mod_GetCurrentVis
	R_NewMap, // R_NewMap
	R_GL3ClearScene, // R_ClearScene
	R_GL3TriRenderMode, // TriRenderMode
	R_GL3TriBegin, // Begin
	R_GL3TriEnd, // End
	R_GL3TriColor4f, // Color4f
	R_GL3TriColor4ub, // Color4ub
	R_GL3TriVertex3fv, // Vertex3fv
	R_GL3TriVertex3f, // Vertex3f
	R_GL3TriCullFace, // CullFace
	R_FillRenderAPI, // R_FillRenderAPI
	R_FillTriAPI, // R_FillTriAPI
	VGUI_SetupDrawing, // VGUI_SetupDrawing
};


int GetRefAPI( int version, ref_interface_t *funcs, ref_api_t *engfuncs, ref_globals_t *globals )
{
	if( version != REF_API_VERSION )
		return 0;

	*funcs = gReffuncs;
	gEngfuncs = *engfuncs;
	gpGlobals = globals;

	gp_cl = (ref_client_t *)ENGINE_GET_PARM( PARM_GET_CLIENT_PTR );
	gp_host = (ref_host_t *)ENGINE_GET_PARM( PARM_GET_HOST_PTR );
	gp_movevars = (struct movevars_s *)ENGINE_GET_PARM( PARM_GET_MOVEVARS_PTR );
	gp_dlights = (dlight_t *)ENGINE_GET_PARM( PARM_GET_DLIGHTS_PTR );

	RETRIEVE_ENGINE_SHARED_CVAR_LIST();

	gEngfuncs.Cvar_RegisterVariable( &r_dlight_virtual_radius );
	gEngfuncs.Cvar_RegisterVariable( &r_lighting_extended );

	return REF_API_VERSION;
}
