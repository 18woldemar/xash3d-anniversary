/*
r_shader.c - ref_gl3: the shader constants
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
A shader file holds both stages, between #ifdef VERTEX_SHADER and #ifdef FRAGMENT_SHADER, and a program is
one file with a set of defines: the variants of a shader are its defines, not copies of it.
The constants are numbered registers, the way the renderer was first written: each program declares its "vsc"
and "psc" arrays and reads its values at fixed indices. The renderer writes them into one shared set here, as it
would into a device's registers, and the backend hands a program the set when it draws.
*/

#include "r_local.h"

float gl3_vsc[GL3_VS_CONSTANTS][4];
float gl3_psc[GL3_PS_CONSTANTS][4];
uint  gl3_vsc_serial = 1, gl3_psc_serial = 1;

// registers start..start+count of the vertex stage, as SetVertexShaderConstantF wrote them
void R_GL3SetVSConstants( int start, const float *data, int count )
{
	if( start < 0 || start + count > GL3_VS_CONSTANTS )
	{
		gEngfuncs.Con_Printf( S_ERROR "%s: registers %d..%d out of range\n", __func__, start, start + count - 1 );
		return;
	}

	if( !memcmp( gl3_vsc[start], data, count * sizeof( gl3_vsc[0] )))
		return;

	memcpy( gl3_vsc[start], data, count * sizeof( gl3_vsc[0] ));
	gl3_vsc_serial++;
}

void R_GL3SetPSConstants( int start, const float *data, int count )
{
	if( start < 0 || start + count > GL3_PS_CONSTANTS )
	{
		gEngfuncs.Con_Printf( S_ERROR "%s: registers %d..%d out of range\n", __func__, start, start + count - 1 );
		return;
	}

	if( !memcmp( gl3_psc[start], data, count * sizeof( gl3_psc[0] )))
		return;

	memcpy( gl3_psc[start], data, count * sizeof( gl3_psc[0] ));
	gl3_psc_serial++;
}
