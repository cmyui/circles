#include <metal_stdlib>
using namespace metal;
#pragma clang fp contract(off)

// Explicit scalar/packed layout matches the C++ streams without float3 padding.
struct SpriteVertex {
  float2 position;
  float2 uv;
  float4 color;
  float4 params;
};
struct SpriteOut {
  float4 position [[position]];
  float2 uv;
  float4 color;
  float4 params [[flat]];
};
vertex SpriteOut sprite_vertex(uint id [[vertex_id]],
                                const device SpriteVertex* vertices [[buffer(0)]],
                                constant float2& screen [[buffer(1)]]) {
  SpriteVertex v = vertices[id];
  float2 clip = v.position / screen * 2.0f - 1.0f;
  return {float4(clip.x, -clip.y, 0, 1), v.uv, v.color, v.params};
}

float coverage(float dist, float pixel) {
  return clamp(0.5f - dist / pixel, 0.0f, 1.0f);
}
fragment float4 sprite_fragment(SpriteOut v [[stage_in]],
                                array<texture2d<float>, 8> textures [[texture(0)]]) {
  constexpr sampler linear_filter(coord::normalized, address::clamp_to_edge,
                                   filter::linear, mip_filter::linear);
  int kind = int(v.params.x + 0.5f);
  float alpha = 1.0f;
  if (kind == 1 || kind == 2) {
    float4 texel = textures[uint(v.params.w + 0.5f)].sample(linear_filter, v.uv);
    if (kind == 1)
      return texel * v.color;
    float edge = 128.0f / 255.0f - v.params.y;
    float half_width = max(0.5f * fwidth(texel.r), 1e-4f) + v.params.z;
    alpha = smoothstep(edge - half_width, edge + half_width, texel.r);
  } else if (kind == 3 || kind == 5) {
    float r = length(v.uv);
    float pixel = max(fwidth(r), 1e-6f);
    alpha = kind == 3 && v.params.z > 0.0f
                ? 1.0f - smoothstep(1.0f - v.params.z, 1.0f, r)
                : coverage(r - 1.0f, pixel);
    if (v.params.y > 0.0f)
      alpha *= 1.0f - coverage(r - v.params.y, pixel);
    if (kind == 5 && v.params.z < 1.0f) {
      constexpr float tau = 6.28318531f;
      float turn = atan2(v.uv.x, -v.uv.y) / tau;
      if (turn < 0.0f) turn += 1.0f;
      alpha *= coverage((turn - v.params.z) * tau * r, pixel);
    }
  } else if (kind == 6) {
    alpha = smoothstep(v.params.y, v.params.y + v.params.z, length(v.uv));
  } else if (kind == 4) {
    float2 q = abs(v.uv) - v.params.yz + v.params.w;
    float dist = length(max(q, 0.0f)) + min(max(q.x, q.y), 0.0f) - v.params.w;
    alpha = coverage(dist, max(fwidth(dist), 1e-6f));
  }
  return float4(v.color.rgb, v.color.a * alpha);
}

struct Piece {
  float2 p1, p2;
  float cosine, sine, rho;
  uchar4 caps;
};
struct BodyParams {
  float4 region;
  float4 squash;
  float radius;
};
struct BodyOut {
  float4 position [[position]];
  float across;
};
// The compiler's fast math and multiply-add contraction are disabled: stable's
// path placement depends on the order and rounding of these operations.
float2 place(float2 local, float2 scale, float2 at, Piece p) {
  return float2(local.x * (scale.x * p.cosine) + local.y * (scale.y * -p.sine) + at.x,
                local.x * (scale.x * p.sine) + local.y * (scale.y * p.cosine) + at.y);
}
vertex BodyOut body_vertex(uint v [[vertex_id]], uint instance [[instance_id]],
                           const device Piece* pieces [[buffer(0)]],
                           constant BodyParams& u [[buffer(1)]],
                           constant float2* rim [[buffer(2)]]) {
  constexpr float3 corners[6] = {
      float3(-0.0003f, -1, 0), float3(1.0003f, -1, 0),
      float3(1.0003f, 0.0001f, 1), float3(-0.0003f, 0.0001f, 1),
      float3(-0.0003f, 1, 0), float3(1.0003f, 1, 0)};
  constexpr uint quad[12] = {0, 1, 2, 0, 2, 3, 2, 3, 4, 2, 4, 5};
  Piece p = pieces[instance];
  float2 position;
  float z;
  if (v < 12) {
    float3 corner = corners[quad[v]];
    position = place(corner.xy, float2(p.rho, u.radius), p.p1, p);
    z = corner.z;
  } else {
    bool end = v < 84;
    uint k = v - (end ? 12 : 84);
    uint triangle = k / 3, corner = k % 3;
    uint count = end ? p.caps.x : (p.caps.z != 0 ? 24 : 0);
    if (triangle >= count) return {float4(2, 2, 2, 1), 0};
    float2 local = corner == 0 ? float2(0) : rim[triangle + corner - 1];
    float2 scale = end ? float2(u.radius, p.caps.y != 0 ? -u.radius : u.radius)
                      : float2(-u.radius);
    position = place(local, scale, end ? p.p2 : p.p1, p);
    z = corner == 0 ? 1.0f : 0.0f;
  }
  if (any(u.squash.zw != float2(1)))
    position = u.squash.xy + (position - u.squash.xy) * u.squash.zw;
  float2 ndc = (position - u.region.xy) / u.region.zw * 2.0f - 1.0f;
  return {float4(ndc.x, -ndc.y, (1.0f - z) * 0.5f, 1),
          z * (1.0f - 1.0f / 128.0f)};
}
fragment float4 body_fragment(BodyOut v [[stage_in]],
                              texture2d<float> gradient [[texture(0)]]) {
  constexpr sampler linear_filter(coord::normalized, address::clamp_to_edge,
                                   filter::linear);
  return gradient.sample(linear_filter, float2(v.across, 0.5f));
}
