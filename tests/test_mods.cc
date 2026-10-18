// Mods' effects and combinations, checked against osu!stable's.
#include <circles/mods.h>
#include <tests/support/test.h>

#include <span>

namespace {

const circles::ModButton& button(char key) {
  for (const auto& row : circles::mod_rows())
    for (const auto& button : row.buttons)
      if (button.key == key)
        return button;
  return circles::mod_rows().front().buttons.front();
}

}  // namespace

static void test_hard_rock_and_easy_scale_difficulty() {
  const auto hard_rock = circles::apply_mods({4, 9, 8, 5}, circles::kHardRock);
  CHECK_NEAR(hard_rock.cs, 5.2, 1e-9);
  CHECK_NEAR(hard_rock.ar, 10, 1e-9);  // capped
  CHECK_NEAR(hard_rock.od, 10, 1e-9);
  CHECK_NEAR(hard_rock.hp, 7, 1e-9);
  const auto easy = circles::apply_mods({4, 9, 8, 5}, circles::kEasy);
  CHECK_NEAR(easy.cs, 2, 1e-9);
  CHECK_NEAR(easy.ar, 4.5, 1e-9);
}

static void test_speed_mods_change_rate_and_feel() {
  CHECK_EQ(circles::mod_rate(circles::kDoubleTime), 1.5);
  CHECK_EQ(circles::mod_rate(circles::kDoubleTime | circles::kNightcore), 1.5);
  CHECK_EQ(circles::mod_rate(circles::kHalfTime), 0.75);
  CHECK(circles::mods_change_pitch(circles::kDoubleTime | circles::kNightcore));
  CHECK(!circles::mods_change_pitch(circles::kDoubleTime));
  // AR 9 at one and a half speed appears as AR 10.33.
  const auto feel =
      circles::displayed_difficulty({4, 9, 8, 5}, circles::kDoubleTime);
  CHECK_NEAR(feel.ar, 10.333, 0.001);
  CHECK_NEAR(feel.od, 9.778, 0.001);
}

static void test_buttons_cycle_and_turn_off_conflicting_mods() {
  using namespace circles;
  Mods mods = press_mod_button(0, button('D'));
  CHECK_EQ(mods, kDoubleTime);
  mods = press_mod_button(mods, button('D'));
  CHECK_EQ(mods, kDoubleTime | kNightcore);
  CHECK_EQ(press_mod_button(mods, button('D')), 0u);
  CHECK_EQ(press_mod_button(kDoubleTime | kHidden, button('E')),
           kHalfTime | kHidden);
  CHECK_EQ(press_mod_button(kEasy, button('A')), kHardRock);
  CHECK_EQ(press_mod_button(kSuddenDeath, button('W')), kNoFail);
  CHECK_EQ(press_mod_button(kRelax | kHidden, button('V')),
           kAutoplay | kHidden);
  // Some special mods do combine.
  CHECK_EQ(press_mod_button(kRelax, button('C')), kRelax | kSpunOut);
  CHECK_EQ(press_mod_button(kNoFail, button('V')), kNoFail | kAutoplay);
}

static void test_score_multipliers_combine() {
  using namespace circles;
  CHECK_NEAR(mod_score_multiplier(kHidden | kHardRock), 1.06 * 1.06, 1e-9);
  CHECK_NEAR(mod_score_multiplier(kEasy | kNoFail), 0.25, 1e-9);
  CHECK_NEAR(mod_score_multiplier(kDoubleTime | kScoreV2), 1.2, 1e-9);
  CHECK_EQ(mod_score_multiplier(kRelax | kHidden), 0);
  CHECK_EQ(mod_acronyms(kHidden | kDoubleTime | kNightcore), "NCHD");
}

int main() {
  test_hard_rock_and_easy_scale_difficulty();
  test_speed_mods_change_rate_and_feel();
  test_buttons_cycle_and_turn_off_conflicting_mods();
  test_score_multipliers_combine();
  return test_result();
}
