#include <circles/gfx.h>

#include <OpenGL/gl3.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stb_image.h>

namespace circles {

namespace {

std::string read_text_file(const char* path) {
  std::ifstream file(path);
  if (!file) {
    fprintf(stderr, "failed to open %s\n", path);
    exit(1);
  }
  std::stringstream ss;
  ss << file.rdbuf();
  return ss.str();
}

GLuint compile_shader(GLenum type, const char* path) {
  const std::string source = read_text_file(path);
  const char*       src = source.c_str();
  const GLuint      shader = glCreateShader(type);
  glShaderSource(shader, 1, &src, nullptr);
  glCompileShader(shader);

  GLint ok;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[1024];
    glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
    fprintf(stderr, "%s:\n%s\n", path, log);
    exit(1);
  }
  return shader;
}

// Glyphs are stored at this many pixels per em. The SDF stays sharp when drawn
// larger or smaller, so this only needs to keep the glyphs' detail.
constexpr f32 kSdfEmPixels = 64;
// Pixels of distance stored around each glyph's outline.
constexpr i32 kSdfPadding = 8;
// The value stored on the outline itself; sprite.frag uses the same (128/255).
constexpr u8  kSdfOnEdge = 128;
constexpr i32 kAtlasWidth = 1024;

const Glyph* find_glyph(const Font& font, char c) {
  if (c < kFirstChar || c > kLastChar)
    return nullptr;
  return &font.glyphs[c - kFirstChar];
}

// Spacing adjustment for a pair of glyphs (usually negative, as in "AV"), in
// ems.
f32 kerning(const Font& font, char previous, char c) {
  if (!previous)
    return 0;
  return stbtt_GetCodepointKernAdvance(&font.info, previous, c) *
         font.ems_per_unit;
}

// How sprite.frag shades a primitive; must match the constants there.
enum Kind : i32 {
  kKindSolid,
  kKindTexture,
  kKindGlyph,
  kKindCircle,
  kKindRoundedRect,
  kKindArc,
  kKindSpotlight,
};

// Per vertex: position (2), texture or shape-local coordinates (2), colour
// (4), kind and its parameters (4).
constexpr i32    kFloatsPerVertex = 12;
constexpr size_t kMaxBatchVertices = 6 * 8192;

struct Params {
  f32 kind, a, b, c;
};

void reserve(Renderer& r, size_t vertex_count) {
  if (r.vertices.size() / kFloatsPerVertex + vertex_count > kMaxBatchVertices)
    flush(r);
}

// A slot in the batch for `texture`, which vertices name in their last
// parameter. A batch samples several textures, so switching between them
// doesn't end it.
f32 use_texture(Renderer& r, GLuint texture) {
  for (i32 slot = 0; slot < r.texture_count; ++slot)
    if (r.textures[slot] == texture)
      return static_cast<f32>(slot);
  if (r.texture_count == static_cast<i32>(r.textures.size())) {
    flush(r);
    r.texture_count = 0;
  }
  r.textures[r.texture_count] = texture;
  return static_cast<f32>(r.texture_count++);
}

void vertex(Renderer& r, f32 x, f32 y, f32 u, f32 v, Color c, Params p) {
  const f32 data[kFloatsPerVertex] = {x,   y,   u,      v,   c.r, c.g,
                                      c.b, c.a, p.kind, p.a, p.b, p.c};
  r.vertices.insert(r.vertices.end(), data, data + kFloatsPerVertex);
}

// Two triangles covering an axis-aligned rectangle, with a colour per row for
// vertical gradients.
void rect_quad(Renderer& r,
               f32       x0,
               f32       y0,
               f32       x1,
               f32       y1,
               f32       u0,
               f32       v0,
               f32       u1,
               f32       v1,
               Color     top,
               Color     bottom,
               Params    p) {
  reserve(r, 6);
  vertex(r, x0, y0, u0, v0, top, p);
  vertex(r, x1, y0, u1, v0, top, p);
  vertex(r, x1, y1, u1, v1, bottom, p);
  vertex(r, x1, y1, u1, v1, bottom, p);
  vertex(r, x0, y1, u0, v1, bottom, p);
  vertex(r, x0, y0, u0, v0, top, p);
}

void circle_quad(Renderer& r,
                 f32       cx,
                 f32       cy,
                 f32       radius,
                 Color     top,
                 Color     bottom,
                 Params    p) {
  if (radius <= 0)
    return;
  // Extend past the circle by a point and a half for the anti-aliased edge.
  const f32 m = 1 + 1.5f / radius;
  const f32 e = radius * m;
  rect_quad(r, cx - e, cy - e, cx + e, cy + e, -m, -m, m, m, top, bottom, p);
}

// `dilate` moves the glyph edge outward in SDF units; `softness` widens its
// blur.
void draw_glyphs(Renderer&        r,
                 const Font&      font,
                 std::string_view text,
                 f32              x,
                 f32              baseline,
                 f32              size,
                 Color            color,
                 Align            align,
                 f32              dilate,
                 f32              softness) {
  if (align != Align::Left) {
    const f32 width = measure_text(font, text, size);
    x -= align == Align::Center ? width / 2 : width;
  }
  const f32 slot = use_texture(r, font.atlas.id);
  f32       pen = x;
  char      previous = 0;
  for (char c : text) {
    const Glyph* glyph = find_glyph(font, c);
    if (!glyph)
      continue;
    pen += kerning(font, previous, c) * size;
    previous = c;
    rect_quad(r, pen + glyph->x0 * size, baseline + glyph->y0 * size,
              pen + glyph->x1 * size, baseline + glyph->y1 * size, glyph->u0,
              glyph->v0, glyph->u1, glyph->v1, color, color,
              {kKindGlyph, dilate, softness, slot});
    pen += glyph->advance * size;
  }
}

}  // namespace

