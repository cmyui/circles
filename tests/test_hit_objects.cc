// Hit objects built from fosu's stable calculations: slider timing and
// scoring points, stacking, Hard Rock's flip and combo colours.
#include <circles/hit_objects.h>
#include <circles/rules.h>
#include <tests/support/test.h>

#include <cmath>
#include <fosu/parser.h>
#include <vector>

namespace {

constexpr const char* kMap = R"(osu file format v14

[General]
StackLeniency: 0.7
Mode: 0

[Difficulty]
HPDrainRate:5
CircleSize:4
OverallDifficulty:8
ApproachRate:9
SliderMultiplier:1
SliderTickRate:1

[TimingPoints]
0,500,4,1,0,100,1,0

[HitObjects]
100,100,1000,6,0,L|200:100,1,100
100,300,6000,2,0,L|400:300,2,300
256,192,9000,1,0,0:0:0:0:
256,192,9100,1,0,0:0:0:0:
)";

std::vector<circles::GameObject> build(circles::Mods mods = 0) {
  static fosu::Parser  parser;
  const fosu::Beatmap* map =
      parser.parse(kMap, circles::stable_parse_options(mods));
  return circles::build_hit_objects(*map, {map->od});
}

}  // namespace

static void test_sliders_take_their_time_and_points_from_velocity() {
  const auto  objects = build();
  // 100 pixels at 200 pixels a second; too short for a tick, and the end is
  // checked 36 ms early.
  const auto& short_slider = objects[0];
  CHECK_EQ(short_slider.end_time, 1500);
  CHECK_EQ(short_slider.points.size(), 1u);
  CHECK(short_slider.points[0].kind == circles::PointKind::End);
  CHECK_EQ(short_slider.points[0].time, 1464);

  // 300 pixels there and back, a tick every 100 pixels on each pass.
  const auto& repeating = objects[1];
  CHECK_EQ(repeating.end_time, 9000);
  const std::vector<circles::i32> times = {6500, 7000, 7500, 8000, 8500, 8964};
  CHECK_EQ(repeating.points.size(), times.size());
  for (size_t i = 0; i < times.size() && i < repeating.points.size(); ++i)
    CHECK_EQ(repeating.points[i].time, times[i]);
  CHECK(repeating.points[2].kind == circles::PointKind::Repeat);
  CHECK(repeating.points[5].kind == circles::PointKind::End);
  CHECK_NEAR(repeating.end_position.x, 100, 1e-3);
}

static void test_close_objects_stack_up_and_left() {
  const auto         objects = build();
  // A tenth of the circle's radius.
  const circles::f32 offset =
      static_cast<circles::f32>(circles::circle_radius(4)) / 10;
  CHECK_EQ(objects[2].stack_count, 1);
  CHECK_NEAR(objects[2].position.x, 256 - offset, 1e-3);
  CHECK_NEAR(objects[2].position.y, 192 - offset, 1e-3);
  CHECK_EQ(objects[3].stack_count, 0);
}

static void test_hard_rock_flips_the_playfield() {
  const auto objects = build(circles::kHardRock);
  CHECK_NEAR(objects[0].position.y, 384 - 100, 1e-3);
  CHECK_NEAR(objects[1].path.back().y, 384 - 300, 1e-3);
}

static void test_combos_count_from_the_second_colour() {
  const auto objects = build();
  CHECK(objects[0].new_combo);
  CHECK_EQ(objects[0].combo_number, 1);
  CHECK_EQ(objects[0].colour, 1);
  CHECK_EQ(objects[3].combo_number, 4);
}

int main() {
  test_sliders_take_their_time_and_points_from_velocity();
  test_close_objects_stack_up_and_left();
  test_hard_rock_flips_the_playfield();
  test_combos_count_from_the_second_colour();
  return test_result();
}
