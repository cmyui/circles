#pragma once

// osu! skins: a folder of images and sounds plus skin.ini. Lookups follow
// osu!'s rules: file names are case-insensitive, name@2x.png is preferred
// over name.png, and animation frames (name-0.png, name-1.png, ...) take
// precedence over a single image. Elements a skin lacks are drawn by the game
// itself.

#include <circles/gfx.h>
#include <circles/types.h>

#include <cstddef>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace circles {

struct SkinConfig {
  std::string          name;
  bool                 cursor_rotate = true;
  bool                 cursor_expand = true;
  bool                 cursor_centre = true;
  bool                 overlay_above_number = true;
  bool                 allow_slider_ball_tint = false;
  // Animation frames per second; when unset, each animation lasts a second.
  f32                  animation_framerate = -1;
  std::vector<Color>   combo_colours;
  std::optional<Color> slider_border;
  std::optional<Color> slider_track;
  std::optional<Color> slider_ball;
  // Number fonts: prefix-0.png through prefix-9.png, and for scores also
  // -comma, -dot, -percent and -x. Overlaps are in 1x pixels.
  std::string          hit_circle_prefix = "default";
  std::string          score_prefix = "score";
  std::string          combo_prefix = "score";
  f32                  hit_circle_overlap = -2;
  f32                  score_overlap = 0;
  f32                  combo_overlap = 0;
};

SkinConfig parse_skin_ini(std::string_view text);

// An image, or one frame of an animation. Sizes in osu!'s 1x pixels are the
// texture's divided by `scale`, which is 2 for @2x images.
struct SkinImage {
  Texture texture;
  f32     scale;

  f32 width() const { return texture.width / scale; }
  f32 height() const { return texture.height / scale; }
};

struct StringHash {
  using is_transparent = void;
  size_t operator()(std::string_view text) const {
    return std::hash<std::string_view>{}(text);
  }
};

// A string-keyed map that can be searched with a string_view.
template <typename Value>
using StringMap =
    std::unordered_map<std::string, Value, StringHash, std::equal_to<>>;

struct Skin {
  std::filesystem::path             directory;  // empty for the built-in skin
  SkinConfig                        config;
  // Lowercase relative paths to files, for case-insensitive lookups.
  StringMap<std::filesystem::path>  files;
  // Frames by lowercase element name, such as "hitcircle" or "score-7".
  StringMap<std::vector<SkinImage>> elements;
};

// Folder names in `directory`, sorted.
std::vector<std::string> list_skins(const std::filesystem::path& directory);
Skin load_skin(const std::filesystem::path& directory);
void destroy_skin(Skin& skin);

// The element's frames, or null if the skin doesn't have it.
const std::vector<SkinImage>* skin_element(const Skin&      skin,
                                           std::string_view name);
// The frame `elapsed` seconds into an animation; the last frame holds.
const SkinImage& skin_frame(const Skin&                   skin,
                            const std::vector<SkinImage>& frames,
                            f64                           elapsed);
// The skin's file for `stem` with one of `extensions`, or an empty path.
std::filesystem::path skin_file(const Skin&                        skin,
                                std::string_view                   stem,
                                std::initializer_list<const char*> extensions);

// Draws an image centred on (x, y) with each 1x pixel `scale` points wide.
void draw_skin_image(Renderer&        r,
                     const SkinImage& image,
                     f32              x,
                     f32              y,
                     f32              scale,
                     Color            tint,
                     f32              angle = 0);

// Draws digits and ", . % x" in a number font, vertically centred on
// `center_y`, each 1x pixel `scale` points wide. Returns the width drawn, or
// nothing (drawing nothing) when the skin doesn't have the font.
std::optional<f32> draw_skin_number(Renderer&        r,
                                    const Skin&      skin,
                                    std::string_view prefix,
                                    std::string_view text,
                                    f32              x,
                                    f32              center_y,
                                    f32              scale,
                                    f32              overlap,
                                    Color            tint,
                                    Align            align);

}  // namespace circles