GLuint load_program(const char* vertex_path, const char* fragment_path) {
  const GLuint vs = compile_shader(GL_VERTEX_SHADER, vertex_path);
  const GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fragment_path);
  const GLuint program = glCreateProgram();
  glAttachShader(program, vs);
  glAttachShader(program, fs);
  glLinkProgram(program);

  GLint ok;
  glGetProgramiv(program, GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[1024];
    glGetProgramInfoLog(program, sizeof(log), nullptr, log);
    fprintf(stderr, "link %s + %s:\n%s\n", vertex_path, fragment_path, log);
    exit(1);
  }
  glDeleteShader(vs);
  glDeleteShader(fs);
  return program;
}

Image load_image(const std::string& path) {
  Image image;
  i32   channels;
  u8*   pixels =
      stbi_load(path.c_str(), &image.width, &image.height, &channels, 4);
  if (!pixels)
    return {};
  image.pixels.assign(
      pixels, pixels + static_cast<size_t>(image.width) * image.height * 4);
  stbi_image_free(pixels);
  return image;
}

Texture upload_texture(const Image& image) {
  Texture texture{.width = image.width, .height = image.height};
  if (image.pixels.empty())
    return texture;
  texture.blank = true;
  for (size_t i = 3; i < image.pixels.size() && texture.blank; i += 4)
    texture.blank = image.pixels[i] == 0;
  glGenTextures(1, &texture.id);
  glBindTexture(GL_TEXTURE_2D, texture.id);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, image.width, image.height, 0,
               GL_RGBA, GL_UNSIGNED_BYTE, image.pixels.data());
  // Backgrounds usually draw smaller than their resolution; mipmaps keep them
  // smooth.
  glGenerateMipmap(GL_TEXTURE_2D);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                  GL_LINEAR_MIPMAP_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  return texture;
}

void destroy_texture(Texture& texture) {
  if (texture.id)
    glDeleteTextures(1, &texture.id);
  texture = {};
}

