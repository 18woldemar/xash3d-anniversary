/*
r_local.h - ref_gl3, the OpenGL 3.3 core renderer
Copyright (C) 2026 18woldemar

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

#ifndef R_LOCAL_H
#define R_LOCAL_H

// Everything not shared between the renderer's files is static, and shared names carry an R_GL3/gl3_
// prefix. The shaders are GLSL 330 and the drawing goes through vertex buffers and uniform blocks only:
// the core profile has no fixed-function pipeline to fall back on.

#include "ref_common.h"
#include "r_gl.h"
#include "com_strings.h"
#include "crtlib.h"
#include "crclib.h"
#include "protocol.h"
#include "ref_params.h"
#include "pmove.h"
#include "mod_local.h"
#include "enginefeatures.h"

#define GL3_MAX_TEXTURES 4096
#define GL3_LIGHTMAP_SIZE 1024 // lightmap page, in lightmap texture coordinates
#define GL3_LIGHTMAP_DYNAMIC 64 // "page" of the faces lit by dynamic lights this frame
#define GL3_MAX_DRAW_STACK 2 // the menu's player model preview pushes a second scene

#define WORLDMODEL ( gp_cl->models[1] )

static inline model_t *R_GL3ModelHandle( int index )
{
	if( index < 0 || index >= gp_cl->nummodels )
		return NULL;
	return gp_cl->models[index];
}

// ref_gl culls GL_FRONT with counter-clockwise front faces
#define GL3_CULL_NONE  0
#define GL3_CULL_FRONT GL_FRONT
#define GL3_CULL_BACK  GL_BACK

typedef struct gl3_texture_s
{
	char        name[256]; // game path with extension, or an image program
	word        srcWidth, srcHeight;
	word        width, height;
	byte        numMips;
	uint        flags;     // texFlags_t (C++ has no |= on enums)
	GLuint      glnum;     // 0: no texture object (yet)
	byte        applied[4]; // filtering, addressing, mip cap and anisotropy last set on it, +1 (0: never)
	rgbdata_t  *original;  // TF_KEEP_SOURCE
	size_t      size;      // bytes in all levels
	float       xscale, yscale; // detail textures
	byte        fogParams[4];   // water: underwater fog colour and density
	uint        hashValue;
	qboolean    kept;      // a WAD texture held across a level change, counted against the budget
	struct gl3_texture_s *nextHash;
} gl3_texture_t;

// R_GL3CullSurface results (gl_local.h CULL_*)
#define GL3_CULL_VISIBLE  0
#define GL3_CULL_BACKSIDE 1 // back of a face of a moving brush
#define GL3_CULL_FRUSTUM  2
#define GL3_CULL_VISFRAME 3 // outside the PVS
#define GL3_CULL_OTHER    4

// render states every drawing path goes through (r_draw.c keeps what the device has)
typedef struct
{
	qboolean blend;
	GLenum   src, dst;
	qboolean alphatest; // in the shaders: a fragment under alpharef is discarded
	qboolean zwrite;
	qboolean ztest;
	GLenum   zfunc;
	GLenum   cull;      // GL3_CULL_NONE, GL_FRONT or GL_BACK
	int      alpharef;  // 0..255
	float    depthbias; // glPolygonOffset units / 2^24
	float    slopebias; // glPolygonOffset factor
} gl3_states_t;

enum
{
	GL3_FRUSTUM_LEFT = 0,
	GL3_FRUSTUM_RIGHT,
	GL3_FRUSTUM_BOTTOM,
	GL3_FRUSTUM_TOP,
	GL3_FRUSTUM_FAR,
	GL3_FRUSTUM_PLANES
};

typedef struct
{
	mplane_t planes[GL3_FRUSTUM_PLANES];
	uint     clipFlags;
} gl3_frustum_t;

// one view (ref_gl's RI)
typedef struct
{
	ref_viewpass_t rvp;
	cl_entity_t   *currententity;
	model_t       *currentmodel;
	gl3_frustum_t   frustum;
	mleaf_t       *viewleaf, *oldviewleaf;
	vec3_t         vforward, vright, vup;
	float          farClip;
	float          viewplanedist;
	matrix4x4      objectMatrix;              // current entity
	matrix4x4      worldviewMatrix;
	matrix4x4      modelviewMatrix;           // worldview * object
	matrix4x4      projectionMatrix;          // OpenGL clip space
	matrix4x4      worldviewProjectionMatrix;
	matrix4x4      objectClipMatrix;          // projection * modelview
	byte           visbytes[(MAX_MAP_LEAFS + 7) / 8]; // PVS of this view

	// underwater fog (ref_gl's R_CheckFog)
	qboolean       fogEnabled;
	qboolean       fogSkybox;
	vec3_t         fogColor;
	float          fogEndInv;    // 1 / GL_FOG_END, the shaders' fog alpha
	int            cached_contents;
	int            cached_waterlevel;
} gl3_view_t;

typedef struct
{
	cl_entity_t *solid[MAX_VISIBLE_PACKET];
	cl_entity_t *trans[MAX_VISIBLE_PACKET];
	cl_entity_t *beams[MAX_VISIBLE_PACKET];
	uint         num_solid, num_trans, num_beams;
} gl3_drawlist_t;

// renderer-wide state (ref_gl's tr)
typedef struct
{
	gl3_drawlist_t   draw_stack[GL3_MAX_DRAW_STACK];
	int             draw_stack_pos;
	gl3_drawlist_t  *draw_list;

	cl_entity_t    *entities;
	uint            max_entities;
	cl_entity_t    *viewent;
	world_static_t *world;

	qboolean        modelviewIdentity;
	int             visframecount;   // PVS generation
	int             dlightframecount;
	int             realframecount;  // views not counted
	int             framecount;
	qboolean        fResetVis;
	qboolean        visValid;        // visbytes hold a PVS

	double          frametime;
	float           blend;
	vec3_t          modelorg;        // view origin in the current model's space
} gl3_globals_t;

//
// r_main.c
//
// r_style: which of the original's two renderers we imitate, or our own look. A style is a preset
// over the cvars that already exist plus the flags below; each cvar stays usable on its own from the
// console command line, so a mixed setting is a debugging tool rather than a broken state.
enum
{
	GL3_STYLE_ORIGINAL = 0, // the accelerated renderer of 1998, which our renderer follows by default
	GL3_STYLE_SOFTWARE,     // the span renderer: 16 bit, point sampling, its own water
	GL3_STYLE_ENHANCED,     // the original plus what a GPU of today affords: ripple water, the HL2 flashlight
};

int R_GL3Style( void );

//
// r_ripple.c
//
//
// r_flashlight.c
//
void R_GL3InitFlashlight( void );
void R_GL3FlashlightNewFrame( void );
void R_GL3FlashlightUpdate( void );
qboolean R_GL3FlashlightActive( void );
qboolean R_GL3FlashlightShadowsWanted( void );
int R_GL3FlashlightCookie( void );
const float *R_GL3FlashlightParams( void );
const float *R_GL3FlashlightLamp( void );
void R_GL3FlashlightObject( const matrix4x4 object, float rows[16] );
void R_GL3FlashlightRows( float rows[16] );
void R_GL3WorldShadowRange( int first, int last );
void R_GL3FreeFlashlight( void );
void R_GL3FlashlightShadow( int first, int last );
void R_GL3StudioDrawShadowCasters( void );
void R_GL3StudioForgetShadowCasters( void );
qboolean R_GL3FlashlightShadowed( void );
int R_GL3FlashlightShadowMap( void );

qboolean R_GL3RipplesWanted( void );
void R_GL3ResetRipples( void );
void R_GL3AnimateRipples( void );
int R_GL3RippleTexture( void );

//
// r_image.c
//
extern int gl3_white_texture;
extern int gl3_particle_texture;

gl3_texture_t *R_GL3GetTexture( unsigned int texnum );
void R_GL3ConvertRow( uint32_t *dst, const byte *src, int width, pixformat_t type );
int R_GL3LoadTexture( const char *name, const byte *buf, size_t size, int flags );
int R_GL3LoadTextureFromBuffer( const char *name, rgbdata_t *pic, texFlags_t flags, qboolean update );
int R_GL3CreateTexture( const char *name, int width, int height, const void *buffer, texFlags_t flags );
void R_GL3UpdateTexture( int texnum, int cols, int rows, int width, int height, const byte *buffer, pixformat_t fmt );
qboolean R_GL3KeepTextureForNextMap( unsigned int texnum );
int R_GL3FindTexture( const char *name );
void R_GL3FreeTexture( unsigned int texnum );
int R_GL3WrapTexture( const char *name, GLuint glnum, int width, int height, uint flags );
void R_GL3ProcessTexture( int texnum, float gamma, int topColor, int bottomColor );
const char *R_GL3TextureName( unsigned int texnum );
const byte *R_GL3TextureData( unsigned int texnum );
void R_GL3BindTexture( int texnum );
void R_GL3BindLightmap( GLuint glnum );
void R_GL3BindRipple( int texnum );
void R_GL3BindCookie( int texnum );
void R_GL3BindShadowMap( int texnum );
void R_GL3EnsureTiledScene( void );
qboolean R_GL3TiledSceneActive( void );
qboolean R_GL3TiledSceneWanted( void );
void R_GL3WaitForGPU( void );
void R_GL3RestoreBackBuffer( void );
void R_GL3UnbindTexture( GLuint glnum );
void R_GL3ForgetBindings( void );
qboolean R_GL3TextureFilteringEnabled( int texnum );

void R_GL3InitImages( void );
void R_GL3ShutdownImages( void );

//
// r_shader.c
//
#define GL3_VS_CONSTANTS 256 // registers of the vertex stage (the studio shader's bones reach 215)
#define GL3_PS_CONSTANTS 32
#define GL3_PS_ALPHAREF  31  // every fragment shader: x the alpha test's reference, -1 with no test

// fixed attribute numbers of every program
enum
{
	GL3_ATTR_POSITION = 0,
	GL3_ATTR_COLOR,
	GL3_ATTR_TEXCOORD,
	GL3_ATTR_LIGHTCOORD,
	GL3_ATTR_NORMAL,
	GL3_ATTR_BONE,
	GL3_ATTR_EXTRA,
	GL3_ATTR_COUNT
};

// fixed texture units of every program
enum
{
	GL3_UNIT_TEXTURE = 0,
	GL3_UNIT_LIGHTMAP,
	GL3_UNIT_RIPPLE,
	GL3_UNIT_COOKIE,
	GL3_UNIT_SHADOW,
	GL3_UNIT_GAMMA,
	GL3_UNIT_COUNT
};

typedef struct
{
	char   name[32];
	GLuint prog;
	GLint  vsc, psc;             // the register arrays' locations, -1 when unused
	int    vsc_count, psc_count; // and their sizes
	uint   vsc_serial, psc_serial; // the shared registers' version it last took
} gl3_program_t;

gl3_program_t *R_GL3CreateProgram( const char *name, const char *source, const char *defines );
void R_GL3UseProgram( gl3_program_t *p );
void R_GL3SetVSConstants( int start, const float *data, int count );
void R_GL3SetPSConstants( int start, const float *data, int count );
void R_GL3CommitConstants( void );
void R_GL3ShutdownPrograms( void );

//
// r_draw.c
//
extern gl3_states_t gl3_wanted;

void R_GL3ApplyStates( void );
void R_GL3SetTransform( const float rows[16] );
void R_GL3SetDrawFog( const float eyeplane[4], const float fog[4] );
void R_GL3EffectFog( qboolean allow );
void R_GL3DrawReset( void );
void R_GL3CountDraw( int vertices );
qboolean R_GL3InitDraw( void );
void R_GL3ShutdownDraw( void );

void R_GL3BeginFrameDraw( void );
void R_GL3EndFrameDraw( void );
void R_GL3FrameStats( double present_time );
void R_GL3Set2DMode( qboolean enable );
void R_GL3Set2DOffset( float x, float y );
void R_GL3SetRenderMode( int mode );
void R_GL3TriRenderMode( int mode );
void R_GL3DrawStretchPic( float x, float y, float w, float h, float s1, float t1, float s2, float t2, int texnum );
void R_GL3FillRGBA( int rendermode, float x, float y, float w, float h, byte r, byte g, byte b, byte a );
void R_GL3Bind( int texnum );
void R_GL3TriBegin( int mode );
void R_GL3TriEnd( void );
void R_GL3TriColor4ub( byte r, byte g, byte b, byte a );
void R_GL3TriColor4f( float r, float g, float b, float a );
void R_GL3TriTexCoord2f( float u, float v );
void R_GL3TriVertex3f( float x, float y, float z );
void R_GL3TriVertex3fv( const float *v );
void R_GL3TriCullFace( TRICULLSTYLE mode );
void R_GL3TriQuad( const vec3_t v[4], const float st[8] );

//
// r_scene.c
//
extern gl3_view_t    gl3_ri;
extern gl3_globals_t gl3_tr;

void R_GL3InitScene( void );
void R_GL3NewMapScene( void );
qboolean R_GL3CullBox( const vec3_t mins, const vec3_t maxs, int clipflags );
cl_entity_t *R_GL3EntityByIndex( int index );
int R_GL3FxBlend( cl_entity_t *e );
void R_GL3ClearScene( void );
qboolean R_GL3AddEntity( cl_entity_t *clent, int type );
void R_GL3PushScene( void );
void R_GL3PopScene( void );
void R_GL3ProcessEntData( qboolean allocate, cl_entity_t *entities, unsigned int max_entities );
void R_GL3ClipMatrix( const matrix4x4 m, float rows[16] );
void R_GL3LoadIdentity( void );
void R_GL3RotateForEntity( cl_entity_t *e );
void R_GL3TranslateForEntity( cl_entity_t *e );
void R_GL3RestoreViewport( void );
void R_GL3RestoreViewViewport( void );
void R_GL3SetDepthRange( float minz, float maxz );
void R_GL3CheckFog( void );
void R_GL3RenderFrame( const ref_viewpass_t *rvp );
int R_GL3WorldToScreen( const vec3_t point, vec3_t screen );
int R_GL3TriWorldToScreen( const float *world, float *screen );
void R_GL3ScreenToWorld( const float *screen, float *point );
void R_GL3GetMatrix( const int pname, float *matrix );
byte *R_GL3GetCurrentVis( void );

//
// r_world.c
//
qboolean R_GL3InitWorld( void );
void R_GL3ShutdownWorld( void );
void R_GL3SubdivideSurface( model_t *mod, msurface_t *fa );
void R_GL3WorldNewMap( void );
int R_GL3CullSurface( const msurface_t *surf, uint clipflags );
void R_GL3LightmapCoord( const vec3_t v, const msurface_t *surf, float sample_size, vec2_t coords );
texture_t *R_GL3TextureAnimation( msurface_t *s );
void R_GL3WorldBeginFrame( void );
void R_GL3WorldNewObject( void );
void R_GL3WorldSetState( const gl3_states_t *states, const float color[4] );
void R_GL3WorldSetFog( qboolean fog );
void R_GL3DefaultStates( gl3_states_t *st );
void R_GL3AddChain( msurface_t *chain, qboolean reverse );
int R_GL3WorldDrawCount( void );
void R_GL3CollectWorld( void );
void R_GL3CollectWaterAlpha( void );
void R_GL3CollectBrushModel( cl_entity_t *e );
void R_GL3WorldUpload( void );
void R_GL3DrawWorldRange( int first, int last );
void R_GL3DrawSky( qboolean fog );

//
// r_sky.c
//
void R_GL3ClearSkyBox( void );
void R_GL3AddSkyBoxSurface( const msurface_t *fa );
void R_GL3DrawSkyBox( void );
void R_GL3SetupSky( int *textures );

//
// r_decal.c
//
void R_GL3ClearDecals( void );
const float *R_GL3DecalVerts( decal_t *pDecal, msurface_t *surf, int *outCount );
void R_GL3DecalShoot( int textureIndex, int entityIndex, int modelIndex, vec3_t pos, int flags, float scale );
int R_GL3CreateDecalList( decallist_t *pList );
void R_GL3DecalRemoveAll( int textureIndex );
void R_GL3ClearAllDecals( void );

//
// r_studio.c
//
#define GL3_MAX_LOCALLIGHTS 4 // gl_studio.c MAX_LOCALLIGHTS

/*
What R_GL3StudioDrawPoints draws the current submodel from (ref_gl's g_studio and m_p* globals without its
CPU vertex path), set through engine_studio_api_t by the client or by the builtin renderer:
- gl3_ri.currententity and gl3_ri.currentmodel (SetRenderModel); header, bodypart, submodel (StudioSetupModel);
  the view model is gl3_ri.currententity == gl3_tr.viewent (ref_gl draws it with glDepthRange 0..0.3);
- bonestransform: bone to world for the vertices, written by the client through StudioGetBoneTransform;
  lighttransform: the same for normals, chrome and local lights (differs only for renderfx Distort, Hologram,
  Explode);
- lighting (StudioSetupLighting, StudioEntityLight): ambientlight and shadelight (0..255), lightvec (world),
  blightvec (lightvec in each bone's space), lightcolor, and the elights: locallight origins, locallightcolor
  (through the linear gamma table), locallightR2 (radius squared);
- forcefaceflags (STUDIO_NF_CHROME: glow shell, the chrome sprite is bound with GL_Bind, r_draw.c gl3_texture),
  rendermode (SetupRenderer), meshmode (GL_SetRenderMode before each submodel), gl3_tr.blend;
- chrome_origin (SetChromeOrigin), framecount (bumped per drawn model: per-model cache cookie);
- doremap: textures from gEngfuncs.CL_GetRemapInfoForEntity( entity )->ptexture instead of the header's.
*/
typedef struct
{
	studiohdr_t        *header;
	mstudiobodyparts_t *bodypart;
	mstudiomodel_t     *submodel;
	qboolean            doremap;
	int                 forcefaceflags;
	int                 rendermode;
	int                 meshmode;
	int                 framecount;

	matrix3x4 rotationmatrix; // entity to world
	matrix3x4 bonestransform[MAXSTUDIOBONES];
	matrix3x4 lighttransform[MAXSTUDIOBONES];

	float     ambientlight;
	float     shadelight;
	vec3_t    lightvec;
	vec3_t    lightspot; // where lightvec hit the floor
	vec3_t    lightcolor;
	vec3_t    blightvec[MAXSTUDIOBONES];
	int       numlocallights;
	dlight_t *locallight[GL3_MAX_LOCALLIGHTS];
	int       locallightcolor[GL3_MAX_LOCALLIGHTS][3];
	float     locallightR2[GL3_MAX_LOCALLIGHTS];
	vec3_t    chrome_origin;

	// the engine's tables (ENGINE_LINEAR_GAMMA_SPACE bypasses them, gl_local.h LightToTexGamma etc.)
	const uint16_t *lightgammatable;
	const uint16_t *screengammatable;
	const uint16_t *lineargammatable;
} gl3_studio_t;

