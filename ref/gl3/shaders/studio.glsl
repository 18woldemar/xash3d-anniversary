// Studio models: one bone per vertex, skinned here, lit per vertex as ref_gl's R_StudioLighting and
// R_LightLambert do (gl_studio.c), including their byte truncations and gamma table lookups.

#ifdef VERTEX_SHADER
// 0..3 world to clip; 4 eye plane; 5 ambient, shade, flat shade (0/1), fullbright (0/1); 6 light colour, a the
// vertex alpha; 7 light direction, w 1 = chrome texcoords; 8 chrome origin, w 1 = colour is the blend (TransAdd);
// 9 view right, w blend; 10 1 / texture size, w 1 = local lights; 11..14 local lights: origin, R^2;
// 15..18 their colours through the linear gamma table (0..1023); 20..211 3x4 bone matrices, three rows each
uniform vec4 vsc[212];

// rows: light gamma, linear gamma, screen gamma
uniform usampler2D s_gamma;

in vec4 a_position; // bone space
in vec3 a_normal;   // bone space
in vec2 a_texcoord; // texels
in float a_extra;   // index into the bones

out vec4 v_color;
out vec2 v_texcoord;
out float v_fogz;

float gamma_table( int row, float index )
{
	return float( texelFetch( s_gamma, ivec2( int( index ), row ), 0 ).r );
}

void main()
{
	int b = int( a_extra ) * 3;
	vec3 world = vec3( dot( vsc[20 + b], a_position ), dot( vsc[21 + b], a_position ), dot( vsc[22 + b], a_position ));
	vec3 normal = vec3( dot( vsc[20 + b].xyz, a_normal ), dot( vsc[21 + b].xyz, a_normal ), dot( vsc[22 + b].xyz, a_normal ));
	vec4 w = vec4( world, 1.0 );
	vec3 lv;
	vec3 rgb;

	gl_Position = vec4( dot( vsc[0], w ), dot( vsc[1], w ), dot( vsc[2], w ), dot( vsc[3], w ));
	v_fogz = dot( vsc[4], w );

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
}
#endif

#ifdef FRAGMENT_SHADER
uniform vec4 psc[32]; // 1 rgb fog colour, a 1 / fog end (0: none); 31 x alpha ref
uniform sampler2D s_texture;

in vec4 v_color;
in vec2 v_texcoord;
in float v_fogz;

out vec4 o_color;

void main()
{
	vec4 c = texture( s_texture, v_texcoord ) * v_color;

	if( c.a * 255.0 <= psc[31].x )
		discard;

	c.rgb = mix( psc[1].rgb, c.rgb, clamp( 1.0 - abs( v_fogz ) * psc[1].a, 0.0, 1.0 ));
	o_color = c;
}
#endif
