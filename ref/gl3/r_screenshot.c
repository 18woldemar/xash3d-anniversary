/*
r_screenshot.c - ref_gl3: screenshots and save game pictures
Copyright (C) 2026 18woldemar
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
The engine asks after the 2D drawing and before the swap, so the batch is drawn first and the back buffer
read. ref_gl applies no gamma either.
*/

#include "r_local.h"

qboolean R_GL3ScreenShot( const char *filename, int shot_type )
{
	const double start = gEngfuncs.pfnTime();
	double read_done, process_done;
	rgbdata_t *shot;
	uint flags = 0;
	int width = 0, height = 0;
	qboolean result;

	shot = (rgbdata_t *)Mem_Calloc( r_temppool, sizeof( rgbdata_t ));
	shot->width = gpGlobals->width;
	shot->height = gpGlobals->height;
	shot->flags = IMAGE_HAS_COLOR;
	shot->type = PF_RGBA_32;
	shot->size = shot->width * shot->height * 4;
	shot->buffer = (byte *)Mem_Malloc( r_temppool, shot->size );

	R_GL3DrawReset();
	if( R_GL3BackendReadScreen( shot->buffer, shot->width, shot->height ))
		flags |= IMAGE_FLIP_Y;
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
		gpGlobals->width, gpGlobals->height, shot->width, shot->height,
		( process_done - read_done ) * 1000.0, ( gEngfuncs.pfnTime() - process_done ) * 1000.0 );

	gEngfuncs.FS_FreeImage( shot );
	return result;
}
