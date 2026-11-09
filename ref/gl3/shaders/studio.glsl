// Studio models: one bone per vertex, skinned here, lit per vertex as ref_gl's R_StudioLighting and
// R_LightLambert do (gl_studio.c), including their byte truncations and gamma table lookups.
// Defines: SPOT, the flashlight's cone on top.

#ifdef VERTEX_SHADER
// 0..3 world to clip; 4 eye plane; 5 ambient, shade, flat shade (0/1), fullbright (0/1); 6 light colour, a the
// vertex alpha; 7 light direction, w 1 = chrome texcoords; 8 chrome origin, w 1 = colour is the blend (TransAdd);
// 9 view right, w blend; 10 1 / texture size, z how far the glow shell stands out, w 1 = local lights; 11..14 local lights: origin, R^2;
// 15..18 their colours through the linear gamma table (0..1023); 20..211 3x4 bone matrices, three rows each;
// 212..215 world to the flashlight's clip space
VS_REGISTERS( 216 )

// rows: light gamma, linear gamma, screen gamma
USAMPLER( s_gamma, 5 )

ATTR( vec4, a_position, 0 ) // bone space
ATTR( vec3, a_normal, 4 )   // bone space
ATTR( vec2, a_texcoord, 2 ) // texels
ATTR( float, a_extra, 6 )   // index into the bones

out vec4 v_color;
out vec2 v_texcoord;
out float v_fogz;
#ifdef SPOT
out vec4 v_spot;
out vec3 v_wpos;
out vec3 v_nrm;
#endif

float gamma_table( int row, float index )
{
	return float( texelFetch( s_gamma, ivec2( int( index ), row ), 0 ).r );
}

void main()
{
	int b = int( a_extra ) * 3;
	vec3 world = vec3( dot( vsc[20 + b], a_position ), dot( vsc[21 + b], a_position ), dot( vsc[22 + b], a_position ));
	vec3 normal = vec3( dot( vsc[20 + b].xyz, a_normal ), dot( vsc[21 + b].xyz, a_normal ), dot( vsc[22 + b].xyz, a_normal ));
	world += normal * vsc[10].z; // the glow shell
	vec4 w = vec4( world, 1.0 );
	vec3 lv;
	vec3 rgb;

	gl_Position = vec4( dot( vsc[0], w ), dot( vsc[1], w ), dot( vsc[2], w ), dot( vsc[3], w ));
	v_fogz = dot( vsc[4], w );
#ifdef SPOT
	// Source lights a model by the cone with a plain Lambert term; the bones give the normal here and the
	// fragment stage has the lamp
	v_spot = vec4( dot( vsc[212], w ), dot( vsc[213], w ), dot( vsc[214], w ), dot( vsc[215], w ));
	v_wpos = world;
	v_nrm = normal;
#endif

	// R_StudioLighting
	if( vsc[8].w > 0.5 )
	{
		lv = vsc[9].www; // TransAdd: the blend, unlit
	}
	else if( vsc[5].w > 0.5 )
	{
		lv = vsc[6].rgb;
	}
	else
	{
		float illum = vsc[5].x;

		if( vsc[5].z > 0.5 )
		{
			illum += vsc[5].y * 0.8;
		}
		else
		{
			float c = ( min( dot( normal, vsc[7].xyz ), 1.0 ) + 0.4953241 ) / 1.4953241;
			illum += vsc[5].y;
			if( c > 0.0 )
				illum -= vsc[5].y * c;
			illum = max( illum, 0.0 );
		}

		illum = min( illum, 255.0 );
		lv = vsc[6].rgb * ( gamma_table( 0, floor( illum * 4.0 )) / 1023.0 );
	}

	// R_LightLambert: local lights add a scalar in linear space
	rgb = floor( lv * 255.0 );
	if( vsc[10].w > 0.5 )
	{
		vec3 total = vec3( 0.0 );

		for( int l = 0; l < 4; l++ )
		{
			vec3 d = vsc[11 + l].xyz - world;
			float r = dot( normal, d );

			if( r > 0.0 && vsc[11 + l].w > 0.0 )
			{
				float r2 = dot( d, d );
				float scale = r2 > 0.0 ? vsc[11 + l].w / ( r2 * sqrt( r2 )) : 0.0001;
				total += vsc[15 + l].rgb + r * scale;
			}
		}

		if( any( notEqual( total, vec3( 0.0 ))))
		{
			for( int c = 0; c < 3; c++ )
			{
				float v = total[c] + gamma_table( 1, floor( lv[c] * 1023.0 ));
				rgb[c] = v > 1023.0 ? 255.0 : floor( gamma_table( 2, floor( v )) / 4.0 );
			}
		}
	}

	v_color = vec4( rgb / 255.0, vsc[6].a );
	if( vsc[10].z > 0.0 )
		v_color = vec4( vsc[6].rgb, 1.0 ); // the glow shell: the entity's colour, unlit

	// R_StudioSetupChrome
	if( vsc[7].w > 0.5 )
	{
		vec3 origin = vec3( vsc[20 + b].w, vsc[21 + b].w, vsc[22 + b].w );
		vec3 dir = normalize( origin - vsc[8].xyz );
		vec3 up = normalize( cross( dir, vsc[9].xyz ));
		vec3 right = normalize( cross( up, dir ));

		v_texcoord = vec2(( dot( normal, right ) + 1.0 ) * 32.0, ( dot( normal, up ) + 1.0 ) * 32.0 ) * vsc[10].xy;
	}
	else
	{
		v_texcoord = a_texcoord * vsc[10].xy;
	}

	VK_FIX_CLIP();
}
#endif

