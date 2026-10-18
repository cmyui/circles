#include <circles/mods.h>
#include <circles/rules.h>

#include <algorithm>

namespace circles {

namespace {

constexpr ModButton kReductionButtons[] = {
    {{kEasy, 0}, {"Easy"}, {"EZ"}, {"selection-mod-easy"}, 'Q'},
    {{kNoFail, 0}, {"No Fail"}, {"NF"}, {"selection-mod-nofail"}, 'W'},
    {{kHalfTime, 0}, {"Half Time"}, {"HT"}, {"selection-mod-halftime"}, 'E'},
};

constexpr ModButton kIncreaseButtons[] = {
    {{kHardRock, 0}, {"Hard Rock"}, {"HR"}, {"selection-mod-hardrock"}, 'A'},
    {{kSuddenDeath, kSuddenDeath | kPerfect},
     {"Sudden Death", "Perfect"},
     {"SD", "PF"},
     {"selection-mod-suddendeath", "selection-mod-perfect"},
     'S'},
    {{kDoubleTime, kDoubleTime | kNightcore},
     {"Double Time", "Nightcore"},
     {"DT", "NC"},
     {"selection-mod-doubletime", "selection-mod-nightcore"},
     'D'},
    {{kHidden, 0}, {"Hidden"}, {"HD"}, {"selection-mod-hidden"}, 'F'},
    {{kFlashlight, 0},
     {"Flashlight"},
     {"FL"},
     {"selection-mod-flashlight"},
     'G'},
};

constexpr ModButton kSpecialButtons[] = {
    {{kRelax, 0}, {"Relax"}, {"RX"}, {"selection-mod-relax"}, 'Z'},
    {{kAutopilot, 0}, {"Autopilot"}, {"AP"}, {"selection-mod-relax2"}, 'X'},
    {{kSpunOut, 0}, {"Spun Out"}, {"SO"}, {"selection-mod-spunout"}, 'C'},
    {{kAutoplay, kAutoplay | kCinema},
     {"Auto", "Cinema"},
     {"AT", "CN"},
     {"selection-mod-autoplay", "selection-mod-cinema"},
     'V'},
    {{kScoreV2, 0}, {"Score V2"}, {"V2"}, {"selection-mod-scorev2"}, 'B'},
};

constexpr ModRow kRows[] = {
    {ModGroup::Reduction, "Difficulty Reduction", kReductionButtons},
    {ModGroup::Increase, "Difficulty Increase", kIncreaseButtons},
    {ModGroup::Special, "Special", kSpecialButtons},
};

// osu!stable's rules for which mods a mod turns off. Perfect, Nightcore and
// Cinema carry Sudden Death's, Double Time's and Auto's bits, so their rules.
struct Conflict {
  Mods mod;
  Mods excludes;
};

constexpr Conflict kConflicts[] = {
    {kNoFail, kSuddenDeath | kPerfect | kRelax | kAutopilot},
    {kEasy, kHardRock},
    {kHardRock, kEasy},
    {kSuddenDeath, kNoFail | kRelax | kAutopilot | kAutoplay | kCinema},
    {kDoubleTime, kHalfTime},
    {kHalfTime, kDoubleTime | kNightcore},
    {kRelax,
     kNoFail | kSuddenDeath | kPerfect | kAutopilot | kAutoplay | kCinema},
    {kAutopilot, kNoFail | kSuddenDeath | kPerfect | kRelax | kSpunOut |
                     kAutoplay | kCinema},
    {kSpunOut, kAutopilot | kAutoplay | kCinema},
    {kAutoplay, kSuddenDeath | kPerfect | kRelax | kAutopilot | kSpunOut},
};

// A button's mods as one set, to clear it.
Mods button_mods(const ModButton& button) {
  return button.mods[0] | button.mods[1];
}

}  // namespace

std::span<const ModRow> mod_rows() {
  return kRows;
}

i32 mod_button_state(Mods mods, const ModButton& button) {
  if (button.mods[1] && (mods & button.mods[1]) == button.mods[1])
    return 2;
  return (mods & button.mods[0]) == button.mods[0] ? 1 : 0;
}

Mods press_mod_button(Mods mods, const ModButton& button) {
  const i32 state = mod_button_state(mods, button);
  mods &= ~button_mods(button);
  if (state == 2 || (state == 1 && !button.mods[1]))
    return mods;
  const Mods chosen = button.mods[state == 1 ? 1 : 0];
  for (const Conflict& conflict : kConflicts)
    if (chosen & conflict.mod)
      mods &= ~conflict.excludes;
  return mods | chosen;
}

Difficulty apply_mods(Difficulty d, Mods mods) {
  if (mods & kHardRock) {
    d.cs = std::min(10.0, d.cs * 1.3);
    d.ar = std::min(10.0, d.ar * 1.4);
    d.od = std::min(10.0, d.od * 1.4);
    d.hp = std::min(10.0, d.hp * 1.4);
  }
  if (mods & kEasy) {
    d.cs *= 0.5;
    d.ar *= 0.5;
    d.od *= 0.5;
    d.hp *= 0.5;
  }
  return d;
}

Difficulty displayed_difficulty(Difficulty d, Mods mods) {
  d = apply_mods(d, mods);
  const f64 rate = mod_rate(mods);
  const f64 preempt = approach_preempt(d.ar) / rate;
  d.ar =
      preempt > 1200 ? 5 - (preempt - 1200) / 120 : 5 + (1200 - preempt) / 150;
  d.od = (80 - (80 - 6 * d.od) / rate) / 6;
  return d;
}

f64 mod_rate(Mods mods) {
  if (mods & kDoubleTime)
    return 1.5;
  if (mods & kHalfTime)
    return 0.75;
  return 1;
}

f64 mod_score_multiplier(Mods mods) {
  f64 multiplier = 1;
  if (mods & kEasy)
    multiplier *= 0.5;
  if (mods & kNoFail)
    multiplier *= 0.5;
  if (mods & kHalfTime)
    multiplier *= 0.3;
  // Score V2 rewards Hard Rock and Double Time more.
  const bool v2 = mods & kScoreV2;
  if (mods & kHardRock)
    multiplier *= v2 ? 1.10 : 1.06;
  if (mods & kDoubleTime)
    multiplier *= v2 ? 1.20 : 1.12;
  if (mods & kHidden)
    multiplier *= 1.06;
  if (mods & kFlashlight)
    multiplier *= 1.12;
  if (mods & kSpunOut)
    multiplier *= 0.9;
  if (mods & (kRelax | kAutopilot))
    multiplier = 0;
  return multiplier;
}

std::string mod_acronyms(Mods mods) {
  std::string text;
  for (const ModRow& row : kRows)
    for (const ModButton& button : row.buttons)
      if (const i32 state = mod_button_state(mods, button))
        text += button.acronyms[state - 1];
  return text;
}

}  // namespace circles
