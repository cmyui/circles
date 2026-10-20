#include <circles/app.h>
#include <circles/slider_body.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace circles {

namespace {

constexpr f64 kPi = 3.141592653589793;
// The cap's half circle, in triangles.
constexpr u8  kCapTriangles = 24;
// Per piece: a quad of 4 triangles, then the end and start caps' triangles.
constexpr i32 kPieceVertices = 3 * (4 + 2 * kCapTriangles);
// Common GPUs' largest viewport, beyond which stable's bodies squash.
constexpr i32 kMaxViewport = 16384;

// .NET's (int) conversion: truncation, out-of-range values becoming
// int.MinValue.
i32 stable_int(f64 value) {
  return value > -2147483649.0 && value < 2147483648.0
             ? static_cast<i32>(value)
             : std::numeric_limits<i32>::min();
}

// XNA's Vector2.Distance and Length: single precision.
f32 distance(Vec2 a, Vec2 b) {
  const f32 x = a.x - b.x;
  const f32 y = a.y - b.y;
  return std::sqrt(x * x + y * y);
}

}  // namespace

BodyShape build_body_shape(const fosu::SliderPath& path,
                           Vec2                    head,
                           f64                     declared_length) {
  BodyShape  shape;
  const auto at = [&](size_t k) {
    return Vec2{head.x + path.points[k].x, head.y + path.points[k].y};
  };
  for (size_t k = 1; k < path.points.size(); ++k) {
    const u8 flags = path.stable_line_flags[k];
    if (flags & fosu::kStableLineStart)
      continue;  // a gap: no line ends here
    shape.lines.push_back({at(k - 1), at(k), flags});
    shape.lengths.push_back(path.cumulative_lengths[k]);
  }
  if (shape.lines.empty())
    return shape;
  shape.length = declared_length == 0 ? shape.lengths.back() : declared_length;

  // SliderOsu.Draw's merging with the whole body showing: a piece ends once
  // it spans more than 6 px (32 for straight lines), at a segment's end, and
  // at the last two lines.
  const auto  count = static_cast<u32>(shape.lines.size());
  u32         first = 0;
  bool        open = false;
  const auto& lines = shape.lines;
  for (u32 i = 0; i < count; ++i) {
    if (!open)
      first = i;
    const f32 limit = lines[i].flags & fosu::kStableLineStraight ? 32 : 6;
    open = !(distance(lines[first].p1, lines[i].p2) > limit ||
             lines[i].flags & fosu::kStableLineSegmentEnd || i + 2 >= count);
    if (!open)
      shape.piece_ends.push_back(i);
  }
  return shape;
}

SnakeState snake_state(const BodyShape& shape, f32 snake) {
  const auto count = static_cast<u32>(shape.lines.size());
  SnakeState state{.done = snake >= 1};
  // Lines to draw: through the first that ends past the length showing.
  const f64  showing = static_cast<f64>(snake) * shape.length;
  u32        lines = count;
  if (!state.done) {
    const auto past =
        std::upper_bound(shape.lengths.begin(), shape.lengths.end(), showing);
    if (past != shape.lengths.end())
      lines = static_cast<u32>(past - shape.lengths.begin()) + 1;
  }
  const f32 cut =
      state.done
          ? 0
          : static_cast<f32>(showing) -
                (lines == 1 ? 0.0f
                            : static_cast<f32>(shape.lengths[lines - 2]));
  // Pieces ending before the second-last line are the whole body's; the
  // second-last and last lines end pieces of their own.
  state.full = static_cast<size_t>(std::lower_bound(shape.piece_ends.begin(),
                                                    shape.piece_ends.end(),
                                                    static_cast<i64>(lines) - 2,
                                                    [](u32 end, i64 limit) {
                                                      return end < limit;
                                                    }) -
                                   shape.piece_ends.begin());
  if (lines >= 2) {
    const u32 first = state.full ? shape.piece_ends[state.full - 1] + 1 : 0;
    state.tail[state.tail_count++] = {first, lines - 2};
  }
  state.tail[state.tail_count++] = {lines - 1, lines - 1, std::max(cut, 0.0f)};
  return state;
}

