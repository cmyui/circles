#version 410 core

in vec2 v_uv;
in vec4 v_color;
flat in vec4 v_params;

uniform sampler2D u_texture;

out vec4 frag_color;

// Must match Kind in gfx.cc.
const int KIND_SOLID = 0;
const int KIND_TEXTURE = 1;
const int KIND_GLYPH = 2;
const int KIND_CIRCLE = 3;
const int KIND_ROUNDED_RECT = 4;
const int KIND_ARC = 5;
const int KIND_SPOTLIGHT = 6;

const float TAU = 6.28318531;
// Glyph atlases store 128/255 exactly on the outline (kSdfOnEdge in gfx.cc);
// higher is inside.
const float GLYPH_ON_EDGE = 128.0 / 255.0;

// Opacity near a shape's edge from the signed distance to it (negative
// inside) and the size of a screen pixel in the same units: a smooth edge one
// pixel wide at any scale.
float coverage(float dist, float pixel) {
  return clamp(0.5 - dist / pixel, 0.0, 1.0);
}

void main() {
  int kind = int(v_params.x + 0.5);
  float alpha = 1.0;

  if (kind == KIND_TEXTURE) {
    frag_color = texture(u_texture, v_uv) * v_color;
    return;
  } else if (kind == KIND_GLYPH) {
    // Parameters: dilation (moves the edge outward), extra softness.
    float dist = texture(u_texture, v_uv).r;
    float edge = GLYPH_ON_EDGE - v_params.y;
    float half_width = max(0.5 * fwidth(dist), 1e-4) + v_params.z;
    alpha = smoothstep(edge - half_width, edge + half_width, dist);
  } else if (kind == KIND_CIRCLE || kind == KIND_ARC) {
    // v_uv spans [-1, 1] across the circle. Parameters: inner radius as a
    // fraction of the radius, then softness (circles) or progress (arcs).
    float r = length(v_uv);
    float pixel = max(fwidth(r), 1e-6);
    if (kind == KIND_CIRCLE && v_params.z > 0.0) {
      alpha = 1.0 - smoothstep(1.0 - v_params.z, 1.0, r);
    } else {
      alpha = coverage(r - 1.0, pixel);
    }
    if (v_params.y > 0.0) {
      alpha *= 1.0 - coverage(r - v_params.y, pixel);
    }
    if (kind == KIND_ARC && v_params.z < 1.0) {
      // Angle clockwise from 12 o'clock (v_uv's y points down), in turns.
      float turn = atan(v_uv.x, -v_uv.y) / TAU;
      if (turn < 0.0) {
        turn += 1.0;
      }
      alpha *= coverage((turn - v_params.z) * TAU * r, pixel);
    }
  } else if (kind == KIND_SPOTLIGHT) {
    // v_uv spans [-1, 1] across the square. Parameters: the hole's radius and
    // its edge's width, as fractions of the half width.
    alpha = smoothstep(v_params.y, v_params.y + v_params.z, length(v_uv));
  } else if (kind == KIND_ROUNDED_RECT) {
    // v_uv is the offset from the centre in points. Parameters: half width,
    // half height, corner radius.
    vec2 q = abs(v_uv) - v_params.yz + v_params.w;
    float dist = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - v_params.w;
    alpha = coverage(dist, max(fwidth(dist), 1e-6));
  }

  frag_color = vec4(v_color.rgb, v_color.a * alpha);
}
