#include <circles/app.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fosu/slider_event.h>
#include <fosu/slider_path.h>
#include <span>
#include <string>
#include <utility>

namespace circles {

namespace {

constexpr f32 kTau = 6.2831853f;
constexpr f64 kHitAnimation = 240;   // ms a hit object takes to burst
constexpr f64 kMissAnimation = 200;  // ms a missed object takes to fade
constexpr f64 kPopupDuration = 750;  // ms a judgement stays on screen
constexpr f64 kPathSpacing = 3;      // osu! pixels between body circles
constexpr f32 kFollowScale = 2.4f;   // follow circle over circle radius
constexpr f32 kMaxSpinSpeed = kTau * 8 / 1000;  // radians per ms
// The clock eases toward the music's position at this rate (per second), and
// snaps to it when they're further apart than the threshold (ms).
constexpr f64 kClockCorrectionRate = 2;
constexpr f64 kResyncThreshold = 50;
constexpr f32 kFailDuration = 2;   // seconds the music takes to wind down
constexpr f64 kFinishDelay = 1.5;  // seconds before showing results
// ms between recorded frames while the keys rest; key changes add their own.
constexpr f64 kRecordInterval = 8;

// Health changes, as fractions of the bar.
constexpr f64 kHealthHit300 = 0.06;
constexpr f64 kHealthHit100 = 0.025;
constexpr f64 kHealthHit50 = 0.005;
constexpr f64 kHealthSliderPart = 0.015;
constexpr f64 kHealthSpin = 0.02;

// Misses and passive drain grow with the map's HP setting.
f64 miss_penalty(f64 hp) {
  return 0.04 + 0.016 * hp;
}

f64 drain_per_second(f64 hp) {
  return 0.02 + 0.008 * hp;
}

const Color kDefaultComboColours[] = {hex_color(0xFFC000), hex_color(0x00CA00),
                                      hex_color(0x127CFF), hex_color(0xF21839)};

const Color kColour300 = hex_color(0x66CCFF);
const Color kColour100 = hex_color(0x88EE55);
const Color kColour50 = hex_color(0xFFCC22);
const Color kColourMiss = hex_color(0xFF4466);

// The 512x384 playfield, scaled and centred in the window.
struct Playfield {
  f32 scale;  // window points per osu! pixel
  f32 x, y;   // top-left corner

