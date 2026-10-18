#version 410 core

// A slider body is drawn as overlapping circles along its path. Each fragment
// writes its distance from its circle's centre as depth, so with the depth
// test the nearest circle wins every pixel: the result is shaded by distance
// from the path itself, like a tube.

in vec2 v_local;  // [-1, 1] across this circle

uniform vec4 u_body_colour;
uniform vec4 u_border_colour;
uniform float u_border_width;  // fraction of the radius
uniform float u_alpha;

out vec4 frag_color;

void main() {
  float r = length(v_local);
  if (r > 1.0) {
    discard;
  }
  gl_FragDepth = r;

  float pixel = max(fwidth(r), 1e-6);
  float border_start = 1.0 - u_border_width;
  // Darker along the spine, brighter toward the edge.
  vec4 body = vec4(u_body_colour.rgb * mix(0.75, 1.3, r / border_start),
                   u_body_colour.a);
  vec4 colour = mix(body, u_border_colour,
                    clamp((r - border_start) / pixel + 0.5, 0.0, 1.0));
  float edge = clamp((1.0 - r) / pixel + 0.5, 0.0, 1.0);
  frag_color = vec4(colour.rgb, colour.a * u_alpha * edge);
}
