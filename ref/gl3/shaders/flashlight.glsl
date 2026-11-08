// The flashlight's cookie and its shadow map, and the filter that reads the map. The world shader and the
// studio shader bind the same two samplers, so this is put in front of both by R_GL3CreateSpotProgram.

#ifdef FRAGMENT_SHADER
uniform sampler2D s_cookie;
uniform sampler2D s_shadow; // the distance along the cone, in the red channel

#define GL3_SHADOW_ATTEN 0.35 // a shadow is never black: shadowatten lifts it, as Source has it

// Four point taps half a texel apart, weighted back together by hand: a 2x2 percentage-closer filter. *white is
// bound when there is no map, and its 1.0 reads as "nothing in the way". The caller passes the bias, in the map's
// own 0..1 units, because the two callers are owed different amounts: the world stands in the map exactly as it is
// drawn, while a model's silhouette in there is one frame old.
float gl3_shadowed( vec2 uv, float depth, float bias )
{
	const float size = 512.0;
	const float texel = 1.0 / size;
	vec2 f = fract( uv * size - 0.5 );
	vec2 base = uv - 0.5 * texel;
	vec4 taps = vec4( texture( s_shadow, base ).r, texture( s_shadow, base + vec2( texel, 0.0 )).r,
		texture( s_shadow, base + vec2( 0.0, texel )).r, texture( s_shadow, base + vec2( texel, texel )).r );
	vec4 lit = step( vec4( depth - bias ), taps );

	return mix( mix( lit.x, lit.y, f.x ), mix( lit.z, lit.w, f.x ), f.y );
}
#endif
