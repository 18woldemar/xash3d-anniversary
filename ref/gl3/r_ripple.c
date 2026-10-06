/*
r_ripple.c - the software renderer's water, as a field of offsets

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

/*
Half-Life's software renderer did not warp water with a sine, the way Quake's did: it ran a ripple
simulation over a 128x128 field of shorts and read the water texture through it (`watertex`/`watertex2`
and WaterTextureDrop / Smooth / Swap / Update in D_SCAN.C). The field is a wave equation on a grid: every
step a cell becomes half the sum of its four neighbours minus its own previous value, damped by 1/64, and
a random drop is thrown in now and then.

The original rebuilt each water texture from the field. We upload the field itself instead and let the
water pixel shader read its offset, which is one 128x128 upload a frame no matter how many water textures
the map has. The numbers are the original's, read from sw.so: the step is 0.05 s, a drop lands every
0.1 s, the damping is >> 6 and the drop spreads a quarter of its value to each neighbour.
*/

#include "r_local.h"

#define GL3_RIPPLE_BITS   7
#define GL3_RIPPLE_SIZE   ( 1 << GL3_RIPPLE_BITS )        // 128, the original's field
#define GL3_RIPPLE_MASK   ( GL3_RIPPLE_SIZE - 1 )
#define GL3_RIPPLE_CELLS  ( GL3_RIPPLE_SIZE * GL3_RIPPLE_SIZE )
#define GL3_RIPPLE_CMASK  ( GL3_RIPPLE_CELLS - 1 )

#define GL3_RIPPLE_STEP   0.05  // D_SCAN.C WaterTextureUpdate
#define GL3_RIPPLE_SPAWN  0.1   // the same function's drop interval

static struct
{
	short  buf[2][GL3_RIPPLE_CELLS];
	short *cur, *old;
	double time, oldtime;
	int    texture;
	byte   rgba[GL3_RIPPLE_CELLS * 4];
} gl3_ripple;

void R_GL3ResetRipples( void )
{
	memset( gl3_ripple.buf, 0, sizeof( gl3_ripple.buf ));
	gl3_ripple.cur = gl3_ripple.buf[0];
	gl3_ripple.old = gl3_ripple.buf[1];
	gl3_ripple.time = gl3_ripple.oldtime = gp_cl->time - GL3_RIPPLE_SPAWN;
}

// WaterTextureDrop: the cell takes the whole value, its four neighbours a quarter each
static void R_GL3SpawnRipple( int x, int y, int val )
{
#define GL3_CELL( x, y ) ((( x ) & GL3_RIPPLE_MASK ) + ((( y ) & GL3_RIPPLE_MASK ) << GL3_RIPPLE_BITS ))
	gl3_ripple.old[GL3_CELL( x, y )] += val;

	val >>= 2;
	gl3_ripple.old[GL3_CELL( x + 1, y )] += val;
	gl3_ripple.old[GL3_CELL( x - 1, y )] += val;
	gl3_ripple.old[GL3_CELL( x, y + 1 )] += val;
	gl3_ripple.old[GL3_CELL( x, y - 1 )] += val;
#undef GL3_CELL
}

// WaterTextureSmooth: one step of the wave equation over the whole field
static void R_GL3RunRipples( void )
{
	const short *old = gl3_ripple.old;
	short *cur = gl3_ripple.cur;

	for( int i = 0; i < GL3_RIPPLE_CELLS; i++ )
	{
		int v = (int)old[( i - GL3_RIPPLE_SIZE ) & GL3_RIPPLE_CMASK]
			+ (int)old[( i + GL3_RIPPLE_SIZE ) & GL3_RIPPLE_CMASK]
			+ (int)old[( i + 1 ) & GL3_RIPPLE_CMASK]
			+ (int)old[( i - 1 ) & GL3_RIPPLE_CMASK];

		v = ( v >> 1 ) - (int)cur[i];
		cur[i] = (short)( v - ( v >> 6 ));
	}
}

// the shader wants the offset the original applied to a texel, which is the cell value over 16
static void R_GL3UploadRipples( void )
{
	byte *dst = gl3_ripple.rgba;

	for( int i = 0; i < GL3_RIPPLE_CELLS; i++, dst += 4 )
	{
		int val = bound( -128, gl3_ripple.cur[i] / 16, 127 );

		dst[0] = dst[1] = dst[2] = (byte)( val + 128 );
		dst[3] = 255;
	}

	if( !gl3_ripple.texture )
	{
		gl3_ripple.texture = R_GL3CreateTexture( "*ripple", GL3_RIPPLE_SIZE, GL3_RIPPLE_SIZE, gl3_ripple.rgba,
			(texFlags_t)( TF_NOMIPMAP | TF_NEAREST ));
		return;
	}

	R_GL3UpdateTexture( gl3_ripple.texture, GL3_RIPPLE_SIZE, GL3_RIPPLE_SIZE, GL3_RIPPLE_SIZE, GL3_RIPPLE_SIZE,
		gl3_ripple.rgba, PF_RGBA_32 );
}

/*
==================
R_GL3AnimateRipples

One step of the simulation, at the original's rate rather than the frame rate.
==================
*/
void R_GL3AnimateRipples( void )
{
	short *swap;

	if( !gl3_ripple.cur )
		R_GL3ResetRipples();

	if( gp_cl->time < gl3_ripple.time ) // a load or a level change moved the clock back
		R_GL3ResetRipples();

	if( gp_cl->time - gl3_ripple.time < GL3_RIPPLE_STEP )
		return;

	gl3_ripple.time = gp_cl->time;

	swap = gl3_ripple.cur;
	gl3_ripple.cur = gl3_ripple.old;
	gl3_ripple.old = swap;

	if( gl3_ripple.time - gl3_ripple.oldtime > GL3_RIPPLE_SPAWN )
	{
		gl3_ripple.oldtime = gl3_ripple.time;
		R_GL3SpawnRipple( gEngfuncs.COM_RandomLong( 0, 0x7fff ), gEngfuncs.COM_RandomLong( 0, 0x7fff ),
			gEngfuncs.COM_RandomLong( 0, 0x3ff ));
	}

	R_GL3RunRipples();
	R_GL3UploadRipples();
}

int R_GL3RippleTexture( void )
{
	if( !gl3_ripple.texture )
		R_GL3UploadRipples();

	return gl3_ripple.texture;
}
