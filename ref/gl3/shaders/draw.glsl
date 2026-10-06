// 2D and the triangle API: the vertex colour modulates one texture (untextured draws bind *white). The rows
// of the transform are separate registers, so the C side needs no matrix layout convention. Blending is
// state; the alpha test of the original is a discard here, with its reference in psc[31].x (every shader's). Fog is the
// original's linear underwater fog on the eye depth (the sky under water).
// Defines: WARP_SCREEN, the software renderer's 320x200 grid under water; WARP_WAVE, ours.

#ifdef VERTEX_SHADER
uniform vec4 vsc[5]; // 0..3 the transform's rows, 4 the eye plane

in vec4 a_position;
in vec4 a_color;
in vec2 a_texcoord;

out vec4 v_color;
out vec2 v_texcoord;
out float v_fogz;

void main()
{
	gl_Position = vec4( dot( vsc[0], a_position ), dot( vsc[1], a_position ), dot( vsc[2], a_position ), dot( vsc[3], a_position ));
	v_color = a_color;
	v_texcoord = a_texcoord;
	v_fogz = dot( vsc[4], a_position );
}
#endif

#ifdef FRAGMENT_SHADER
uniform vec4 psc[32]; // 0 fog: rgb colour, a 1 / fog end (0: none); 1 the warp: xy buffer size, z clock; 31 x alpha reference
uniform sampler2D s_texture;

in vec4 v_color;
in vec2 v_texcoord;
in float v_fogz;

out vec4 o_color;

void main()
{
	vec2 st = v_texcoord;

#if defined( WARP_SCREEN )
	// Under water the software renderer drew the 3D view into a small buffer (r_warpbuffer, vid.maxwarpwidth
	// by maxwarpheight) and stretched it back over the screen through its row and column tables - a plain
	// magnification, with no sine in it (D_SCAN.C D_WarpScreen). The scene is drawn at full size and read
	// back through the same grid, which gives the same blocks without a second pass over the geometry.
	st = ( floor( st * psc[1].xy ) + 0.5 ) / psc[1].xy;
#elif defined( WARP_WAVE )
	// Ours, and neither renderer's: the span renderer's idea of a moving view under water, at full
	// resolution. Two sine waves across each other, about ten screen pixels of travel, slow enough to read as
	// water rather than as a wobble.
	const vec2 amp = vec2( 0.008, 0.012 );
	const vec2 freq = vec2( 9.0, 14.0 );
	const vec2 speed = vec2( 1.1, 0.9 );
	st = clamp( st + sin( st.yx * freq + psc[1].z * speed ) * amp, 0.0, 1.0 );
#endif

	vec4 c = texture( s_texture, st ) * v_color;

	if( c.a * 255.0 <= psc[31].x )
		discard;

	c.rgb = mix( psc[0].rgb, c.rgb, clamp( 1.0 - abs( v_fogz ) * psc[0].a, 0.0, 1.0 ));
	o_color = c;
}
#endif
