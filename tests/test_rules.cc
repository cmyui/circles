// osu!standard's difficulty conversions and scoring, checked against values
// osu! uses.
#include <circles/rules.h>
#include <tests/support/test.h>

static void test_difficulty_settings_convert_to_gameplay_values() {
  CHECK_NEAR(circles::circle_radius(4), 36.48, 1e-9);
  CHECK_EQ(circles::approach_preempt(0), 1800);
  CHECK_EQ(circles::approach_preempt(5), 1200);
  CHECK_EQ(circles::approach_preempt(9), 600);
  CHECK_EQ(circles::approach_preempt(10), 450);
  CHECK_EQ(circles::approach_fade_in(450), 400);
  CHECK_EQ(circles::approach_fade_in(225), 200);
  const auto windows = circles::hit_windows(8);
  CHECK_EQ(windows.hit300, 32);
  CHECK_EQ(windows.hit100, 76);
  CHECK_EQ(windows.hit50, 120);
  // Fractional windows truncate to whole ms.
  CHECK_EQ(circles::hit_windows(8.5).hit300, 29);
}

static void test_presses_are_judged_by_error_on_either_side() {
  using circles::Judgement;
  const auto windows = circles::hit_windows(5);  // 50, 100 and 150 ms
  CHECK(circles::judge_press(windows, -49) == Judgement::Hit300);
  CHECK(circles::judge_press(windows, 49) == Judgement::Hit300);
  // A window's edge belongs to the next judgement.
  CHECK(circles::judge_press(windows, 50) == Judgement::Hit100);
  CHECK(circles::judge_press(windows, -149) == Judgement::Hit50);
  CHECK(circles::judge_press(windows, 150) == Judgement::Miss);
}

static void test_sliders_are_judged_by_the_fraction_of_parts_hit() {
  using circles::Judgement;
  CHECK(circles::slider_judgement(4, 4) == Judgement::Hit300);
  CHECK(circles::slider_judgement(3, 4) == Judgement::Hit100);
  CHECK(circles::slider_judgement(2, 4) == Judgement::Hit100);
  CHECK(circles::slider_judgement(1, 4) == Judgement::Hit50);
  CHECK(circles::slider_judgement(0, 4) == Judgement::Miss);
}

static void test_spinners_need_more_turns_at_higher_od() {
  using circles::Judgement;
  CHECK_NEAR(circles::spins_per_second(5), 5, 1e-9);
  CHECK_NEAR(circles::spins_per_second(0), 3, 1e-9);
  CHECK_NEAR(circles::spins_per_second(10), 7.5, 1e-9);
  CHECK_EQ(circles::required_spins(3000, 5), 9);
  CHECK_EQ(circles::required_spins(100, 0), 1);
  CHECK(circles::spinner_judgement(15, 15) == Judgement::Hit300);
  CHECK(circles::spinner_judgement(13.5, 15) == Judgement::Hit100);
  CHECK(circles::spinner_judgement(11.25, 15) == Judgement::Hit50);
  CHECK(circles::spinner_judgement(11, 15) == Judgement::Miss);
}

static void test_score_grows_with_combo() {
  using circles::Judgement;
  CHECK_EQ(circles::hit_score(Judgement::Hit300, 1, 4), 300);
  CHECK_EQ(circles::hit_score(Judgement::Hit300, 26, 4), 1500);
  CHECK_EQ(circles::hit_score(Judgement::Hit100, 26, 4), 500);
  CHECK_EQ(circles::hit_score(Judgement::Miss, 50, 4), 0);
  CHECK_EQ(circles::difficulty_multiplier(5, 4, 8, 400, 100), 4);
}

static void test_accuracy_and_grades() {
  using circles::Counts;
  using circles::Grade;
  CHECK_NEAR(circles::accuracy({.hit300 = 1, .hit100 = 1}), 400.0 / 600, 1e-9);
  CHECK(circles::grade({.hit300 = 100}) == Grade::SS);
  CHECK(circles::grade({.hit300 = 95, .hit100 = 5}) == Grade::S);
  // A miss rules out S, but enough 300s still make an A.
  CHECK(circles::grade({.hit300 = 95, .hit100 = 4, .miss = 1}) == Grade::A);
  CHECK(circles::grade({.hit300 = 95, .hit50 = 5}) == Grade::A);
  CHECK(circles::grade({.hit300 = 85, .hit100 = 15}) == Grade::A);
  CHECK(circles::grade({.hit300 = 85, .hit100 = 14, .miss = 1}) == Grade::B);
  CHECK(circles::grade({.hit300 = 75, .hit100 = 25}) == Grade::B);
  CHECK(circles::grade({.hit300 = 65, .hit100 = 35}) == Grade::C);
  CHECK(circles::grade({.hit300 = 50, .hit100 = 50}) == Grade::D);
}

int main() {
  test_difficulty_settings_convert_to_gameplay_values();
  test_presses_are_judged_by_error_on_either_side();
  test_sliders_are_judged_by_the_fraction_of_parts_hit();
  test_spinners_need_more_turns_at_higher_od();
  test_score_grows_with_combo();
  test_accuracy_and_grades();
  return test_result();
}
