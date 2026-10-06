/*
r_image.c - ref_gl3: textures
Copyright (C) 2026 18woldemar
Texture table, flags and mipmap filters follow ref/gl/gl_image.c, Copyright (C) 2010 Uncle Mike

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
A texture number is an index into gl3_textures; slot 0 is never used. Every texture is GL_RGBA8, filled one
row at a time from gl3_row, whose texels are uint32 0xAARRGGBB: uploaded as GL_BGRA with
GL_UNSIGNED_INT_8_8_8_8_REV, that reads the same on either byte order. Mipmaps are built on the CPU, with
the filters ref_gl uses, so every level is the one the original would have drawn.
Filtering and addressing belong to the texture object in OpenGL, so they are set on it when they change
and binding is a single call.
*/

#include "r_local.h"

#define GL3_MAX_TEXTURE_SIZE 4096
#define GL3_TEXTURES_HASH_SIZE ( GL3_MAX_TEXTURES >> 2 )

static gl3_texture_t gl3_textures[GL3_MAX_TEXTURES];

// A texture a map takes from a WAD is read and decoded at every level change, and the next maps ask for the
// same ones. Those are kept when the map that loaded them goes away, up to this much memory; past it a level
// change frees them as it always did.
#define GL3_KEPT_TEXTURE_BUDGET ( 40 * 1024 * 1024 )
static size_t gl3_kept_textures;
static gl3_texture_t *gl3_textures_hash[GL3_TEXTURES_HASH_SIZE];
static int gl3_num_textures; // one past the highest slot in use

int gl3_white_texture;
int gl3_particle_texture;
// what each texture unit holds: 0 the diffuse, 1 the lightmap, 2 the ripple field, 3 the flashlight's cookie,
// 4 its shadow map
#define GL3_TEXTURE_UNITS 5
static GLuint  gl3_bound[GL3_TEXTURE_UNITS];
static int     gl3_active_unit;
static cvar_t *gl_anisotropy;
static float   gl3_max_anisotropy; // 0: the driver has no anisotropic filtering
static cvar_t *gl_texture_nearest;
static uint32_t gl3_row[GL3_MAX_TEXTURE_SIZE];
static byte *gl3_mip_buffer;
static size_t gl3_mip_buffer_size;
static poolhandle_t gl3_image_pool;

gl3_texture_t *R_GL3GetTexture( unsigned int texnum )
{
	if( texnum >= GL3_MAX_TEXTURES )
	{
		gEngfuncs.Host_Error( "%s: texnum (%d) >= %d\n", __func__, texnum, GL3_MAX_TEXTURES );
		texnum = 0;
	}

	return &gl3_textures[texnum];
}

/*
==================
R_GL3ConvertRow

One row of any uncompressed engine format into native 0xAARRGGBB texels.
==================
*/
void R_GL3ConvertRow( uint32_t *dst, const byte *src, int width, pixformat_t type )
{
	int x;

	switch( type )
	{
	case PF_RGBA_32:
		for( x = 0; x < width; x++, src += 4 )
			dst[x] = ((uint32_t)src[3] << 24 ) | ((uint32_t)src[0] << 16 ) | ((uint32_t)src[1] << 8 ) | src[2];
		break;
	case PF_BGRA_32:
		for( x = 0; x < width; x++, src += 4 )
			dst[x] = ((uint32_t)src[3] << 24 ) | ((uint32_t)src[2] << 16 ) | ((uint32_t)src[1] << 8 ) | src[0];
		break;
	case PF_RGB_24:
		for( x = 0; x < width; x++, src += 3 )
			dst[x] = 0xff000000u | ((uint32_t)src[0] << 16 ) | ((uint32_t)src[1] << 8 ) | src[2];
		break;
	case PF_BGR_24:
		for( x = 0; x < width; x++, src += 3 )
			dst[x] = 0xff000000u | ((uint32_t)src[2] << 16 ) | ((uint32_t)src[1] << 8 ) | src[0];
		break;
	case PF_LUMINANCE:
		for( x = 0; x < width; x++, src++ )
			dst[x] = 0xff000000u | ((uint32_t)src[0] << 16 ) | ((uint32_t)src[0] << 8 ) | src[0];
		break;
	default:
		memset( dst, 0, width * sizeof( *dst ));
		break;
	}
}

static int R_GL3BytesPerPixel( pixformat_t type )
{
	switch( type )
	{
	case PF_RGB_24:
	case PF_BGR_24:
		return 3;
	case PF_LUMINANCE:
		return 1;
	default:
		return 4;
	}
}