Font load_font(const char* path, i32 index) {
  Font          font;
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    fprintf(stderr, "failed to open %s\n", path);
    exit(1);
  }
  font.file_data.assign(std::istreambuf_iterator<char>(file),
                        std::istreambuf_iterator<char>());
  const u8* data = font.file_data.data();
  const i32 offset = stbtt_GetFontOffsetForIndex(data, index);
  if (offset < 0 || !stbtt_InitFont(&font.info, data, offset)) {
    fprintf(stderr, "failed to parse font %d in %s\n", index, path);
    exit(1);
  }
  font.ems_per_unit = stbtt_ScaleForMappingEmToPixels(&font.info, 1);

  // 'H' has a flat top exactly at cap height.
  i32 x0, y0, x1, y1;
  stbtt_GetCodepointBox(&font.info, 'H', &x0, &y0, &x1, &y1);
  font.cap_height = y1 * font.ems_per_unit;

  // Shelf packing: fill a row left to right, then start a new row below.
  struct Bitmap {
    u8* pixels;
    i32 width, height;
    i32 atlas_x, atlas_y;
  };
  constexpr i32 kGlyphCount = kLastChar - kFirstChar + 1;
  Bitmap        bitmaps[kGlyphCount];
  const f32     pixel_scale = kSdfEmPixels * font.ems_per_unit;
  i32           shelf_x = 0, shelf_y = 0, shelf_height = 0;
  for (i32 i = 0; i < kGlyphCount; ++i) {
    const i32 codepoint = kFirstChar + i;
    auto&     bitmap = bitmaps[i];
    // Outline-less glyphs (space) return null and leave these at 0.
    i32       x_offset = 0, y_offset = 0;
    bitmap.width = bitmap.height = 0;
    bitmap.pixels = stbtt_GetCodepointSDF(
        &font.info, pixel_scale, codepoint, kSdfPadding, kSdfOnEdge,
        static_cast<f32>(kSdfOnEdge) / kSdfPadding, &bitmap.width,
        &bitmap.height, &x_offset, &y_offset);
    if (shelf_x + bitmap.width > kAtlasWidth) {
      shelf_x = 0;
      shelf_y += shelf_height;
      shelf_height = 0;
    }
    bitmap.atlas_x = shelf_x;
    bitmap.atlas_y = shelf_y;
    shelf_x += bitmap.width;
    shelf_height = std::max(shelf_height, bitmap.height);

    i32 advance, left_side_bearing;
    stbtt_GetCodepointHMetrics(&font.info, codepoint, &advance,
                               &left_side_bearing);
    auto& glyph = font.glyphs[i];
    glyph.advance = advance * font.ems_per_unit;
    glyph.x0 = x_offset / kSdfEmPixels;
    glyph.y0 = y_offset / kSdfEmPixels;
    glyph.x1 = (x_offset + bitmap.width) / kSdfEmPixels;
    glyph.y1 = (y_offset + bitmap.height) / kSdfEmPixels;
  }

  const i32       atlas_height = shelf_y + shelf_height;
  std::vector<u8> atlas(static_cast<size_t>(kAtlasWidth) * atlas_height, 0);
  for (i32 i = 0; i < kGlyphCount; ++i) {
    const auto& bitmap = bitmaps[i];
    for (i32 row = 0; row < bitmap.height; ++row)
      std::copy_n(bitmap.pixels + row * bitmap.width, bitmap.width,
                  atlas.begin() + (bitmap.atlas_y + row) * kAtlasWidth +
                      bitmap.atlas_x);
    stbtt_FreeSDF(bitmap.pixels, nullptr);
    auto& glyph = font.glyphs[i];
    glyph.u0 = static_cast<f32>(bitmap.atlas_x) / kAtlasWidth;
    glyph.v0 = static_cast<f32>(bitmap.atlas_y) / atlas_height;
    glyph.u1 = static_cast<f32>(bitmap.atlas_x + bitmap.width) / kAtlasWidth;
    glyph.v1 = static_cast<f32>(bitmap.atlas_y + bitmap.height) / atlas_height;
  }

  font.atlas = {.width = kAtlasWidth, .height = atlas_height};
  glGenTextures(1, &font.atlas.id);
  glBindTexture(GL_TEXTURE_2D, font.atlas.id);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, kAtlasWidth, atlas_height, 0, GL_RED,
               GL_UNSIGNED_BYTE, atlas.data());
  // Linear filtering blends neighbouring distances, keeping edges smooth at
  // any scale.
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  return font;
}