BodyPiece piece_at(const BodyShape& shape, const SnakeState& state, size_t i) {
  if (i >= state.full)
    return state.tail[i - state.full];
  return {i ? shape.piece_ends[i - 1] + 1 : 0, shape.piece_ends[i]};
}

namespace {

struct DisplayLine {
  Vec2 p1, p2;
};

// A piece's line on screen; the snaking end's is cut in playfield
// coordinates first.
DisplayLine piece_line(const BodyShape&        shape,
                       const BodyPiece&        piece,
                       const DisplayTransform& display) {
  const Vec2 p1 = shape.lines[piece.first].p1;
  Vec2       p2 = shape.lines[piece.last].p2;
  if (piece.cut > 0) {
    // XNA's Vector2.Normalize.
    const Vec2 d = p2 - p1;
    const f32  inverse = 1.0f / std::sqrt(d.x * d.x + d.y * d.y);
    p2 = p1 + Vec2{d.x * inverse, d.y * inverse} * piece.cut;
  }
  return {display(p1), display(p2)};
}

f32 line_angle(const DisplayLine& line) {
  return static_cast<f32>(std::atan2(static_cast<f64>(line.p2.y - line.p1.y),
                                     static_cast<f64>(line.p2.x - line.p1.x)));
}

// TexturedSliderRendererGL.DrawLineOGL's choices for one piece.
PieceInstance make_instance(const DisplayLine* previous,
                            const DisplayLine& line,
                            const DisplayLine* next) {
  const f32     theta = line_angle(line);
  PieceInstance instance{
      .p1 = line.p1,
      .p2 = line.p2,
      .cos = static_cast<f32>(std::cos(static_cast<f64>(theta))),
      .sin = static_cast<f32>(std::sin(static_cast<f64>(theta))),
      .rho = distance(line.p2, line.p1),
      .end_cap = kCapTriangles,
      .mirrored = 0,
      .start_cap = !previous || !(line.p1 == previous->p2),
  };
  if (next) {
    // Only the outside of the turn into the next piece is capped.
    f32 turn = line_angle(*next) - theta;
    if (static_cast<f64>(turn) > kPi)
      turn -= 6.2831855f;
    if (static_cast<f64>(turn) < -kPi)
      turn += 6.2831855f;
    i32 triangles = 0;
    if (turn < 0) {
      instance.mirrored = 1;
      triangles = static_cast<i32>(
          std::ceil(-static_cast<f64>(turn) * 24.0f / kPi + 0.0));
    } else if (turn > 0) {
      triangles = static_cast<i32>(
          std::ceil(static_cast<f64>(turn * 24.0f) / kPi + 0.0));
    }
    instance.end_cap = static_cast<u8>(std::min<i32>(triangles, kCapTriangles));
  }
  return instance;
}

// Pieces [begin, end) of `state`, each line worked out once.
void add_instances(std::vector<PieceInstance>& out,
                   const BodyShape&            shape,
                   const SnakeState&           state,
                   size_t                      begin,
                   size_t                      end,
                   const DisplayTransform&     display) {
  if (begin >= end)
    return;
  const auto line = [&](size_t k) {
    return piece_line(shape, piece_at(shape, state, k), display);
  };
  DisplayLine previous = begin ? line(begin - 1) : DisplayLine{};
  DisplayLine current = line(begin);
  for (size_t i = begin; i < end; ++i) {
    const bool  has_next = i + 1 < state.count();
    DisplayLine next = has_next ? line(i + 1) : DisplayLine{};
    out.push_back(make_instance(i ? &previous : nullptr, current,
                                has_next ? &next : nullptr));
    previous = current;
    current = next;
  }
}

BodySquash body_squash(const BodyShape&        shape,
                       const SnakeState&       state,
                       const DisplayTransform& display,
                       f32                     radius,
                       bool&                   visible) {
  // SliderOsu.Draw's bounds: the first piece's start and each piece's end.
  const DisplayLine first =
      piece_line(shape, piece_at(shape, state, 0), display);
  Vec2 low = first.p1, high = first.p1;
  for (size_t i = 0; i < state.count(); ++i) {
    const Vec2 p = piece_line(shape, piece_at(shape, state, i), display).p2;
    if (p.x < low.x)
      low.x = p.x;
    else if (p.x > high.x)
      high.x = p.x;
    if (p.y < low.y)
      low.y = p.y;
    else if (p.y > high.y)
      high.y = p.y;
  }
  const f64 margin = static_cast<f64>(radius);
  const i32 width = stable_int(static_cast<f64>(high.x - low.x) + margin * 2.3);
  const i32 height =
      stable_int(static_cast<f64>(high.y - low.y) + margin * 2.3);
  const i32 left = stable_int(static_cast<f64>(low.x) - margin * 1.15);
  const i32 top = stable_int(static_cast<f64>(low.y) - margin * 1.15);
  // Above or left of the screen, the area starts at its edge.
  const i32 above = std::max(-top, 0);
  const i64 view_width = width;
  const i64 view_height = static_cast<i64>(height) - above;
  // A flipped viewport puts everything outside the texture.
  visible = view_width > 0 && view_height > 0;
  if (!visible)
    return {};
  return {{static_cast<f32>(left + std::max(-left, 0)),
           static_cast<f32>(top + above)},
          {static_cast<f32>(std::min<i64>(view_width, kMaxViewport)) /
               static_cast<f32>(view_width),
           static_cast<f32>(std::min<i64>(view_height, kMaxViewport)) /
               static_cast<f32>(view_height)}};
}

BodyTarget create_target(i32 width, i32 height) {
  BodyTarget target{.width = width, .height = height};
  glGenTextures(1, &target.colour);
  glBindTexture(GL_TEXTURE_2D, target.colour);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA,
               GL_UNSIGNED_BYTE, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glGenRenderbuffers(1, &target.depth);
  glBindRenderbuffer(GL_RENDERBUFFER, target.depth);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, width, height);
  glGenFramebuffers(1, &target.framebuffer);
  glBindFramebuffer(GL_FRAMEBUFFER, target.framebuffer);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         target.colour, 0);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                            GL_RENDERBUFFER, target.depth);
  return target;
}

