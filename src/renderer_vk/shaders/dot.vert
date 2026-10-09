#version 450
// Ported from Quake3e (code/renderervk/shaders), GPLv2+
// Copyright (C) Artem Kharytoniuk (Quake-III-Arena-Kenny-Edition)
// Copyright (C) 2016 Eugene (Quake3e)
// Part of ET: Legacy, licensed under the GNU GPL version 3 or later

// 128 bytes
layout(push_constant) uniform Transform {
	mat4 mvp;
};

layout(set = 0, binding = 0) buffer SSBO {
	int sampled;
};

layout(location = 0) in vec3 in_position;

out gl_PerVertex {
	vec4 gl_Position;
	float gl_PointSize;
};

void main() {
	sampled = 0;
	gl_Position = mvp * vec4(in_position, 1.0);
	gl_PointSize = 1.0;
}
