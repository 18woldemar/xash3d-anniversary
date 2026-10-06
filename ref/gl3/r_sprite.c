/*
r_sprite.c - ref_gl3: sprite models
Copyright (C) 2026 xash3d-xenon
Follows ref/gl/gl_sprite.c and gl_triapi.c, Copyright (C) 2010 Uncle Mike

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
A sprite is one or two quads (two while an additive-format sprite lerps between frames) in the triangle
batch, plus a multiply pass with the light under it for alpha-tested sprites. Every render state is the
sprite's own, as in ref_gl (no GL_SetRenderMode). The textures come from the engine's sprite loader.
*/

#include "r_local.h"
#include "pm_defs.h"
#include "sprite.h"

#define GLARE_FALLOFF 19000.0f

// gl_triapi.c TriSpriteTexture: binds a sprite frame for the triangle API
qboolean R_GL3SpriteTexture( model_t *sprite, int frame )
{
	int texnum;

	if( !sprite || sprite->type != mod_sprite || !sprite->cache.data )
		return false;

	texnum = gEngfuncs.R_GetSpriteFrame( sprite, frame, 0.0f )->gl_texturenum;
	if( texnum == 0 )
		return false;

	if( texnum < 0 || texnum >= GL3_MAX_TEXTURES )
		texnum = R_GL3FindTexture( REF_DEFAULT_TEXTURE );

	R_GL3Bind( texnum );
	return true;
}