void destroy_target(BodyTarget& target) {
  glDeleteFramebuffers(1, &target.framebuffer);
  glDeleteRenderbuffers(1, &target.depth);
  glDeleteTextures(1, &target.colour);
  target = {};
}

void clear_target(const BodyTarget& target) {
  glBindFramebuffer(GL_FRAMEBUFFER, target.framebuffer);
  glClearColor(0, 0, 0, 0);
  glClearDepth(1);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

struct Rgba {
  u8 r, g, b, a;
};

Rgba to_rgba(Color c) {
  const auto byte = [](f32 v) {
    return static_cast<u8>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255));
  };
  return {byte(c.r), byte(c.g), byte(c.b), byte(c.a)};
}

// MmSliderRendererGL's 128x1 texture: from the edge (0) to the centre (1), a
// faint shadow, the border, then the colour darkened to lightened, rendered
// as a line strip and so interpolated linearly between its stops.
GLuint body_gradient(SliderBodyRenderer& s,
                     Rgba                colour,
                     Rgba                border,
                     f32                 radius) {
  const f32 aa = std::min(std::max(0.5f / radius, 0.01171875f), 0.0625f);
  std::array<u8, 12> key{colour.r, colour.g, colour.b, border.r,
                         border.g, border.b, border.a};
  std::memcpy(&key[7], &aa, sizeof(aa));
  for (const auto& [k, texture] : s.gradients)
    if (k == key)
      return texture;

  // ColourHelper.Lighten2 and Darken, on the colour at alpha 180.
  const auto lighten = [](u8 v) {
    return static_cast<u8>(std::min(255.0f, v * 1.125f + 63.75f));
  };
  const auto darken = [](u8 v) {
    return static_cast<u8>(std::min(255.0f, v / (1.0f + 0.1f)));
  };
  const Rgba inner{lighten(colour.r), lighten(colour.g), lighten(colour.b),
                   180};
  const Rgba outer{darken(colour.r), darken(colour.g), darken(colour.b), 180};
  const struct {
    f32  x;
    Rgba c;
  } stops[] = {{-0.5f, {0, 0, 0, 0}},
               {0.0f, {0, 0, 0, 0}},
               {0.078125f - aa, {0, 0, 0, 64}},
               {0.078125f + aa, border},
               {0.1875f - aa, border},
               {0.1875f + aa, outer},
               {1.0f, inner},
               {1.5f, inner}};
  std::array<u8, 128 * 4> texels{};
  for (i32 i = 0; i < 128; ++i) {
    const f32 x = (i + 0.5f) / 128;
    size_t    k = 1;
    while (k + 1 < std::size(stops) && stops[k].x < x)
      ++k;
    const auto& a = stops[k - 1];
    const auto& b = stops[k];
    const f32   t = (x - a.x) / (b.x - a.x);
    const auto  mix = [&](u8 from, u8 to) {
      return static_cast<u8>(std::lround(from + (to - from) * t));
    };
    texels[i * 4 + 0] = mix(a.c.r, b.c.r);
    texels[i * 4 + 1] = mix(a.c.g, b.c.g);
    texels[i * 4 + 2] = mix(a.c.b, b.c.b);
    texels[i * 4 + 3] = mix(a.c.a, b.c.a);
  }
  GLuint texture;
  glGenTextures(1, &texture);
  glBindTexture(GL_TEXTURE_2D, texture);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 128, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE,
               texels.data());
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  s.gradients.push_back({key, texture});
  return texture;
}

