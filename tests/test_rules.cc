// osu!standard's difficulty conversions and scoring, checked against values
// osu! uses.
#include <circles/rules.h>
#include <tests/support/test.h>

static void test_difficulty_settings_convert_to_gameplay_values() {
  // Stable widens circles by 0.041%.
  CHECK_NEAR(circles::circle_radius(4), 36.48 * 1.00041, 1e-9);
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

static void test_spinners_are_judged_in_half_turns() {
  using circles::Judgement;
  // The requirement earns a 50; each half turn more is a step up.
  CHECK(circles::spinner_judgement(14, 15) == Judgement::Miss);
  CHECK(circles::spinner_judgement(15, 15) == Judgement::Hit50);
  CHECK(circles::spinner_judgement(16, 15) == Judgement::Hit100);
  CHECK(circles::spinner_judgement(17, 15) == Judgement::Hit300);
  CHECK(circles::spinner_judgement(0, 0) == Judgement::Hit50);
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
  CHECK_NEAR(circles::accuracy({.hit300 = 1, .hit100 = 1}), 400.0 / 600, 1e-6);
  CHECK(circles::grade({.hit300 = 100}) == Grade::SS);
  CHECK(circles::grade({.hit300 = 95, .hit100 = 5}) == Grade::S);
  // A miss rules out S, but enough 300s still make an A.
  CHECK(circles::grade({.hit300 = 95, .hit100 = 4, .miss = 1}) == Grade::A);
  CHECK(circles::grade({.hit300 = 95, .hit50 = 5}) == Grade::A);
  // Exactly 1% 50s still allows an S.
  CHECK(circles::grade({.hit300 = 99, .hit50 = 1}) == Grade::S);
  // Stable's single-precision ratio makes exactly 80% 300s count as more.
  CHECK(circles::grade({.hit300 = 80, .hit100 = 20}) == Grade::A);
  CHECK(circles::grade({}) == Grade::D);
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
  test_spinners_are_judged_in_half_turns();
  test_score_grows_with_combo();
  test_accuracy_and_grades();
  return test_result();
}
