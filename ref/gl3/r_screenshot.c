/*
r_screenshot.c - ref_gl3: screenshots and save game pictures
Copyright (C) 2026 xash3d-xenon
Sizes and image processing follow ref/gl/gl_backend.c VID_ScreenShot, Copyright (C) 2010 Uncle Mike

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
The back buffer lives in EDRAM and cannot be locked: Resolve copies it into a texture, whose memory is
laid out in the GPU's tiled order (XGAddress2DTiledOffset), and a read-only lock reads it. The engine asks
after the 2D drawing and before Present, so the batch is drawn first. ref_gl applies no gamma either.
*/

#include <xtl.h>
#include <xgraphics.h> // before the engine headers, which break xboxmath.h
#include "r_local.h"

static IDirect3DTexture9 *gl3_shot_texture;

static qboolean R_GL3ReadBackBuffer( byte *rgba, int width, int height )
{
	D3DLOCKED_RECT lr;

	if( !gl3_shot_texture && FAILED( gl3_device->CreateTexture( width, height, 1, D3DUSAGE_CPU_CACHED_MEMORY, D3DFMT_X8R8G8B8, D3DPOOL_MANAGED, &gl3_shot_texture, NULL )))
	{
		gEngfuncs.Con_Printf( S_ERROR "ref_gl3: can't create the screenshot texture\n" );
		return false;
	}

	R_GL3DrawReset();
	gl3_device->Resolve( D3DRESOLVE_RENDERTARGET0, NULL, gl3_shot_texture, NULL, 0, 0, NULL, 0.0f, 0, NULL );

	if( FAILED( gl3_shot_texture->LockRect( 0, &lr, NULL, D3DLOCK_READONLY )))
		return false;

	for( int y = 0; y < height; y++ )
	{
		for( int x = 0; x < width; x++ )
		{
			const uint32_t texel = ((const uint32_t *)lr.pBits)[XGAddress2DTiledOffset( x, y, width, sizeof( uint32_t ))];
			byte *out = rgba + ( y * width + x ) * 4;

			out[0] = ( texel >> 16 ) & 0xff;
			out[1] = ( texel >> 8 ) & 0xff;
			out[2] = texel & 0xff;
			out[3] = 0xff;
		}
	}

	gl3_shot_texture->UnlockRect( 0 );
	return true;
}

qboolean R_GL3ScreenShot( const char *filename, int shot_type )
{
	const double start = gEngfuncs.pfnTime();
	double read_done, process_done;
	rgbdata_t *shot;
	uint flags = 0; // rows are read top-down already (ref_gl flips GL's)
	int width = 0, height = 0;
	qboolean result;

	shot = (rgbdata_t *)Mem_Calloc( r_temppool, sizeof( rgbdata_t ));
	shot->width = ( gpGlobals->width + 3 ) & ~3;
	shot->height = ( gpGlobals->height + 3 ) & ~3;
	shot->flags = IMAGE_HAS_COLOR;
	shot->type = PF_RGBA_32;
	shot->size = shot->width * shot->height * 4;
	shot->buffer = (byte *)Mem_Malloc( r_temppool, shot->size );

	if( !R_GL3ReadBackBuffer( shot->buffer, shot->width, shot->height ))
	{
		gEngfuncs.FS_FreeImage( shot );
		return false;
	}
	read_done = gEngfuncs.pfnTime();

	switch( shot_type )
	{
	case VID_SNAPSHOT:
		gEngfuncs.fsapi->AllowDirectPaths( true );
		break;
	case VID_LEVELSHOT:
	case VID_MINISHOT:
		flags |= IMAGE_RESAMPLE;
		height = shot_type == VID_MINISHOT ? 200 : 480;
		width = Q_rint( height * ((double)shot->width / shot->height ));
		break;
	case VID_MAPSHOT:
		flags |= IMAGE_RESAMPLE | IMAGE_QUANTIZE; // GoldSrc's overviews are 8-bit
		height = 768;
		width = 1024;
		break;
	}

	gEngfuncs.Image_Process( &shot, width, height, flags, 0.0f );
	process_done = gEngfuncs.pfnTime();
	result = gEngfuncs.FS_SaveImage( filename, shot );
	gEngfuncs.fsapi->AllowDirectPaths( false );

	// the save picture is taken while the game waits: say where its time went
	gEngfuncs.Con_Reportf( "shot %s: %.0f ms (read back %.0f, %dx%d -> %dx%d %.0f, write %.0f ms)\n",
		filename, ( gEngfuncs.pfnTime() - start ) * 1000.0, ( read_done - start ) * 1000.0,
		( gpGlobals->width + 3 ) & ~3, ( gpGlobals->height + 3 ) & ~3, shot->width, shot->height,
		( process_done - read_done ) * 1000.0, ( gEngfuncs.pfnTime() - process_done ) * 1000.0 );

	gEngfuncs.FS_FreeImage( shot );
	return result;
}

void R_GL3ShutdownScreenShots( void )
{
	if( !gl3_shot_texture )
		return;

	gl3_shot_texture->Release(); // never bound to a sampler
	gl3_shot_texture = NULL;
}
