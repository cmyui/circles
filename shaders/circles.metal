#include <metal_stdlib>
using namespace metal;

// ---------------------------------------------------------------------------
// Sprites: shapes, images and text in window points, as gfx.cc batches them.

// Per vertex: position (window points: origin top-left, y down), texture or
// shape-local coordinates, colour, then the shape kind and its parameters.
struct SpriteVertex {
  packed_float2 position;
  packed_float2 uv;
  packed_float4 color;
  packed_float4 params;
};

struct SpriteFragment {
  float4 position [[position]];
  float2 uv;
  float4 color;
  float4 params [[flat]];
};

vertex SpriteFragment sprite_vertex(uint id [[vertex_id]],
                                    const device SpriteVertex* vertices [[buffer(0)]],
                                    constant float2& screen_size [[buffer(1)]]) {
  const SpriteVertex v = vertices[id];
  const float2 clip = float2(v.position) / screen_size * 2.0 - 1.0;
  SpriteFragment out;
  out.position = float4(clip.x, -clip.y, 0.0, 1.0);
  out.uv = v.uv;
  out.color = v.color;
  out.params = v.params;
  return out;
}

// Must match Kind in gfx.cc.
constant int KIND_TEXTURE = 1;
constant int KIND_GLYPH = 2;
constant int KIND_CIRCLE = 3;
constant int KIND_ROUNDED_RECT = 4;
constant int KIND_ARC = 5;
constant int KIND_SPOTLIGHT = 6;

constant float TAU = 6.28318531;
// Glyph atlases store 128/255 exactly on the outline (kSdfOnEdge in gfx.cc);
// higher is inside.
constant float GLYPH_ON_EDGE = 128.0 / 255.0;

// Opacity near a shape's edge from the signed distance to it (negative
// inside) and the size of a screen pixel in the same units: a smooth edge one
// pixel wide at any scale.
float coverage(float dist, float pixel) {
  return clamp(0.5 - dist / pixel, 0.0, 1.0);
}

// The batch's textures; textured kinds name theirs in params.w.
float4 sample_texture(array<texture2d<float>, 8> textures, sampler s, float slot, float2 uv) {
  switch (int(slot + 0.5)) {
    case 0: return textures[0].sample(s, uv);
    case 1: return textures[1].sample(s, uv);
    case 2: return textures[2].sample(s, uv);
    case 3: return textures[3].sample(s, uv);
    case 4: return textures[4].sample(s, uv);
    case 5: return textures[5].sample(s, uv);
    case 6: return textures[6].sample(s, uv);
    default: return textures[7].sample(s, uv);
  }
}

fragment float4 sprite_fragment(SpriteFragment in [[stage_in]],
                                array<texture2d<float>, 8> textures [[texture(0)]],
                                sampler s [[sampler(0)]]) {
  const int kind = int(in.params.x + 0.5);
  float alpha = 1.0;

  if (kind == KIND_TEXTURE) {
    return sample_texture(textures, s, in.params.w, in.uv) * in.color;
  } else if (kind == KIND_GLYPH) {
    // Parameters: dilation (moves the edge outward), extra softness.
    const float dist = sample_texture(textures, s, in.params.w, in.uv).r;
    const float edge = GLYPH_ON_EDGE - in.params.y;
    const float half_width = max(0.5 * fwidth(dist), 1e-4) + in.params.z;
    alpha = smoothstep(edge - half_width, edge + half_width, dist);
  } else if (kind == KIND_CIRCLE || kind == KIND_ARC) {
    // uv spans [-1, 1] across the circle. Parameters: inner radius as a
    // fraction of the radius, then softness (circles) or progress (arcs).
    const float r = length(in.uv);
    const float pixel = max(fwidth(r), 1e-6);
    if (kind == KIND_CIRCLE && in.params.z > 0.0) {
      alpha = 1.0 - smoothstep(1.0 - in.params.z, 1.0, r);
    } else {
      alpha = coverage(r - 1.0, pixel);
    }
    if (in.params.y > 0.0) {
      alpha *= 1.0 - coverage(r - in.params.y, pixel);
    }
    if (kind == KIND_ARC && in.params.z < 1.0) {
      // Angle clockwise from 12 o'clock (uv's y points down), in turns.
      float turn = atan2(in.uv.x, -in.uv.y) / TAU;
      if (turn < 0.0) {
        turn += 1.0;
      }
      alpha *= coverage((turn - in.params.z) * TAU * r, pixel);
    }
  } else if (kind == KIND_SPOTLIGHT) {
    // uv spans [-1, 1] across the square. Parameters: the hole's radius and
    // its edge's width, as fractions of the half width.
    alpha = smoothstep(in.params.y, in.params.y + in.params.z, length(in.uv));
  } else if (kind == KIND_ROUNDED_RECT) {
    // uv is the offset from the centre in points. Parameters: half width,
    // half height, corner radius.
    const float2 q = abs(in.uv) - in.params.yz + in.params.w;
    const float dist = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - in.params.w;
    alpha = coverage(dist, max(fwidth(dist), 1e-6));
  }

  return float4(in.color.rgb, in.color.a * alpha);
}