  Vec2 to_screen(Vec2 p) const { return {x + p.x * scale, y + p.y * scale}; }
  Vec2 to_playfield(Vec2 p) const {
    return {(p.x - x) / scale, (p.y - y) / scale};
  }
};

Playfield playfield(const App& app) {
  const f32 scale = std::min(app.height * 0.8f / kPlayfieldHeight,
                             app.width * 0.8f / kPlayfieldWidth);
  return {scale, (app.width - kPlayfieldWidth * scale) / 2,
          (app.height - kPlayfieldHeight * scale) / 2 + app.height * 0.02f};
}

const Vec2 kPlayfieldCentre{kPlayfieldWidth / 2, kPlayfieldHeight / 2};

// ---- Clock ----

f64 audio_time_at(const Gameplay& game, f64 wall) {
  return game.clock_time + (wall - game.clock_wall) * 1000;
}

void set_clock(Gameplay& game, f64 audio_time, f64 wall) {
  game.clock_time = audio_time;
  game.clock_wall = wall;
}

bool in_break(const Gameplay& game) {
  for (const auto& gap : game.map->breaks)
    if (game.time >= gap.start && game.time <= gap.end)
      return true;
  return false;
}

bool any_held(const Gameplay& game) {
  return std::find(std::begin(game.held), std::end(game.held), true) !=
         std::end(game.held);
}

// Skipping lands this long before the first object appears.
f64 skip_target(const Gameplay& game) {
  return game.objects.front().time - game.preempt - 1000;
}

bool can_skip(const Gameplay& game) {
  return game.phase == GamePhase::Playing &&
         game.time < skip_target(game) - 1000;
}

// ---- Loading ----

std::vector<Vec2> sample_path(const fosu::SliderPath& path, Vec2 head) {
  const i32 steps =
      std::max(1, static_cast<i32>(std::ceil(path.distance() / kPathSpacing)));
  std::vector<Vec2> points;
  points.reserve(steps + 1);
  for (i32 i = 0; i <= steps; ++i) {
    const fosu::PathPoint p =
        fosu::slider_position_at(path, static_cast<f64>(i) / steps);
    points.push_back({head.x + p.x, head.y + p.y});
  }
  return points;
}

// ---- Slider geometry ----

Vec2 path_point(const GameObject& object, f64 progress) {
  const f64 index = std::clamp(progress, 0.0, 1.0) * (object.path.size() - 1);
  const size_t i = static_cast<size_t>(index);
  if (i + 1 >= object.path.size())
    return object.path.back();
  return lerp(object.path[i], object.path[i + 1], static_cast<f32>(index - i));
}

i32 current_span(const GameObject& object, f64 time) {
  if (object.span_duration <= 0)
    return 0;
  return std::clamp(
      static_cast<i32>(std::floor((time - object.time) / object.span_duration)),
      0, object.slides - 1);
}

Vec2 ball_position(const GameObject& object, f64 time) {
  if (object.span_duration <= 0)
    return object.end_position;
  const f64 elapsed =
      std::clamp(time - object.time, 0.0, object.end_time - object.time);
  const i32 span = current_span(object, time);
  f64       progress = elapsed / object.span_duration - span;
  if (span % 2)
    progress = 1 - progress;
  return path_point(object, progress);
}

// ---- Scoring ----

f32 hitsound_pan(Vec2 position) {
  return (position.x / kPlayfieldWidth - 0.5f) * 0.6f;
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

void change_health(Gameplay& game, f64 amount) {
  game.health = std::clamp(game.health + amount, 0.0, 1.0);
}

void add_combo(App& app) {
  auto& game = app.game;
  game.max_combo = std::max(game.max_combo, ++game.combo);
  game.combo_changed = app.now;
}

void break_combo(App& app) {
  auto& game = app.game;
  if (game.combo >= 20)
    play_sound(app.audio, Sound::ComboBreak, 0.8f);
  game.combo = 0;
  game.combo_changed = app.now;
}

// Records a whole object's judgement: a circle, a slider's end or a spinner.
void finish_object(App&      app,
                   size_t    index,
                   Judgement judgement,
                   Vec2      position,
                   bool      adds_combo) {
  auto& game = app.game;
  auto& state = game.states[index];
  state.result = judgement;
  state.result_time = game.time;
  ++game.judged;
  game.popups.push_back({game.time, position, judgement});
  switch (judgement) {
    case Judgement::Hit300:
      ++game.counts.hit300;
      change_health(game, kHealthHit300);
      break;
    case Judgement::Hit100:
      ++game.counts.hit100;
      change_health(game, kHealthHit100);
      break;
    case Judgement::Hit50:
      ++game.counts.hit50;
      change_health(game, kHealthHit50);
      break;
    default:
      ++game.counts.miss;
      change_health(game, -miss_penalty(game.hp));
      break;
  }
  if (judgement == Judgement::Miss) {
    break_combo(app);
    return;
  }
  if (adds_combo)
    add_combo(app);
  game.score += hit_score(judgement, game.combo, game.score_multiplier);
}

// Judges a circle, or a slider's head (one part of the slider).
void judge_head(App&      app,
                size_t    index,
                Judgement judgement,
                f64       time,
                f64       offset) {
  auto&       game = app.game;
  const auto& object = game.objects[index];
  auto&       state = game.states[index];
  state.head = judgement;
  state.head_time = time;
  if (judgement != Judgement::Miss) {
    game.offsets.push_back(offset);
    game.recent_errors.push_back({app.now, offset, judgement});
  }
  if (object.kind == ObjectKind::Circle) {
    if (judgement != Judgement::Miss)
      play_hitsound(app, object.hitsound, object.volume, object.position);
    finish_object(app, index, judgement, object.position, true);
    return;
  }
  ++state.parts_total;
  if (judgement == Judgement::Miss) {
    break_combo(app);
    change_health(game, -2 * kHealthSliderPart);
    game.popups.push_back({time, object.position, Judgement::Miss});
    return;
  }
  ++state.parts_hit;
  add_combo(app);
  game.score += 30;
  change_health(game, kHealthSliderPart);
  play_hitsound(app, object.edge_hitsounds.front(), object.volume,
                object.position);
}

// Misses every circle and slider head whose last chance ended before `time`.
void expire_heads(App& app, f64 time) {
  auto& game = app.game;
  for (size_t i = game.first_active; i < game.objects.size(); ++i) {
    const auto& object = game.objects[i];
    if (object.time + game.windows.hit50 >= time)
      break;
    if (object.kind != ObjectKind::Spinner &&
        game.states[i].head == Judgement::None)
      judge_head(app, i, Judgement::Miss, object.time + game.windows.hit50,
                 game.windows.hit50);
  }
}

void handle_press(App& app, const Press& press) {
  auto& game = app.game;
  expire_heads(app, press.time);
  const Vec2 cursor = playfield(app).to_playfield(press.cursor);
  // Only the earliest unjudged circle or slider head can be hit ("note
  // lock"), so mashing can't hit objects out of order.
  for (size_t i = game.first_active; i < game.objects.size(); ++i) {
    const auto& object = game.objects[i];
    if (object.kind == ObjectKind::Spinner ||
        game.states[i].head != Judgement::None)
      continue;
    const f64 offset = press.time - object.time;
    if (offset < -kMissWindow ||
        (cursor - object.position).length() > game.radius)
      return;
    judge_head(app, i, judge_press(game.windows, offset), press.time, offset);
    return;
  }
}

void score_slider_part(App& app, size_t index, const fosu::SliderEvent& event) {
  auto&       game = app.game;
  const auto& object = game.objects[index];
  auto&       state = game.states[index];
  ++state.parts_total;
  if (!state.tracking) {
    // As in osu!, missing a slider's end costs accuracy and health, not combo.
    if (event.type != fosu::SliderEventType::LegacyLastTick)
      break_combo(app);
    change_health(game, -kHealthSliderPart);
    return;
  }
  ++state.parts_hit;
  add_combo(app);
  change_health(game, kHealthSliderPart);
  const Vec2 position =
      object.position + Vec2{event.position.x, event.position.y};
  if (event.type == fosu::SliderEventType::Tick) {
    game.score += 10;
    play_sound(
        app.audio,
        hitsound_effect(object.hitsound.normal_set, HitsoundKind::SliderTick),
        object.volume, hitsound_pan(position));
    return;
  }
  game.score += 30;
  if (event.type == fosu::SliderEventType::LegacyLastTick) {
    state.end_hit = true;  // its sound waits for the end itself
    return;
  }
  play_hitsound(app, object.edge_hitsounds[event.span_index + 1], object.volume,
                position);
}

void update_sliders(App& app) {
  auto&      game = app.game;
  const Vec2 cursor = playfield(app).to_playfield(game.cursor);
  const bool holding = any_held(game);
  for (size_t i = game.first_active;
       i < game.objects.size() && game.objects[i].time <= game.time; ++i) {
    const auto& object = game.objects[i];
    auto&       state = game.states[i];
    if (object.kind != ObjectKind::Slider || state.result != Judgement::None)
      continue;
    // Once tracking, the cursor may wander as far as the follow circle.
    const Vec2 ball = ball_position(object, game.time);
    const f64  reach = game.radius * (state.tracking ? kFollowScale : 1);
    const bool tracking = holding && (cursor - ball).length() <= reach;
    if (tracking != state.tracking) {
      state.tracking = tracking;
      state.tracking_changed = game.time;
    }
    const auto events = game.map->slider_events[object.slider];
    while (state.result == Judgement::None &&
           state.next_event < events.size() &&
           events[state.next_event].time <= game.time) {
      const auto& event = events[state.next_event++];
      switch (event.type) {
        case fosu::SliderEventType::Head:
          break;
        case fosu::SliderEventType::Tick:
        case fosu::SliderEventType::Repeat:
        case fosu::SliderEventType::LegacyLastTick:
          score_slider_part(app, i, event);
          break;
        case fosu::SliderEventType::Tail:
          // A slider shorter than the hit window ends before its head
          // expires.
          if (state.head == Judgement::None)
            judge_head(app, i, Judgement::Miss, game.time, game.windows.hit50);
          if (state.end_hit)
            play_hitsound(app, object.edge_hitsounds.back(), object.volume,
                          object.end_position);
          finish_object(app, i,
                        slider_judgement(state.parts_hit, state.parts_total),
                        object.end_position, false);
          break;
      }
    }
  }
}

void update_spinners(App& app, f64 elapsed) {
  auto&      game = app.game;
  const Vec2 cursor = playfield(app).to_playfield(game.cursor);
  const bool holding = any_held(game);
  for (size_t i = game.first_active;
       i < game.objects.size() && game.objects[i].time <= game.time; ++i) {
    const auto& object = game.objects[i];
    auto&       state = game.states[i];
    if (object.kind != ObjectKind::Spinner || state.result != Judgement::None)
      continue;
    if (holding && game.time <= object.end_time) {
      const Vec2 offset = cursor - kPlayfieldCentre;
      const f32  angle = std::atan2(offset.y, offset.x);
      if (state.has_angle) {
        f32 delta = angle - state.last_angle;
        if (delta > kTau / 2)
          delta -= kTau;
        else if (delta < -kTau / 2)
          delta += kTau;
        const f32 limit = kMaxSpinSpeed * static_cast<f32>(elapsed);
        delta = std::clamp(delta, -limit, limit);
        state.rotation += delta;
        const f32 rpm = std::abs(delta) /
                        std::max(static_cast<f32>(elapsed), 1e-3f) * 60000 /
                        kTau;
        state.rpm = approach(state.rpm, rpm, 6, app.dt);
      }
      state.last_angle = angle;
      state.has_angle = true;
    } else {
      state.has_angle = false;
      state.rpm = approach(state.rpm, 0, 6, app.dt);
    }

    const f64 turns = std::abs(state.rotation) / kTau;
    while (state.spins_scored < static_cast<i32>(turns)) {
      change_health(game, kHealthSpin);
      if (++state.spins_scored <= object.required_spins) {
        game.score += 100;
      } else {
        game.score += 1000;
        state.bonus_time = game.time;
        play_sound(app.audio, Sound::SpinnerBonus, 0.6f);
      }
    }
    if (game.time >= object.end_time)
      finish_object(app, i, spinner_judgement(turns, object.required_spins),
                    kPlayfieldCentre, true);
  }
}

// ---- Phases ----

void pause(App& app) {
  auto& game = app.game;
  set_clock(game, audio_time_at(game, app.now), app.now);
  music_pause(app.audio);
  game.phase = GamePhase::Paused;
  game.phase_changed = app.now;
  game.menu_selection = 0;
}

void resume(App& app) {
  auto& game = app.game;
  game.clock_wall = app.now;
  if (game.audio_started)
    music_play(app.audio);
  game.resync_after = app.now + 0.3;
  game.phase = GamePhase::Playing;
}

void fail(App& app) {
  auto& game = app.game;
  game.phase = GamePhase::Failed;
  game.phase_changed = app.now;
  game.menu_selection = 0;
  play_sound(app.audio, Sound::ComboBreak, 1);
}

void skip_intro(App& app) {
  auto& game = app.game;
  if (!can_skip(game))
    return;
  const f64 target = skip_target(game) + app.audio_offset;
  set_clock(game, target, app.now);
  if (game.audio_ok && target >= 0) {
    music_seek(app.audio, target);
    music_play(app.audio);
    game.audio_started = true;
  }
  game.resync_after = app.now + 0.3;
  play_sound(app.audio, Sound::MenuClick, 0.6f);
}

// ---- Replays ----

// Each hit key's bits in a replay frame. osu! marks a keyboard key with a
// mouse button's bit too.
constexpr u32 kReplayBits[kHitKeyCount] = {kReplayKey1 | kReplayMouse1,
                                           kReplayKey2 | kReplayMouse2,
                                           kReplayMouse1, kReplayMouse2};

u32 replay_keys(const Gameplay& game) {
  u32 keys = 0;
  for (i32 key = 0; key < kHitKeyCount; ++key)
    if (game.held[key])
      keys |= kReplayBits[key];
  return keys;
}

bool replay_holds(u32 keys, i32 key) {
  switch (key) {
    case kKey1:
      return keys & kReplayKey1;
    case kKey2:
      return keys & kReplayKey2;
    // A mouse button's bit alongside its keyboard key's is that key.
    case kMouse1:
      return (keys & kReplayMouse1) && !(keys & kReplayKey1);
    default:
      return (keys & kReplayMouse2) && !(keys & kReplayKey2);
  }
}

// Holds or releases a hit key. A press while playing is judged at `time`
// where `cursor` (window points) was.
void change_key(Gameplay& game, i32 key, bool down, f64 time, Vec2 cursor) {
  if (game.held[key] == down)
    return;
  game.held[key] = down;
  if (!down)
    return;
  ++game.press_counts[key];
  if (game.phase == GamePhase::Playing)
    game.pending.push_back({time, cursor});
}

void record_frame(App& app, f64 time) {
  auto& game = app.game;
  // The clock's corrections can step back slightly; frames stay in order.
  if (!game.frames.empty())
    time = std::max(time, game.frames.back().time);
  game.frames.push_back(
      {time, playfield(app).to_playfield(app.cursor), replay_keys(game)});
}

// Applies the replay's input up to the current time: its key changes as
// presses and releases, then its cursor, interpolated between frames.
void play_replay(App& app) {
  auto&           game = app.game;
  const auto&     frames = game.frames;
  const Playfield field = playfield(app);
  while (game.next_frame < frames.size() &&
         frames[game.next_frame].time <= game.time) {
    const ReplayFrame& frame = frames[game.next_frame++];
    for (i32 key = 0; key < kHitKeyCount; ++key)
      change_key(game, key, replay_holds(frame.keys, key), frame.time,
                 field.to_screen(frame.position));
  }
  if (frames.empty())
    return;
  const size_t next = std::min(game.next_frame, frames.size() - 1);
  const auto&  from = frames[game.next_frame > 0 ? game.next_frame - 1 : 0];
  const auto&  to = frames[next];
  const f64    span = to.time - from.time;
  const f32    t =
      span > 0 ? clamp01(static_cast<f32>((game.time - from.time) / span)) : 1;
  game.cursor = field.to_screen(lerp(from.position, to.position, t));
  game.replay_trail.push_back({app.now, game.cursor});
}

void set_hit_key(App& app, i32 key, bool down, f64 wall) {
  auto& game = app.game;
  if (game.held[key] == down)
    return;
  // Whole ms, as replays store them, so watching judges presses identically.
  const f64 time = std::round(audio_time_at(game, wall) - app.audio_offset);
  change_key(game, key, down, time, app.cursor);
  if (game.phase == GamePhase::Playing)
    record_frame(app, time);
}

void quit_to_song_select(App& app) {
  // A failed map's music is still winding down. Stopping it lets song select
  // restart the preview at normal speed instead of carrying on slowed.
  if (app.game.phase == GamePhase::Failed)
    music_pause(app.audio);
  switch_screen(app, Screen::SongSelect);
}

std::span<const char* const> menu_items(const Gameplay& game) {
  static constexpr const char* kPaused[] = {"Continue", "Retry", "Quit"};
  static constexpr const char* kFailed[] = {"Retry", "Quit"};
  if (game.phase == GamePhase::Failed)
    return kFailed;
  return kPaused;
}

struct MenuButton {
  f32 x, y, w, h;
};

MenuButton menu_button(const App& app, size_t index) {
  constexpr f32 kWidth = 320, kHeight = 60, kGap = 16;
  return {(app.width - kWidth) / 2,
          app.height * 0.45f + index * (kHeight + kGap), kWidth, kHeight};
}

i32 menu_item_at(const App& app, Vec2 point) {
  const auto items = menu_items(app.game);
  for (size_t i = 0; i < items.size(); ++i) {
    const MenuButton b = menu_button(app, i);
    if (contains(point, b.x, b.y, b.w, b.h))
      return static_cast<i32>(i);
  }
  return -1;
}

void activate_menu_item(App& app, i32 index) {
  const auto items = menu_items(app.game);
  if (index < 0 || index >= static_cast<i32>(items.size()))
    return;
  play_sound(app.audio, Sound::MenuClick, 0.7f);
  const std::string_view item = items[index];
  if (item == "Continue")
    resume(app);
  else if (item == "Retry")
    gameplay_start(app, app.game.map_index, app.game.replay_path);
  else
    quit_to_song_select(app);
}

void finish(App& app) {
  auto&       game = app.game;
  std::string player = game.replay_player;
  std::string replay_path = game.replay_path;
  if (!game.watching) {
    const char* user = std::getenv("USER");
    player = user ? user : "player";
    replay_path = save_replay(
        app, game.map_index,
        {.beatmap_md5 = app.library.maps[game.map_index].md5,
         .player = player,
         .counts = game.counts,
         .score = game.score,
         .max_combo = game.max_combo,
         .perfect = game.counts.miss == 0 && game.combo == game.max_combo,
         .timestamp = std::time(nullptr),
         .frames = std::move(game.frames)});
    if (replay_path.empty())
      show_toast(app, "Couldn't save the replay");
  }
  app.results = Results{
      .map_index = game.map_index,
      .score = game.score,
      .max_combo = game.max_combo,
      .counts = game.counts,
      .offsets = game.offsets,
      .hit50_window = game.windows.hit50,
      .player = std::move(player),
      .replay_path = std::move(replay_path),
  };
  switch_screen(app, Screen::Results);
}

// ---- Drawing ----

f32 appear_alpha(const Gameplay& game, const GameObject& object) {
  return clamp01(static_cast<f32>((game.time - (object.time - game.preempt)) /
                                  game.fade_in));
}

Color combo_colour(const Gameplay& game, const GameObject& object) {
  return game.combo_colours[object.colour % game.combo_colours.size()];
}

// Skin images for hit objects are drawn so that a 128-pixel hit circle spans
// the circle: this many points per 1x pixel.
f32 object_scale(f32 radius) {
  return radius / 64;
}

enum class CirclePart : u8 { Hit, SliderStart, SliderEnd };

void draw_hit_circle(App&       app,
                     Vec2       center,
                     f32        radius,
                     Color      colour,
                     f32        alpha,
                     i32        number,
                     CirclePart part = CirclePart::Hit) {
  if (alpha <= 0)
    return;
  auto&       r = app.renderer;
  const Skin& skin = app.skin;
  const auto  draw_number_text = [&] {
    const f32 size = radius * 0.8f;
    draw_text_shadowed(r, app.bold, std::to_string(number), center.x,
                       centered_baseline(app.bold, size, center.y), size,
                       fade(kWhite, alpha), Align::Center);
  };

  // Slider ends and starts use the hit circle's images unless the skin has
  // their own.
  struct Names {
    const char* circle;
    const char* overlay;
  };
  constexpr Names kHit{"hitcircle", "hitcircleoverlay"};
  const Names     names =
      part == CirclePart::SliderStart
          ? Names{"sliderstartcircle", "sliderstartcircleoverlay"}
      : part == CirclePart::SliderEnd
          ? Names{"sliderendcircle", "sliderendcircleoverlay"}
          : kHit;
  const auto* circle = skin_element(skin, names.circle);
  const char* overlay_name = names.overlay;
  if (!circle) {
    circle = skin_element(skin, kHit.circle);
    overlay_name = kHit.overlay;
  }
  if (circle) {
    const f32   scale = object_scale(radius);
    const auto* overlay = skin_element(skin, overlay_name);
    const auto  draw_number = [&] {
      if (number > 0 &&
          !draw_skin_number(r, skin, skin.config.hit_circle_prefix,
                            std::to_string(number), center.x, center.y,
                            scale * 0.8f, skin.config.hit_circle_overlap,
                            fade(kWhite, alpha), Align::Center))
        draw_number_text();
    };
    const auto draw_overlay = [&] {
      if (overlay)
        draw_skin_image(r, overlay->front(), center.x, center.y, scale,
                        fade(kWhite, alpha));
    };
    draw_skin_image(r, circle->front(), center.x, center.y, scale,
                    fade(colour, alpha));
    if (skin.config.overlay_above_number) {
      draw_number();
      draw_overlay();
    } else {
      draw_overlay();
      draw_number();
    }
    return;
  }

  draw_circle(r, center.x, center.y + radius * 0.05f, radius * 1.12f,
              fade(kBlack, 0.35f * alpha), 0, 0.25f);
  draw_circle(r, center.x, center.y, radius, fade(kWhite, alpha));
  draw_circle_gradient(r, center.x, center.y, radius * 0.86f,
                       fade(shade(colour, 1.15f), alpha),
                       fade(shade(colour, 0.65f), alpha));
  draw_circle(r, center.x, center.y, radius * 0.86f,
              fade(kWhite, 0.18f * alpha), 0.9f);
  if (number > 0)
    draw_number_text();
}

// A circle or slider head, including its burst after a hit.
void draw_head(App&               app,
               const Playfield&   field,
               const GameObject&  object,
               const ObjectState& state,
               f32                alpha_scale) {
  const auto&      game = app.game;
  const Vec2       center = field.to_screen(object.position);
  const f32        radius = static_cast<f32>(game.radius) * field.scale;
  const Color      colour = combo_colour(game, object);
  const f32        appear = appear_alpha(game, object);
  const CirclePart part = object.kind == ObjectKind::Slider
                              ? CirclePart::SliderStart
                              : CirclePart::Hit;
  if (state.head == Judgement::None) {
    draw_hit_circle(app, center, radius, colour, appear * alpha_scale,
                    object.combo_number, part);
  } else if (state.head == Judgement::Miss) {
    const f32 t =
        static_cast<f32>((game.time - state.head_time) / kMissAnimation);
    if (t < 1)
      draw_hit_circle(app, {center.x, center.y + t * radius * 0.3f}, radius,
                      colour, (1 - t) * appear * alpha_scale,
                      object.combo_number, part);
  } else {
    const f32 t =
        static_cast<f32>((game.time - state.head_time) / kHitAnimation);
    if (t < 1)
      draw_hit_circle(app, center, radius * (1 + 0.45f * ease_out(t)), colour,
                      (1 - t) * alpha_scale, 0, part);
  }
}

void draw_approach_circle(App&             app,
                          const Playfield& field,
                          size_t           index,
                          f32              alpha_scale) {
  const auto& game = app.game;
  const auto& object = game.objects[index];
  if (object.kind == ObjectKind::Spinner ||
      game.states[index].head != Judgement::None || game.time >= object.time)
    return;
  const Vec2 center = field.to_screen(object.position);
  const f32  radius = static_cast<f32>(game.radius) * field.scale;
  const f32  growth =
      1 + 3 * static_cast<f32>((object.time - game.time) / game.preempt);
  const Color colour = fade(combo_colour(game, object),
                            appear_alpha(game, object) * alpha_scale);
  if (const auto* approach = skin_element(app.skin, "approachcircle")) {
    draw_skin_image(app.renderer, approach->front(), center.x, center.y,
                    object_scale(radius) * growth, colour);
    return;
  }
  const f32 ring = radius * growth;
  draw_circle(app.renderer, center.x, center.y, ring, colour,
              1 - radius * 0.09f / ring);
}

// Draws instanced circles along the path; see slider.frag.
void draw_slider_body(App&                  app,
                      std::span<const Vec2> centers,
                      f32                   radius,
                      Color                 body,
                      Color                 border,
                      f32                   alpha) {
  flush(app.renderer);
  const auto& s = app.slider_renderer;
  glUseProgram(s.program);
  glUniform2f(s.screen_size, app.width, app.height);
  glUniform1f(s.radius, radius);
  glUniform4f(s.body_colour, body.r, body.g, body.b, body.a);
  glUniform4f(s.border_colour, border.r, border.g, border.b, border.a);
  glUniform1f(s.border_width, 0.13f);
  glUniform1f(s.alpha, alpha);
  glBindVertexArray(s.vao);
  glBindBuffer(GL_ARRAY_BUFFER, s.center_vbo);
  glBufferData(GL_ARRAY_BUFFER, centers.size_bytes(), centers.data(),
               GL_STREAM_DRAW);
  const auto count = static_cast<GLsizei>(centers.size());
  glEnable(GL_DEPTH_TEST);
  glClear(GL_DEPTH_BUFFER_BIT);
  // The first pass leaves each pixel's distance to the path in the depth
  // buffer; the second shades only the fragment at that distance, so
  // overlapping circles blend once.
  glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
  glDepthFunc(GL_LESS);
  glDrawArraysInstanced(GL_TRIANGLES, 0, 6, count);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glDepthMask(GL_FALSE);
  glDepthFunc(GL_EQUAL);
  glDrawArraysInstanced(GL_TRIANGLES, 0, 6, count);
  glDepthMask(GL_TRUE);
  glDepthFunc(GL_LESS);
  glDisable(GL_DEPTH_TEST);
}

// Points from `at` back along the slider, toward `toward`.
void draw_reverse_arrow(App& app, Vec2 at, Vec2 toward, f32 radius, f32 alpha) {
  auto&     r = app.renderer;
  Vec2      direction = toward - at;
  const f32 length = direction.length();
  if (length < 1e-3f)
    return;
  direction = direction * (1 / length);
  if (const auto* arrow = skin_element(app.skin, "reversearrow")) {
    draw_skin_image(r, arrow->front(), at.x, at.y, object_scale(radius),
                    fade(kWhite, alpha), std::atan2(direction.y, direction.x));
    return;
  }
  const Vec2 normal{-direction.y, direction.x};
  const Vec2 tip = at + direction * (radius * 0.35f);
  const Vec2 back = at - direction * (radius * 0.15f);
  const Vec2 wing_a = back + normal * (radius * 0.4f);
  const Vec2 wing_b = back - normal * (radius * 0.4f);
  draw_line(r, wing_a.x, wing_a.y, tip.x, tip.y, radius * 0.13f,
            fade(kWhite, alpha));
  draw_line(r, wing_b.x, wing_b.y, tip.x, tip.y, radius * 0.13f,
            fade(kWhite, alpha));
}

void draw_slider(App&             app,
                 const Playfield& field,
                 size_t           index,
                 f32              alpha_scale) {
  auto&       r = app.renderer;
  const auto& game = app.game;
  const auto& skin = app.skin;
  const auto& object = game.objects[index];
  const auto& state = game.states[index];
  const f32   radius = static_cast<f32>(game.radius) * field.scale;
  const f32   scale = object_scale(radius);
  const Color colour = combo_colour(game, object);
  f32         body_alpha = appear_alpha(game, object) * alpha_scale;
  if (state.result != Judgement::None)
    body_alpha *= 1 - clamp01(static_cast<f32>((game.time - state.result_time) /
                                               kHitAnimation));
  // The body grows from the head as the slider fades in.
  const f32 snake = clamp01(static_cast<f32>(
      (game.time - (object.time - game.preempt)) / (game.preempt / 3)));
  const i32 span = current_span(object, game.time);

  if (body_alpha > 0) {
    const size_t count = std::min(
        object.path.size(),
        static_cast<size_t>(std::ceil(snake * (object.path.size() - 1))) + 1);
    app.scratch.clear();
    for (size_t k = 0; k < count; ++k)
      app.scratch.push_back(field.to_screen(object.path[k]));
    const Color track = skin.config.slider_track.value_or(shade(colour, 0.5f));
    draw_slider_body(app, app.scratch, radius, fade(track, 0.85f),
                     skin.config.slider_border.value_or(kWhite), body_alpha);

    const auto* tick_image = skin_element(skin, "sliderscorepoint");
    for (const auto& event : game.map->slider_events[object.slider]) {
      if (event.type != fosu::SliderEventType::Tick ||
          event.span_index != span || event.time <= game.time ||
          event.path_progress > snake)
        continue;
      const Vec2 tick = field.to_screen(
          object.position + Vec2{event.position.x, event.position.y});
      if (tick_image) {
        draw_skin_image(r, tick_image->front(), tick.x, tick.y, scale,
                        fade(kWhite, body_alpha));
        continue;
      }
      draw_circle(r, tick.x, tick.y, radius * 0.16f,
                  fade(kBlack, 0.4f * body_alpha));
      draw_circle(r, tick.x, tick.y, radius * 0.11f, fade(kWhite, body_alpha));
    }

    if (snake >= 1) {
      draw_hit_circle(app, field.to_screen(object.path.back()), radius, colour,
                      body_alpha * 0.9f, 0, CirclePart::SliderEnd);
      // While repeats remain, an arrow marks where the ball turns back.
      if (span < object.slides - 1 && state.result == Judgement::None) {
        const bool   at_end = span % 2 == 0;
        const size_t inward = std::min<size_t>(object.path.size() - 1, 6);
        const Vec2   turn = at_end ? object.path.back() : object.path.front();
        const Vec2   toward = at_end
                                  ? object.path[object.path.size() - 1 - inward]
                                  : object.path[inward];
        if (!at_end)
          draw_hit_circle(app, field.to_screen(turn), radius, colour,
                          body_alpha, 0, CirclePart::SliderEnd);
        draw_reverse_arrow(app, field.to_screen(turn), field.to_screen(toward),
                           radius, body_alpha);
      }
    }
  }

  draw_head(app, field, object, state, alpha_scale);

  if (game.time < object.time)
    return;
  const bool active = state.result == Judgement::None;
  const Vec2 ball = field.to_screen(active ? ball_position(object, game.time)
                                           : object.end_position);
  const f32  since =
      static_cast<f32>((game.time - state.tracking_changed) / 150);
  const bool tracking = active && state.tracking;
  const f32  follow = tracking
                          ? 1 + (kFollowScale - 1) * ease_out(since)
                          : kFollowScale - (kFollowScale - 1) * ease_out(since);
  const f32  follow_alpha =
      (tracking ? ease_out(since) : 1 - ease_out(since)) * alpha_scale;
  if (follow_alpha > 0) {
    if (const auto* image = skin_element(skin, "sliderfollowcircle")) {
      // The image is drawn at full size when the follow circle is.
      draw_skin_image(r, image->front(), ball.x, ball.y,
                      scale * follow / kFollowScale,
                      fade(kWhite, follow_alpha));
    } else {
      const f32 ring = radius * follow;
      draw_circle(r, ball.x, ball.y, ring, fade(colour, 0.15f * follow_alpha));
      draw_circle(r, ball.x, ball.y, ring, fade(kWhite, 0.9f * follow_alpha),
                  1 - radius * 0.1f / ring);
    }
  }
  if (!active)
    return;
  if (const auto* frames = skin_element(skin, "sliderb")) {
    // The ball rolls along the path, facing the way it travels.
    const Vec2 ahead = field.to_screen(ball_position(object, game.time + 1));
    const Vec2 travel = ahead - ball;
    const f32  angle =
        travel.length() > 1e-4f ? std::atan2(travel.y, travel.x) : 0;
    const Color tint = skin.config.allow_slider_ball_tint
                           ? colour
                           : skin.config.slider_ball.value_or(kWhite);
    draw_skin_image(r,
                    skin_frame(skin, *frames, (game.time - object.time) / 1000),
                    ball.x, ball.y, scale, fade(tint, alpha_scale), angle);
    return;
  }
  draw_circle(r, ball.x, ball.y, radius * 0.85f, fade(kWhite, alpha_scale));
  draw_circle_gradient(r, ball.x, ball.y, radius * 0.72f,
                       fade(shade(colour, 1.2f), alpha_scale),
                       fade(shade(colour, 0.7f), alpha_scale));
}

// osu!'s newer spinner style: a glow that fills in with progress, and a top
// and bottom layer turning with the player's rotation.
void draw_skin_spinner(App&               app,
                       const Playfield&   field,
                       const GameObject&  object,
                       const ObjectState& state,
                       f32                alpha,
                       f32                completion) {
  auto&       r = app.renderer;
  const auto& skin = app.skin;
  const auto& game = app.game;
  const Vec2  c = field.to_screen(kPlayfieldCentre);
  // These images are drawn at 0.625 of their 1x size in playfield pixels.
  const f32   scale = 0.625f * field.scale;
  const f32   elapsed =
      clamp01(static_cast<f32>((game.time - object.time) /
                               std::max(1.0, object.end_time - object.time)));
  if (const auto* glow = skin_element(skin, "spinner-glow"))
    draw_skin_image(r, glow->front(), c.x, c.y, scale,
                    fade(hex_color(0x0397FF), alpha * completion));
  if (const auto* bottom = skin_element(skin, "spinner-bottom"))
    draw_skin_image(r, bottom->front(), c.x, c.y, scale, fade(kWhite, alpha),
                    state.rotation / 3);
  if (const auto* top = skin_element(skin, "spinner-top"))
    draw_skin_image(r, top->front(), c.x, c.y, scale, fade(kWhite, alpha),
                    state.rotation);
  // The middle reddens as time runs out.
  if (const auto* middle = skin_element(skin, "spinner-middle"))
    draw_skin_image(r, middle->front(), c.x, c.y, scale,
                    fade(mix(kWhite, hex_color(0xFF2222), elapsed), alpha));
  if (const auto* middle = skin_element(skin, "spinner-middle2"))
    draw_skin_image(r, middle->front(), c.x, c.y, scale, fade(kWhite, alpha));
  const f32 label_y = kPlayfieldHeight * 0.3f * field.scale;
  if (const auto* spin = skin_element(skin, "spinner-spin");
      spin && game.time - object.time < 1500)
    draw_skin_image(r, spin->front(), c.x, c.y + label_y, ui_scale(app),
                    fade(kWhite, alpha));
  if (const auto* clear = skin_element(skin, "spinner-clear");
      clear && completion >= 1)
    draw_skin_image(r, clear->front(), c.x, c.y - label_y, ui_scale(app),
                    fade(kWhite, alpha));
}

void draw_spinner(App&             app,
                  const Playfield& field,
                  size_t           index,
                  f32              alpha_scale) {
  auto&       r = app.renderer;
  const auto& game = app.game;
  const auto& object = game.objects[index];
  const auto& state = game.states[index];
  f32         alpha =
      clamp01(static_cast<f32>((game.time - (object.time - 400)) / 400)) *
      alpha_scale;
  if (state.result != Judgement::None)
    alpha *=
        1 - clamp01(static_cast<f32>((game.time - state.result_time) / 300));
  if (alpha <= 0)
    return;
  const Vec2 c = field.to_screen(kPlayfieldCentre);
  const f32  size = kPlayfieldHeight * 0.48f * field.scale;
  const f32  turns = std::abs(state.rotation) / kTau;
  const f32  completion = clamp01(turns / object.required_spins);
  const bool skinned = skin_element(app.skin, "spinner-top");

  if (skinned) {
    draw_skin_spinner(app, field, object, state, alpha, completion);
  } else {
    const f32 remaining =
        1 -
        clamp01(static_cast<f32>((game.time - object.time) /
                                 std::max(1.0, object.end_time - object.time)));
    draw_circle(r, c.x, c.y, size, fade(kBlack, 0.4f * alpha));
    draw_arc(r, c.x, c.y, size * 0.97f, 0.9f, completion,
             fade(completion >= 1 ? hex_color(0x66FFAA) : kColour300, alpha));
    draw_circle(r, c.x, c.y, size, fade(kWhite, 0.6f * alpha), 0.985f);
    // A ring closing in as time runs out.
    if (remaining > 0 && game.time >= object.time) {
      const f32 ring = size * remaining;
      draw_circle(r, c.x, c.y, ring, fade(kWhite, 0.8f * alpha),
                  1 - 3 / (ring + 3));
    }
    // Spokes turning with the player's rotation.
    for (i32 k = 0; k < 3; ++k) {
      const f32  angle = state.rotation + k * kTau / 3;
      const Vec2 d{std::cos(angle), std::sin(angle)};
      draw_line(r, c.x + d.x * size * 0.15f, c.y + d.y * size * 0.15f,
                c.x + d.x * size * 0.7f, c.y + d.y * size * 0.7f, size * 0.035f,
                fade(kWhite, 0.85f * alpha));
    }
    draw_circle(r, c.x, c.y, size * 0.1f, fade(kWhite, alpha));
  }

  const f32   label_size = size * 0.16f;
  const char* label = completion >= 1                  ? "CLEAR!"
                      : game.time - object.time < 1500 ? "SPIN!"
                                                       : nullptr;
  if (label && !skinned)
    draw_text_shadowed(
        r, app.heavy, label, c.x,
        centered_baseline(app.heavy, label_size, c.y - size * 0.35f),
        label_size, fade(kWhite, alpha), Align::Center);
  char rpm[32];
  snprintf(rpm, sizeof(rpm), "%d RPM", static_cast<i32>(state.rpm));
  draw_text_shadowed(r, app.bold, rpm, c.x, c.y + size * 0.62f,
                     label_size * 0.6f, fade(kWhite, 0.85f * alpha),
                     Align::Center);
  const f32 bonus_age = static_cast<f32>((game.time - state.bonus_time) / 600);
  if (bonus_age < 1) {
    char bonus[32];
    snprintf(bonus, sizeof(bonus), "+%d",
             (state.spins_scored - object.required_spins) * 1000);
    const f32 bonus_size = label_size * (1.4f - 0.3f * ease_out(bonus_age));
    draw_text_shadowed(
        r, app.heavy, bonus, c.x,
        centered_baseline(app.heavy, bonus_size, c.y + size * 0.4f), bonus_size,
        fade(kWhite, (1 - bonus_age) * alpha), Align::Center);
  }
}

// Dashes leading from one object to the next in a combo, vanishing as the
// next object's time arrives.
void draw_follow_points(App&             app,
                        const Playfield& field,
                        size_t           end,
                        f32              alpha_scale) {
  const auto& game = app.game;
  const auto* sprite = skin_element(app.skin, "followpoint");
  for (size_t i = std::max<size_t>(game.first_active, 1); i < end; ++i) {
    const auto& from = game.objects[i - 1];
    const auto& to = game.objects[i];
    if (to.new_combo || from.kind == ObjectKind::Spinner ||
        to.kind == ObjectKind::Spinner)
      continue;
    const Vec2 delta = to.position - from.end_position;
    const f32  distance = delta.length();
    const f32  radius = static_cast<f32>(game.radius);
    if (distance < radius * 2.5f)
      continue;
    const Vec2 direction = delta * (1 / distance);
    const f32  angle = std::atan2(direction.y, direction.x);
    const f64  duration = to.time - from.end_time;
    for (f32 d = radius * 1.5f; d < distance - radius * 1.2f; d += 32) {
      const f64 passes = from.end_time + duration * (d / distance);
      const f64 appears = passes - game.preempt;
      const f32 alpha =
          clamp01(static_cast<f32>((game.time - appears) / 200)) *
          (1 - clamp01(static_cast<f32>((game.time - passes) / 120)));
      if (alpha <= 0)
        continue;
      if (sprite) {
        // Skins draw follow points one 1x pixel per playfield pixel.
        const Vec2 at = field.to_screen(from.end_position + direction * d);
        draw_skin_image(
            app.renderer,
            skin_frame(app.skin, *sprite, (game.time - appears) / 1000), at.x,
            at.y, field.scale, fade(kWhite, alpha * alpha_scale), angle);
        continue;
      }
      const Vec2 a = field.to_screen(from.end_position + direction * (d - 5));
      const Vec2 b = field.to_screen(from.end_position + direction * (d + 5));
      draw_line(app.renderer, a.x, a.y, b.x, b.y, 2.2f * field.scale,
                fade(kWhite, 0.7f * alpha * alpha_scale));
    }
  }
}

void draw_popups(App& app, const Playfield& field, f32 alpha_scale) {
  const auto& game = app.game;
  const f32   radius = static_cast<f32>(game.radius) * field.scale;
  for (const auto& popup : game.popups) {
    const f32 age = static_cast<f32>(game.time - popup.time);
    if (age < 0 || age > kPopupDuration)
      continue;
    const f32 pop = 0.7f + 0.3f * ease_out(age / 120);
    const f32 alpha = (1 - clamp01((age - 450) / 300)) * alpha_scale;
    Vec2      at = field.to_screen(popup.position);
    at.y += popup.judgement == Judgement::Miss ? age * 0.04f : -age * 0.015f;

    const char* element = popup.judgement == Judgement::Hit300   ? "hit300"
                          : popup.judgement == Judgement::Hit100 ? "hit100"
                          : popup.judgement == Judgement::Hit50  ? "hit50"
                                                                 : "hit0";
    if (const auto* frames = skin_element(app.skin, element)) {
      draw_skin_image(app.renderer, skin_frame(app.skin, *frames, age / 1000),
                      at.x, at.y, field.scale * pop, fade(kWhite, alpha));
      continue;
    }
    // Without a skin, 300s are the norm, so only worse judgements get a
    // popup.
    const char* text;
    Color       colour;
    switch (popup.judgement) {
      case Judgement::Hit100:
        text = "100";
        colour = kColour100;
        break;
      case Judgement::Hit50:
        text = "50";
        colour = kColour50;
        break;
      case Judgement::Miss:
        text = "MISS";
        colour = kColourMiss;
        break;
      default:
        continue;
    }
    const f32 size = radius * 0.6f * pop;
    draw_text_shadowed(app.renderer, app.bold, text, at.x,
                       centered_baseline(app.bold, size, at.y), size,
                       fade(colour, alpha), Align::Center);
  }
}

void draw_hit_error_bar(App& app) {
  auto&       r = app.renderer;
  const auto& game = app.game;
  const f32   cx = app.width / 2, y = app.height - 24;
  const f32   scale =
      110 / static_cast<f32>(game.windows.hit50);  // points per ms
  const auto zone = [&](f64 window, Color colour) {
    const f32 half = static_cast<f32>(window) * scale;
    draw_rect(r, cx - half, y - 2, half * 2, 4, colour);
  };
  zone(game.windows.hit50, fade(kColour50, 0.55f));
  zone(game.windows.hit100, fade(kColour100, 0.6f));
  zone(game.windows.hit300, fade(kColour300, 0.7f));
  draw_rect(r, cx - 1, y - 9, 2, 18, kWhite);
  for (const auto& error : game.recent_errors) {
    const f32 age = static_cast<f32>((app.now - error.wall_time) / 4);
    if (age >= 1)
      continue;
    const Color colour = error.judgement == Judgement::Hit300   ? kColour300
                         : error.judgement == Judgement::Hit100 ? kColour100
                                                                : kColour50;
    draw_rect(r, cx + static_cast<f32>(error.offset) * scale - 1, y - 7, 2, 14,
              fade(colour, 0.8f * (1 - age)));
  }
  // The average of recent hits: early to the left, late to the right.
  const size_t recent = std::min<size_t>(game.offsets.size(), 20);
  if (!recent)
    return;
  f64 sum = 0;
  for (size_t i = game.offsets.size() - recent; i < game.offsets.size(); ++i)
    sum += game.offsets[i];
  const f32 x = cx + static_cast<f32>(sum / recent) * scale;
  draw_triangle(r, x - 5, y - 17, x + 5, y - 17, x, y - 11, kWhite);
}

void draw_key_overlay(App& app) {
  auto&                 r = app.renderer;
  const auto&           game = app.game;
  constexpr const char* kLabels[] = {"K1", "K2", "M1", "M2"};
  constexpr f32         kSize = 42, kGap = 8;
  const f32             x = app.width - kSize - 12;
  f32                   y =
      app.height / 2 - (kHitKeyCount * kSize + (kHitKeyCount - 1) * kGap) / 2;
  for (i32 k = 0; k < kHitKeyCount; ++k, y += kSize + kGap) {
    const bool held = game.held[k];
    // Switched-off mouse buttons are dimmed.
    const f32  alpha = k >= kMouse1 && !app.mouse_buttons ? 0.3f : 1;
    draw_rounded_rect(r, x, y, kSize, kSize, 8,
                      held ? fade(hex_color(0xFFD27A), 0.95f * alpha)
                           : fade(kBlack, 0.45f * alpha));
    const std::string label = game.press_counts[k]
                                  ? std::to_string(game.press_counts[k])
                                  : kLabels[k];
    draw_text(r, app.bold, label, x + kSize / 2,
              centered_baseline(app.bold, 14, y + kSize / 2), 14,
              fade(held ? hex_color(0x332200) : kWhite, alpha), Align::Center);
  }
}

// osu!'s health bar: a background, then the fill cropped to the health left
// with a marker at its end. Skins with a scorebar-marker use the newer layout.
void draw_skin_health_bar(App&                          app,
                          const std::vector<SkinImage>& fill_frames,
                          f32                           health) {
  auto&       r = app.renderer;
  const auto& skin = app.skin;
  const f32   scale = ui_scale(app);
  if (const auto* background = skin_element(skin, "scorebar-bg")) {
    const SkinImage& image = background->front();
    draw_image_region(r, image.texture, 0, 0, image.width() * scale,
                      image.height() * scale, 0, 0, 1, 1, kWhite);
  }
  const bool       newer = skin_element(skin, "scorebar-marker");
  const Vec2       origin = (newer ? Vec2{12, 12.5f} : Vec2{4.8f, 16}) * scale;
  const SkinImage& fill =
      skin_frame(skin, fill_frames, std::fmod(app.now, 1.0));
  const f32 width = fill.width() * scale * health;
  const f32 height = fill.height() * scale;
  draw_image_region(r, fill.texture, origin.x, origin.y, width, height, 0, 0,
                    health, 1, kWhite);
  if (const auto* marker =
          skin_element(skin, newer ? "scorebar-marker" : "scorebar-ki"))
    draw_skin_image(r, marker->front(), origin.x + width, origin.y + height / 2,
                    scale, kWhite);
}

void draw_hud(App& app) {
  auto&         r = app.renderer;
  const auto&   game = app.game;
  const auto&   skin = app.skin;
  const auto&   config = skin.config;
  constexpr f32 kMargin = 16;

  // Health
  const f32     health = clamp01(game.displayed_health);
  if (const auto* bar = skin_element(skin, "scorebar-colour")) {
    draw_skin_health_bar(app, *bar, health);
  } else {
    const f32 bar_width = app.width * 0.36f, bar_height = 10;
    draw_rounded_rect(r, kMargin, kMargin, bar_width, bar_height,
                      bar_height / 2, fade(kBlack, 0.5f));
    const f32   flash = 0.5f + 0.5f * std::sin(static_cast<f32>(app.now) * 12);
    const Color fill =
        health < 0.3f ? mix(hex_color(0xFF3355), hex_color(0xFF99AA), flash)
                      : mix(hex_color(0xFF6699), kWhite, health);
    if (health > 0)
      draw_rounded_rect(r, kMargin, kMargin, bar_width * health, bar_height,
                        bar_height / 2, fill);
  }

  // Score, accuracy and song progress
  char score[32], accuracy_text[32];
  snprintf(score, sizeof(score), "%08lld",
           static_cast<long long>(std::llround(game.displayed_score)));
  snprintf(accuracy_text, sizeof(accuracy_text), "%.2f%%",
           accuracy(game.counts) * 100);
  f32 pie_x, pie_y;
  if (const auto* digit = skin_element(skin, config.score_prefix + "-0")) {
    const f32 score_scale = ui_scale(app);
    const f32 accuracy_scale = score_scale * 0.6f;
    const f32 score_height = digit->front().height() * score_scale;
    const f32 accuracy_height = digit->front().height() * accuracy_scale;
    draw_skin_number(r, skin, config.score_prefix, score, app.width - kMargin,
                     kMargin + score_height / 2, score_scale,
                     config.score_overlap, kWhite, Align::Right);
    pie_y = kMargin + score_height + 8 + accuracy_height / 2;
    const f32 width =
        draw_skin_number(r, skin, config.score_prefix, accuracy_text,
                         app.width - kMargin, pie_y, accuracy_scale,
                         config.score_overlap, kWhite, Align::Right)
            .value_or(0);
    pie_x = app.width - kMargin - width - 18;
  } else {
    const f32 score_size = 40, accuracy_size = 22;
    const f32 score_baseline = kMargin + app.bold.cap_height * score_size;
    draw_text_shadowed(r, app.bold, score, app.width - kMargin, score_baseline,
                       score_size, kWhite, Align::Right);
    const f32 accuracy_baseline =
        score_baseline + 12 + app.bold.cap_height * accuracy_size;
    draw_text_shadowed(r, app.bold, accuracy_text, app.width - kMargin,
                       accuracy_baseline, accuracy_size, kWhite, Align::Right);
    pie_x = app.width - kMargin -
            measure_text(app.bold, accuracy_text, accuracy_size) - 18;
    pie_y = accuracy_baseline - app.bold.cap_height * accuracy_size / 2;
  }
  const f64  first = game.objects.front().time;
  const bool intro = game.time < first;
  const f32  progress = static_cast<f32>(
      intro ? 1 - (first - game.time) / std::max(1.0, first + 3000)
            : (game.time - first) / std::max(1.0, game.end_time - first));
  draw_circle(r, pie_x, pie_y, 9, fade(kWhite, 0.2f));
  draw_arc(r, pie_x, pie_y, 9, 0, clamp01(progress),
           fade(intro ? hex_color(0x88EE88) : kWhite, 0.85f));
  draw_circle(r, pie_x, pie_y, 9, kWhite, 0.85f);

  // Combo
  if (game.combo > 0) {
    const f32 pop =
        1 + 0.25f * (1 - ease_out(static_cast<f32>(
                             (app.now - game.combo_changed) / 0.15)));
    char text[32];
    snprintf(text, sizeof(text), "%dx", game.combo);
    if (const auto* digit = skin_element(skin, config.combo_prefix + "-0")) {
      const f32 scale = ui_scale(app) * pop;
      draw_skin_number(
          r, skin, config.combo_prefix, text, kMargin,
          app.height - kMargin - digit->front().height() * scale / 2, scale,
          config.combo_overlap, kWhite, Align::Left);
    } else {
      draw_text_shadowed(r, app.bold, text, kMargin, app.height - kMargin,
                         54 * pop, kWhite);
    }
  }

  draw_hit_error_bar(app);
  draw_key_overlay(app);
}

void draw_game_overlays(App& app) {
  auto&       r = app.renderer;
  const auto& game = app.game;

  if (game.phase == GamePhase::Playing && in_break(game)) {
    for (const auto& gap : game.map->breaks) {
      if (game.time < gap.start || game.time > gap.end)
        continue;
      const f32 alpha = clamp01(static_cast<f32>(
          std::min(game.time - gap.start, gap.end - game.time) / 400));
      const f32 left =
          static_cast<f32>((gap.end - game.time) / (gap.end - gap.start));
      draw_text_shadowed(r, app.heavy, "BREAK", app.width / 2,
                         app.height * 0.42f, 48, fade(kWhite, 0.85f * alpha),
                         Align::Center);
      const f32 width = app.width * 0.3f * left;
      draw_rounded_rect(r, (app.width - width) / 2, app.height * 0.42f + 20,
                        width, 4, 2, fade(kWhite, 0.7f * alpha));
    }
  }

  if (can_skip(game)) {
    const f32 pulse = 0.5f + 0.5f * std::sin(static_cast<f32>(app.now) * 4);
    if (const auto* skip = skin_element(app.skin, "play-skip")) {
      const SkinImage& image = skip->front();
      const f32        scale = ui_scale(app);
      draw_skin_image(r, image, app.width - image.width() * scale / 2,
                      app.height - image.height() * scale / 2, scale,
                      fade(kWhite, 0.7f + 0.3f * pulse));
    } else {
      const f32 w = 180, h = 56, x = app.width - w - 70,
                y = app.height - h - 70;
      draw_button(app, x, y, w, h, "SKIP  >>", 0.3f + 0.4f * pulse,
                  hex_color(0x6655CC));
      draw_text_shadowed(r, app.regular, "Space", x + w / 2, y + h + 18, 13,
                         fade(kWhite, 0.7f), Align::Center);
    }
  }

  if (game.watching)
    draw_text_shadowed(r, app.bold, "Watching " + game.replay_player,
                       app.width / 2, 92, 20, fade(kWhite, 0.8f),
                       Align::Center);

  if (game.phase == GamePhase::Playing)
    return;
  const bool failed = game.phase == GamePhase::Failed;
  // A failed map falls away before its menu appears.
  const f32  appear = clamp01(static_cast<f32>(
      (app.now - game.phase_changed - (failed ? 0.8 : 0)) / 0.3));
  if (appear <= 0)
    return;
  draw_rect(r, 0, 0, app.width, app.height,
            fade(failed ? hex_color(0x300010) : kBlack, 0.65f * appear));
  draw_text_shadowed(r, app.heavy, failed ? "FAILED" : "PAUSED", app.width / 2,
                     app.height * 0.33f, 72, fade(kWhite, appear),
                     Align::Center);
  const auto items = menu_items(game);
  for (size_t i = 0; i < items.size(); ++i) {
    const MenuButton       b = menu_button(app, i);
    const std::string_view item = items[i];
    const Color            accent = item == "Continue" ? hex_color(0x44BB77)
                                    : item == "Retry"  ? hex_color(0xDD9933)
                                                       : hex_color(0xCC3366);
    draw_button(app, b.x, b.y, b.w, b.h, item,
                static_cast<i32>(i) == game.menu_selection ? 1.0f : 0.0f,
                fade(accent, appear));
  }
}

}  // namespace

SliderRenderer create_slider_renderer() {
  SliderRenderer s{};
  s.program = load_program("shaders/slider.vert", "shaders/slider.frag");
  s.screen_size = glGetUniformLocation(s.program, "u_screen_size");
  s.radius = glGetUniformLocation(s.program, "u_radius");
  s.body_colour = glGetUniformLocation(s.program, "u_body_colour");
  s.border_colour = glGetUniformLocation(s.program, "u_border_colour");
  s.border_width = glGetUniformLocation(s.program, "u_border_width");
  s.alpha = glGetUniformLocation(s.program, "u_alpha");

  glGenVertexArrays(1, &s.vao);
  glBindVertexArray(s.vao);
  // Every instance is a quad, corners in [-1, 1], centred on its path point.
  constexpr f32 kCorners[] = {-1, -1, 1, -1, 1, 1, 1, 1, -1, 1, -1, -1};
  glGenBuffers(1, &s.corner_vbo);
  glBindBuffer(GL_ARRAY_BUFFER, s.corner_vbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof(kCorners), kCorners, GL_STATIC_DRAW);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(f32), nullptr);
  glEnableVertexAttribArray(0);
  glGenBuffers(1, &s.center_vbo);
  glBindBuffer(GL_ARRAY_BUFFER, s.center_vbo);
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vec2), nullptr);
  glEnableVertexAttribArray(1);
  glVertexAttribDivisor(1, 1);
  return s;
}

