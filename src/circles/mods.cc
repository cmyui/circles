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

// osu!stable's rule for one mod: the mods it turns off, or the mod it
// needs.
Mods apply_rule(Mods mods, Mods mod) {
  switch (mod) {
    case kCinema:
      return mods | kAutoplay;
    case kNoFail:
      return mods & ~(kSuddenDeath | kPerfect | kAutopilot | kRelax);
    case kEasy:
      return mods & ~kHardRock;
    case kHardRock:
      return mods & ~kEasy;
    case kSuddenDeath:
      return mods & ~(kNoFail | kAutopilot | kRelax);
    case kPerfect:
      return mods | kSuddenDeath;
    case kDoubleTime:
      return mods & ~kHalfTime;
    case kNightcore:
      return mods | kDoubleTime;
    case kHalfTime:
      return mods & ~(kDoubleTime | kNightcore);
    case kRelax:
      return mods & ~(kAutopilot | kNoFail | kSuddenDeath | kPerfect);
    case kAutopilot:
      return mods & ~(kSpunOut | kRelax | kNoFail | kSuddenDeath | kPerfect);
    case kSpunOut:
      return mods & ~kAutopilot;
    default:
      return mods;
  }
}

// Stable's check after any change: every mod's rule in bit order, then Auto
// overrules the mods that play for you or fail you.
Mods enforce_rules(Mods mods) {
  for (Mods mod = 1; mod < kScoreV2; mod <<= 1)
    if (mods & mod)
      mods = apply_rule(mods, mod);
  if (mods & kAutoplay)
    mods &= ~(kSpunOut | kRelax | kAutopilot | kSuddenDeath | kPerfect);
  return mods;
}

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
    return enforce_rules(mods);
  // The chosen mod's own rule first, then everything's.
  const Mods chosen = button.mods[state == 1 ? 1 : 0];
  const Mods mod = state == 1 ? button.mods[1] & ~button.mods[0] : chosen;
  return enforce_rules(apply_rule(mods | chosen, mod));
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
  // Score V2 rewards Hard Rock and Double Time more, applied first.
  f64 multiplier = 1;
  if (mods & kScoreV2) {
    if (mods & kHardRock)
      multiplier *= 1.10;
    if (mods & kDoubleTime)
      multiplier *= 1.20;
    mods &= ~(kHardRock | kDoubleTime);
  }
  // In osu!stable's order, which the floating-point result depends on.
  f64 rest = 1;
  if (mods & kNoFail)
    rest *= 0.5;
  if (mods & kEasy)
    rest *= 0.5;
  if (mods & kHalfTime)
    rest *= 0.3;
  if (mods & kHidden)
    rest *= 1.06;
  if (mods & kHardRock)
    rest *= 1.06;
  if (mods & kDoubleTime)
    rest *= 1.12;
  if (mods & kFlashlight)
    rest *= 1.12;
  if (mods & kSpunOut)
    rest *= 0.9;
  if (mods & (kRelax | kAutopilot))
    rest *= 0;
  return multiplier * rest;
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
