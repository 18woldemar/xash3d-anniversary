// The flashlight's shadow map: the geometry drawn from the light, keeping the distance along its cone in a float
// target, which keeps the compare in the lighting shader simple. Defines: STUDIO, skinned by the bones.

#ifdef VERTEX_SHADER
VS_REGISTERS( 216 ) // world: 8..11 object to the light's clip space; studio: 20..211 bones, 212..215 world to it

ATTR( vec4, a_position, 0 )
#ifdef STUDIO
ATTR( float, a_extra, 6 )
#endif

out float v_depth; // the clip w, which is the distance along the cone

void main()
{
#ifdef STUDIO
	int b = int( a_extra ) * 3;
	vec4 w = vec4( dot( vsc[20 + b], a_position ), dot( vsc[21 + b], a_position ), dot( vsc[22 + b], a_position ), 1.0 );

	gl_Position = vec4( dot( vsc[212], w ), dot( vsc[213], w ), dot( vsc[214], w ), dot( vsc[215], w ));
#else
	gl_Position = vec4( dot( vsc[8], a_position ), dot( vsc[9], a_position ), dot( vsc[10], a_position ), dot( vsc[11], a_position ));
#endif
	v_depth = gl_Position.w;
	VK_FIX_CLIP();
}
#endif

#ifdef FRAGMENT_SHADER
PS_REGISTERS( 1 ) // x: 1 / the cone's far distance

in float v_depth;

out vec4 o_color;

void main()
{
	o_color = vec4( clamp( v_depth * psc[0].x, 0.0, 1.0 ), 0.0, 0.0, 1.0 );
}
#endif