bool gameplay_start(App& app, i32 map_index, std::string replay_path) {
  if (map_index < 0)
    return false;
  const auto& info = app.library.maps[map_index];
  auto&       game = app.game;
  game = Gameplay{};
  game.map_index = map_index;
  if (!replay_path.empty()) {
    auto replay = read_replay(replay_path);
    if (!replay)
      return false;
    game.watching = true;
    game.replay_path = std::move(replay_path);
    game.replay_player = std::move(replay->player);
    game.frames = std::move(replay->frames);
  }
  game.parser = std::make_unique<fosu::Parser>();
  game.map = game.parser->parse_file(info.osu_path.c_str(),
                                     {.calculate_slider_paths = true,
                                      .calculate_slider_events = true,
                                      .apply_stacking = true});
  if (!game.map || game.map->hit_objects.empty())
    return false;
  const auto& map = *game.map;

  game.radius = circle_radius(map.cs);
  game.preempt = approach_preempt(map.ar);
  game.fade_in = approach_fade_in(game.preempt);
  game.windows = hit_windows(map.od);
  game.hp = map.hp;
  // A map's own colours win over the skin's.
  for (u32 rgb : map.combo_colours)
    game.combo_colours.push_back(hex_color(rgb));
  if (game.combo_colours.empty())
    game.combo_colours = app.skin.config.combo_colours;
  if (game.combo_colours.empty())
    game.combo_colours.assign(std::begin(kDefaultComboColours),
                              std::end(kDefaultComboColours));
  const SampleSet default_set =
      sample_set_or(static_cast<i32>(map.sample_set), SampleSet::Normal);

  i32       combo_number = 0;
  i32       colour = -1;
  bool      after_spinner = true;  // the first object starts a combo too
  size_t    timing = 0;
  f32       volume = 1;
  SampleSet timing_set = default_set;
  for (const auto& hit : map.hit_objects) {
    if (hit.is_hold())
      continue;
    // Objects use the timing point in effect at their time; osu! looks a few
    // ms ahead so objects exactly on a point use it.
    while (timing < map.timing_points.size() &&
           map.timing_points[timing].time <= hit.time + 5) {
      const auto& point = map.timing_points[timing++];
      volume = point.volume / 100.0f;
      timing_set =
          sample_set_or(static_cast<i32>(point.sample_set), default_set);
    }
    const i32  sample_volume = hit_sample_volume(hit.hit_sample);

    GameObject object{
        .kind = hit.is_slider()    ? ObjectKind::Slider
                : hit.is_spinner() ? ObjectKind::Spinner
                                   : ObjectKind::Circle,
        .time = hit.time,
        .end_time = std::max(hit.time, hit.end_time),
        .position = {hit.x, hit.y},
        .end_position = {hit.x, hit.y},
        .combo_number = 0,
        .colour = 0,
        .new_combo = hit.new_combo || after_spinner || hit.is_spinner(),
        .hitsound = resolve_hitsound(hit.hitsound, hit.hit_sample, timing_set),
        .volume = sample_volume > 0 ? sample_volume / 100.0f : volume,
    };
    if (object.new_combo) {
      colour += 1 + hit.combo_skip;
      combo_number = 0;
    }
    object.combo_number = ++combo_number;
    object.colour = colour;
    after_spinner = object.kind == ObjectKind::Spinner;

    if (object.kind == ObjectKind::Slider) {
      const auto& slider = map.sliders[hit.slider];
      object.slider = hit.slider;
      object.slides = std::max(1, slider.slides);
      object.span_duration = (object.end_time - object.time) / object.slides;
      object.path = sample_path(map.slider_paths[hit.slider], object.position);
      object.end_position =
          object.slides % 2 ? object.path.back() : object.position;
      object.edge_hitsounds =
          edge_hitsounds(slider.edge_sounds, slider.edge_sets,
                         object.slides + 1, object.hitsound, timing_set);
    } else if (object.kind == ObjectKind::Spinner) {
      object.required_spins =
          required_spins(object.end_time - object.time, map.od);
    }
    game.objects.push_back(std::move(object));
  }
  if (game.objects.empty())
    return false;
  game.states.assign(game.objects.size(), ObjectState{});

  const f64 first = game.objects.front().time;
  for (const auto& object : game.objects)
    game.end_time = std::max(game.end_time, object.end_time);
  f64 break_time = 0;
  for (const auto& gap : map.breaks)
    break_time += gap.end - gap.start;
  game.score_multiplier =
      difficulty_multiplier(map.hp, map.cs, map.od, game.objects.size(),
                            (game.end_time - first - break_time) / 1000);

  // Start early enough for the lead-in and for the first object to appear.
  const f64 start = std::min(
      {0.0, first - game.preempt - 1000, -static_cast<f64>(map.audio_lead_in)});
  set_clock(game, start + app.audio_offset, app.now);
  game.time = start;
  game.audio_ok = music_load(app.audio, info.audio_path);
  if (game.audio_ok) {
    music_pause(app.audio);
    music_set_speed(app.audio, 1);
    music_seek(app.audio, 0);
  } else {
    show_toast(app, "Audio file missing; playing silently");
  }
  select_map(app, map_index, false);
  return true;
}