// Draws `pieces` into the target's lower-left area of `width` by `height`
// pixels, which shows the framebuffer from (x, y).
void draw_pieces(App&             app,
                 GLuint           framebuffer,
                 i32              x,
                 i32              y,
                 i32              width,
                 i32              height,
                 const BodyCache& cache) {
  auto& s = app.slider_bodies;
  if (s.batch.empty())
    return;
  glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
  glViewport(0, 0, width, height);
  glUseProgram(s.program);
  glUniform1f(s.radius, cache.radius);
  glUniform4f(s.region, static_cast<f32>(x), static_cast<f32>(y),
              static_cast<f32>(width), static_cast<f32>(height));
  glUniform4f(s.squash, cache.squash.origin.x, cache.squash.origin.y,
              cache.squash.scale.x, cache.squash.scale.y);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, cache.gradient);
  glBindVertexArray(s.vao);
  glBindBuffer(GL_ARRAY_BUFFER, s.instances);
  glBufferData(GL_ARRAY_BUFFER, s.batch.size() * sizeof(PieceInstance),
               s.batch.data(), GL_STREAM_DRAW);
  glDisable(GL_BLEND);
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_LEQUAL);
  const auto count = static_cast<GLsizei>(s.batch.size());
  glDrawArraysInstanced(GL_TRIANGLES, 0, kPieceVertices, count);
  ++app.renderer.draw_calls;
  app.renderer.vertices_drawn += kPieceVertices * count;
  s.batch.clear();
}

}  // namespace

