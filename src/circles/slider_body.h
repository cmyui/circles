#pragma once

// Slider bodies as osu!stable draws them (SliderOsu.Draw with
// TexturedSliderRendererGL). Stable merges the path's lines into pieces,
// draws each piece as a quad with only as much of a round cap at its end as
// its turn into the next piece needs, and shades them by depth into a texture
// that it redraws as the body snakes in. On unusual paths the pieces leave
// stable's gaps and notches, which this keeps.
//
// The pixels are stable's; the work isn't. Pieces are worked out once per
// slider and drawn by the GPU from one record each, and a body's texture
// keeps the pieces that can no longer change, with their depth, so a frame
// redraws at most the few at the snaking end. A body with nothing left to
// snake costs one textured quad.

#include <circles/gfx.h>
#include <circles/math.h>
#include <circles/types.h>

#include <OpenGL/gl3.h>
#include <array>
#include <cstddef>
#include <fosu/slider_path.h>
#include <utility>
#include <vector>

namespace circles {

struct App;

// One straight line of stable's slider path, in playfield coordinates.
struct BodyLine {
  Vec2 p1, p2;
  u8   flags = 0;  // fosu::StableLineFlags
};

// What a slider's body is drawn from.
struct BodyShape {
  std::vector<BodyLine> lines;
  std::vector<f64>      lengths;     // along the path to each line's end
  f64                   length = 0;  // stable's SpatialLength
  std::vector<u32>      piece_ends;  // each piece's last line, fully snaked
};

// `path` is a stable-mode path, relative to `head`, the slider's stacked
// position; `declared_length` is its length in the .osu file.
BodyShape build_body_shape(const fosu::SliderPath& path,
                           Vec2                    head,
                           f64                     declared_length);

// Lines `first` to `last` merged into one; the piece at the snaking end is
// cut to `cut` px from its start when that's positive.
struct BodyPiece {
  u32 first = 0;
  u32 last = 0;
  f32 cut = 0;
};

// The pieces stable draws with part of the body showing: the first `full`
// of the fully snaked body's pieces, then up to two at the snaking end.
struct SnakeState {
  size_t                   full = 0;
  std::array<BodyPiece, 2> tail{};
  u32                      tail_count = 0;
  bool                     done = false;  // fully snaked

  size_t count() const { return full + tail_count; }
  // Pieces that further snaking can't change. A piece's cap depends on the
  // next piece, so the last of the full ones waits for the tail to settle.
  size_t settled() const { return done ? count() : full ? full - 1 : 0; }
};

// `snake` is stable's snaking progress: the fraction of the body showing,
// up to 1.
SnakeState snake_state(const BodyShape& shape, f32 snake);
BodyPiece piece_at(const BodyShape& shape, const SnakeState& state, size_t i);

// Stable's FieldToDisplay: playfield coordinates to framebuffer pixels.
struct DisplayTransform {
  f32  ratio;
  Vec2 offset;

  Vec2 operator()(Vec2 p) const {
    return {p.x * ratio + offset.x, p.y * ratio + offset.y};
  }
  bool operator==(const DisplayTransform&) const = default;
};

// Stable sizes its drawing area to a body's bounds, which the GPU clamps to
// its largest viewport: a bigger body is squashed toward its top left by
// `scale`, from `origin`, in framebuffer pixels.
struct BodySquash {
  Vec2 origin{0, 0};
  Vec2 scale{1, 1};
  bool operator==(const BodySquash&) const = default;
};

// A piece as the GPU draws it, in framebuffer pixels.
struct PieceInstance {
  Vec2 p1, p2;
  f32  cos, sin;   // of its angle
  f32  rho;        // its length
  u8   end_cap;    // triangles of the cap at its end, 24 for a half circle
  u8   mirrored;   // turning the other way, so the cap is flipped
  u8   start_cap;  // a whole half circle at its start, after a gap
  u8   padding = 0;
};

// A colour target with a 16-bit depth buffer, as stable draws bodies into.
struct BodyTarget {
  GLuint framebuffer = 0;
  GLuint colour = 0;
  GLuint depth = 0;
  i32    width = 0;
  i32    height = 0;
};

// What a visible slider's body texture holds.
struct BodyCache {
  size_t           object = 0;
  bool             used = false;  // drawn this frame
  BodyTarget       target;
  BodyTarget       scratch;       // private snaking image when batching sliders
  i32              x = 0, y = 0;  // the target's place in the framebuffer
  size_t           settled = 0;   // pieces drawn into it
  bool             squashable = false;
  // What the drawn pieces depend on.
  DisplayTransform display{};
  f32              radius = 0;
  i32              screen_width = 0, screen_height = 0;  // pixels
  BodySquash       squash{};
  GLuint           gradient = 0;
};

// The bodies of a play's visible sliders. release_slider_bodies frees them.
struct SliderBodies {
  std::vector<BodyCache> caches;
};

struct SliderBodyRenderer {
  GLuint                                             program = 0;
  GLuint                                             vao = 0;
  GLuint                                             instances = 0;
  GLint                                              radius = -1;
  GLint                                              region = -1;
  GLint                                              squash = -1;
  // Gradients across the body, by colour, border and antialiasing width.
  std::vector<std::pair<std::array<u8, 12>, GLuint>> gradients;
  // Where a snaking body is finished, one body at a time.
  BodyTarget                                         scratch;
  std::vector<PieceInstance>                         batch;
};

SliderBodyRenderer create_slider_body_renderer();

// Draws object `index`'s body in its combo `colour` with `snake` of it
// showing (stable's snaking progress), over the playfield whose origin is
// `origin` and scale `scale` (window points), moved by `shift` points, at
// `alpha`.
void draw_slider_body(App&   app,
                      size_t index,
                      Color  colour,
                      f32    snake,
                      f32    scale,
                      Vec2   origin,
                      Vec2   shift,
                      f32    alpha);
// Frees the bodies not drawn since the last call.
void sweep_slider_bodies(App& app);
void release_slider_bodies(SliderBodies& bodies);

}  // namespace circles