f32 measure_text(const Font& font, std::string_view text, f32 size) {
  f32  pen = 0;
  char previous = 0;
  for (char c : text) {
    const Glyph* glyph = find_glyph(font, c);
    if (!glyph)
      continue;
    pen += kerning(font, previous, c) + glyph->advance;
    previous = c;
  }
  return pen * size;
}

Renderer create_renderer() {
  Renderer r{};
  r.program = load_program("shaders/sprite.vert", "shaders/sprite.frag");
  r.screen_size_location = glGetUniformLocation(r.program, "u_screen_size");
  glGenVertexArrays(1, &r.vao);
  glBindVertexArray(r.vao);
  glGenBuffers(1, &r.vbo);
  glBindBuffer(GL_ARRAY_BUFFER, r.vbo);
  constexpr GLsizei kStride = kFloatsPerVertex * sizeof(f32);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, kStride, nullptr);
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, kStride,
                        reinterpret_cast<void*>(2 * sizeof(f32)));
  glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, kStride,
                        reinterpret_cast<void*>(4 * sizeof(f32)));
  glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, kStride,
                        reinterpret_cast<void*>(8 * sizeof(f32)));
  for (GLuint i = 0; i < 4; ++i)
    glEnableVertexAttribArray(i);
  r.vertices.reserve(kMaxBatchVertices * kFloatsPerVertex);
  // Slots no vertex samples still need a complete texture bound.
  r.blank =
      upload_texture({.pixels = {255, 255, 255, 255}, .width = 1, .height = 1})
          .id;
  glUseProgram(r.program);
  constexpr GLint kUnits[] = {0, 1, 2, 3, 4, 5, 6, 7};
  glUniform1iv(glGetUniformLocation(r.program, "u_textures"),
               static_cast<GLsizei>(r.textures.size()), kUnits);
  return r;
}

void begin_frame(Renderer& r, f32 width, f32 height) {
  if (r.reuse_uploads)
    begin_upload_frame(r.uploads);
  r.width = width;
  r.height = height;
  r.vertices.clear();
  r.texture_count = 0;
  r.draw_calls = 0;
  r.vertices_drawn = 0;
}

void end_frame(Renderer& r) {
  if (r.reuse_uploads)
    end_upload_frame(r.uploads);
}

void flush(Renderer& r) {
  if (r.vertices.empty())
    return;
  glUseProgram(r.program);
  glUniform2f(r.screen_size_location, r.width, r.height);
  for (i32 slot = 0; slot < static_cast<i32>(r.textures.size()); ++slot) {
    glActiveTexture(GL_TEXTURE0 + slot);
    glBindTexture(GL_TEXTURE_2D,
                  slot < r.texture_count ? r.textures[slot] : r.blank);
  }
  glActiveTexture(GL_TEXTURE0);
  glBindVertexArray(r.vao);
  if (r.reuse_uploads) {
    const size_t      offset = upload_vertices(r.uploads, r.vertices.data(),
                                               r.vertices.size() * sizeof(f32));
    constexpr GLsizei stride = kFloatsPerVertex * sizeof(f32);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(offset));
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(offset + 2 * sizeof(f32)));
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(offset + 4 * sizeof(f32)));
    glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(offset + 8 * sizeof(f32)));
  } else {
    glBindBuffer(GL_ARRAY_BUFFER, r.vbo);
    glBufferData(GL_ARRAY_BUFFER, r.vertices.size() * sizeof(f32),
                 r.vertices.data(), GL_STREAM_DRAW);
  }
  const auto count = static_cast<GLsizei>(r.vertices.size() / kFloatsPerVertex);
  glDrawArrays(GL_TRIANGLES, 0, count);
  ++r.draw_calls;
  r.vertices_drawn += count;
  r.vertices.clear();
}

void draw_rect(Renderer& r, f32 x, f32 y, f32 w, f32 h, Color color) {
  rect_quad(r, x, y, x + w, y + h, 0, 0, 0, 0, color, color,
            {kKindSolid, 0, 0, 0});
}

