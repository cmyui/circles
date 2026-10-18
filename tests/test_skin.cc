// skin.ini parsing, including quirks real skins rely on.
#include <circles/skin.h>
#include <tests/support/test.h>

namespace {

bool same_colour(circles::Color a, circles::Color b) {
  return std::abs(a.r - b.r) < 1e-6f && std::abs(a.g - b.g) < 1e-6f &&
         std::abs(a.b - b.b) < 1e-6f;
}

}  // namespace

static void test_general_settings_accept_the_misspelled_overlay_key() {
  const auto config = circles::parse_skin_ini(
      "[General]\r\nName: Amaestric\r\nCursorRotate: 0\r\nCursorCentre: 1\r\n"
      "HitCircleOverlayAboveNumer: 0\r\nAnimationFramerate: 24\r\n");
  CHECK_EQ(config.name, "Amaestric");
  CHECK(!config.cursor_rotate);
  CHECK(config.cursor_centre);
  CHECK(!config.overlay_above_number);
  CHECK_EQ(config.animation_framerate, 24);
}

static void test_combo_colours_follow_their_numbers_not_file_order() {
  const auto config = circles::parse_skin_ini(
      "[Colours]\nCombo2: 185,138,255 \n//Combo3: 1,2,3\n"
      "Combo1: 255,149,215\nCombo9: 1,1,1\nSliderBorder: 140,140,140\n"
      "SliderTrackOverride: 0,0,0\n");
  CHECK_EQ(config.combo_colours.size(), 2u);
  CHECK(same_colour(config.combo_colours[0], circles::hex_color(0xFF95D7)));
  CHECK(same_colour(config.combo_colours[1], circles::hex_color(0xB98AFF)));
  CHECK(config.slider_border &&
        same_colour(*config.slider_border, circles::hex_color(0x8C8C8C)));
  CHECK(config.slider_track &&
        same_colour(*config.slider_track, circles::hex_color(0x000000)));
}

static void test_font_prefixes_are_case_insensitive_paths() {
  const auto config = circles::parse_skin_ini(
      "[Fonts]\nHitCirclePrefix: Fonts\\Default\nScoreOverlap: 3\n"
      "HitCircleOverlap: 50 // tighter digits\n");
  CHECK_EQ(config.hit_circle_prefix, "fonts/default");
  CHECK_EQ(config.score_overlap, 3);
  CHECK_EQ(config.hit_circle_overlap, 50);
}

static void test_missing_settings_keep_osu_defaults() {
  const auto config = circles::parse_skin_ini("");
  CHECK(config.name.empty());
  CHECK(config.overlay_above_number);
  CHECK_EQ(config.hit_circle_prefix, "default");
  CHECK_EQ(config.combo_prefix, "score");
  CHECK_EQ(config.hit_circle_overlap, -2);
  CHECK(config.combo_colours.empty());
  CHECK(!config.slider_track);
}

static void test_keys_only_count_in_their_section() {
  const auto config =
      circles::parse_skin_ini("[Mania]\nName: wrong\nCombo1: 1,2,3\n");
  CHECK(config.name.empty());
  CHECK(config.combo_colours.empty());
}

int main() {
  test_general_settings_accept_the_misspelled_overlay_key();
  test_combo_colours_follow_their_numbers_not_file_order();
  test_font_prefixes_are_case_insensitive_paths();
  test_missing_settings_keep_osu_defaults();
  test_keys_only_count_in_their_section();
  return test_result();
}