extern gl3_studio_t gl3_studio;

void R_GL3StudioInit( void );
qboolean R_GL3CullModel( const cl_entity_t *e, const vec3_t absmin, const vec3_t absmax );
void R_GL3StudioResetPlayerModels( void );
qboolean R_GL3StudioFillAPI( struct engine_studio_api_s *api, struct r_studio_interface_s *pDefaultDraw );
void R_GL3StudioSetDrawInterface( struct r_studio_interface_s *pDraw );
float R_GL3StudioEstimateFrame( cl_entity_t *e, mstudioseqdesc_t *pseqdesc, double time );
void R_GL3StudioLerpMovement( cl_entity_t *e, double time, vec3_t origin, vec3_t angles );
studiohdr_t *R_GL3StudioGetHeader( void );
void R_GL3StudioLoadTextures( model_t *mod, void *data );
void R_GL3StudioUnloadTextures( void *data );
int R_GL3EntityRenderMode( cl_entity_t *ent );
void R_GL3DrawStudioModel( cl_entity_t *e );
void R_GL3RunViewmodelEvents( void );
void R_GL3DrawViewModel( void );
void R_GL3StudioDrawPoints( void ); // r_studiomesh.c

//
// r_studiomesh.c
//
qboolean R_GL3InitStudioMeshes( void );
void R_GL3ShutdownStudioMeshes( void );
void R_GL3StudioFreeMeshes( const studiohdr_t *header );
void R_GL3StudioGammaChanged( void );
void R_GL3StudioBeginFrame( void );