/*
==================
R_GL3BoxFilter3x3

Fully transparent black texels of a one-bit alpha image take the colour of their visible neighbours,
so filtering does not pull a dark fringe into the edges (gl_image.c GL_ApplyFilter).
==================
*/
static void R_GL3BoxFilter3x3( byte *out, const byte *in, int w, int h, int x, int y )
{
	int r = 0, g = 0, b = 0, count = 0;

	for( int i = -1; i <= 1; i++ )
	{
		for( int j = -1; j <= 1; j++ )
		{
			int u = x + i, v = y + j;
			const byte *pixel;

			if( u < 0 || u >= w || v < 0 || v >= h )
				continue;

			pixel = &in[( u + v * w ) * 4];
			if( pixel[3] != 0 )
			{
				r += pixel[0];
				g += pixel[1];
				b += pixel[2];
				count++;
			}
		}
	}

	if( count == 0 )
		count = 1;

	out[0] = r / count;
	out[1] = g / count;
	out[2] = b / count;
}

static void R_GL3ApplyFilter( byte *data, int width, int height )
{
	byte *in = data;

	for( int i = 0; i < width * height; i++, in += 4 )
	{
		if( in[0] == 0 && in[1] == 0 && in[2] == 0 && in[3] == 0 )
			R_GL3BoxFilter3x3( in, data, width, height, i % width, i / width );
	}
}

// in place, RGBA bytes: quarters the image (gl_image.c GL_BuildMipMap without normal maps)
static void R_GL3BuildMipMap( byte *in, int width, int height )
{
	byte *out = in;
	int mipWidth = Q_max( 1, width >> 1 );
	int mipHeight = Q_max( 1, height >> 1 );
	int stride = width * 4;

	for( int y = 0; y < mipHeight; y++, in += stride * 2 )
	{
		const byte *next = (( y << 1 ) + 1 < height ) ? in + stride : in;

		for( int x = 0, row = 0; x < mipWidth; x++, row += 8, out += 4 )
		{
			if(( x << 1 ) + 1 < width )
			{
				out[0] = ( in[row + 0] + in[row + 4] + next[row + 0] + next[row + 4] ) >> 2;
				out[1] = ( in[row + 1] + in[row + 5] + next[row + 1] + next[row + 5] ) >> 2;
				out[2] = ( in[row + 2] + in[row + 6] + next[row + 2] + next[row + 6] ) >> 2;
				out[3] = ( in[row + 3] + in[row + 7] + next[row + 3] + next[row + 7] ) >> 2;
			}
			else
			{
				out[0] = ( in[row + 0] + next[row + 0] ) >> 1;
				out[1] = ( in[row + 1] + next[row + 1] ) >> 1;
				out[2] = ( in[row + 2] + next[row + 2] ) >> 1;
				out[3] = ( in[row + 3] + next[row + 3] ) >> 1;
			}
		}
	}
}

static int R_GL3MipCount( int width, int height )
{
	int count = 1;

	while( width > 1 || height > 1 )
	{
		width = Q_max( 1, width >> 1 );
		height = Q_max( 1, height >> 1 );
		count++;
	}

	return count;
}

static void R_GL3ActiveUnit( int unit )
{
	if( unit == gl3_active_unit )
		return;

	glActiveTexture( GL_TEXTURE0 + unit );
	gl3_active_unit = unit;
}

static void R_GL3BindUnit( int unit, GLuint glnum )
{
	if( gl3_bound[unit] == glnum )
		return;

	R_GL3ActiveUnit( unit );
	glBindTexture( GL_TEXTURE_2D, glnum );
	gl3_bound[unit] = glnum;
}

// a deleted texture name may come back from glGenTextures for another texture: no unit may still claim it
void R_GL3UnbindTexture( GLuint glnum )
{
	if( !glnum )
		return;

	for( int i = 0; i < GL3_TEXTURE_UNITS; i++ )
	{
		if( gl3_bound[i] == glnum )
			R_GL3BindUnit( i, 0 );
	}
}

// something bound textures behind the cache's back (a framebuffer's setup): every unit binds anew
void R_GL3ForgetBindings( void )
{
	for( int i = 0; i < GL3_TEXTURE_UNITS; i++ )
		gl3_bound[i] = (GLuint)-1;
	gl3_active_unit = -1;
}

static void R_GL3ReleaseTexture( gl3_texture_t *tex )
{
	if( !tex->glnum )
		return;

	R_GL3UnbindTexture( tex->glnum );
	glDeleteTextures( 1, &tex->glnum );
	tex->glnum = 0;
	memset( tex->applied, 0, sizeof( tex->applied ));
}

