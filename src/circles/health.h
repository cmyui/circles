#pragma once

// osu!stable's health system, in its units: the bar holds 200, every scoring
// event restores or costs some, and it drains at a rate worked out per map
// by simulating a perfect play.

#include <circles/hit_objects.h>
#include <circles/types.h>

#include <fosu/beatmap.h>
#include <span>
#include <vector>

namespace circles {

inline constexpr f64 kHealthMax = 200;

// Everything that changes score, combo or health, as stable names them.
enum class ScoreEvent : u8 {
  Hit300,
  Hit100,
  Hit50,
  Miss,  // a whole object: breaks combo and counts a miss
  SliderTick,
  SliderRepeat,  // also a slider's head
  SliderEnd,
  SliderBreak,    // a missed head, tick or repeat: breaks combo only
  SliderEndMiss,  // costs health but not combo
  SpinnerSpin,    // an odd half turn: health only
  SpinnerPoints,  // an even half turn: 100 points
  SpinnerBonus,
};

// The bonus for the last object of a combo: geki with no 100s or worse in
// the combo, katu with no 50s or misses, mu otherwise.
enum class ComboBonus : u8 { None, Mu, Katu, Geki };

struct HealthRules {
  f64 drain_rate = 0;        // health per ms
  f64 multiplier = 1;        // on what hits restore
  f64 combo_multiplier = 1;  // on combo-end bonuses
  i32 max_combo = 0;
  f64 max_combo_portion = 1;  // Score V2's, for a perfect play
};

// What an event restores (positive) or costs. `hp` is the map's HP setting
// after mods.
f64 health_change(ScoreEvent         event,
                  ComboBonus         bonus,
                  f64                hp,
                  const HealthRules& rules);

// `preempt` is the map's approach time, after mods.
HealthRules calculate_health_rules(const std::vector<GameObject>& objects,
                                   std::span<const fosu::Break>   breaks,
                                   f64                            hp,
                                   f64                            preempt,
                                   i32                            version);

}  // namespace circles