// the two frames an additive sprite lerps between; ent->latched.prevblending holds them
static float R_SpriteFrameInterpolant( cl_entity_t *ent, mspriteframe_t **oldframe, mspriteframe_t **curframe )
{
	msprite_t *psprite = (msprite_t *)ent->model->cache.data;
	int frame = (int)ent->curstate.frame;
	float lerpFrac = 1.0f;
	const qboolean doInterp = FBitSet( ent->curstate.effects, EF_NOINTERP ) ? false : true;

	if( frame < 0 )
	{
		frame = 0;
	}
	else if( frame >= psprite->numframes )
	{
		gEngfuncs.Con_Reportf( S_WARN "%s: no such frame %d (%s)\n", __func__, frame, ent->model->name );
		frame = psprite->numframes - 1;
	}

	if( psprite->frames[frame].type == FRAME_SINGLE )
	{
		if( doInterp )
		{
			if( ent->latched.prevblending[0] >= psprite->numframes || psprite->frames[ent->latched.prevblending[0]].type != FRAME_SINGLE )
			{
				// switched between single and angled frames, or the model changed
				ent->latched.prevblending[0] = ent->latched.prevblending[1] = frame;
				ent->latched.sequencetime = gp_cl->time;
				lerpFrac = 1.0f;
			}

			if( ent->latched.sequencetime < gp_cl->time )
			{
				if( frame != ent->latched.prevblending[1] )
				{
					ent->latched.prevblending[0] = ent->latched.prevblending[1];
					ent->latched.prevblending[1] = frame;
					ent->latched.sequencetime = gp_cl->time;
					lerpFrac = 0.0f;
				}
				else lerpFrac = ( gp_cl->time - ent->latched.sequencetime ) * 11.0f;
			}
			else
			{
				ent->latched.prevblending[0] = ent->latched.prevblending[1] = frame;
				ent->latched.sequencetime = gp_cl->time;
				lerpFrac = 0.0f;
			}
		}
		else
		{
			ent->latched.prevblending[0] = ent->latched.prevblending[1] = frame;
			lerpFrac = 1.0f;
		}

		if( ent->latched.prevblending[0] >= psprite->numframes )
		{
			ent->latched.prevblending[0] = ent->latched.prevblending[1] = frame;
			ent->latched.sequencetime = gp_cl->time;
			lerpFrac = 0.0f;
		}

		if( oldframe ) *oldframe = psprite->frames[ent->latched.prevblending[0]].frameptr;
		if( curframe ) *curframe = psprite->frames[frame].frameptr;
	}
	else if( psprite->frames[frame].type == FRAME_GROUP )
	{
		mspritegroup_t *pspritegroup = (mspritegroup_t *)psprite->frames[frame].frameptr;
		float *pintervals = pspritegroup->intervals;
		int numframes = pspritegroup->numframes;
		float fullinterval = pintervals[numframes - 1];
		float jinterval = pintervals[1] - pintervals[0];
		float time = gp_cl->time;
		float jtime = 0.0f;
		float targettime = time - ((int)( time / fullinterval )) * fullinterval; // intervals are positive
		int i, j;

		// the loop from the last frame to the first is timed as the first frame
		for( i = 0, j = numframes - 1; i < ( numframes - 1 ); i++ )
		{
			if( pintervals[i] > targettime )
				break;
			j = i;
			jinterval = pintervals[i] - jtime;
			jtime = pintervals[i];
		}

		if( doInterp )
			lerpFrac = ( targettime - jtime ) / jinterval;
		else j = i;

		if( oldframe ) *oldframe = pspritegroup->frames[j];
		if( curframe ) *curframe = pspritegroup->frames[i];
	}
	else if( psprite->frames[frame].type == FRAME_ANGLED )
	{
		float yaw = ent->angles[YAW];
		int angleframe = (int)( Q_rint(( gl3_ri.rvp.viewangles[1] - yaw + 45.0f ) / 360 * 8 ) - 4 ) & 7;
		mspritegroup_t *pspritegroup;

		if( doInterp )
		{
			if( ent->latched.prevblending[0] >= psprite->numframes || psprite->frames[ent->latched.prevblending[0]].type != FRAME_ANGLED )
			{
				ent->latched.prevblending[0] = ent->latched.prevblending[1] = frame;
				ent->latched.sequencetime = gp_cl->time;
				lerpFrac = 1.0f;
			}

			if( ent->latched.sequencetime < gp_cl->time )
			{
				if( frame != ent->latched.prevblending[1] )
				{
					ent->latched.prevblending[0] = ent->latched.prevblending[1];
					ent->latched.prevblending[1] = frame;
					ent->latched.sequencetime = gp_cl->time;
					lerpFrac = 0.0f;
				}
				else lerpFrac = ( gp_cl->time - ent->latched.sequencetime ) * ent->curstate.framerate;
			}
			else
			{
				ent->latched.prevblending[0] = ent->latched.prevblending[1] = frame;
				ent->latched.sequencetime = gp_cl->time;
				lerpFrac = 0.0f;
			}
		}
		else
		{
			ent->latched.prevblending[0] = ent->latched.prevblending[1] = frame;
			lerpFrac = 1.0f;
		}

		pspritegroup = (mspritegroup_t *)psprite->frames[ent->latched.prevblending[0]].frameptr;
		if( oldframe ) *oldframe = pspritegroup->frames[angleframe];

		pspritegroup = (mspritegroup_t *)psprite->frames[frame].frameptr;
		if( curframe ) *curframe = pspritegroup->frames[angleframe];
	}

	return lerpFrac;
}

static qboolean R_CullSpriteModel( cl_entity_t *e, const vec3_t origin )
{
	vec3_t sprite_mins, sprite_maxs;
	float scale = 1.0f;

	if( !e->model->cache.data )
		return true;

	if( e->curstate.scale > 0.0f )
		scale = e->curstate.scale;

	// no rotation for sprites
	VectorScale( e->model->mins, scale, sprite_mins );
	VectorScale( e->model->maxs, scale, sprite_maxs );
	VectorAdd( sprite_mins, origin, sprite_mins );
	VectorAdd( sprite_maxs, origin, sprite_maxs );

	return R_GL3CullModel( e, sprite_mins, sprite_maxs );
}