// an image without IMAGE_HAS_ALPHA is opaque: OpenGL uploads it without an alpha channel, whatever the data holds
static void R_GL3Row( const byte *src, int width, pixformat_t type, qboolean opaque )
{
	R_GL3ConvertRow( gl3_row, src, width, type );

	if( opaque )
	{
		for( int x = 0; x < width; x++ )
			gl3_row[x] |= 0xff000000u;
	}
}

// the texture is bound to unit 0, and the level already allocated by R_GL3Upload
static qboolean R_GL3WriteLevel( gl3_texture_t *tex, int level, const byte *src, int width, int height, pixformat_t type, qboolean opaque )
{
	int bpp = R_GL3BytesPerPixel( type );

	for( int y = 0; y < height; y++ )
	{
		if( src )
			R_GL3Row( src + y * width * bpp, width, type, opaque );
		else memset( gl3_row, 0, width * sizeof( *gl3_row ));

		glTexSubImage2D( GL_TEXTURE_2D, level, 0, y, width, 1, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, gl3_row );
	}

	return true;
}

/*
==================
R_GL3Upload

Creates (or re-creates, when the size changes) the texture object and fills every level.
==================
*/
static qboolean R_GL3Upload( gl3_texture_t *tex, rgbdata_t *pic )
{
	int width = pic->width, height = pic->height;
	int levels = ( pic->buffer && !FBitSet( tex->flags, TF_NOMIPMAP )) ? R_GL3MipCount( width, height ) : 1;
	pixformat_t type = (pixformat_t)pic->type;
	const byte *src = pic->buffer;
	qboolean opaque = !FBitSet( pic->flags, IMAGE_HAS_ALPHA );

	if( !ImageRAW( type ))
	{
		gEngfuncs.Con_Printf( S_ERROR "%s: %s has unsupported format %d\n", __func__, tex->name, type );
		return false;
	}

	if( FBitSet( pic->flags, IMAGE_CUBEMAP | IMAGE_MULTILAYER ) || pic->depth > 1 || pic->numMips > 1 )
	{
		gEngfuncs.Con_Printf( S_ERROR "%s: %s is a cubemap, a layered image or has mipmaps, not supported yet\n", __func__, tex->name );
		return false;
	}

	if( width > GL3_MAX_TEXTURE_SIZE || height > GL3_MAX_TEXTURE_SIZE )
	{
		gEngfuncs.Con_Printf( S_ERROR "%s: %s is %dx%d, larger than %d\n", __func__, tex->name, width, height, GL3_MAX_TEXTURE_SIZE );
		return false;
	}

	if( tex->glnum && ( tex->width != width || tex->height != height || tex->numMips != levels ))
		R_GL3ReleaseTexture( tex );

	if( !tex->glnum )
	{
		glGenTextures( 1, &tex->glnum );
		R_GL3BindUnit( 0, tex->glnum );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, levels - 1 );

		// every level's storage first: the rows go in with glTexSubImage2D
		for( int level = 0; level < levels; level++ )
		{
			glTexImage2D( GL_TEXTURE_2D, level, GL_RGBA8, Q_max( 1, width >> level ), Q_max( 1, height >> level ),
				0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, NULL );
		}
	}
	else R_GL3BindUnit( 0, tex->glnum );

	tex->srcWidth = tex->width = width;
	tex->srcHeight = tex->height = height;
	memcpy( tex->fogParams, pic->fogParams, sizeof( tex->fogParams ));
	tex->numMips = levels;
	tex->size = 0;

	// mipmaps and the alpha filter work on an RGBA copy
	if( src && ( levels > 1 || FBitSet( pic->flags, IMAGE_ONEBIT_ALPHA )) && !FBitSet( tex->flags, TF_NOMIPMAP ))
	{
		size_t needed = (size_t)width * height * 4;
		int bpp = R_GL3BytesPerPixel( type );

		if( needed > gl3_mip_buffer_size )
		{
			gl3_mip_buffer = (byte *)Mem_Realloc( gl3_image_pool, gl3_mip_buffer, needed );
			gl3_mip_buffer_size = needed;
		}

		for( int y = 0; y < height; y++ )
		{
			byte *out = gl3_mip_buffer + y * width * 4;

			R_GL3Row( src + y * width * bpp, width, type, opaque );
			for( int x = 0; x < width; x++, out += 4 )
			{
				out[0] = ( gl3_row[x] >> 16 ) & 0xff;
				out[1] = ( gl3_row[x] >> 8 ) & 0xff;
				out[2] = gl3_row[x] & 0xff;
				out[3] = gl3_row[x] >> 24;
			}
		}

		if( FBitSet( pic->flags, IMAGE_ONEBIT_ALPHA ))
			R_GL3ApplyFilter( gl3_mip_buffer, width, height );

		src = gl3_mip_buffer;
		type = PF_RGBA_32;
	}

	for( int level = 0; level < levels; level++ )
	{
		int w = Q_max( 1, width >> level );
		int h = Q_max( 1, height >> level );

		if( !R_GL3WriteLevel( tex, level, src, w, h, type, opaque ))
		{
			R_GL3ReleaseTexture( tex );
			return false;
		}

		tex->size += w * h * 4;
		if( level + 1 < levels )
			R_GL3BuildMipMap( gl3_mip_buffer, w, h );
	}

	SetBits( tex->flags, TF_IMG_UPLOADED );
	return true;
}

