#include <circles/skin.h>
#include <circles/strings.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

namespace circles {

namespace {

namespace fs = std::filesystem;

// "r,g,b", with an optional alpha that osu! ignores.
std::optional<Color> parse_colour(std::string_view value) {
  const auto fields = split(value, ',');
  if (fields.size() < 3)
    return std::nullopt;
  u32 rgb = 0;
  for (i32 i = 0; i < 3; ++i) {
    const i32 component = field_value(trim(fields[i]));
    if (component < 0 || component > 255)
      return std::nullopt;
    rgb = rgb << 8 | static_cast<u32>(component);
  }
  return hex_color(rgb);
}

bool parse_flag(std::string_view value) {
  return value != "0" && lowercase(value) != "false";
}

// Font prefixes may name subfolders with either slash.
std::string font_prefix(std::string_view value) {
  std::string prefix = lowercase(value);
  std::replace(prefix.begin(), prefix.end(), '\\', '/');
  return prefix;
}

std::optional<SkinImage> load_skin_image(const Skin&        skin,
                                         const std::string& stem) {
  for (const auto& [suffix, scale] :
       {std::pair{"@2x.png", 2.0f}, std::pair{".png", 1.0f}}) {
    const auto file = skin.files.find(stem + suffix);
    if (file == skin.files.end())
      continue;
    const Image image = load_image(file->second.string());
    if (!image.pixels.empty())
      return SkinImage{upload_texture(image), scale};
  }
  return std::nullopt;
}

// `frame_separator` joins the stem and frame number of an animated element,
// or is null for elements that are never animated.
void load_element(Skin&              skin,
                  const std::string& stem,
                  const char*        frame_separator = nullptr) {
  std::vector<SkinImage> frames;
  if (frame_separator) {
    while (auto frame = load_skin_image(
               skin, stem + frame_separator + std::to_string(frames.size())))
      frames.push_back(*frame);
  }
  if (frames.empty())
    if (auto image = load_skin_image(skin, stem))
      frames.push_back(*image);
  if (!frames.empty())
    skin.elements[stem] = std::move(frames);
}

// The element name for a character in a number font.
std::string number_glyph(std::string_view prefix, char c) {
  std::string name(prefix);
  switch (c) {
    case ',':
      return name + "-comma";
    case '.':
      return name + "-dot";
    case '%':
      return name + "-percent";
    case 'x':
      return name + "-x";
    default:
      return name + '-' + c;
  }
}

constexpr const char* kElements[] = {
    "hitcircle",
    "hitcircleoverlay",
    "approachcircle",
    "sliderstartcircle",
    "sliderstartcircleoverlay",
    "sliderendcircle",
    "sliderendcircleoverlay",
    "reversearrow",
    "sliderfollowcircle",
    "sliderscorepoint",
    "cursor",
    "cursormiddle",
    "cursortrail",
    "spinner-glow",
    "spinner-bottom",
    "spinner-top",
    "spinner-middle",
    "spinner-middle2",
    "spinner-clear",
    "spinner-spin",
    "spinner-approachcircle",
    "scorebar-bg",
    "scorebar-ki",
    "scorebar-marker",
    "play-skip",
    "ranking-x",
    "ranking-s",
    "ranking-a",
    "ranking-b",
    "ranking-c",
    "ranking-d",
    "ranking-xh",
    "ranking-sh",
    "selection-mod-easy",
    "selection-mod-nofail",
    "selection-mod-halftime",
    "selection-mod-hardrock",
    "selection-mod-suddendeath",
    "selection-mod-perfect",
    "selection-mod-doubletime",
    "selection-mod-nightcore",
    "selection-mod-hidden",
    "selection-mod-flashlight",
    "selection-mod-relax",
    "selection-mod-relax2",
    "selection-mod-spunout",
    "selection-mod-autoplay",
    "selection-mod-cinema",
    "selection-mod-scorev2",
};

constexpr const char* kAnimatedElements[] = {
    "hit0", "hit50", "hit100", "hit300", "followpoint", "scorebar-colour",
};

}  // namespace

SkinConfig parse_skin_ini(std::string_view text) {
  SkinConfig                         config;
  std::string                        section;
  std::vector<std::pair<i32, Color>> combos;
  for (std::string_view line : split(text, '\n')) {
    line = trim(line);
    if (line.empty() || line.starts_with("//"))
      continue;
    if (line.front() == '[') {
      section = lowercase(line.substr(1, line.find(']') - 1));
      continue;
    }
    const size_t colon = line.find(':');
    if (colon == line.npos)
      continue;
    const std::string      key = lowercase(trim(line.substr(0, colon)));
    const std::string_view value = trim(
        line.substr(colon + 1).substr(0, line.substr(colon + 1).find("//")));
    if (section == "general") {
      if (key == "name")
        config.name = value;
      else if (key == "cursorrotate")
        config.cursor_rotate = parse_flag(value);
      else if (key == "cursorexpand")
        config.cursor_expand = parse_flag(value);
      else if (key == "cursorcentre")
        config.cursor_centre = parse_flag(value);
      // osu! has always accepted this misspelling, which many skins use.
      else if (key == "hitcircleoverlayabovenumber" ||
               key == "hitcircleoverlayabovenumer")
        config.overlay_above_number = parse_flag(value);
      else if (key == "allowsliderballtint")
        config.allow_slider_ball_tint = parse_flag(value);
      else if (key == "animationframerate")
        config.animation_framerate = static_cast<f32>(field_value(value));
    } else if (section == "colours") {
      const auto colour = parse_colour(value);
      if (!colour)
        continue;
      if (key.starts_with("combo") && key.size() == 6 && key[5] >= '1' &&
          key[5] <= '8')
        combos.push_back({key[5] - '0', *colour});
      else if (key == "sliderborder")
        config.slider_border = colour;
      else if (key == "slidertrackoverride")
        config.slider_track = colour;
      else if (key == "sliderball")
        config.slider_ball = colour;
    } else if (section == "fonts") {
      if (key == "hitcircleprefix")
        config.hit_circle_prefix = font_prefix(value);
      else if (key == "scoreprefix")
        config.score_prefix = font_prefix(value);
      else if (key == "comboprefix")
        config.combo_prefix = font_prefix(value);
      else if (key == "hitcircleoverlap")
        config.hit_circle_overlap = static_cast<f32>(field_value(value));
      else if (key == "scoreoverlap")
        config.score_overlap = static_cast<f32>(field_value(value));
      else if (key == "combooverlap")
        config.combo_overlap = static_cast<f32>(field_value(value));
    }
  }
  std::stable_sort(combos.begin(), combos.end(),
                   [](const auto& a, const auto& b) {
                     return a.first < b.first;
                   });
  for (const auto& [number, colour] : combos)
    config.combo_colours.push_back(colour);
  return config;
}

std::vector<std::string> list_skins(const fs::path& directory) {
  std::vector<std::string> names;
  std::error_code          error;
  for (const auto& entry : fs::directory_iterator(directory, error))
    if (entry.is_directory())
      names.push_back(entry.path().filename().string());
  std::sort(names.begin(), names.end());
  return names;
}

Skin load_skin(const fs::path& directory) {
  Skin skin;
  skin.directory = directory;
  std::error_code error;
  for (const auto& entry : fs::recursive_directory_iterator(directory, error))
    if (entry.is_regular_file())
      skin.files[lowercase(
          entry.path().lexically_relative(directory).generic_string())] =
          entry.path();

  if (const auto ini = skin.files.find("skin.ini"); ini != skin.files.end()) {
    std::ifstream     file(ini->second, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
    skin.config = parse_skin_ini(text);
  }
  if (skin.config.name.empty())
    skin.config.name = directory.filename().string();

  for (const char* name : kElements)
    load_element(skin, name);
  for (const char* name : kAnimatedElements)
    load_element(skin, name, "-");
  // The slider ball's frames are numbered without a separator.
  load_element(skin, "sliderb", "");
  for (const std::string& prefix :
       {skin.config.hit_circle_prefix, skin.config.score_prefix,
        skin.config.combo_prefix}) {
    for (char c : std::string_view("0123456789,.%x"))
      if (!skin.elements.contains(number_glyph(prefix, c)))
        load_element(skin, number_glyph(prefix, c));
  }
  return skin;
}

void destroy_skin(Skin& skin) {
  for (auto& [name, frames] : skin.elements)
    for (auto& frame : frames)
      destroy_texture(frame.texture);
  skin = Skin{};
}

const std::vector<SkinImage>* skin_element(const Skin&      skin,
                                           std::string_view name) {
  const auto element = skin.elements.find(name);
  return element == skin.elements.end() ? nullptr : &element->second;
}

const SkinImage& skin_frame(const Skin&                   skin,
                            const std::vector<SkinImage>& frames,
                            f64                           elapsed) {
  const f64  framerate = skin.config.animation_framerate > 0
                             ? skin.config.animation_framerate
                             : static_cast<f64>(frames.size());
  const auto frame =
      static_cast<i64>(std::floor(std::max(0.0, elapsed) * framerate));
  return frames[std::min<size_t>(static_cast<size_t>(frame),
                                 frames.size() - 1)];
}

fs::path skin_file(const Skin&                        skin,
                   std::string_view                   stem,
                   std::initializer_list<const char*> extensions) {
  for (const char* extension : extensions) {
    const auto file = skin.files.find(lowercase(stem) + extension);
    if (file != skin.files.end())
      return file->second;
  }
  return {};
}

void draw_skin_image(Renderer&        r,
                     const SkinImage& image,
                     f32              x,
                     f32              y,
                     f32              scale,
                     Color            tint,
                     f32              angle) {
  draw_sprite(r, image.texture, x, y, image.width() * scale,
              image.height() * scale, angle, tint);
}

std::optional<f32> skin_number_width(const Skin&      skin,
                                     std::string_view prefix,
                                     std::string_view text,
                                     f32              scale,
                                     f32              overlap) {
  f32 width = overlap * scale;
  for (char c : text) {
    const auto* frames = skin_element(skin, number_glyph(prefix, c));
    if (!frames)
      return std::nullopt;
    width += (frames->front().width() - overlap) * scale;
  }
  return width;
}

std::optional<f32> draw_skin_number(Renderer&        r,
                                    const Skin&      skin,
                                    std::string_view prefix,
                                    std::string_view text,
                                    f32              x,
                                    f32              center_y,
                                    f32              scale,
                                    f32              overlap,
                                    Color            tint,
                                    Align            align) {
  std::vector<const SkinImage*> glyphs;
  f32                           width = 0;
  for (char c : text) {
    const auto* frames = skin_element(skin, number_glyph(prefix, c));
    if (!frames)
      return std::nullopt;
    glyphs.push_back(&frames->front());
    width += (glyphs.back()->width() - overlap) * scale;
  }
  width += overlap * scale;
  if (align != Align::Left)
    x -= align == Align::Center ? width / 2 : width;
  for (const SkinImage* glyph : glyphs) {
    const f32 glyph_width = glyph->width() * scale;
    draw_skin_image(r, *glyph, x + glyph_width / 2, center_y, scale, tint);
    x += glyph_width - overlap * scale;
  }
  return width;
}

}  // namespace circles
