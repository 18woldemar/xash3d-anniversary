// World and brush model surfaces: texture x colour x lightmap in one pass. The rows of the transform are
// separate registers. Fog is GoldSrc's: the texture pass and the lightmap pass that multiplies it are fogged
// separately (GL_LINEAR, start 0).
// Defines: WARP, ref_gl's EmitWaterPolys warp in the vertex stage; WARP_SW, the software renderer's water,
// which had no vertex warp and no sine at all: the texture itself is read through a ripple field.

#ifdef VERTEX_SHADER
uniform vec4 vsc[7]; // 0..3 the transform's rows, 4 xy the scrolling offset, 5 x time y wave height, 6 the eye plane

in vec4 a_position;
in vec2 a_texcoord;
in vec2 a_lightcoord;

out vec2 v_st;
out vec2 v_lm;
out float v_fogz;

#ifdef WARP
// GoldSrc's r_turbsin[(int)x & 255]: 8 * sin( 2 pi k / 256 ) repeats every 256, so the mask is not needed
float turbsin( float x )
{
	return 8.0 * sin( trunc( x ) * 0.0245436926 );
}
#endif

void main()
{
	vec4 p = a_position;

#if defined( WARP )
	const float turbscale = 256.0 / 6.28318531;
	float time = vsc[5].x;

	// wave height is zero for most water, which leaves z as it is
	float nv = turbsin( time * 160.0 + p.y + p.x ) + 8.0;
	nv = ( turbsin( p.x * 5.0 + time * 171.0 - p.y ) + 8.0 ) * 0.8 + nv;
	p.z = nv * vsc[5].y + p.z;

	v_st.x = ( a_texcoord.x + turbsin(( a_texcoord.y * 0.125 + time ) * turbscale )) / 64.0;
	v_st.y = ( a_texcoord.y + turbsin(( a_texcoord.x * 0.125 + time ) * turbscale )) / 64.0;
#elif defined( WARP_SW )
	v_st = a_texcoord / 64.0;
#else
	v_st = a_texcoord + vsc[4].xy;
#endif

	v_lm = a_lightcoord;
	v_fogz = dot( vsc[6], p );
	gl_Position = vec4( dot( vsc[0], p ), dot( vsc[1], p ), dot( vsc[2], p ), dot( vsc[3], p ));
}
#endif

#ifdef FRAGMENT_SHADER
// 0 colour; 1 rgb the texture term's fog, a 1 / fog end (0: no fog); 2 rgb the lightmap term's fog, a 1 when that
// term is fogged; 3 x the lightmap factor (R_GL3LightmapScale); 31 x alpha ref
uniform vec4 psc[32];
uniform sampler2D s_texture;
uniform sampler2D s_lightmap;
uniform sampler2D s_ripple;

in vec2 v_st;
in vec2 v_lm;
in float v_fogz;

out vec4 o_color;

void main()
{
#ifdef WARP_SW
	// the original moved a texel by the field's value over 16, x forward and y back, and wrapped inside the tile
	// (r_ripple.c fills the field, 128 cells over the tile). The offset jumps from cell to cell, so the texture is
	// read with the unwarped gradients: otherwise every jump picks a far mip level and the water turns to mush.
	float val = texture( s_ripple, v_st ).r * 255.0 - 128.0;
	vec4 c = textureGrad( s_texture, v_st + vec2( val, -val ) / 128.0, dFdx( v_st ), dFdy( v_st ));
#else
	vec4 c = texture( s_texture, v_st );
#endif

	float f = clamp( 1.0 - abs( v_fogz ) * psc[1].a, 0.0, 1.0 );
	float f2 = 1.0 - psc[2].a * ( 1.0 - f );

	c *= psc[0];
	c.rgb = mix( psc[1].rgb, c.rgb, f ) * mix( psc[2].rgb, texture( s_lightmap, v_lm ).rgb * psc[3].x, f2 );

	if( c.a * 255.0 <= psc[31].x )
		discard;

	o_color = c;
}
#endif
