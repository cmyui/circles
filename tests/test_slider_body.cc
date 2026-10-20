// Slider bodies' pieces as osu!stable's SliderOsu.Draw merges them, fully
// snaked and while snaking.
#include <circles/slider_body.h>
#include <tests/support/test.h>

#include <fosu/parser.h>
#include <string>

namespace {

circles::BodyShape shape(const std::string& slider) {
  static fosu::Parser parser;
  const std::string   input =
      "osu file format v14\n[Difficulty]\nSliderMultiplier:1\n"
      "[TimingPoints]\n0,500,4,1,0,100,1,0\n[HitObjects]\n" +
      slider + "\n";
  const fosu::Beatmap* map =
      parser.parse(input, {.calculate_slider_paths = true});
  const auto& hit = map->hit_objects[0];
  return circles::build_body_shape(map->slider_paths[hit.slider],
                                   {hit.x, hit.y},
                                   map->sliders[hit.slider].length);
}

circles::f32 span(const circles::BodyShape& s,
                  circles::u32              first,
                  circles::u32              last) {
  return (s.lines[last].p2 - s.lines[first].p1).length();
}

}  // namespace

// Each straight line ends a segment, so each is a piece.
static void test_straight_lines_stay_apart() {
  const auto s = shape("0,0,1000,2,0,L|10:0|10:10|20:10,1,30");
  CHECK_EQ(s.lines.size(), 3u);
  CHECK_EQ(s.piece_ends.size(), 3u);
}

// A curve's lines merge until a piece spans more than 6 px, except at the
// last two lines, which end pieces of their own.
static void test_curves_merge_past_six_pixels() {
  const auto s = shape("0,0,1000,2,0,B|100:100|200:0,1,250");
  CHECK(s.piece_ends.size() < s.lines.size());
  circles::u32 first = 0;
  for (size_t p = 0; p < s.piece_ends.size(); ++p) {
    const circles::u32 last = s.piece_ends[p];
    if (last + 2 < s.lines.size()) {
      CHECK(span(s, first, last) > 6);
      if (last > first)
        CHECK(span(s, first, last - 1) <= 6);
    }
    first = last + 1;
  }
}

// Snaking shows the whole body's first pieces, then the rest of the way to
// the snaking end in up to two, the last cut where the body ends.
static void test_snaking_ends_in_a_cut_piece() {
  const auto s = shape("0,0,1000,2,0,B|100:100|200:0,1,250");
  const auto half = circles::snake_state(s, 0.5f);
  CHECK(!half.done);
  CHECK_EQ(half.tail_count, 2u);
  CHECK(half.full < s.piece_ends.size());
  CHECK_EQ(half.settled(), half.full - 1);
  const auto end = circles::piece_at(s, half, half.count() - 1);
  // The cut lands half way along the body.
  CHECK(end.cut > 0);
  const double line_start = end.last ? s.lengths[end.last - 1] : 0;
  CHECK_NEAR(line_start + end.cut, s.length / 2, 1e-3);

  const auto whole = circles::snake_state(s, 1);
  CHECK(whole.done);
  CHECK_EQ(whole.count(), s.piece_ends.size());
  CHECK_EQ(whole.settled(), whole.count());
}

int main() {
  test_straight_lines_stay_apart();
  test_curves_merge_past_six_pixels();
  test_snaking_ends_in_a_cut_piece();
  return test_result();
}