// brightness of a glow: 0 when something is in front of it, less and bigger with the distance
static float R_SpriteGlowBlend( vec3_t origin, int renderfx, float *pscale )
{
	vec3_t glowDist;
	float dist, brightness;

	VectorSubtract( origin, gl3_ri.rvp.vieworigin, glowDist );
	dist = VectorLength( glowDist );

	if( !FBitSet( gl3_ri.rvp.flags, RF_DRAW_CUBEMAP ))
	{
		// ref_gl's r_traceglow 0: studio models do not hide glows
		pmtrace_t *tr = gEngfuncs.EV_VisTraceLine( gl3_ri.rvp.vieworigin, origin, PM_GLASS_IGNORE | PM_STUDIO_IGNORE );

		if(( 1.0f - tr->fraction ) * dist > 8.0f )
			return 0.0f;
	}

	if( renderfx == kRenderFxNoDissipation )
		return 1.0f;

	brightness = GLARE_FALLOFF / ( dist * dist );
	brightness = bound( 0.05f, brightness, 1.0f );
	*pscale *= dist * ( 1.0f / 200.0f );

	return brightness;
}

static qboolean R_SpriteOccluded( cl_entity_t *e, vec3_t origin, float *pscale )
{
	float blend;
	vec3_t v;

	if( e->curstate.rendermode != kRenderGlow )
		return R_CullSpriteModel( e, origin );

	// glows: no frustum test, only the screen rectangle
	R_GL3TriWorldToScreen( origin, v );

	if( v[0] < gl3_ri.rvp.viewport[0] || v[0] > gl3_ri.rvp.viewport[0] + gl3_ri.rvp.viewport[2] )
		return true;
	if( v[1] < gl3_ri.rvp.viewport[1] || v[1] > gl3_ri.rvp.viewport[1] + gl3_ri.rvp.viewport[3] )
		return true;

	blend = R_SpriteGlowBlend( origin, e->curstate.renderfx, pscale );
	gl3_tr.blend *= blend;

	return blend <= 0.01f;
}

static void R_DrawSpriteQuad( const mspriteframe_t *frame, const vec3_t org, const vec3_t v_right, const vec3_t v_up, float scale )
{
	static const float st[8] = { 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f };
	vec3_t v[4];

	VectorMA( org, frame->down * scale, v_up, v[0] );
	VectorMA( v[0], frame->left * scale, v_right, v[0] );
	VectorMA( org, frame->up * scale, v_up, v[1] );
	VectorMA( v[1], frame->left * scale, v_right, v[1] );
	VectorMA( org, frame->up * scale, v_up, v[2] );
	VectorMA( v[2], frame->right * scale, v_right, v[2] );
	VectorMA( org, frame->down * scale, v_up, v[3] );
	VectorMA( v[3], frame->right * scale, v_right, v[3] );
	R_GL3TriQuad( v, st );
}

static qboolean R_SpriteHasLightmap( cl_entity_t *e, int texFormat )
{
	if( !r_sprite_lighting->value || texFormat != SPR_ALPHTEST )
		return false;

	if( FBitSet( e->curstate.effects, EF_FULLBRIGHT ) || e->curstate.renderamt <= 127 )
		return false;

	switch( e->curstate.rendermode )
	{
	case kRenderNormal:
	case kRenderTransAlpha:
	case kRenderTransTexture:
		return true;
	}

	return false;
}

static qboolean R_SpriteAllowLerping( cl_entity_t *e, msprite_t *psprite )
{
	if( !r_sprite_lerping->value || psprite->numframes <= 1 || psprite->texFormat != SPR_ADDITIVE )
		return false;

	return e->curstate.rendermode != kRenderNormal && e->curstate.rendermode != kRenderTransAlpha;
}

