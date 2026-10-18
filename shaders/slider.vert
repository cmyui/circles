#version 410 core

layout(location = 0) in vec2 a_corner;  // [-1, 1]: a corner of this circle's quad
layout(location = 1) in vec2 a_center;  // per instance: a path point in window points

uniform vec2 u_screen_size;  // window size in points
uniform float u_radius;      // in points

out vec2 v_local;

void main() {
  vec2 position = a_center + a_corner * u_radius;
  vec2 clip = position / u_screen_size * 2.0 - 1.0;
  gl_Position = vec4(clip.x, -clip.y, 0.0, 1.0);
  v_local = a_corner;
}