// gl_image.c GL_ProcessImage without compressed formats
static void R_GL3ProcessImage( gl3_texture_t *tex, rgbdata_t *pic )
{
	uint img_flags = 0;

	if( FBitSet( tex->flags, TF_FORCE_COLOR ))
		SetBits( pic->flags, IMAGE_HAS_COLOR );
	if( FBitSet( pic->flags, IMAGE_HAS_ALPHA ))
		SetBits( tex->flags, TF_HAS_ALPHA );
	if( FBitSet( pic->flags, IMAGE_PREMULTIPLIED ))
		SetBits( tex->flags, TF_PREMULTIPLIED );
	if( FBitSet( pic->flags, IMAGE_HAS_LUMA ))
		SetBits( tex->flags, TF_HAS_LUMA );
	if( FBitSet( pic->flags, IMAGE_QUAKEPAL ))
		SetBits( tex->flags, TF_QUAKEPAL );

	if( FBitSet( tex->flags, TF_MAKELUMA ))
	{
		SetBits( img_flags, IMAGE_MAKE_LUMA );
		ClearBits( tex->flags, TF_MAKELUMA );
	}

	if( !FBitSet( tex->flags, TF_IMG_UPLOADED ) && FBitSet( tex->flags, TF_KEEP_SOURCE ))
		tex->original = gEngfuncs.FS_CopyImage( pic ); // the pic is expanded to RGBA below

	if( pic->type == PF_INDEXED_24 || pic->type == PF_INDEXED_32 )
		SetBits( img_flags, IMAGE_FORCE_RGBA );

	if( pic->buffer )
		gEngfuncs.Image_Process( &pic, 0, 0, img_flags, 0 );

	if( FBitSet( pic->flags, IMAGE_PLAYERDECAL ) && pic->buffer && pic->type == PF_RGBA_32
		&& FBitSet( pic->flags, IMAGE_HAS_ALPHA ) && !FBitSet( pic->flags, IMAGE_PREMULTIPLIED ))
	{
		byte *p = pic->buffer;

		for( int i = 0; i < pic->width * pic->height; i++, p += 4 )
		{
			p[0] = ( p[0] * p[3] + 127 ) / 255;
			p[1] = ( p[1] * p[3] + 127 ) / 255;
			p[2] = ( p[2] * p[3] + 127 ) / 255;
		}
		SetBits( pic->flags, IMAGE_PREMULTIPLIED );
		SetBits( tex->flags, TF_PREMULTIPLIED );
	}
}

static qboolean R_GL3CheckTexName( const char *name )
{
	if( COM_StringEmptyOrNULL( name ))
		return false;

	if( Q_strlen( name ) >= sizeof( gl3_textures->name ))
	{
		gEngfuncs.Con_Printf( S_ERROR "LoadTexture: too long name %s\n", name );
		return false;
	}

	return true;
}

static gl3_texture_t *R_GL3TextureForName( const char *name )
{
	for( gl3_texture_t *tex = gl3_textures_hash[COM_HashKey( name, GL3_TEXTURES_HASH_SIZE )]; tex; tex = tex->nextHash )
	{
		if( !Q_stricmp( tex->name, name ))
			return tex;
	}

	return NULL;
}

