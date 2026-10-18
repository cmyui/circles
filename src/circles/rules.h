#pragma once

// osu!standard's conversions from difficulty settings to gameplay values, and
// its scoring. Times are milliseconds and lengths are osu! pixels (the
// playfield is 512x384).

#include <circles/types.h>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace circles {

enum class Judgement : u8 { None, Miss, Hit50, Hit100, Hit300 };

inline constexpr f32 kPlayfieldWidth = 512;
inline constexpr f32 kPlayfieldHeight = 384;

// Presses earlier than this before an object are ignored rather than missed.
inline constexpr f64 kMissWindow = 400;

// osu!stable's interpolation of a difficulty setting: `mid` at 5, `min` at
// 0 and `max` at 10.
inline f64 map_difficulty_range(f64 difficulty, f64 min, f64 mid, f64 max) {
  if (difficulty > 5)
    return mid + (max - mid) * (difficulty - 5) / 5;
  if (difficulty < 5)
    return mid - (mid - min) * (5 - difficulty) / 5;
  return mid;
}

// Stable widens circles by a hair to make up for an old rounding error.
inline f64 circle_radius(f64 cs) {
  return (54.4 - 4.48 * cs) * 1.00041;
}

// How long before its time an object appears.
// Whole ms, as osu!stable truncates it.
inline f64 approach_preempt(f64 ar) {
  return std::trunc(map_difficulty_range(ar, 1800, 1200, 450));
}

// How long an appearing object takes to become opaque.
inline f64 approach_fade_in(f64 preempt) {
  return 400 * std::min(1.0, preempt / 450);
}

// Largest timing error, either side, that still earns each judgement.
struct HitWindows {
  f64 hit300;
  f64 hit100;
  f64 hit50;
};

// Whole ms, as osu!stable truncates them.
inline HitWindows hit_windows(f64 od) {
  return {std::floor(80 - 6 * od), std::floor(140 - 8 * od),
          std::floor(200 - 10 * od)};
}

// Judgement for a press `offset` ms from an object's time (negative is early).
// A press must land strictly inside a window, as in osu!stable.
inline Judgement judge_press(const HitWindows& windows, f64 offset) {
  const f64 error = std::abs(offset);
  if (error < windows.hit300)
    return Judgement::Hit300;
  if (error < windows.hit100)
    return Judgement::Hit100;
  if (error < windows.hit50)
    return Judgement::Hit50;
  return Judgement::Miss;
}

// osu!stable counts spinners in half turns: `required` earns a 50, one more
// a 100 and two more a 300.
inline Judgement spinner_judgement(i32 half_turns, i32 required) {
  if (half_turns <= required - 1)
    return Judgement::Miss;
  if (half_turns > required + 1)
    return Judgement::Hit300;
  if (half_turns > required)
    return Judgement::Hit100;
  return Judgement::Hit50;
}

// A slider is judged by the fraction of its head, ticks, repeats and end that
// were hit.
inline Judgement slider_judgement(i32 hit, i32 total) {
  if (total <= 0 || hit <= 0)
    return Judgement::Miss;
  if (hit == total)
    return Judgement::Hit300;
  if (hit * 2 >= total)
    return Judgement::Hit100;
  return Judgement::Hit50;
}

inline i32 judgement_value(Judgement judgement) {
  switch (judgement) {
    case Judgement::Hit300:
      return 300;
    case Judgement::Hit100:
      return 100;
    case Judgement::Hit50:
      return 50;
    default:
      return 0;
  }
}

// osu!stable's score multiplier from the map's settings and object density,
// in its single-precision arithmetic, rounding halves to even as .NET does.
inline f64 difficulty_multiplier(f64    hp,
                                 f64    cs,
                                 f64    od,
                                 size_t objects,
                                 i32    drain_seconds) {
  const f32 density = std::clamp(
      static_cast<f32>(objects) / static_cast<f32>(drain_seconds) * 8.0f, 0.0f,
      16.0f);
  const f32 points = (static_cast<f32>(hp) + static_cast<f32>(od) +
                      static_cast<f32>(cs) + density) /
                     38.0f * 5.0f;
  return std::nearbyint(static_cast<f64>(points));
}

// Score for a judgement, growing with the combo before it, as osu!stable
// counts.
inline i64 hit_score(Judgement judgement, i32 combo, f64 multiplier) {
  const i32 value = judgement_value(judgement);
  return value +
         static_cast<i64>(std::max(combo - 1, 0) * ((value / 25) * multiplier));
}

struct Counts {
  i32 hit300 = 0;
  i32 hit100 = 0;
  i32 hit50 = 0;
  i32 miss = 0;
  // Combo-ending 300s with no 100s or worse in their combo, and those with
  // only 100s; replays record them.
  i32 geki = 0;
  i32 katu = 0;

  i32 total() const { return hit300 + hit100 + hit50 + miss; }
};

// 0 to 1, in single precision as osu!stable keeps it.
inline f32 accuracy(const Counts& counts) {
  if (!counts.total())
    return 1;
  return static_cast<f32>(counts.hit50 * 50 + counts.hit100 * 100 +
                          counts.hit300 * 300) /
         (counts.total() * 300);
}

enum class Grade : u8 { SS, S, A, B, C, D };

// osu!stable's grades, with its single-precision ratios: exactly 80% 300s
// counts as more than 80%. Nothing judged is a D.
inline Grade grade(const Counts& counts) {
  const i32 total = counts.total();
  if (!total)
    return Grade::D;
  const f64  ratio300 = static_cast<f32>(counts.hit300) / total;
  const f64  ratio50 = static_cast<f32>(counts.hit50) / total;
  const bool clean = counts.miss == 0;
  if (ratio300 == 1)
    return Grade::SS;
  if (ratio300 > 0.9 && ratio50 <= 0.01 && clean)
    return Grade::S;
  if ((ratio300 > 0.8 && clean) || ratio300 > 0.9)
    return Grade::A;
  if ((ratio300 > 0.7 && clean) || ratio300 > 0.8)
    return Grade::B;
  if (ratio300 > 0.6)
    return Grade::C;
  return Grade::D;
}

}  // namespace circles
