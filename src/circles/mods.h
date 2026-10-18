#pragma once

// osu!standard's mods as osu!stable defines them: their bits (as replays
// store them), which combine, and how they change a map's settings, speed
// and score.

#include <circles/types.h>

#include <span>
#include <string>

namespace circles {

using Mods = u32;

inline constexpr Mods kNoFail = 1 << 0;
inline constexpr Mods kEasy = 1 << 1;
inline constexpr Mods kTouchDevice = 1 << 2;
inline constexpr Mods kHidden = 1 << 3;
inline constexpr Mods kHardRock = 1 << 4;
inline constexpr Mods kSuddenDeath = 1 << 5;
inline constexpr Mods kDoubleTime = 1 << 6;
inline constexpr Mods kRelax = 1 << 7;
inline constexpr Mods kHalfTime = 1 << 8;
inline constexpr Mods kNightcore = 1 << 9;  // set with kDoubleTime
inline constexpr Mods kFlashlight = 1 << 10;
inline constexpr Mods kAutoplay = 1 << 11;
inline constexpr Mods kSpunOut = 1 << 12;
inline constexpr Mods kAutopilot = 1 << 13;
inline constexpr Mods kPerfect = 1 << 14;  // set with kSuddenDeath
inline constexpr Mods kCinema = 1 << 22;
inline constexpr Mods kScoreV2 = 1 << 29;

// A mod select button. Some cycle through two mods, as osu!'s do: the second
// replaces the first on the next press.
struct ModButton {
  Mods        mods[2];  // the second is 0 for a single mod
  const char* names[2];
  const char* acronyms[2];
  const char* skin_images[2];  // e.g. "selection-mod-hardrock"
  char        key;             // its keyboard shortcut in mod select
};

enum class ModGroup : u8 { Reduction, Increase, Special };

struct ModRow {
  ModGroup                   group;
  const char*                title;
  std::span<const ModButton> buttons;
};

// Mod select's rows, in osu!stable's order.
std::span<const ModRow> mod_rows();

// Presses a button: turns its mod on, steps a cycling button to its second
// mod, or turns the button off; mods that can't go with the result come off.
Mods press_mod_button(Mods mods, const ModButton& button);
// Which of the button's mods is on: 0, 1 (the first) or 2 (the second).
i32 mod_button_state(Mods mods, const ModButton& button);

// A map's difficulty settings, before or after mods.
struct Difficulty {
  f64 cs;
  f64 ar;
  f64 od;
  f64 hp;
};

Difficulty apply_mods(Difficulty difficulty, Mods mods);
// The settings a map plays like with `mods`: apply_mods, then approach rate
// and accuracy as a speed change makes them feel.
Difficulty displayed_difficulty(Difficulty difficulty, Mods mods);
// How many times as fast the song plays.
f64 mod_rate(Mods mods);
// Whether a rate change also changes pitch, as Nightcore's does.
inline bool mods_change_pitch(Mods mods) {
  return mods & kNightcore;
}
f64 mod_score_multiplier(Mods mods);
// Like "HDDT", or "" for none.
std::string mod_acronyms(Mods mods);

}  // namespace circles
