#include <circles/judging.h>

#include <algorithm>
#include <cmath>
#include <random>

namespace circles {

namespace {

constexpr u32 kLeftKeys = kReplayMouse1 | kReplayKey1;
constexpr u32 kRightKeys = kReplayMouse2 | kReplayKey2;
constexpr u8  kLeft = 1;
constexpr u8  kRight = 2;
constexpr i32 kHittableRange = 400;  // ms either side a press can reach
constexpr f64 kFadeOut = 240;        // ms objects stay visible after ending
constexpr f64 kSixtyFrame = 1000.0 / 60;
constexpr f64 kPi = 3.14159265358979323846;
constexpr f64 kMaxSpinVelocity = 0.05;  // radians per ms
constexpr f64 kAutoSpinVelocity = 0.03;
constexpr f64 kRelaxReverseAccel = 4;  // Relax spins back this much slower

f32 distance_squared(Vec2 a, Vec2 b) {
  const Vec2 d = a - b;
  return d.x * d.x + d.y * d.y;
}

void increase_health(Gameplay& game, f64 amount) {
  game.health = std::clamp(game.health + amount, 0.0, kHealthMax);
}

void reduce_health(Gameplay& game, f64 amount) {
  game.health = std::max(0.0, game.health - amount);
}

// Score V2's total from its portions: up to 700,000 for combo and 300,000
// for accuracy, with spinner bonus on top.
void update_score_v2(Gameplay& game) {
  const f64 judged =
      static_cast<f64>(game.counts.total()) / game.objects.size();
  game.score = static_cast<i64>(
      (game.combo_portion / game.health_rules.max_combo_portion * 700000 +
       std::pow(static_cast<f64>(accuracy(game.counts)), 10) * judged * 300000 +
       game.bonus_portion) *
      game.mod_multiplier);
}

Judgement judgement_of(ScoreEvent event) {
  switch (event) {
    case ScoreEvent::Hit300:
      return Judgement::Hit300;
    case ScoreEvent::Hit100:
      return Judgement::Hit100;
    case ScoreEvent::Hit50:
      return Judgement::Hit50;
    default:
      return Judgement::Miss;
  }
}

ScoreEvent event_of(Judgement judgement) {
  switch (judgement) {
    case Judgement::Hit300:
      return ScoreEvent::Hit300;
    case Judgement::Hit100:
      return ScoreEvent::Hit100;
    case Judgement::Hit50:
      return ScoreEvent::Hit50;
    default:
      return ScoreEvent::Miss;
  }
}

// Stable's IncreaseScoreHit: an event's points, combo and health.
void score_event(App& app, size_t index, ScoreEvent event, ComboBonus bonus) {
  auto&       game = app.game;
  const auto& object = game.objects[index];
  i32         points = 0;
  bool        adds_combo = true;  // or breaks it, for misses
  bool        negative = false;
  switch (event) {
    case ScoreEvent::Hit300:
    case ScoreEvent::Hit100:
    case ScoreEvent::Hit50:
      points = judgement_value(judgement_of(event));
      adds_combo = object.kind != ObjectKind::Slider;
      break;
    case ScoreEvent::Miss:
    case ScoreEvent::SliderBreak:
      negative = true;
      break;
    case ScoreEvent::SliderEndMiss:
      negative = true;
      adds_combo = false;
      break;
    case ScoreEvent::SliderTick:
      points = 10;
      break;
    case ScoreEvent::SliderRepeat:
    case ScoreEvent::SliderEnd:
      points = 30;
      break;
    case ScoreEvent::SpinnerSpin:
      adds_combo = false;
      break;
    case ScoreEvent::SpinnerPoints:
      points = 100;
      adds_combo = false;
      break;
    case ScoreEvent::SpinnerBonus:
      points = 1100;
      adds_combo = false;
      break;
  }
  switch (event) {
    case ScoreEvent::Hit300:
      ++game.counts.hit300;
      break;
    case ScoreEvent::Hit100:
      ++game.counts.hit100;
      break;
    case ScoreEvent::Hit50:
      ++game.counts.hit50;
      break;
    case ScoreEvent::Miss:
      ++game.counts.miss;
      break;
    default:
      break;
  }
  if (bonus == ComboBonus::Geki)
    ++game.counts.geki;
  else if (bonus == ComboBonus::Katu)
    ++game.counts.katu;

  const i32 combo_before = game.combo;
  if (adds_combo && !negative) {
    game.max_combo = std::max(game.max_combo, ++game.combo);
    game.combo_changed = app.now;
  } else if (adds_combo) {
    if (game.combo > 20 && !(game.mods & (kRelax | kAutopilot)))
      play_sound(app.audio, Sound::ComboBreak, 0.8f);
    game.combo = 0;
    game.combo_changed = app.now;
    // Sudden Death, and Perfect with it, fails at the first break.
    if (game.mods & kSuddenDeath)
      game.health = 0;
  }

  const f64 change =
      health_change(event, bonus, game.hp_setting, game.health_rules);
  if (change > 0)
    increase_health(game, change);
  else
    reduce_health(game, -change);

  if (game.mods & kScoreV2) {
    if (event == ScoreEvent::SpinnerPoints)
      game.bonus_portion += 100;
    else if (event == ScoreEvent::SpinnerBonus)
      game.bonus_portion += 500;
    else if (points > 0)
      game.combo_portion += points * (1 + game.combo / 10.0);
    update_score_v2(game);
  } else if (event == ScoreEvent::Hit300 || event == ScoreEvent::Hit100 ||
             event == ScoreEvent::Hit50) {
    game.score +=
        hit_score(judgement_of(event), combo_before, game.score_multiplier);
  } else {
    game.score += points;
  }
  game.force_frame = true;
}

bool ends_combo(const Gameplay& game, size_t index) {
  return index + 1 == game.objects.size() || game.objects[index + 1].new_combo;
}

// Stable's HitObjectManager.Hit for a whole object: Perfect's strictness,
// the combo's end-of-combo bonus, then the score.
void judge_object(App& app, size_t index, Judgement judgement, Vec2 where) {
  auto& game = app.game;
  auto& state = game.states[index];
  if (game.mods & kPerfect &&
      (judgement == Judgement::Hit100 || judgement == Judgement::Hit50))
    judgement = Judgement::Miss;
  state.hit = true;
  state.result = judgement;
  state.result_time = game.time;
  if (judgement == Judgement::Hit100)
    ++game.combo_katu;
  else if (judgement == Judgement::Hit50 || judgement == Judgement::Miss)
    ++game.combo_bad;

  ComboBonus bonus = ComboBonus::None;
  if (ends_combo(game, index)) {
    // An earlier object in the combo still unjudged (a slider not yet over)
    // spoils the bonus.
    bool unfinished = false;
    if (game.objects[index].kind != ObjectKind::Spinner) {
      for (size_t i = index + 1; i-- > 0;) {
        if (!game.states[i].hit) {
          unfinished = true;
          break;
        }
        if (game.objects[i].new_combo)
          break;
      }
    }
    if (judgement > Judgement::Miss) {
      if (game.combo_katu == 0 && game.combo_bad == 0 && !unfinished)
        bonus = ComboBonus::Geki;
      else if (game.combo_bad == 0 && !unfinished)
        bonus = ComboBonus::Katu;
      else
        bonus = ComboBonus::Mu;
    }
    game.combo_katu = game.combo_bad = 0;
  }
  // Misses tilt a little either way, at random.
  static std::mt19937 engine{std::random_device{}()};
  game.popups.push_back(
      {game.time, where, judgement,
       std::uniform_real_distribution<f32>(-0.15f, 0.15f)(engine)});
  score_event(app, index, event_of(judgement), bonus);
}

// A slider's head left unhit past its window. Stable looks the head up as
// if it were its slider, so a slider that ends its combo resets the combo's
// tally here.
void miss_head(App& app, size_t index) {
  auto& game = app.game;
  auto& state = game.states[index];
  state.head = Judgement::Miss;
  state.head_time = game.time;
  if (ends_combo(game, index))
    game.combo_katu = game.combo_bad = 0;
  game.popups.push_back(
      {game.time, game.objects[index].position, Judgement::Miss});
  score_event(app, index, ScoreEvent::SliderBreak, ComboBonus::None);
}

void log_hit(App& app, f64 offset, Judgement judgement) {
  app.game.offsets.push_back(offset);
  app.game.recent_errors.push_back({app.now, offset, judgement});
}

// The objects stable keeps "minimal": from the first not long past to just
// past the approaching ones.
template <typename Visit>
void for_each_near(Gameplay& game, i32 time, Visit&& visit) {
  for (size_t i = game.first_active; i < game.objects.size(); ++i) {
    if (game.objects[i].end_time < time - game.preempt)
      continue;
    if (!visit(i) || game.objects[i].time > time + game.preempt)
      return;
  }
}

bool visible(const Gameplay& game, size_t index, i32 time) {
  const GameObject& object = game.objects[index];
  if (object.kind == ObjectKind::Spinner)
    return time >= object.time - 400 && time <= object.end_time;
  return time >= object.time - game.preempt &&
         time <= object.end_time + kFadeOut;
}

// Stable's onGamePress: the first object in reach takes the press, if note
// lock lets it.
void game_press(App& app, i32 time, Vec2 cursor) {
  auto& game = app.game;
  if (game.phase != GamePhase::Playing || game.outro)
    return;
  const bool autopilot = game.mods & kAutopilot;
  // Autopilot's players only time their presses, so they get more room.
  const f32  radius = autopilot ? 100 : static_cast<f32>(game.radius);
  i32        found = -1;
  for_each_near(game, time, [&](size_t i) {
    const GameObject& object = game.objects[i];
    if (!game.states[i].hit && object.time - game.preempt <= time &&
        object.time + game.windows.hit50 >= time &&
        distance_squared(cursor, object.position) <= radius * radius) {
      found = static_cast<i32>(i);
      return false;
    }
    return true;
  });
  if (found < 0)
    return;
  const GameObject& object = game.objects[found];
  auto&             state = game.states[found];

  // A circle under a slider still running on its stack can't be hit yet.
  if (object.kind == ObjectKind::Circle && found > 0) {
    const size_t below = found - 1;
    if (game.objects[below].stack_count > 0 && visible(game, below, time) &&
        !game.states[below].hit)
      return;
  }
  // Note lock: the earliest object still in its window must go first.
  bool next = true;
  for_each_near(game, time, [&](size_t i) {
    if (game.objects[i].time + game.windows.hit50 <= time || game.states[i].hit)
      return true;
    if (game.objects[i].time < object.time && static_cast<i32>(i) != found)
      next = false;
    return false;
  });
  if (!next ||
      std::abs(object.time - time) >= kHittableRange - (autopilot ? 200 : 0)) {
    state.shake_time = app.now;
    return;
  }

  const f64       offset = time - object.time;
  const Judgement judgement = judge_press(game.windows, offset);
  game.force_frame = true;
  if (object.kind == ObjectKind::Circle) {
    state.head = judgement;
    state.head_time = time;
    if (judgement > Judgement::Miss) {
      log_hit(app, offset, judgement);
      play_hitsound(app, object.hitsound, object.volume, object.position);
    }
    judge_object(app, found, judgement, object.position);
  } else if (object.kind == ObjectKind::Slider &&
             state.head == Judgement::None) {
    state.head = judgement;
    state.head_time = time;
    // The side that hit the head holds the slider.
    state.down_button = game.press_left    ? kLeft
                        : game.press_right ? kRight
                                           : game.down_sides;
    if (judgement > Judgement::Miss) {
      log_hit(app, offset, judgement);
      play_hitsound(app, object.edge_hitsounds.front(),
                    object.edge_volumes.front(), object.position);
      score_event(app, found, ScoreEvent::SliderRepeat, ComboBonus::None);
    } else {
      score_event(app, found, ScoreEvent::SliderBreak, ComboBonus::None);
    }
  }
}

// ---- Sliders ----

void play_tick(App& app, const GameObject& object, const SliderPoint& point) {
  const auto&     game = app.game;
  const auto*     timing = timing_point_at(game.timing, point.time + 2);
  // The object's own sample set, else the timing point's.
  const SampleSet set = sample_set_or(
      object.sample_set,
      sample_set_or(timing ? static_cast<i32>(timing->sample_set) : 0,
                    game.default_sample_set));
  play_sound(app.audio, hitsound_effect(set, HitsoundKind::SliderTick),
             timing ? timing->volume / 100.0f : object.volume,
             hitsound_pan(point.position));
}

// Stable's SliderOsu.GetScorePoints, for the slider being played: whether
// the ball is followed, and the next tick, repeat or end due.
void score_slider(App& app, size_t index, i32 time, Vec2 cursor) {
  auto&       game = app.game;
  const auto& object = game.objects[index];
  auto&       state = game.states[index];
  if (state.hit || time < object.time)
    return;
  // Which side holds the slider: the one that hit its head, until the other
  // takes over by a press while it's held.
  bool       acceptable = false;
  const bool swap = game.game_down && !(game.last_sides == (kLeft | kRight) &&
                                        game.last_sides2 == game.down_sides);
  if (game.game_down) {
    if (state.down_button == 0 ||
        (game.down_sides != (kLeft | kRight) && swap)) {
      state.down_button = game.press_left    ? kLeft
                          : game.press_right ? kRight
                                             : game.down_sides;
      acceptable = true;
    } else if (game.down_sides & state.down_button) {
      acceptable = true;
    }
  } else {
    state.down_button = 0;
  }
  acceptable |= swap || game.mods & kRelax;

  bool allowable = false;
  if (acceptable) {
    const f32 radius =
        static_cast<f32>(game.radius) * (state.tracking ? 2.4f : 1);
    if (const auto ball = tracked_ball_position(object, time))
      allowable = distance_squared(cursor, *ball) < radius * radius;
  }
  if (allowable && !state.tracking) {
    state.tracking = true;
    state.tracking_changed = game.time;
    state.init_time = time;
    game.force_frame = true;
  }

  const auto count = static_cast<i32>(object.points.size());
  i32        due = 0;
  while (due < count && object.points[due].time <= time)
    ++due;
  const i32 done = state.points_hit + state.points_missed;
  if (done < due) {
    const SliderPoint& point = object.points[done];
    if (allowable && state.init_time <= point.time) {
      ++state.points_hit;
      // Stable names the part by how many are due, not which one it was.
      const ScoreEvent event = due == count ? ScoreEvent::SliderEnd
                               : due % (count / object.slides) == 0
                                   ? ScoreEvent::SliderRepeat
                                   : ScoreEvent::SliderTick;
      score_event(app, index, event, ComboBonus::None);
    } else {
      ++state.points_missed;
      score_event(app, index,
                  state.points_hit + state.points_missed == count
                      ? ScoreEvent::SliderEndMiss
                      : ScoreEvent::SliderBreak,
                  ComboBonus::None);
    }
  }
  if (!allowable && state.tracking &&
      state.points_hit + state.points_missed < count) {
    state.tracking = false;
    state.tracking_changed = game.time;
    game.force_frame = true;
  }
}

// Stable's SliderOsu.Update sounds: each tick or repeat as the ball passes
// it while the slider is held, scored or not, one per frame.
void sound_slider(App& app, size_t index, i32 time) {
  auto&       game = app.game;
  const auto& object = game.objects[index];
  auto&       state = game.states[index];
  const auto  count = static_cast<i32>(object.points.size());
  i32         passed = -1;
  for (i32 i = 0; i + 1 < count && object.points[i].time <= time; ++i)
    passed = i;
  const bool playing = game.audio_started || !game.audio_ok;
  if (time < object.time || time > object.end_time || !playing) {
    state.last_sounded = -1;
    return;
  }
  if (!state.tracking) {
    state.last_sounded = passed;
    return;
  }
  if (passed <= state.last_sounded)
    return;
  state.last_sounded = passed;
  const i32          per_span = count / object.slides;
  const SliderPoint& point = object.points[passed];
  if ((passed + 1) % per_span > 0) {
    play_tick(app, object, point);
  } else {
    const i32 edge = (passed + 1) / per_span;
    play_hitsound(app, object.edge_hitsounds[edge], object.edge_volumes[edge],
                  point.position);
  }
}

// Stable's SliderOsu.Hit, at the slider's end: judged by the fraction of its
// head and points followed; Score V2 also caps it by the head's timing.
void finish_slider(App& app, size_t index) {
  auto&       game = app.game;
  const auto& object = game.objects[index];
  auto&       state = game.states[index];
  state.tracking = false;
  const i32 hit = state.points_hit + (state.head > Judgement::Miss);
  const f64 fraction = static_cast<f64>(hit) / (object.points.size() + 1);
  Judgement judgement = Judgement::Miss;
  if (game.mods & kScoreV2) {
    if (fraction == 1 && state.head == Judgement::Hit300)
      judgement = Judgement::Hit300;
    else if (fraction >= 0.5 && state.head >= Judgement::Hit100)
      judgement = Judgement::Hit100;
    else if (fraction > 0)
      judgement = Judgement::Hit50;
  } else {
    judgement =
        slider_judgement(hit, static_cast<i32>(object.points.size()) + 1);
  }
  if (fraction > 0) {
    const bool late =
        game.time > object.time + (object.end_time - object.time) / 2;
    const size_t edge = late ? object.edge_hitsounds.size() - 1 : 0;
    play_hitsound(app, object.edge_hitsounds[edge], object.edge_volumes[edge],
                  object.end_position);
  }
  judge_object(app, index, judgement, object.end_position);
}

// ---- Spinners ----

// Stable's SpinnerOsu.GetScorePoints, on 60 Hz frames: the cursor's angular
// speed sets the speed the spinner eases toward; each new half turn scores.
void score_spinner(App& app, size_t index, i32 time, Vec2 cursor) {
  auto&       game = app.game;
  const auto& object = game.objects[index];
  auto&       state = game.states[index];
  const Vec2  offset = cursor - Vec2{kPlayfieldWidth / 2, kPlayfieldHeight / 2};
  const f64   angle = std::atan2(static_cast<f64>(offset.y), offset.x);
  f64         diff = angle - state.last_angle;
  if (angle - state.last_angle < -kPi)
    diff = 2 * kPi + angle - state.last_angle;
  else if (state.last_angle - angle < -kPi)
    diff = -2 * kPi - state.last_angle + angle;
  const f64 time_diff = game.last_scorable_time > 0
                            ? time - game.last_scorable_time
                            : kSixtyFrame;
  const f64 decay = std::pow(0.999, time_diff);
  state.frame_variance = decay * state.frame_variance + (1 - decay) * time_diff;
  if (diff == 0) {
    state.target_velocity =
        state.zero_count++ < 1 ? state.target_velocity / 3 : 0;
  } else {
    state.zero_count = 0;
    if (!(game.mods & kRelax) && ((!game.left && !game.right) ||
                                  time < object.time || time > object.end_time))
      diff = 0;
    if (std::abs(diff) < kPi) {
      // Below about 58 fps, frames count for their real length.
      state.target_velocity =
          state.frame_variance / game.rate > kSixtyFrame * 1.04f
              ? diff / (time_diff / game.rate)
              : diff / kSixtyFrame;
    } else {
      state.target_velocity = 0;
    }
  }
  state.last_angle = angle;

  const i32 half_turns = static_cast<i32>(state.half_turns);
  if (half_turns == state.last_half_turns)
    return;
  state.last_half_turns = half_turns;
  const i32 scored = ++state.scored_turns;
  const i32 required = object.required_spins;
  if (scored > required + 3 && (scored - (required + 3)) % 2 == 0) {
    state.bonus = 1000 * (scored - (required + 3)) / 2;
    state.bonus_time = game.time;
    if (!(game.mods & kCinema))
      play_sound(app.audio, Sound::SpinnerBonus, 0.6f);
    score_event(app, index, ScoreEvent::SpinnerBonus, ComboBonus::None);
  } else if (scored > 1 && scored % 2 == 0) {
    score_event(app, index, ScoreEvent::SpinnerPoints, ComboBonus::None);
  } else if (scored > 1) {
    score_event(app, index, ScoreEvent::SpinnerSpin, ComboBonus::None);
  }
}

// Stable's SpinnerOsu.Update, every frame: the spinner's speed eases toward
// the target, limited in acceleration, and its turning is counted in half
// turns.
void turn_spinner(App& app, size_t index, i32 time, i32 elapsed) {
  auto&       game = app.game;
  const auto& object = game.objects[index];
  auto&       state = game.states[index];
  if (state.hit || time < object.time)
    return;
  const f64 rpm_decay = std::pow(0.9, elapsed / kSixtyFrame);
  state.rpm = state.rpm * rpm_decay + (1 - rpm_decay) *
                                          (std::abs(state.velocity) * 1000) /
                                          (2 * kPi) * 60;
  if (!(time < object.end_time && time > object.time) ||
      game.phase == GamePhase::Recovering)
    return;
  const f64 length = object.end_time - object.time;
  const f64 max_accel =
      (0.00008 + std::max(0.0, (5000 - length) / 1000 / 2000)) * elapsed /
      game.rate;
  const bool relax = game.mods & kRelax;
  if (game.mods & (kSpunOut | kAutopilot)) {
    state.velocity = kAutoSpinVelocity;
  } else if (state.target_velocity > state.velocity) {
    state.velocity +=
        std::min(state.target_velocity - state.velocity,
                 state.velocity < 0 && relax ? max_accel / kRelaxReverseAccel
                                             : max_accel);
  } else {
    state.velocity +=
        std::max(state.target_velocity - state.velocity,
                 state.velocity > 0 && relax ? -max_accel / kRelaxReverseAccel
                                             : -max_accel);
  }
  state.velocity =
      std::clamp(state.velocity, -kMaxSpinVelocity, kMaxSpinVelocity);
  const f32 addition = static_cast<f32>(state.velocity * elapsed);
  state.rotation += static_cast<f32>(addition / game.rate);
  state.half_turns += std::abs(static_cast<f32>(addition / kPi));
}

void finish_spinner(App& app, size_t index) {
  auto&           game = app.game;
  const auto&     object = game.objects[index];
  const Judgement judgement =
      spinner_judgement(game.states[index].scored_turns, object.required_spins);
  if (judgement > Judgement::Miss)
    play_hitsound(app, object.hitsound, object.volume, object.position);
  judge_object(app, index, judgement, object.position);
}

// ---- Breaks and drain ----

// Stable pauses drain during breaks, before the first object and after the
// last, and from v8 between a break and the objects either side of it.
void update_drain(App& app, i32 time) {
  auto&       game = app.game;
  const auto& objects = game.objects;
  if (game.before_gameplay && time > objects.front().time - game.preempt)
    game.before_gameplay = false;
  else if (!game.after_gameplay &&
           time > objects.back().end_time + game.windows.hit50)
    game.after_gameplay = true;
  bool in_break = game.before_gameplay || game.after_gameplay;
  for (const auto& gap : game.breaks)
    in_break |= time >= gap.start && time <= gap.end;
  if (in_break) {
    game.resume_after_break = time;
    while (!game.drain_stops.empty() && game.drain_stops.front() < time)
      game.drain_stops.pop_front();
  }
  if (game.resume_after_break >= 0) {
    bool reached = false;
    for_each_near(game, time, [&](size_t i) {
      reached |=
          objects[i].time <= time && objects[i].time >= game.resume_after_break;
      return !reached;
    });
    if (reached)
      game.resume_after_break = -1;
  }
  const bool before_stop = game.drain_stops.empty() ||
                           time <= game.drain_stops.front() ||
                           game.map->format_version < 8;
  game.draining = time >= objects.front().time - game.preempt &&
                  time <= objects.back().end_time &&
                  game.resume_after_break < 0 && !game.outro && before_stop;
}

}  // namespace

f32 hitsound_pan(Vec2 position) {
  return (position.x / kPlayfieldWidth - 0.5f) * 0.8f;
}

void play_hitsound(App&            app,
                   const Hitsound& hitsound,
                   f32             volume,
                   Vec2            position) {
  const f32 pan = hitsound_pan(position);
  play_sound(app.audio,
             hitsound_effect(hitsound.normal_set, HitsoundKind::Normal), volume,
             pan);
  constexpr std::pair<u32, HitsoundKind> kAdditions[] = {
      {2, HitsoundKind::Whistle},
      {4, HitsoundKind::Finish},
      {8, HitsoundKind::Clap},
  };
  for (const auto& [bit, kind] : kAdditions)
    if (hitsound.additions & bit)
      play_sound(app.audio, hitsound_effect(hitsound.addition_set, kind),
                 volume, pan);
}

void start_judging(App& app, const fosu::Beatmap& map) {
  auto& game = app.game;
  game.health_rules =
      calculate_health_rules(game.objects, game.breaks, game.hp_setting,
                             game.preempt, map.format_version);
  if (game.mods & kHalfTime)
    game.health_rules.drain_rate *= 0.75;
  game.recoveries = game.mods & kEasy && !(game.mods & kSuddenDeath) ? 2 : 0;
  // Drain pauses at the end of the last object before each break.
  for (const auto& gap : game.breaks) {
    if (gap.start <= game.objects.front().time)
      continue;
    const f64 middle = gap.start + std::trunc((gap.end - gap.start) / 2);
    size_t    after = 0;
    while (after < game.objects.size() && game.objects[after].time < middle)
      ++after;
    if (after > 0)
      game.drain_stops.push_back(
          static_cast<i32>(game.objects[after - 1].end_time));
  }
}

StepOutcome play_step(App& app,
                      i32  time,
                      Vec2 cursor,
                      u32  keys,
                      bool scorable) {
  auto&     game = app.game;
  const i32 elapsed = game.stepped ? time - game.step_time : 0;
  game.step_time = time;
  game.stepped = true;
  game.time = time;

  // Input: a press is a side going down; both at once still make one.
  const bool left = keys & kLeftKeys;
  const bool right = keys & kRightKeys;
  game.press_left = left && !game.left;
  game.press_right = right && !game.right;
  if (left != game.left || right != game.right) {
    game.last_sides2 = game.last_sides;
    game.last_sides = game.down_sides;
    game.down_sides = (left ? kLeft : 0) | (right ? kRight : 0);
  }
  const bool pressed = game.press_left || game.press_right;
  if (pressed)
    game.game_down = true;
  else if (!left && !right)
    game.game_down = false;
  game.left = left;
  game.right = right;
  // Relax ignores presses and makes its own.
  if (pressed && !(game.mods & kRelax))
    game_press(app, time, cursor);

  // The slider or spinner in play.
  game.active = -1;
  for_each_near(game, time, [&](size_t i) {
    const GameObject& object = game.objects[i];
    if (object.time != object.end_time && object.time <= time &&
        (object.end_time >= time || !game.states[i].hit)) {
      game.active = static_cast<i32>(i);
      return false;
    }
    return true;
  });
  // Watching a replay, stable judges only at its frames, though it scores
  // Auto's sliders every frame.
  const bool judging = !game.watching || scorable;
  if (game.active >= 0 && (judging || game.mods & kAutoplay)) {
    if (game.objects[game.active].kind == ObjectKind::Slider)
      score_slider(app, game.active, time, cursor);
    else if (scorable)
      score_spinner(app, game.active, time, cursor);
  }

  // Spinners turn every frame; objects whose time has passed are judged.
  for_each_near(game, time, [&](size_t i) {
    const GameObject& object = game.objects[i];
    const auto&       state = game.states[i];
    if (object.kind == ObjectKind::Spinner)
      turn_spinner(app, i, time, elapsed);
    else if (object.kind == ObjectKind::Slider)
      sound_slider(app, i, time);
    if (!judging)
      return true;
    switch (object.kind) {
      case ObjectKind::Slider:
        if (object.time + game.windows.hit50 < time &&
            state.head == Judgement::None)
          miss_head(app, i);
        if (object.end_time <= time && !state.hit)
          finish_slider(app, i);
        break;
      case ObjectKind::Spinner:
        if (object.end_time < time && !state.hit)
          finish_spinner(app, i);
        break;
      case ObjectKind::Circle:
        if (object.end_time + game.windows.hit50 < time && !state.hit) {
          game.states[i].head = Judgement::Miss;
          game.states[i].head_time = time;
          judge_object(app, i, Judgement::Miss, object.position);
        }
        break;
    }
    return true;
  });

  if (game.draining)
    reduce_health(game, game.health_rules.drain_rate * elapsed);
  if (game.mods & kSuddenDeath && game.counts.miss > 0)
    game.health = 0;

  StepOutcome outcome = StepOutcome::Continue;
  if (game.health <= 0 && !game.watching &&
      !(game.mods & (kNoFail | kRelax | kAutopilot)) &&
      time > game.objects.front().time && game.phase == GamePhase::Playing) {
    if (game.recoveries > 0) {
      if (game.health < 160)
        increase_health(game, 160);
      --game.recoveries;
      outcome = StepOutcome::Recover;
    } else {
      outcome = game.mods & kPerfect ? StepOutcome::Restart : StepOutcome::Fail;
    }
  }

  if (game.mods & kRelax && game.phase == GamePhase::Playing) {
    for_each_near(game, time, [&](size_t i) {
      const GameObject& object = game.objects[i];
      const auto&       state = game.states[i];
      if (object.time - time < 12 &&
          ((object.kind == ObjectKind::Circle && !state.hit) ||
           (object.kind == ObjectKind::Slider &&
            state.head == Judgement::None)))
        game_press(app, time, cursor);
      return true;
    });
  }

  update_drain(app, time);
  if (scorable)
    game.last_scorable_time = time;
  return outcome;
}

}  // namespace circles