static gl3_texture_t *R_GL3AllocTexture( const char *name, uint flags )
{
	gl3_texture_t *tex = NULL;

	for( int i = 1; i < GL3_MAX_TEXTURES; i++ )
	{
		if( !gl3_textures[i].name[0] )
		{
			tex = &gl3_textures[i];
			break;
		}
	}

	if( !tex )
	{
		gEngfuncs.Host_Error( "%s: out of texture slots (%d)\n", __func__, GL3_MAX_TEXTURES );
		return NULL;
	}

	memset( tex, 0, sizeof( *tex ));
	Q_strncpy( tex->name, name, sizeof( tex->name ));
	tex->flags = flags;
	tex->xscale = tex->yscale = 1.0f;
	gl3_num_textures = Q_max( gl3_num_textures, ( tex - gl3_textures ) + 1 );

	tex->hashValue = COM_HashKey( name, GL3_TEXTURES_HASH_SIZE );
	tex->nextHash = gl3_textures_hash[tex->hashValue];
	gl3_textures_hash[tex->hashValue] = tex;
	return tex;
}

// a slot for a texture made elsewhere (the scene target); freeing the slot deletes it
int R_GL3WrapTexture( const char *name, GLuint glnum, int width, int height, uint flags )
{
	gl3_texture_t *tex = R_GL3AllocTexture( name, flags );

	if( !tex )
		return 0;

	tex->glnum = glnum;
	tex->srcWidth = tex->width = width;
	tex->srcHeight = tex->height = height;
	tex->numMips = 1;
	tex->size = width * height * 4;
	return tex - gl3_textures;
}

static void R_GL3DeleteTexture( gl3_texture_t *tex )
{
	gl3_texture_t **prev;

	if( !tex->name[0] || tex == gl3_textures )
		return;

	for( prev = &gl3_textures_hash[tex->hashValue]; *prev; prev = &( *prev )->nextHash )
	{
		if( *prev == tex )
		{
			*prev = tex->nextHash;
			break;
		}
	}

	// a texture uploaded again carries a new size, and the budget must not run below zero: an unsigned
	// wrap there would read as an enormous cache and stop it keeping anything at all
	if( tex->kept )
		gl3_kept_textures -= Q_min( gl3_kept_textures, tex->size );

	R_GL3ReleaseTexture( tex );
	if( tex->original )
		gEngfuncs.FS_FreeImage( tex->original );
	memset( tex, 0, sizeof( *tex ));
}

// a failed upload leaves no half-made slot behind
static int R_GL3FinishLoad( gl3_texture_t *tex, rgbdata_t *pic )
{
	R_GL3ProcessImage( tex, pic );

	if( !R_GL3Upload( tex, pic ))
	{
		R_GL3DeleteTexture( tex );
		return 0;
	}

	return tex - gl3_textures;
}

int R_GL3LoadTexture( const char *name, const byte *buf, size_t size, int flags )
{
	gl3_texture_t *tex;
	uint pic_flags = 0;
	rgbdata_t *pic;
	int texnum;

	if( !R_GL3CheckTexName( name ))
		return 0;

	if(( tex = R_GL3TextureForName( name )))
	{
		gEngfuncs.Image_ClearForceFlags(); // the caller's flags were meant for this load
		return tex - gl3_textures;
	}

	if( FBitSet( flags, TF_NOFLIP_TGA ))
		SetBits( pic_flags, IL_DONTFLIP_TGA );
	if( FBitSet( flags, TF_KEEP_SOURCE ) && !FBitSet( flags, TF_EXPAND_SOURCE ))
		SetBits( pic_flags, IL_KEEP_8BIT );
	gEngfuncs.Image_SetForceFlags( pic_flags );

	pic = gEngfuncs.FS_LoadImage( name, buf, size );
	if( !pic )
		return 0;

	tex = R_GL3AllocTexture( name, flags );
	texnum = R_GL3FinishLoad( tex, pic );
	gEngfuncs.FS_FreeImage( pic );
	return texnum;
}

int R_GL3LoadTextureFromBuffer( const char *name, rgbdata_t *pic, texFlags_t flags, qboolean update )
{
	gl3_texture_t *tex;

	if( !R_GL3CheckTexName( name ))
		return 0;

	tex = R_GL3TextureForName( name );
	if( tex && !update )
		return tex - gl3_textures;

	if( !pic )
		return 0;

	if( update )
	{
		if( !tex )
			gEngfuncs.Host_Error( "%s: couldn't find texture %s for update\n", __func__, name );
		SetBits( tex->flags, flags );
	}
	else tex = R_GL3AllocTexture( name, flags );

	return R_GL3FinishLoad( tex, pic );
}

