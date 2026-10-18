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

inline f64 circle_radius(f64 cs) {
  return 54.4 - 4.48 * cs;
}

// How long before its time an object appears.
inline f64 approach_preempt(f64 ar) {
  return ar < 5 ? 1200 + 600 * (5 - ar) / 5 : 1200 - 750 * (ar - 5) / 5;
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

// osu!stable's turns per second to clear a spinner.
inline f64 spins_per_second(f64 od) {
  return od < 5 ? 3 + 0.4 * od : 2.5 + 0.5 * od;
}

// Turns are counted continuously, as osu!lazer does, which counts more
// strictly than stable; lazer's 0.6 factor keeps the requirement comparable.
inline i32 required_spins(f64 duration, f64 od) {
  return std::max(
      1, static_cast<i32>(duration / 1000 * 0.6 * spins_per_second(od)));
}

inline Judgement spinner_judgement(f64 turns, i32 required) {
  const f64 completion = turns / required;
  if (completion >= 1)
    return Judgement::Hit300;
  if (completion >= 0.9)
    return Judgement::Hit100;
  if (completion >= 0.75)
    return Judgement::Hit50;
  return Judgement::Miss;
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

// osu!stable's score multiplier from the map's settings and object density.
// It rounds halves to even, as .NET does.
inline f64 difficulty_multiplier(f64    hp,
                                 f64    cs,
                                 f64    od,
                                 size_t objects,
                                 i32    drain_seconds) {
  const f64 density =
      drain_seconds > 0 ? static_cast<f64>(objects) / drain_seconds : 1e9;
  return std::nearbyint((hp + cs + od + std::clamp(density * 8, 0.0, 16.0)) /
                        38 * 5);
}

// Score for a judgement, growing with the combo before it, as osu!stable
// counts.
inline i64 hit_score(Judgement judgement, i32 combo, f64 multiplier) {
  const i32 value = judgement_value(judgement);
  return value +
         static_cast<i64>(value * std::max(combo - 1, 0) * multiplier / 25);
}

struct Counts {
  i32 hit300 = 0;
  i32 hit100 = 0;
  i32 hit50 = 0;
  i32 miss = 0;

  i32 total() const { return hit300 + hit100 + hit50 + miss; }
};

// 0 to 1.
inline f64 accuracy(const Counts& counts) {
  if (!counts.total())
    return 1;
  return (300.0 * counts.hit300 + 100.0 * counts.hit100 + 50.0 * counts.hit50) /
         (300.0 * counts.total());
}

enum class Grade : u8 { SS, S, A, B, C, D };

inline Grade grade(const Counts& counts) {
  const i32 total = counts.total();
  if (!total || counts.hit300 == total)
    return Grade::SS;
  const f64  ratio300 = static_cast<f64>(counts.hit300) / total;
  const f64  ratio50 = static_cast<f64>(counts.hit50) / total;
  const bool clean = counts.miss == 0;
  if (ratio300 > 0.9 && ratio50 < 0.01 && clean)
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