void gameplay_update(App& app) {
  auto& game = app.game;
  if (game.phase != GamePhase::Playing) {
    const i32 hovered = menu_item_at(app, app.cursor);
    if (hovered >= 0 && hovered != game.menu_selection) {
      game.menu_selection = hovered;
      play_sound(app.audio, Sound::MenuHover, 0.5f);
    }
  }
  if (game.phase == GamePhase::Paused)
    return;
  if (game.phase == GamePhase::Failed) {
    const f32 t = static_cast<f32>(app.now - game.phase_changed);
    music_set_speed(app.audio, std::max(0.05f, 1 - t / kFailDuration));
    if (t >= kFailDuration)
      music_pause(app.audio);
    return;
  }

  // Start the music when its time comes, then follow it. Easing absorbs small
  // drift and the music position's coarse updates without objects jumping.
  f64 audio_time = audio_time_at(game, app.now);
  if (game.audio_ok && !game.audio_started && audio_time >= 0) {
    music_seek(app.audio, audio_time);
    music_play(app.audio);
    game.audio_started = true;
    game.resync_after = app.now + 0.3;
  } else if (game.audio_started && music_playing(app.audio) &&
             app.now >= game.resync_after) {
    const f64 error = music_position(app.audio) - audio_time;
    if (std::abs(error) > kResyncThreshold)
      game.clock_time += error;
    else
      game.clock_time += error * std::min(1.0, app.dt * kClockCorrectionRate);
    audio_time = audio_time_at(game, app.now);
  }
  const f64 previous = game.time;
  game.time = audio_time - app.audio_offset;
  const f64 elapsed = std::max(0.0, game.time - previous);
  if (game.watching) {
    play_replay(app);
  } else {
    game.cursor = app.cursor;
    if (game.frames.empty() ||
        game.time - game.frames.back().time >= kRecordInterval)
      record_frame(app, game.time);
  }

  for (const auto& press : game.pending)
    handle_press(app, press);
  game.pending.clear();
  expire_heads(app, game.time);
  update_sliders(app);
  update_spinners(app, elapsed);

  while (game.first_active < game.objects.size() &&
         game.states[game.first_active].result != Judgement::None &&
         game.time > game.objects[game.first_active].end_time + 1000)
    ++game.first_active;

  if (!in_break(game) && game.time >= game.objects.front().time &&
      game.time <= game.end_time)
    change_health(game, -drain_per_second(game.hp) * elapsed / 1000);
  // A replay fails where its input ends early, as when its player failed or
  // quit, not by health, which may have run differently.
  const bool replay_over = game.watching && !game.frames.empty() &&
                           game.time > game.frames.back().time + 1000;
  if (!game.finished && (game.watching ? replay_over : game.health <= 0)) {
    fail(app);
    return;
  }

  game.displayed_score =
      game.score + (game.displayed_score - game.score) * std::exp(-12 * app.dt);
  game.displayed_health = approach(game.displayed_health,
                                   static_cast<f32>(game.health), 10, app.dt);
  game.dim = approach(game.dim, in_break(game) ? 0.5f : 0.8f, 3, app.dt);
  std::erase_if(game.popups, [&](const Popup& popup) {
    return game.time - popup.time > kPopupDuration;
  });
  std::erase_if(game.recent_errors, [&](const HitError& error) {
    return app.now - error.wall_time > 4;
  });

  if (!game.finished && game.judged == game.objects.size()) {
    game.finished = true;
    game.finished_at = app.now;
  }
  // Only once, though the screen keeps updating while it fades out.
  if (game.finished && app.now - game.finished_at > kFinishDelay &&
      app.transition_started < 0)
    finish(app);
}