SliderBodyRenderer create_slider_body_renderer() {
  SliderBodyRenderer s;
  s.program =
      load_program("shaders/slider_body.vert", "shaders/slider_body.frag");
  s.radius = glGetUniformLocation(s.program, "u_radius");
  s.region = glGetUniformLocation(s.program, "u_region");
  s.squash = glGetUniformLocation(s.program, "u_squash");
  // CalculateCapMesh's half circle.
  std::array<f32, 2 * (kCapTriangles + 1)> rim{};
  rim[1] = -1;
  for (i32 i = 1; i < kCapTriangles; ++i) {
    const f32 angle = static_cast<f32>(i) * (3.1415927f / 24.0f);
    rim[2 * i] = static_cast<f32>(std::sin(static_cast<f64>(angle)));
    rim[2 * i + 1] = -static_cast<f32>(std::cos(static_cast<f64>(angle)));
  }
  rim[2 * kCapTriangles + 1] = 1;
  glUseProgram(s.program);
  glUniform2fv(glGetUniformLocation(s.program, "u_rim"), kCapTriangles + 1,
               rim.data());
  glUniform1i(glGetUniformLocation(s.program, "u_gradient"), 0);

  glGenVertexArrays(1, &s.vao);
  glBindVertexArray(s.vao);
  glGenBuffers(1, &s.instances);
  glBindBuffer(GL_ARRAY_BUFFER, s.instances);
  constexpr GLsizei kStride = sizeof(PieceInstance);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, kStride,
                        reinterpret_cast<void*>(offsetof(PieceInstance, p1)));
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, kStride,
                        reinterpret_cast<void*>(offsetof(PieceInstance, p2)));
  glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, kStride,
                        reinterpret_cast<void*>(offsetof(PieceInstance, cos)));
  glVertexAttribIPointer(
      3, 4, GL_UNSIGNED_BYTE, kStride,
      reinterpret_cast<void*>(offsetof(PieceInstance, end_cap)));
  for (GLuint i = 0; i < 4; ++i) {
    glEnableVertexAttribArray(i);
    glVertexAttribDivisor(i, 1);
  }
  return s;
}

namespace {

// Puts back the GL state the batched drawing expects.
struct ScreenRestorer {
  const App& app;
  ~ScreenRestorer() {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, app.pixel_width, app.pixel_height);
    glEnable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
  }
};

}  // namespace