int R_GL3CreateTexture( const char *name, int width, int height, const void *buffer, texFlags_t texflags )
{
	uint flags = texflags;
	qboolean update = FBitSet( flags, TF_UPDATE ) ? true : false;
	rgbdata_t pic;

	ClearBits( flags, TF_UPDATE | TF_TEXTURE_3D );

	if( FBitSet( flags, TF_CUBEMAP | TF_ARB_16BIT | TF_ARB_FLOAT ))
	{
		gEngfuncs.Con_Printf( S_ERROR "%s: %s needs a cubemap or a float texture, not supported yet\n", __func__, name );
		return 0;
	}

	memset( &pic, 0, sizeof( pic ));
	pic.width = width;
	pic.height = height;
	pic.depth = 1;
	pic.type = PF_RGBA_32;
	pic.size = width * height * 4;
	pic.buffer = (byte *)buffer;

	if( !FBitSet( flags, TF_LUMINANCE ) && !FBitSet( flags, TF_ALPHACONTRAST ))
		SetBits( pic.flags, IMAGE_HAS_COLOR );
	if( FBitSet( flags, TF_HAS_ALPHA ))
		SetBits( pic.flags, IMAGE_HAS_ALPHA );

	int texnum = R_GL3LoadTextureFromBuffer( name, &pic, (texFlags_t)flags, update );

	if( !Q_strcmp( name, REF_WHITE_TEXTURE ))
		gl3_white_texture = texnum;
	else if( !Q_strcmp( name, REF_PARTICLE_TEXTURE ))
		gl3_particle_texture = texnum;
	return texnum;
}

void R_GL3UpdateTexture( int texnum, int cols, int rows, int width, int height, const byte *buffer, pixformat_t fmt )
{
	gl3_texture_t *tex = R_GL3GetTexture( texnum );
	rgbdata_t pic, *copy;

	if( !tex->name[0] || !ImageRAW( fmt ))
	{
		gEngfuncs.Con_DPrintf( S_ERROR "%s: can't update texture %d with format %d\n", __func__, texnum, fmt );
		return;
	}

	memset( &pic, 0, sizeof( pic ));
	pic.width = cols;
	pic.height = rows;
	pic.depth = 1;
	pic.type = fmt;
	pic.size = cols * rows * R_GL3BytesPerPixel( fmt );
	pic.buffer = (byte *)buffer;

	if( cols == width && rows == height )
	{
		R_GL3Upload( tex, &pic );
		return;
	}

	copy = gEngfuncs.FS_CopyImage( &pic );
	gEngfuncs.Image_Process( &copy, width, height, IMAGE_RESAMPLE, 0 );
	R_GL3Upload( tex, copy );
	gEngfuncs.FS_FreeImage( copy );
}

int R_GL3FindTexture( const char *name )
{
	gl3_texture_t *tex;

	if( !R_GL3CheckTexName( name ) || !( tex = R_GL3TextureForName( name )))
		return 0;

	return tex - gl3_textures;
}

/*
==================
R_GL3KeepTextureForNextMap

The map that loaded this texture is going away. True when the texture stays behind for the maps after it:
only the ones read out of a WAD, which every map shares, and only while they fit the budget.
==================
*/
qboolean R_GL3KeepTextureForNextMap( unsigned int texnum )
{
	gl3_texture_t *tex;

	if( texnum == 0 || texnum >= GL3_MAX_TEXTURES )
		return false;

	tex = &gl3_textures[texnum];

	if( !tex->name[0] || !Q_strstr( tex->name, ".wad/" ))
		return false;

	if( tex->kept )
		return true; // already paid for

	if( gl3_kept_textures + tex->size > GL3_KEPT_TEXTURE_BUDGET )
		return false;

	gl3_kept_textures += tex->size;
	tex->kept = true;
	return true;
}

void R_GL3FreeTexture( unsigned int texnum )
{
	if( texnum > 0 && texnum < GL3_MAX_TEXTURES )
		R_GL3DeleteTexture( &gl3_textures[texnum] );
}

void R_GL3ProcessTexture( int texnum, float gamma, int topColor, int bottomColor )
{
	gl3_texture_t *tex;
	rgbdata_t *pic;
	int flags;

	if( texnum <= 0 || texnum >= GL3_MAX_TEXTURES )
		return;
	tex = &gl3_textures[texnum];

	if( gamma != -1.0f )
		flags = IMAGE_LIGHTGAMMA;
	else if( topColor != -1 && bottomColor != -1 )
		flags = IMAGE_REMAP;
	else
	{
		gEngfuncs.Con_Printf( S_ERROR "%s: bad operation for %s\n", __func__, tex->name );
		return;
	}

	if( !tex->original )
	{
		gEngfuncs.Con_Printf( S_ERROR "%s: no input data for %s\n", __func__, tex->name );
		return;
	}

	pic = gEngfuncs.FS_CopyImage( tex->original );
	if( pic->type == PF_INDEXED_24 || pic->type == PF_INDEXED_32 )
		SetBits( flags, IMAGE_FORCE_RGBA );

	gEngfuncs.Image_Process( &pic, topColor, bottomColor, flags, 0.0f );
	R_GL3Upload( tex, pic );
	gEngfuncs.FS_FreeImage( pic );
}