void gameplay_draw(App& app) {
  const auto& game = app.game;
  draw_backdrop(app, game.dim, 0);
  Playfield field = playfield(app);
  f32       alpha_scale = 1;
  if (game.phase == GamePhase::Failed) {
    const f32 t = static_cast<f32>(app.now - game.phase_changed);
    field.y += t * t * 300;
    alpha_scale = 1 - clamp01(t / 1.5f);
  }

  size_t end = game.first_active;
  while (end < game.objects.size() &&
         game.objects[end].time - game.preempt <= game.time)
    ++end;
  if (alpha_scale > 0) {
    draw_follow_points(app, field, end, alpha_scale);
    // Earlier objects draw on top of later ones.
    for (size_t i = end; i-- > game.first_active;) {
      switch (game.objects[i].kind) {
        case ObjectKind::Circle:
          draw_head(app, field, game.objects[i], game.states[i], alpha_scale);
          break;
        case ObjectKind::Slider:
          draw_slider(app, field, i, alpha_scale);
          break;
        case ObjectKind::Spinner:
          draw_spinner(app, field, i, alpha_scale);
          break;
      }
    }
    for (size_t i = end; i-- > game.first_active;)
      draw_approach_circle(app, field, i, alpha_scale);
    draw_popups(app, field, alpha_scale);
  }
  draw_hud(app);
  draw_game_overlays(app);
}

