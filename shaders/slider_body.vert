#version 410 core

// One piece of a slider body per instance, as osu!stable's
// TexturedSliderRendererGL draws it: a quad along the piece, then the
// triangles of a round cap at its end and, after a gap, at its start. Depth
// runs from 0 on the centre line to 0.5 at the edge, so with the depth test
// the nearest piece shades each pixel.

layout(location = 0) in vec2 a_p1;  // per instance, framebuffer pixels
layout(location = 1) in vec2 a_p2;
layout(location = 2) in vec3 a_line;  // cos and sin of its angle, its length
// Triangles of the end cap, whether it's mirrored, whether there's a start cap.
layout(location = 3) in uvec4 a_caps;

uniform float u_radius;   // pixels
uniform vec2 u_rim[25];   // the cap's half circle, from (0, -1) to (0, 1)
uniform vec4 u_region;    // the framebuffer area drawn: x, y, width, height
uniform vec4 u_squash;    // origin xy, scale zw; see BodySquash

out float v_across;  // 0 at the edge to nearly 1 on the centre line

const int kCap = 24;
// The quad's corners: x along the piece, y across it, z toward the centre.
const vec3 kCorners[6] = vec3[](
    vec3(-0.0003, -1.0, 0.0), vec3(1.0003, -1.0, 0.0),
    vec3(1.0003, 0.0001, 1.0), vec3(-0.0003, 0.0001, 1.0),
    vec3(-0.0003, 1.0, 0.0), vec3(1.0003, 1.0, 0.0));
// Its two quads, each split in two.
const int kQuad[12] = int[](0, 1, 2, 0, 2, 3, 2, 3, 4, 2, 4, 5);

// XNA's Vector3.Transform by scale * rotation * translation, an operation at
// a time.
vec2 place(vec2 local, vec2 scale, vec2 at) {
  precise float x =
      local.x * (scale.x * a_line.x) + local.y * (scale.y * -a_line.y) + at.x;
  precise float y =
      local.x * (scale.x * a_line.y) + local.y * (scale.y * a_line.x) + at.y;
  return vec2(x, y);
}

void main() {
  int v = gl_VertexID;
  vec2 position;
  float z;
  if (v < 12) {
    vec3 corner = kCorners[kQuad[v]];
    position = place(corner.xy, vec2(a_line.z, u_radius), a_p1);
    z = corner.z;
  } else {
    bool end = v < 12 + 3 * kCap;
    int k = v - (end ? 12 : 12 + 3 * kCap);
    int triangle = k / 3;
    int corner = k % 3;
    int count = end ? int(a_caps.x) : (a_caps.z != 0u ? kCap : 0);
    if (triangle >= count) {
      gl_Position = vec4(2.0, 2.0, 2.0, 1.0);  // no triangle
      v_across = 0.0;
      return;
    }
    // A fan from the centre, around the rim.
    vec2 local = corner == 0 ? vec2(0.0) : u_rim[triangle + corner - 1];
    vec2 scale = end ? vec2(u_radius, a_caps.y != 0u ? -u_radius : u_radius)
                     : vec2(-u_radius);
    position = place(local, scale, end ? a_p2 : a_p1);
    z = corner == 0 ? 1.0 : 0.0;
  }
  if (u_squash.zw != vec2(1.0))
    position = u_squash.xy + (position - u_squash.xy) * u_squash.zw;
  vec2 ndc = (position - u_region.xy) / u_region.zw * 2.0 - 1.0;
  gl_Position = vec4(ndc, -z, 1.0);
  v_across = z * (1.0 - 1.0 / 128.0);
}