const char *R_GL3TextureName( unsigned int texnum )
{
	return R_GL3GetTexture( texnum )->name;
}

const byte *R_GL3TextureData( unsigned int texnum )
{
	rgbdata_t *original = R_GL3GetTexture( texnum )->original;

	return original ? original->buffer : NULL;
}

// ref_gl's GL_TextureFilteringEnabled: gl_texture_nearest, the 25th update's "texture filtering" switched
// off, points the mipmapped textures and those that allow it (sky, fonts); lightmaps stay filtered
static qboolean R_GL3Filtered( const gl3_texture_t *tex )
{
	if( FBitSet( tex->flags, TF_NEAREST ))
		return false;

	if( FBitSet( tex->flags, TF_DEPTHMAP ))
		return true;

	if(( FBitSet( tex->flags, TF_NOMIPMAP ) || tex->numMips <= 1 ) && !FBitSet( tex->flags, TF_ALLOW_NEAREST ))
		return true;

	return gl_texture_nearest->value == 0.0f;
}

/*
==================
R_GL3ApplySampler

Filtering, addressing and the mip cap of the texture bound to the active unit, set only when they differ
from what the texture object already holds.
==================
*/
static void R_GL3ApplySampler( gl3_texture_t *tex, qboolean filtered, qboolean clamp, int mipcap, int anisotropy )
{
	const byte want[4] = { (byte)( filtered + 1 ), (byte)( clamp + 1 ), (byte)( mipcap + 1 ), (byte)( anisotropy + 1 ) };

	if( !memcmp( tex->applied, want, sizeof( want )))
		return;

	if( tex->applied[0] != want[0] )
	{
		const GLenum mag = filtered ? GL_LINEAR : GL_NEAREST;
		GLenum min = mag;

		if( tex->numMips > 1 )
			min = filtered ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST;

		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mag );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, min );
	}

	if( tex->applied[1] != want[1] )
	{
		const GLenum wrap = clamp ? GL_CLAMP_TO_EDGE : GL_REPEAT;

		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap );
	}

	if( tex->applied[2] != want[2] )
		glTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_MAX_LOD, (float)mipcap );

	if( tex->applied[3] != want[3] && gl3_max_anisotropy > 0.0f )
		glTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, (float)Q_max( 1, anisotropy ));

	memcpy( tex->applied, want, sizeof( want ));
}

/*
==================
R_GL3BindTexture

Binds texture unit 0 with the texture's filtering and addressing. An empty or unknown slot binds *white
(the caller's colour still shows), as ref_gl binds its default texture.
==================
*/
void R_GL3BindTexture( int texnum )
{
	gl3_texture_t *tex = ( texnum > 0 && texnum < GL3_MAX_TEXTURES ) ? &gl3_textures[texnum] : NULL;
	qboolean filtered;
	int anisotropy = 1;

	if( !tex || !tex->glnum )
		tex = &gl3_textures[gl3_white_texture];

	// a BSP miptex carries four mip levels and the software renderer read exactly those (d_mipcap and
	// friends in D_SURF.C); ours builds the whole chain, so that style stops the sampler at level 3
	const int mipcap = R_GL3Style( ) == GL3_STYLE_SOFTWARE ? 3 : 15;

	filtered = R_GL3Filtered( tex );

	// ref_gl sets gl_anisotropy on every mipmapped texture
	if( tex->numMips > 1 && filtered && gl3_max_anisotropy > 0.0f )
		anisotropy = (int)bound( 1.0f, gl_anisotropy->value, gl3_max_anisotropy );

	R_GL3BindUnit( 0, tex->glnum );
	R_GL3ActiveUnit( 0 );
	R_GL3ApplySampler( tex, filtered, FBitSet( tex->flags, TF_CLAMP | TF_BORDER ) ? true : false, mipcap, anisotropy );
}

// unit 1: lightmaps, filtered, clamped, one level (0: *white)
void R_GL3BindLightmap( GLuint glnum )
{
	if( !glnum )
		glnum = gl3_textures[gl3_white_texture].glnum;

	R_GL3BindUnit( 1, glnum );
}

