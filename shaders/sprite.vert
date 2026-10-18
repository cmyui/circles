#version 410 core

layout(location = 0) in vec2 a_position;  // window points: origin top-left, y down
layout(location = 1) in vec2 a_uv;        // texture coordinates, or coordinates within the shape
layout(location = 2) in vec4 a_color;
layout(location = 3) in vec4 a_params;    // shape kind, then the kind's parameters

uniform vec2 u_screen_size;  // window size in points

out vec2 v_uv;
out vec4 v_color;
flat out vec4 v_params;

void main() {
  vec2 clip = a_position / u_screen_size * 2.0 - 1.0;
  gl_Position = vec4(clip.x, -clip.y, 0.0, 1.0);
  v_uv = a_uv;
  v_color = a_color;
  v_params = a_params;
}
