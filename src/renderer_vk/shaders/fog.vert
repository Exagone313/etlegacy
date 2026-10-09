#version 450
// Ported from Quake3e (code/renderervk/shaders), GPLv2+
// Copyright (C) Artem Kharytoniuk (Quake-III-Arena-Kenny-Edition)
// Copyright (C) 2016 Eugene (Quake3e)
// Part of ET: Legacy, licensed under the GNU GPL version 3 or later

// 64 bytes
layout(push_constant) uniform Transform {
	mat4 mvp;
};

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

layout(location = 0) in vec3 in_position;

layout(location = 4) out vec2 fog_tex_coord;
layout(location = 5) out float fog_depth;

out gl_PerVertex {
	vec4 gl_Position;
};

// ET fog volume pass, same as RB_CalcFogTexCoords():
// fogEyeT.x is added to t, the eye depth in the fog when the eye is inside,
// or 1.0 with a zero fogDepthVector for the level-wide fog
// lightPos holds the eye-space depth vector of the global distance fog
void main() {
	gl_Position = mvp * vec4(in_position, 1.0);

	float s = dot(in_position, fogDistanceVector.xyz) + fogDistanceVector.w;
	float t = dot(in_position, fogDepthVector.xyz) + fogDepthVector.w + fogEyeT.x;

	fog_tex_coord = vec2(s, t);

	fog_depth = abs(dot(in_position, lightPos.xyz) + lightPos.w);
}