void draw_rect_gradient(Renderer& r,
                        f32       x,
                        f32       y,
                        f32       w,
                        f32       h,
                        Color     top,
                        Color     bottom) {
  rect_quad(r, x, y, x + w, y + h, 0, 0, 0, 0, top, bottom,
            {kKindSolid, 0, 0, 0});
}

void draw_rounded_rect(Renderer& r,
                       f32       x,
                       f32       y,
                       f32       w,
                       f32       h,
                       f32       radius,
                       Color     color) {
  draw_rounded_rect_gradient(r, x, y, w, h, radius, color, color);
}

void draw_rounded_rect_gradient(Renderer& r,
                                f32       x,
                                f32       y,
                                f32       w,
                                f32       h,
                                f32       radius,
                                Color     top,
                                Color     bottom) {
  const f32 half_w = w / 2, half_h = h / 2;
  radius = std::min({radius, half_w, half_h});
  // Extend by a point so the anti-aliased edge isn't cut off.
  constexpr f32 kMargin = 1;
  rect_quad(r, x - kMargin, y - kMargin, x + w + kMargin, y + h + kMargin,
            -half_w - kMargin, -half_h - kMargin, half_w + kMargin,
            half_h + kMargin, top, bottom,
            {kKindRoundedRect, half_w, half_h, radius});
}

void draw_circle(Renderer& r,
                 f32       cx,
                 f32       cy,
                 f32       radius,
                 Color     color,
                 f32       inner,
                 f32       softness) {
  circle_quad(r, cx, cy, radius, color, color,
              {kKindCircle, inner, softness, 0});
}

void draw_spotlight(Renderer& r,
                    f32       cx,
                    f32       cy,
                    f32       radius,
                    f32       softness,
                    Color     color) {
  // A square reaching every corner of the window, with the hole in its
  // middle.
  const f32 cover =
      std::max({std::hypot(cx, cy), std::hypot(r.width - cx, cy),
                std::hypot(cx, r.height - cy),
                std::hypot(r.width - cx, r.height - cy), radius + softness});
  circle_quad(r, cx, cy, cover, color, color,
              {kKindSpotlight, radius / cover, softness / cover, 0});
}

void draw_circle_gradient(Renderer& r,
                          f32       cx,
                          f32       cy,
                          f32       radius,
                          Color     top,
                          Color     bottom) {
  circle_quad(r, cx, cy, radius, top, bottom, {kKindCircle, 0, 0, 0});
}

void draw_arc(Renderer& r,
              f32       cx,
              f32       cy,
              f32       radius,
              f32       inner,
              f32       progress,
              Color     color) {
  if (progress <= 0)
    return;
  circle_quad(r, cx, cy, radius, color, color,
              {kKindArc, inner, std::min(progress, 1.0f), 0});
}

void draw_line(Renderer& r,
               f32       x0,
               f32       y0,
               f32       x1,
               f32       y1,
               f32       width,
               Color     color) {
  f32       dx = x1 - x0, dy = y1 - y0;
  const f32 length = std::sqrt(dx * dx + dy * dy);
  if (length < 1e-4f) {
    draw_circle(r, x0, y0, width / 2, color);
    return;
  }
  dx /= length;
  dy /= length;
  // A capsule: a rounded rectangle in the line's frame, the caps its corners.
  constexpr f32 kMargin = 1;
  const f32     half_width = width / 2;
  const f32     half_length = length / 2 + half_width;
  const f32     ex = half_length + kMargin, ey = half_width + kMargin;
  const f32     cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
  const Params  p{kKindRoundedRect, half_length, half_width, half_width};
  reserve(r, 6);
  auto corner = [&](f32 along, f32 across) {
    vertex(r, cx + dx * ex * along - dy * ey * across,
           cy + dy * ex * along + dx * ey * across, ex * along, ey * across,
           color, p);
  };
  corner(-1, -1);
  corner(1, -1);
  corner(1, 1);
  corner(1, 1);
  corner(-1, 1);
  corner(-1, -1);
}