#ifdef FRAGMENT_SHADER
// 1 rgb fog colour, a 1 / fog end (0: none); 2 rgb the flashlight, a its linear attenuation;
// 3 xyz where the lamp hangs, w 1 / its range; 31 x alpha ref
PS_REGISTERS( 32 )
SAMPLER( s_texture, 0 )

in vec4 v_color;
in vec2 v_texcoord;
in float v_fogz;
#ifdef SPOT
in vec4 v_spot;
in vec3 v_wpos;
in vec3 v_nrm;
#endif

out vec4 o_color;

void main()
{
	vec4 skin = texture( s_texture, v_texcoord );
	vec4 c = skin * v_color;
	float fog = clamp( 1.0 - abs( v_fogz ) * psc[1].a, 0.0, 1.0 );

	if( c.a * 255.0 <= psc[31].x )
		discard;

	c.rgb = mix( psc[1].rgb, c.rgb, fog );

#ifdef SPOT
	// the cookie read through the projection and falling off with the distance along it, lighting the model's own
	// skin. The map holds the world of this frame and the models of the one before it, since posing a model twice
	// would move its animation on: the bias has to cover how far a model moved, and 0.02 of the range is 15 units.
	vec2 uv = v_spot.xy / v_spot.w * 0.5 + 0.5;

	if( v_spot.w > 0.0 && all( equal( clamp( uv, 0.0, 1.0 ), uv )))
	{
		vec3 tolamp = psc[3].xyz - v_wpos;
		float d = length( tolamp );
		float att = clamp( psc[2].a / d, 0.0, 1.0 );
		float endfade = clamp(( 1.0 - d * psc[3].a ) * 2.5, 0.0, 1.0 );
		float ndotl = clamp( dot( normalize( v_nrm ), tolamp / d ), 0.0, 1.0 );
		float lit = gl3_shadowed( uv, v_spot.w * psc[3].a, 0.02 + 0.01 * ( 1.0 - ndotl ));

		lit = clamp( mix( mix( lit, 1.0, GL3_SHADOW_ATTEN ), lit, att ), 0.0, 1.0 );
		c.rgb += skin.rgb * texture( s_cookie, uv ).rgb * psc[2].rgb * att * endfade * ndotl * lit * fog;
	}
#endif

	o_color = c;
}
#endif
