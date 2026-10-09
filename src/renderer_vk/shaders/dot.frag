#version 450
// Ported from Quake3e (code/renderervk/shaders), GPLv2+
// Copyright (C) Artem Kharytoniuk (Quake-III-Arena-Kenny-Edition)
// Copyright (C) 2016 Eugene (Quake3e)
// Part of ET: Legacy, licensed under the GNU GPL version 3 or later

layout(set = 0, binding = 0) buffer SSBO {
	int sampled;
};

layout(location = 0) out vec4 out_color;
layout(early_fragment_tests) in; // force Early Fragment Tests

void main() {
	//atomicAdd( sampled, 1 );
	sampled = 1;
	discard;
	//out_color = vec4( 0.0, 1.0, 0.0, 1.0 );
}