// ---------------------------------------------------------------------------
// Slider bodies: one piece per instance, as osu!stable's
// TexturedSliderRendererGL draws it: a quad along the piece, then the
// triangles of a round cap at its end and, after a gap, at its start. Depth
// runs from 0 on the centre line to 0.5 at the edge, so with the depth test
// the nearest piece shades each pixel.

struct Piece {
  packed_float2 p1;  // framebuffer pixels
  packed_float2 p2;
  float cos, sin, rho;  // its angle's cosine and sine, its length
  // Triangles of the end cap, whether it's mirrored, whether there's a start
  // cap.
  uchar end_cap, mirrored, start_cap, padding;
};

struct PieceUniforms {
  float4 region;  // the framebuffer area drawn: x, y, width, height
  float4 squash;  // origin xy, scale zw; see BodySquash
  float radius;   // pixels
};

struct PieceFragment {
  float4 position [[position]];
  float across;  // 0 at the edge to nearly 1 on the centre line
};

constant int kCap = 24;
// The quad's corners: x along the piece, y across it, z toward the centre.
constant float3 kCorners[6] = {
    float3(-0.0003, -1.0, 0.0), float3(1.0003, -1.0, 0.0),
    float3(1.0003, 0.0001, 1.0), float3(-0.0003, 0.0001, 1.0),
    float3(-0.0003, 1.0, 0.0), float3(1.0003, 1.0, 0.0)};
// Its two quads, each split in two.
constant int kQuad[12] = {0, 1, 2, 0, 2, 3, 2, 3, 4, 2, 4, 5};

// XNA's Vector3.Transform by scale * rotation * translation, an operation at
// a time: exact arithmetic, no fused multiply-adds.
#pragma METAL fp math_mode(safe)
#pragma METAL fp contract(off)
float2 place(float2 local, float2 scale, float2 at, float3 line) {
  const float x = local.x * (scale.x * line.x) + local.y * (scale.y * -line.y) + at.x;
  const float y = local.x * (scale.x * line.y) + local.y * (scale.y * line.x) + at.y;
  return float2(x, y);
}
// The rest of the placement keeps its rounding too: no fused multiply-adds.
#pragma METAL fp math_mode(fast)
#pragma METAL fp contract(off)

vertex PieceFragment piece_vertex(uint v [[vertex_id]],
                                  uint instance [[instance_id]],
                                  const device Piece* pieces [[buffer(0)]],
                                  constant PieceUniforms& u [[buffer(1)]],
                                  constant float2* rim [[buffer(2)]]) {
  const Piece p = pieces[instance];
  const float3 line = float3(p.cos, p.sin, p.rho);
  PieceFragment out;
  float2 position;
  float z;
  if (v < 12) {
    const float3 corner = kCorners[kQuad[v]];
    position = place(corner.xy, float2(line.z, u.radius), float2(p.p1), line);
    z = corner.z;
  } else {
    const bool end = v < 12 + 3 * kCap;
    const int k = int(v) - (end ? 12 : 12 + 3 * kCap);
    const int triangle = k / 3;
    const int corner = k % 3;
    const int count = end ? int(p.end_cap) : (p.start_cap != 0 ? kCap : 0);
    if (triangle >= count) {
      out.position = float4(2.0, 2.0, 2.0, 1.0);  // no triangle
      out.across = 0.0;
      return out;
    }
    // A fan from the centre, around the rim.
    const float2 local = corner == 0 ? float2(0.0) : rim[triangle + corner - 1];
    const float2 scale = end ? float2(u.radius, p.mirrored != 0 ? -u.radius : u.radius)
                             : float2(-u.radius);
    position = place(local, scale, end ? float2(p.p2) : float2(p.p1), line);
    z = corner == 0 ? 1.0 : 0.0;
  }
  if (any(u.squash.zw != float2(1.0)))
    position = u.squash.xy + (position - u.squash.xy) * u.squash.zw;
  const float2 ndc = (position - u.region.xy) / u.region.zw * 2.0 - 1.0;
  // The target's first row is the region's top, so y flips. Depth is what
  // stable's OpenGL renderer stores for z: 0 on the centre line, 0.5 at the
  // edge.
  out.position = float4(ndc.x, -ndc.y, -z * 0.5 + 0.5, 1.0);
  out.across = z * (1.0 - 1.0 / 128.0);
  return out;
}

// The body's colour across it, from MmSliderRendererGL's gradient.
fragment float4 piece_fragment(PieceFragment in [[stage_in]],
                               texture2d<float> gradient [[texture(0)]],
                               sampler s [[sampler(0)]]) {
  return gradient.sample(s, float2(in.across, 0.5));
}