void draw_triangle(Renderer& r,
                   f32       x0,
                   f32       y0,
                   f32       x1,
                   f32       y1,
                   f32       x2,
                   f32       y2,
                   Color     color) {
  reserve(r, 3);
  const Params p{kKindSolid, 0, 0, 0};
  vertex(r, x0, y0, 0, 0, color, p);
  vertex(r, x1, y1, 0, 0, color, p);
  vertex(r, x2, y2, 0, 0, color, p);
}

void draw_image_region(Renderer&      r,
                       const Texture& texture,
                       f32            x,
                       f32            y,
                       f32            w,
                       f32            h,
                       f32            u0,
                       f32            v0,
                       f32            u1,
                       f32            v1,
                       Color          tint) {
  if (!texture.id)
    return;
  const f32 slot = use_texture(r, texture.id);
  rect_quad(r, x, y, x + w, y + h, u0, v0, u1, v1, tint, tint,
            {kKindTexture, 0, 0, slot});
}

void draw_sprite(Renderer&      r,
                 const Texture& texture,
                 f32            cx,
                 f32            cy,
                 f32            w,
                 f32            h,
                 f32            angle,
                 Color          tint) {
  if (!texture.id || texture.blank)
    return;
  const f32    slot = use_texture(r, texture.id);
  // y points down, so a positive angle turns clockwise on screen.
  const f32    c = std::cos(angle), s = std::sin(angle);
  const Params p{kKindTexture, 0, 0, slot};
  auto         corner = [&](f32 u, f32 v) {
    const f32 dx = (u - 0.5f) * w, dy = (v - 0.5f) * h;
    vertex(r, cx + dx * c - dy * s, cy + dx * s + dy * c, u, v, tint, p);
  };
  reserve(r, 6);
  corner(0, 0);
  corner(1, 0);
  corner(1, 1);
  corner(1, 1);
  corner(0, 1);
  corner(0, 0);
}

void draw_image_cover(Renderer&      r,
                      const Texture& texture,
                      f32            x,
                      f32            y,
                      f32            w,
                      f32            h,
                      Color          tint,
                      f32            zoom,
                      f32            shift_x,
                      f32            shift_y) {
  if (!texture.id || w <= 0 || h <= 0)
    return;
  // The fraction of the image that covers the rectangle along each axis, then
  // what's left of it after zooming.
  const f32  target_aspect = w / h;
  const f32  image_aspect = static_cast<f32>(texture.width) / texture.height;
  const bool wider = image_aspect > target_aspect;
  const f32  cover_u = wider ? target_aspect / image_aspect : 1;
  const f32  cover_v = wider ? 1 : image_aspect / target_aspect;
  const f32  visible_u = cover_u / zoom;
  const f32  visible_v = cover_v / zoom;
  // Panning stays within the margin the zoom adds, so it moves the same share
  // of the rectangle along both axes however much of the image is cropped.
  const f32  center_u = 0.5f + shift_x * (cover_u - visible_u) / 2;
  const f32  center_v = 0.5f + shift_y * (cover_v - visible_v) / 2;
  const f32  slot = use_texture(r, texture.id);
  rect_quad(r, x, y, x + w, y + h, center_u - visible_u / 2,
            center_v - visible_v / 2, center_u + visible_u / 2,
            center_v + visible_v / 2, tint, tint, {kKindTexture, 0, 0, slot});
}

void draw_text(Renderer&        r,
               const Font&      font,
               std::string_view text,
               f32              x,
               f32              baseline,
               f32              size,
               Color            color,
               Align            align) {
  draw_glyphs(r, font, text, x, baseline, size, color, align, 0, 0);
}

void draw_text_shadowed(Renderer&        r,
                        const Font&      font,
                        std::string_view text,
                        f32              x,
                        f32              baseline,
                        f32              size,
                        Color            color,
                        Align            align) {
  const f32 offset = std::max(1.0f, size * 0.04f);
  draw_glyphs(r, font, text, x, baseline + offset, size,
              fade(kBlack, 0.6f * color.a), align, 0.08f, 0.12f);
  draw_glyphs(r, font, text, x, baseline, size, color, align, 0, 0);
}

}  // namespace circles