void gameplay_key(App& app, i32 key, i32 action, f64 wall) {
  auto& game = app.game;
  if (key == GLFW_KEY_Z || key == GLFW_KEY_X) {
    if (action != GLFW_REPEAT && !game.watching)
      set_hit_key(app, key == GLFW_KEY_Z ? kKey1 : kKey2, action == GLFW_PRESS,
                  wall);
    return;
  }
  if (action == GLFW_RELEASE)
    return;

  if (game.phase != GamePhase::Playing) {
    const i32 count = static_cast<i32>(menu_items(game).size());
    if (key == GLFW_KEY_UP || key == GLFW_KEY_DOWN) {
      const i32 step = key == GLFW_KEY_UP ? -1 : 1;
      game.menu_selection =
          (std::max(game.menu_selection, 0) + step + count) % count;
      play_sound(app.audio, Sound::MenuHover, 0.5f);
    } else if (key == GLFW_KEY_ENTER || key == GLFW_KEY_KP_ENTER) {
      activate_menu_item(app, game.menu_selection);
    } else if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS &&
               game.phase == GamePhase::Paused) {
      resume(app);
    }
    return;
  }
  if (action != GLFW_PRESS)
    return;
  switch (key) {
    case GLFW_KEY_ESCAPE:
      pause(app);
      break;
    case GLFW_KEY_SPACE:
      skip_intro(app);
      break;
    case GLFW_KEY_GRAVE_ACCENT:  // quick retry
      gameplay_start(app, game.map_index, game.replay_path);
      break;
  }
}

void gameplay_click(App& app, i32 button, i32 action, f64 wall) {
  auto&      game = app.game;
  const bool hit_button =
      button == GLFW_MOUSE_BUTTON_LEFT || button == GLFW_MOUSE_BUTTON_RIGHT;
  // Releases always count, so a button held through a pause, or while mouse
  // buttons are switched off, isn't stuck down.
  if (hit_button && !game.watching &&
      (action == GLFW_RELEASE ||
       (game.phase == GamePhase::Playing && app.mouse_buttons)))
    set_hit_key(app, button == GLFW_MOUSE_BUTTON_LEFT ? kMouse1 : kMouse2,
                action == GLFW_PRESS, wall);
  if (game.phase != GamePhase::Playing && button == GLFW_MOUSE_BUTTON_LEFT &&
      action == GLFW_PRESS)
    activate_menu_item(app, menu_item_at(app, app.cursor));
}

void gameplay_focus_lost(App& app) {
  if (app.game.phase == GamePhase::Playing && !app.game.finished)
    pause(app);
}

}  // namespace circles
