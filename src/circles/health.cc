#include <circles/health.h>
#include <circles/rules.h>

#include <algorithm>

namespace circles {

namespace {

constexpr f64 kHit300 = 6;
constexpr f64 kHit100 = 2.2;
constexpr f64 kHit50 = 0.4;
constexpr f64 kSliderRepeat = 4;
constexpr f64 kSliderTick = 3;
constexpr f64 kComboGeki = 14;
constexpr f64 kComboKatu = 10;
constexpr f64 kComboMu = 6;

// The bar as stable's HpBar tracks it. Gains stop at full, though the
// uncapped total keeps them; a negative drain, over overlapping objects,
// raises health past full.
struct Bar {
  f64 health = kHealthMax;
  f64 uncapped = kHealthMax;

  void reduce(f64 amount) {
    uncapped = std::max(0.0, uncapped - amount);
    health = std::max(0.0, health - amount);
  }
  void increase(f64 amount) {
    uncapped += amount;
    health = std::clamp(health + amount, 0.0, kHealthMax);
  }
  void change(f64 amount) {
    if (amount > 0)
      increase(amount);
    else
      reduce(-amount);
  }
};

}  // namespace

f64 health_change(ScoreEvent         event,
                  ComboBonus         bonus,
                  f64                hp,
                  const HealthRules& rules) {
  const f64 n = rules.multiplier;
  f64       change = 0;
  switch (event) {
    case ScoreEvent::Hit300:
      change = n * kHit300;
      break;
    case ScoreEvent::Hit100:
      change = n * map_difficulty_range(hp, kHit100 * 8, kHit100, kHit100);
      break;
    case ScoreEvent::Hit50:
      change = n * map_difficulty_range(hp, kHit50 * 8, kHit50, kHit50);
      break;
    case ScoreEvent::Miss:
      return map_difficulty_range(hp, -6, -25, -40);
    case ScoreEvent::SliderTick:
      return n * kSliderTick;
    case ScoreEvent::SliderRepeat:
    case ScoreEvent::SliderEnd:
      return n * kSliderRepeat;
    case ScoreEvent::SliderBreak:
    case ScoreEvent::SliderEndMiss:
      return map_difficulty_range(hp, -4, -15, -28);
    case ScoreEvent::SpinnerSpin:
    case ScoreEvent::SpinnerPoints:
      return n * 1.7;
    case ScoreEvent::SpinnerBonus:
      return n * 2;
  }
  const f64 c = rules.combo_multiplier;
  switch (bonus) {
    case ComboBonus::Geki:
      return c * kComboGeki + change;
    case ComboBonus::Katu:
      return c * kComboKatu + change;
    case ComboBonus::Mu:
      return c * kComboMu + change;
    case ComboBonus::None:
      break;
  }
  return change;
}

// Stable's CalculateHpDropRate: play the map perfectly with a trial drain
// rate, and ease the rate (and raise what hits restore) until health stays
// comfortably high throughout.
HealthRules calculate_health_rules(const std::vector<GameObject>& objects,
                                   std::span<const fosu::Break>   breaks,
                                   f64                            hp,
                                   f64                            preempt,
                                   i32                            version) {
  HealthRules rules;
  if (objects.empty())
    return rules;
  const f64 lowest_ever = map_difficulty_range(hp, 195, 160, 60);
  const f64 lowest_combo_end = map_difficulty_range(hp, 198, 170, 80);
  const f64 lowest_end = map_difficulty_range(hp, 198, 180, 80);
  const f64 recovery_available = map_difficulty_range(hp, 8, 4, 0);
  f64       drop = 0.05;
  while (true) {
    Bar  bar;
    i32  max_combo = 0;
    i32  combo = 0;
    f64  portion = 0;
    auto gain = [&](ScoreEvent event, ComboBonus bonus, bool adds_combo,
                    i32 points) {
      bar.change(health_change(event, bonus, hp, rules));
      combo += adds_combo;
      portion += points * (1 + combo / 10.0);
    };
    i32    last = static_cast<i32>(objects.front().time - preempt);
    bool   fail = false;
    size_t next_break = 0;
    i32    combo_too_low = 0;
    for (size_t i = 0; i < objects.size(); ++i) {
      const GameObject& h = objects[i];
      const i32         start = static_cast<i32>(h.time);
      const i32         end = static_cast<i32>(h.end_time);
      i32               break_time = 0;
      if (next_break < breaks.size()) {
        const i32 break_start = static_cast<i32>(breaks[next_break].start);
        const i32 break_end = static_cast<i32>(breaks[next_break].end);
        if (break_start >= last && break_end <= start) {
          // From v8, drain stops at the last object before a break.
          break_time = version < 8 ? break_end - break_start : break_end - last;
          ++next_break;
        }
      }
      bar.reduce(drop * (start - last - break_time));
      last = end;
      if (bar.health <= lowest_ever) {
        fail = true;
        drop *= 0.96;
        break;
      }
      bar.reduce(drop * (end - start));
      if (h.kind == ObjectKind::Slider) {
        // Repeats as stable classifies its points, which can differ from
        // the slider's repeat count when its passes get uneven ticks.
        const i32 repeats = static_cast<i32>(std::count_if(
            h.points.begin(), h.points.end(), [](const SliderPoint& point) {
              return point.kind == PointKind::Repeat;
            }));
        const i32 heads_and_ends = repeats + 2;
        const i32 ticks = static_cast<i32>(h.points.size()) - repeats - 1;
        max_combo += heads_and_ends + ticks - 1;
        for (i32 j = 0; j < heads_and_ends; ++j)
          gain(ScoreEvent::SliderRepeat, ComboBonus::None, true, 30);
        for (i32 j = 0; j < ticks; ++j)
          gain(ScoreEvent::SliderTick, ComboBonus::None, true, 10);
      } else if (h.kind == ObjectKind::Spinner) {
        for (i32 j = 0; j < h.required_spins; ++j)
          bar.change(health_change(ScoreEvent::SpinnerPoints, ComboBonus::None,
                                   hp, rules));
      }
      const bool adds_combo = h.kind != ObjectKind::Slider;
      if (i + 1 == objects.size() || objects[i + 1].new_combo) {
        gain(ScoreEvent::Hit300, ComboBonus::Geki, adds_combo, 300);
        if (bar.health < lowest_combo_end && ++combo_too_low > 2) {
          rules.combo_multiplier *= 1.07;
          rules.multiplier *= 1.03;
          fail = true;
          break;
        }
      } else {
        gain(ScoreEvent::Hit300, ComboBonus::None, adds_combo, 300);
      }
      ++max_combo;
    }
    if (!fail && bar.health < lowest_end) {
      fail = true;
      drop *= 0.94;
      rules.combo_multiplier *= 1.01;
      rules.multiplier *= 1.01;
    }
    const f64 recovery = (bar.uncapped - kHealthMax) / objects.size();
    if (!fail && recovery < recovery_available) {
      fail = true;
      drop *= 0.96;
      rules.combo_multiplier *= 1.02;
      rules.multiplier *= 1.01;
    }
    if (fail)
      continue;
    rules.drain_rate = drop;
    rules.max_combo = max_combo;
    rules.max_combo_portion = std::max(portion, 1.0);
    return rules;
  }
}

}  // namespace circles
