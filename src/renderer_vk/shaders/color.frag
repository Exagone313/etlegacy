#version 450
// Ported from Quake3e (code/renderervk/shaders), GPLv2+
// Copyright (C) Artem Kharytoniuk (Quake-III-Arena-Kenny-Edition)
// Copyright (C) 2016 Eugene (Quake3e)
// Part of ET: Legacy, licensed under the GNU GPL version 3 or later

layout(location = 0) out vec4 out_color;

layout (constant_id = 4) const int color_mode = 0;

void main()
{
	if ( color_mode == 1 )
		out_color = vec4( 1.0, 1.0, 1.0, 1.0 ); // white
	else
	if ( color_mode == 2 )
		out_color = vec4( 0.2, 1.0, 0.2, 1.0 ); // green
	else
	if ( color_mode == 3 )
		out_color = vec4( 1.0, 0.33, 0.2, 1.0 ); // red
	else
		out_color = vec4( 0.0, 0.0, 0.0, 1.0 ); // black
}
