#version 410 core

// The body's colour across it, from MmSliderRendererGL's gradient.

in float v_across;

uniform sampler2D u_gradient;

out vec4 frag_color;

void main() {
  frag_color = texture(u_gradient, vec2(v_across, 0.5));
}
