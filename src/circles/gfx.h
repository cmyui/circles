#pragma once

// 2D drawing in window points (origin top-left, y down): shapes, images and
// signed-distance-field text, batched into as few draw calls as possible.

#include <circles/types.h>

#include <OpenGL/gl3.h>
#include <stb_truetype.h>
#include <string>
#include <string_view>
#include <vector>

namespace circles {

struct Color {
  f32 r, g, b, a;
};

inline Color hex_color(u32 rgb, f32 a = 1) {
  return {((rgb >> 16) & 255) / 255.0f, ((rgb >> 8) & 255) / 255.0f,
          (rgb & 255) / 255.0f, a};
}

inline Color fade(Color c, f32 alpha) {
  return {c.r, c.g, c.b, c.a * alpha};
}

inline Color mix(Color a, Color b, f32 t) {
  return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t,
          a.a + (b.a - a.a) * t};
}

// Scales the colour channels and leaves alpha: below 1 darkens, above 1
// brightens.
inline Color shade(Color c, f32 k) {
  return {c.r * k, c.g * k, c.b * k, c.a};
}

inline constexpr Color kWhite{1, 1, 1, 1};
inline constexpr Color kBlack{0, 0, 0, 1};

// Compiles and links a vertex + fragment shader pair. Exits with the log on
// failure.
GLuint load_program(const char* vertex_path, const char* fragment_path);

// Decoded RGBA8 pixels. Loading is safe on any thread; uploading needs the GL
// thread.
struct Image {
  std::vector<u8> pixels;
  i32             width = 0;
  i32             height = 0;
};

struct Texture {
  GLuint id = 0;
  i32    width = 0;
  i32    height = 0;
};

// Empty on failure.
Image load_image(const std::string& path);
Texture upload_texture(const Image& image);
void destroy_texture(Texture& texture);

// Fonts cover printable ASCII only.
inline constexpr char kFirstChar = ' ';
inline constexpr char kLastChar = '~';

// Lengths are in ems: fractions of the font size.
struct Glyph {
  // Quad corners relative to the pen position on the baseline, y down.
  f32 x0, y0, x1, y1;
  // The same corners in the atlas.
  f32 u0, v0, u1, v1;
  // How far the pen moves right after this glyph.
  f32 advance;
};

struct Font {
  // stb_truetype reads glyph data straight from these bytes, so a Font moves
  // but doesn't copy.
  std::vector<u8> file_data;
  stbtt_fontinfo  info;
  f32             ems_per_unit;  // font file units to ems
  f32             cap_height;    // height of capitals and digits, in ems
  Glyph           glyphs[kLastChar - kFirstChar + 1];
  // Single channel: each texel is the distance to the nearest glyph outline.
  Texture         atlas;

  Font() = default;
  Font(Font&&) = default;
  Font& operator=(Font&&) = default;
  Font(const Font&) = delete;
  Font& operator=(const Font&) = delete;
};

// Loads font number `index` of a .ttf or .ttc file and builds its SDF atlas.
// Exits on failure.
Font load_font(const char* path, i32 index);

// The sum of glyph advances plus kerning.
f32 measure_text(const Font& font, std::string_view text, f32 size);

// The baseline that vertically centres capitals and digits on `center_y`.
inline f32 centered_baseline(const Font& font, f32 size, f32 center_y) {
  return center_y + font.cap_height * size / 2;
}

enum class Align : u8 { Left, Center, Right };

struct Renderer {
  GLuint           program;
  GLuint           vao;
  GLuint           vbo;
  GLint            screen_size_location;
  std::vector<f32> vertices;
  GLuint           texture = 0;  // sampled by the batched vertices
  f32              width = 0;    // window size in points
  f32              height = 0;
  // This frame's draws, for profiling.
  u32              draw_calls = 0;
  u32              vertices_drawn = 0;
};

Renderer create_renderer();
void begin_frame(Renderer& r, f32 width, f32 height);
// Draws the batch. Needed before drawing with any other program.
void flush(Renderer& r);

void draw_rect(Renderer& r, f32 x, f32 y, f32 w, f32 h, Color color);
void draw_rect_gradient(Renderer& r,
                        f32       x,
                        f32       y,
                        f32       w,
                        f32       h,
                        Color     top,
                        Color     bottom);
void draw_rounded_rect(Renderer& r,
                       f32       x,
                       f32       y,
                       f32       w,
                       f32       h,
                       f32       radius,
                       Color     color);
void draw_rounded_rect_gradient(Renderer& r,
                                f32       x,
                                f32       y,
                                f32       w,
                                f32       h,
                                f32       radius,
                                Color     top,
                                Color     bottom);
// A disc, or a ring when `inner` (a fraction of the radius) is above 0.
// `softness`, also a fraction of the radius, blurs the outer edge.
void draw_circle(Renderer& r,
                 f32       cx,
                 f32       cy,
                 f32       radius,
                 Color     color,
                 f32       inner = 0,
                 f32       softness = 0);
// Covers the window except a hole of `radius` around (cx, cy), its edge
// fading over `softness` points.
void draw_spotlight(Renderer& r,
                    f32       cx,
                    f32       cy,
                    f32       radius,
                    f32       softness,
                    Color     color);
void draw_circle_gradient(Renderer& r,
                          f32       cx,
                          f32       cy,
                          f32       radius,
                          Color     top,
                          Color     bottom);
// The part of a disc or ring from 12 o'clock clockwise through `progress` (0
// to 1) of a turn.
void draw_arc(Renderer& r,
              f32       cx,
              f32       cy,
              f32       radius,
              f32       inner,
              f32       progress,
              Color     color);
// A line with round caps.
void draw_line(Renderer& r,
               f32       x0,
               f32       y0,
               f32       x1,
               f32       y1,
               f32       width,
               Color     color);
void draw_triangle(Renderer& r,
                   f32       x0,
                   f32       y0,
                   f32       x1,
                   f32       y1,
                   f32       x2,
                   f32       y2,
                   Color     color);
// The part of a texture from (u0, v0) to (u1, v1), stretched over the
// rectangle.
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
                       Color          tint);
// A texture centred on (cx, cy), rotated clockwise by `angle` radians.
void draw_sprite(Renderer&      r,
                 const Texture& texture,
                 f32            cx,
                 f32            cy,
                 f32            w,
                 f32            h,
                 f32            angle,
                 Color          tint);
// Fills the rectangle, cropping whichever image dimension overflows to keep
// its aspect ratio. `zoom` above 1 crops further; shift_x and shift_y (-1 to
// 1) pan within the margin the zoom adds.
void draw_image_cover(Renderer&      r,
                      const Texture& texture,
                      f32            x,
                      f32            y,
                      f32            w,
                      f32            h,
                      Color          tint,
                      f32            zoom = 1,
                      f32            shift_x = 0,
                      f32            shift_y = 0);
// `x` is the left edge, centre or right edge of the text, per `align`.
void draw_text(Renderer&        r,
               const Font&      font,
               std::string_view text,
               f32              x,
               f32              baseline,
               f32              size,
               Color            color,
               Align            align = Align::Left);
// draw_text with a soft drop shadow, for text over busy backgrounds.
void draw_text_shadowed(Renderer&        r,
                        const Font&      font,
                        std::string_view text,
                        f32              x,
                        f32              baseline,
                        f32              size,
                        Color            color,
                        Align            align = Align::Left);

}  // namespace circles
