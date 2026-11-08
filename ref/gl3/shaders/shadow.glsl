// The flashlight's shadow map: the geometry drawn from the light, keeping the distance along its cone in a float
// target, which keeps the compare in the lighting shader simple. Defines: STUDIO, skinned by the bones.

#ifdef VERTEX_SHADER
uniform vec4 vsc[216]; // world: 8..11 object to the light's clip space; studio: 20..211 bones, 212..215 world to it

in vec4 a_position;
in float a_extra;

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
}
#endif

#ifdef FRAGMENT_SHADER
uniform vec4 psc[1]; // x: 1 / the cone's far distance

in float v_depth;

out vec4 o_color;

void main()
{
	o_color = vec4( clamp( v_depth * psc[0].x, 0.0, 1.0 ), 0.0, 0.0, 1.0 );
}
#endif