// an extra unit's texture with its own sampling
static void R_GL3BindExtra( int unit, int texnum, qboolean filtered, qboolean clamp )
{
	gl3_texture_t *tex = R_GL3GetTexture( texnum );

	R_GL3BindUnit( unit, tex->glnum );
	R_GL3ActiveUnit( unit );
	R_GL3ApplySampler( tex, filtered, clamp, 15, 1 );
}

// unit 2: the ripple field of the software water, tiled. The span renderer read one value per cell,
// which is what makes its water blocky, so the software style samples it point. Ours reads the field
// between cells instead - the same wave, without the grid showing.
void R_GL3BindRipple( int texnum )
{
	R_GL3BindExtra( 2, texnum, R_GL3Style( ) != GL3_STYLE_SOFTWARE, false );
}

// unit 3: the flashlight's cookie, filtered and clamped so the cone fades into its black edge
void R_GL3BindCookie( int texnum )
{
	R_GL3BindExtra( 3, texnum, true, true );
}

// unit 4: the flashlight's shadow map, point sampled - its depth is packed across three channels,
// which any filtering would blend into nonsense
void R_GL3BindShadowMap( int texnum )
{
	R_GL3BindExtra( 4, texnum, false, true );
}

qboolean R_GL3TextureFilteringEnabled( int texnum )
{
	if( texnum < 0 )
		return true;

	return R_GL3Filtered( R_GL3GetTexture( texnum ));
}

static void R_GL3TextureStats_f( void )
{
	int count = 0;
	size_t bytes = 0;

	qboolean list = gEngfuncs.Cmd_Argc() > 1;

	for( int i = 1; i < gl3_num_textures; i++ )
	{
		const gl3_texture_t *tex = &gl3_textures[i];

		if( !tex->glnum )
			continue;
		count++;
		bytes += tex->size;
		if( list )
			gEngfuncs.Con_Printf( "%4d %4dx%-4d mips %2d flags %08x %s\n", i, tex->width, tex->height, tex->numMips, tex->flags, tex->name );
	}

	gEngfuncs.Con_Printf( "ref_gl3: %d textures, %u KB\n", count, (uint)( bytes >> 10 ));
}

void R_GL3InitImages( void )
{
	memset( gl3_textures, 0, sizeof( gl3_textures ));
	memset( gl3_textures_hash, 0, sizeof( gl3_textures_hash ));
	gl3_kept_textures = 0;
	Q_strncpy( gl3_textures->name, "*unused*", sizeof( gl3_textures->name ));
	gl3_num_textures = 1;
	gl3_image_pool = Mem_AllocPool( "ref_gl3 images" );
	gl_anisotropy = gEngfuncs.Cvar_Get( "gl_anisotropy", "8", FCVAR_ARCHIVE, "textures anisotropic filter" );
	gl_texture_nearest = gEngfuncs.Cvar_Get( "gl_texture_nearest", "0", FCVAR_ARCHIVE, "disable texture filter" );
	memset( gl3_bound, 0, sizeof( gl3_bound ));
	gl3_active_unit = -1;
	R_GL3ActiveUnit( 0 );

	// anisotropic filtering is an extension in 3.3, present in every driver that matters
	gl3_max_anisotropy = 0.0f;
	{
		GLint count = 0;

		glGetIntegerv( GL_NUM_EXTENSIONS, &count );
		for( int i = 0; i < count; i++ )
		{
			const char *ext = (const char *)glGetStringi( GL_EXTENSIONS, i );

			if( ext && ( !Q_strcmp( ext, "GL_EXT_texture_filter_anisotropic" ) || !Q_strcmp( ext, "GL_ARB_texture_filter_anisotropic" )))
			{
				glGetFloatv( GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &gl3_max_anisotropy );
				break;
			}
		}
	}

	gEngfuncs.Cmd_AddCommand( "r_texstats", R_GL3TextureStats_f, "log the number and memory of textures; with any argument, every texture" );
}

void R_GL3ShutdownImages( void )
{
	gEngfuncs.Cmd_RemoveCommand( "r_texstats" );

	for( int i = 1; i < gl3_num_textures; i++ )
		R_GL3DeleteTexture( &gl3_textures[i] );

	gl3_num_textures = 0;
	gl3_white_texture = 0;
	gl3_particle_texture = 0;
	gl3_mip_buffer = NULL;
	gl3_mip_buffer_size = 0;
	Mem_FreePool( &gl3_image_pool );
}