/*
=================
R_DrawSpriteModel
=================
*/
void R_GL3DrawSpriteModel( cl_entity_t *e )
{
	model_t *model = e->model;
	msprite_t *psprite = (msprite_t *)model->cache.data;
	const int rendermode = e->curstate.rendermode;
	mspriteframe_t *frame = NULL, *oldframe = NULL;
	vec3_t origin, color, color2 = { 0.0f, 0.0f, 0.0f };
	vec3_t v_right, v_up;
	float scale, lerp = 1.0f;
	qboolean lightmap, visible = true;
	int type;

	if( FBitSet( gl3_ri.rvp.flags, RF_DRAW_CUBEMAP ))
		return;

	VectorCopy( e->origin, origin );

	// attached to another entity
	if( e->curstate.aiment > 0 && e->curstate.movetype == MOVETYPE_FOLLOW )
	{
		cl_entity_t *parent = R_GL3EntityByIndex( e->curstate.aiment );

		if( parent && parent->model )
		{
			// ref_gl clamps to MAXSTUDIOATTACHMENTS and would read past the four attachments
			if( parent->model->type == mod_studio && e->curstate.body > 0 )
				VectorCopy( parent->attachment[Q_min( e->curstate.body, (int)ARRAYSIZE( parent->attachment )) - 1], origin );
			else VectorCopy( parent->origin, origin );
		}
	}

	scale = e->curstate.scale;
	if( !scale ) scale = 1.0f;

	if( R_SpriteOccluded( e, origin, &scale ))
		return;

	R_GL3LoadIdentity();
	R_GL3EffectFog( rendermode != kRenderGlow && rendermode != kRenderTransAdd );

	switch( rendermode )
	{
	case kRenderTransAlpha:
		gl3_wanted.zwrite = FALSE;
		// fallthrough
	case kRenderTransColor:
	case kRenderTransTexture:
		gl3_wanted.blend = TRUE;
		gl3_wanted.src = D3DBLEND_SRCALPHA;
		gl3_wanted.dst = D3DBLEND_INVSRCALPHA;
		break;
	case kRenderGlow:
		gl3_wanted.ztest = FALSE;
		// fallthrough
	case kRenderTransAdd:
		gl3_wanted.blend = TRUE;
		gl3_wanted.src = D3DBLEND_SRCALPHA;
		gl3_wanted.dst = D3DBLEND_ONE;
		gl3_wanted.zwrite = FALSE;
		break;
	default:
		gl3_wanted.blend = FALSE;
		break;
	}

	gl3_wanted.alphatest = TRUE;
	gl3_wanted.alpharef = 0;

	// rendercolor 0 0 0 is a Valve Hammer Editor bug: white
	if( e->curstate.rendercolor.r || e->curstate.rendercolor.g || e->curstate.rendercolor.b )
	{
		color[0] = (float)e->curstate.rendercolor.r * ( 1.0f / 255.0f );
		color[1] = (float)e->curstate.rendercolor.g * ( 1.0f / 255.0f );
		color[2] = (float)e->curstate.rendercolor.b * ( 1.0f / 255.0f );
	}
	else VectorSet( color, 1.0f, 1.0f, 1.0f );

	lightmap = R_SpriteHasLightmap( e, psprite->texFormat );
	if( lightmap )
	{
		colorVec lightColor = R_LightPoint( origin );

		color2[0] = (float)lightColor.r * ( 1.0f / 255.0f );
		color2[1] = (float)lightColor.g * ( 1.0f / 255.0f );
		color2[2] = (float)lightColor.b * ( 1.0f / 255.0f );
		gl3_wanted.alpharef = 85; // GL_GREATER 1/3
	}

	if( R_SpriteAllowLerping( e, psprite ))
		lerp = R_SpriteFrameInterpolant( e, &oldframe, &frame );
	else frame = oldframe = gEngfuncs.R_GetSpriteFrame( model, (int)e->curstate.frame, e->angles[YAW] );

	type = psprite->type;

	// parallel sprites with a roll turn with it
	if( e->angles[ROLL] != 0.0f && type == SPR_FWD_PARALLEL )
		type = SPR_FWD_PARALLEL_ORIENTED;

	switch( type )
	{
	case SPR_ORIENTED:
	{
		vec3_t v_forward;

		AngleVectors( e->angles, v_forward, v_right, v_up );
		VectorScale( v_forward, 0.01f, v_forward ); // against z-fighting
		VectorSubtract( origin, v_forward, origin );
		break;
	}
	case SPR_FACING_UPRIGHT:
		VectorSet( v_right, origin[1] - gl3_ri.rvp.vieworigin[1], -( origin[0] - gl3_ri.rvp.vieworigin[0] ), 0.0f );
		VectorSet( v_up, 0.0f, 0.0f, 1.0f );
		VectorNormalize( v_right );
		break;
	case SPR_FWD_PARALLEL_UPRIGHT:
	{
		const float dot = gl3_ri.vforward[2];

		// cos( 1 degree ); ref_gl returns here without restoring its states
		if(( dot > 0.999848f ) || ( dot < -0.999848f ))
			visible = false;
		VectorSet( v_up, 0.0f, 0.0f, 1.0f );
		VectorSet( v_right, gl3_ri.vforward[1], -gl3_ri.vforward[0], 0.0f );
		VectorNormalize( v_right );
		break;
	}
	case SPR_FWD_PARALLEL_ORIENTED:
	{
		const float angle = e->angles[ROLL] * ( M_PI2 / 360.0f );
		float sr, cr;

		SinCos( angle, &sr, &cr );
		for( int i = 0; i < 3; i++ )
		{
			v_right[i] = ( gl3_ri.vright[i] * cr + gl3_ri.vup[i] * sr );
			v_up[i] = gl3_ri.vright[i] * -sr + gl3_ri.vup[i] * cr;
		}
		break;
	}
	case SPR_FWD_PARALLEL:
	default:
		VectorCopy( gl3_ri.vright, v_right );
		VectorCopy( gl3_ri.vup, v_up );
		break;
	}

	gl3_wanted.cull = psprite->facecull == SPR_CULL_NONE ? D3DCULL_NONE : GL3_CULL_FRONT;

	if( visible && oldframe == frame )
	{
		R_GL3TriColor4f( color[0], color[1], color[2], gl3_tr.blend );
		R_GL3Bind( frame->gl_texturenum );
		R_DrawSpriteQuad( frame, origin, v_right, v_up, scale );
	}
	else if( visible )
	{
		float ilerp;

		lerp = bound( 0.0f, lerp, 1.0f );
		ilerp = 1.0f - lerp;

		if( ilerp != 0.0f )
		{
			R_GL3TriColor4f( color[0], color[1], color[2], gl3_tr.blend * ilerp );
			R_GL3Bind( oldframe->gl_texturenum );
			R_DrawSpriteQuad( oldframe, origin, v_right, v_up, scale );
		}

		if( lerp != 0.0f )
		{
			R_GL3TriColor4f( color[0], color[1], color[2], gl3_tr.blend * lerp );
			R_GL3Bind( frame->gl_texturenum );
			R_DrawSpriteQuad( frame, origin, v_right, v_up, scale );
		}
	}

	// the sprite 'lightmap': multiplies what the first pass drew (where it wrote its depth) by the light
	if( lightmap && visible )
	{
		gl3_wanted.blend = r_lightmap->value ? FALSE : TRUE;
		gl3_wanted.zfunc = D3DCMP_EQUAL;
		gl3_wanted.alphatest = FALSE;
		gl3_wanted.src = D3DBLEND_ZERO;
		gl3_wanted.dst = D3DBLEND_SRCCOLOR;
		R_GL3TriColor4f( color2[0], color2[1], color2[2], gl3_tr.blend );
		R_GL3Bind( gl3_white_texture );
		R_DrawSpriteQuad( frame, origin, v_right, v_up, scale );
		gl3_wanted.zfunc = D3DCMP_LESSEQUAL;
		gl3_wanted.blend = FALSE;
	}

	gl3_wanted.cull = GL3_CULL_FRONT;
	gl3_wanted.alphatest = FALSE;
	gl3_wanted.alpharef = 0;
	gl3_wanted.zwrite = TRUE;

	if( rendermode != kRenderNormal )
	{
		gl3_wanted.blend = FALSE;
		gl3_wanted.ztest = TRUE;
	}
}
