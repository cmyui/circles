// Judging as play passes objects.
#include <circles/app.h>
#include <circles/hit_objects.h>
#include <circles/judging.h>
#include <circles/rules.h>
#include <tests/support/test.h>

#include <fosu/parser.h>
#include <memory>

namespace {

// A slider from 1000 to 2000 ms, a circle at 1200, and one long after.
constexpr const char* kMap = R"(osu file format v14

[General]
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
100,100,1000,2,0,L|300:100,1,200
256,192,1200,1,0,0:0:0:0:
256,192,20000,1,0,0:0:0:0:
)";

// A play of the map from its start, without a window or audio.
std::unique_ptr<circles::App> play() {
  static fosu::Parser  parser;
  const fosu::Beatmap* map =
      parser.parse(kMap, circles::stable_parse_options(0));
  auto  app = std::make_unique<circles::App>();
  auto& game = app->game;
  game.objects = circles::build_hit_objects(*map, {map->od});
  game.states.assign(game.objects.size(), {});
  game.preempt = circles::approach_preempt(9);
  game.windows = circles::hit_windows(8);
  game.phase = circles::GamePhase::Playing;
  return app;
}

}  // namespace

static void test_a_time_jump_still_judges_the_objects_it_passes() {
  auto  app = play();
  auto& game = app->game;
  CHECK_EQ(game.objects[0].end_time, 2000);
  // Into the slider, then far past it and the circle, as when the clock
  // catches up with the music after the game loop stalls.
  circles::play_step(*app, 1100, {}, 0, true);
  circles::play_step(*app, 10000, {}, 0, true);
  for (size_t i = 0; i < 2; ++i) {
    CHECK(game.states[i].hit);
    CHECK(game.states[i].result == circles::Judgement::Miss);
  }
  CHECK(!game.states[2].hit);
}

int main() {
  test_a_time_jump_still_judges_the_objects_it_passes();
  return test_result();
}