void draw_slider_body(App&   app,
                      size_t index,
                      Color  colour,
                      f32    snake,
                      f32    scale,
                      Vec2   origin,
                      Vec2   shift,
                      f32    alpha) {
  auto&       game = app.game;
  auto&       s = app.slider_bodies;
  const auto& object = game.objects[index];
  const auto& shape = object.body;
  if (shape.lines.empty() || alpha <= 0)
    return;
  const f32              pixels = app.pixel_width / app.width;  // per point
  const DisplayTransform display{scale * pixels,
                                 {origin.x * pixels, origin.y * pixels}};
  const f32              radius = static_cast<f32>(game.radius) * display.ratio;
  const auto&            skin = app.skin.config;
  const GLuint           gradient =
      body_gradient(s, to_rgba(skin.slider_track.value_or(colour)),
                    to_rgba(skin.slider_border.value_or(kWhite)), radius);

  auto cache = std::find_if(game.bodies.caches.begin(),
                            game.bodies.caches.end(), [&](const BodyCache& c) {
                              return c.object == index;
                            });
  if (cache == game.bodies.caches.end()) {
    game.bodies.caches.push_back({.object = index});
    cache = game.bodies.caches.end() - 1;
  }
  cache->used = true;
  flush(app.renderer);
  ScreenRestorer restore{app};

  // A new place on screen: a new texture, over the body's area.
  if (!(cache->display == display) || cache->radius != radius ||
      cache->screen_width != app.pixel_width ||
      cache->screen_height != app.pixel_height) {
    destroy_target(cache->target);
    cache->display = display;
    cache->radius = radius;
    cache->screen_width = app.pixel_width;
    cache->screen_height = app.pixel_height;
    cache->settled = 0;
    cache->squash = {};
    Vec2 low = display(shape.lines[0].p1), high = low;
    for (const BodyLine& line : shape.lines)
      for (const Vec2 p : {display(line.p1), display(line.p2)}) {
        low = {std::min(low.x, p.x), std::min(low.y, p.y)};
        high = {std::max(high.x, p.x), std::max(high.y, p.y)};
      }
    const f32 margin = radius * 2.3f;
    cache->squashable = high.x - low.x + margin > kMaxViewport ||
                        high.y - low.y + margin > kMaxViewport;
    i32 x0 = 0, y0 = 0, x1 = app.pixel_width, y1 = app.pixel_height;
    if (!cache->squashable) {
      const f32 edge = radius * 1.15f + 2;
      x0 = std::max(x0, static_cast<i32>(std::floor(low.x - edge)));
      y0 = std::max(y0, static_cast<i32>(std::floor(low.y - edge)));
      x1 = std::min(x1, static_cast<i32>(std::ceil(high.x + edge)));
      y1 = std::min(y1, static_cast<i32>(std::ceil(high.y + edge)));
    }
    cache->x = x0;
    cache->y = y0;
    if (x1 > x0 && y1 > y0) {
      cache->target = create_target(x1 - x0, y1 - y0);
      clear_target(cache->target);
    }
  }
  const BodyTarget& target = cache->target;
  if (!target.framebuffer)
    return;  // off screen

  const SnakeState state = snake_state(shape, snake);
  BodySquash       squash;
  if (cache->squashable) {
    bool visible;
    squash = body_squash(shape, state, display, radius, visible);
    if (!visible)
      return;
  }
  // Pieces drawn for a different squash, colour or later snake are wrong.
  if (!(squash == cache->squash) || gradient != cache->gradient ||
      state.settled() < cache->settled) {
    clear_target(target);
    cache->squash = squash;
    cache->gradient = gradient;
    cache->settled = 0;
  }
  const size_t settled = state.settled();
  add_instances(s.batch, shape, state, cache->settled, settled, display);
  draw_pieces(app, target.framebuffer, cache->x, cache->y, target.width,
              target.height, *cache);
  cache->settled = settled;

  // The snaking end changes every frame: drawn over a copy of the rest.
  const BodyTarget* shown = &target;
  if (!state.done) {
    if (s.scratch.width < app.pixel_width ||
        s.scratch.height < app.pixel_height) {
      destroy_target(s.scratch);
      s.scratch = create_target(app.pixel_width, app.pixel_height);
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, target.framebuffer);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s.scratch.framebuffer);
    glBlitFramebuffer(0, 0, target.width, target.height, 0, 0, target.width,
                      target.height, GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT,
                      GL_NEAREST);
    add_instances(s.batch, shape, state, settled, state.count(), display);
    draw_pieces(app, s.scratch.framebuffer, cache->x, cache->y, target.width,
                target.height, *cache);
    shown = &s.scratch;
  }

  draw_image_region(
      app.renderer,
      {.id = shown->colour, .width = shown->width, .height = shown->height},
      cache->x / pixels + shift.x, cache->y / pixels + shift.y,
      target.width / pixels, target.height / pixels, 0, 0,
      static_cast<f32>(target.width) / shown->width,
      static_cast<f32>(target.height) / shown->height, fade(kWhite, alpha));
  // The scratch is reused by the next snaking body.
  if (shown == &s.scratch)
    flush(app.renderer);
}

void sweep_slider_bodies(App& app) {
  auto& caches = app.game.bodies.caches;
  for (auto& cache : caches)
    if (!cache.used)
      destroy_target(cache.target);
  std::erase_if(caches, [](const BodyCache& c) {
    return !c.used;
  });
  for (auto& cache : caches)
    cache.used = false;
}

void release_slider_bodies(SliderBodies& bodies) {
  for (auto& cache : bodies.caches)
    destroy_target(cache.target);
  bodies.caches.clear();
}

}  // namespace circles