//
// r_sprite.c
//
qboolean R_GL3SpriteTexture( model_t *sprite, int frame );
void R_GL3DrawSpriteModel( cl_entity_t *e );

//
// r_beams.c, r_particles.c
//
void R_GL3DrawBeams( int fTrans, BEAM *active_beams );
void R_GL3DrawParticles( double frametime, particle_t *active_particles, float partsize );
void R_GL3DrawTracers( double frametime, particle_t *active_tracers );

//
// r_light.c
//
void R_GL3InitLightmaps( void );
float R_GL3LightmapScale( void );
void R_GL3CheckOverbright( void );
void R_GL3ShutdownLightmaps( void );
void R_GL3LightmapsNewMap( void );
void R_GL3CreateSurfaceLightmap( msurface_t *surf, model_t *mod );
void R_GL3RebuildLightmaps( void );
qboolean R_GL3SurfaceHasLightmap( const msurface_t *surf );
qboolean R_GL3CheckLightmap( msurface_t *fa );
void R_GL3DynamicBegin( void );
qboolean R_GL3DynamicLightmap( msurface_t *fa, float *scale, float offset[2] );
void R_GL3LightmapsCommit( void );
GLuint R_GL3LightmapTexture( int page );

//
// r_device.c
//
qboolean R_GL3CreateDevice( int width, int height );
void R_GL3DestroyDevice( void );
void R_GL3Clear( byte r, byte g, byte b );
void R_GL3Present( void );
qboolean R_GL3SetMSAA( int samples );
void R_GL3BeginTiledScene( void );
void R_GL3EndTiledScene( void );
int R_GL3TakeTiledScene( void );

// r_screenshot.c
qboolean R_GL3ScreenShot( const char *filename, int shot_type );
void R_GL3ShutdownScreenShots( void );

#endif // R_LOCAL_H
