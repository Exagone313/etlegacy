#version 450
// Ported from Quake3e (code/renderervk/shaders), GPLv2+
// Copyright (C) Artem Kharytoniuk (Quake-III-Arena-Kenny-Edition)
// Copyright (C) 2016 Eugene (Quake3e)
// Part of ET: Legacy, licensed under the GNU GPL version 3 or later

layout(set = 0, binding = 0) uniform UBO {
	// light/env parameters:
	vec4 eyePos;				// vertex
	vec4 lightPos;				// vertex: light origin
	vec4 lightColor;			// fragment: rgb + 1/(r*r)
	vec4 lightVector;			// fragment: linear dynamic light
//#ifdef USE_FOG
	// fog parameters:
	vec4 fogDistanceVector;		// vertex
	vec4 fogDepthVector;		// vertex
	vec4 fogEyeT;				// vertex
	vec4 fogColor;				// fragment
//#endif
};

layout(set = 2, binding = 0) uniform sampler2D fog_texture;

layout(location = 4) in vec2 fog_tex_coord;
layout(location = 5) in float fog_depth;

layout(location = 0) out vec4 out_color;

// ET fog volume pass: lightColor and lightVector hold the global distance fog color and
// its parameters (end, 1/(end-start), density, mode: 0 none, 1 GL_LINEAR, 2 GL_EXP)
void main() {
	vec4 color = texture(fog_texture, fog_tex_coord) * fogColor;

	if ( lightVector.w != 0.0 ) {
		float fog_factor;
		if ( lightVector.w == 2.0 ) {
			fog_factor = exp( -lightVector.z * fog_depth );
		} else {
			fog_factor = ( lightVector.x - fog_depth ) * lightVector.y;
		}
		color.rgb = mix( lightColor.rgb, color.rgb, clamp( fog_factor, 0.0, 1.0 ) );
	}

	out_color = color;
}
