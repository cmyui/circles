#include <circles/app.h>
#include <circles/judging.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <random>
#include <span>
#include <string>
#include <utility>

namespace circles {

namespace {

constexpr f32 kTau = 6.2831853f;
constexpr f64 kHitAnimation = 240;    // ms a hit object takes to burst
constexpr f64 kMissAnimation = 60;    // ms a missed circle takes to fade
constexpr f64 kPopupDuration = 1100;  // ms a judgement stays on screen
constexpr f32 kFollowScale = 2.4f;    // follow circle over circle radius
constexpr f32 kFailDuration = 2;      // seconds the music takes to wind down
constexpr f64 kSixtyFrame = 1000.0 / 60;
// Stable shows results 3 seconds after the last object ends.
constexpr f64 kResultsDelay = 3000;

const Color kDefaultComboColours[] = {hex_color(0xFFC000), hex_color(0x00CA00),
                                      hex_color(0x127CFF), hex_color(0xF21839)};

const Color kColour300 = hex_color(0x66CCFF);
const Color kColour100 = hex_color(0x88EE55);
const Color kColour50 = hex_color(0xFFCC22);
const Color kColourMiss = hex_color(0xFF4466);

// The 512x384 playfield, placed as osu!stable places it: scaled with the
// window's height as if it were 480 tall, centred across, and a little above
// centre down.
struct Playfield {
  f32 scale;  // window points per osu! pixel
  f32 x, y;   // top-left corner

  Vec2 to_screen(Vec2 p) const { return {x + p.x * scale, y + p.y * scale}; }
  Vec2 to_playfield(Vec2 p) const {
    return {(p.x - x) / scale, (p.y - y) / scale};
  }
};

// Stable shortens windows squarer than 5:4 to that shape; here that game
// area is centred in the window instead.
Playfield playfield(const App& app) {
  const f32 height = std::min(app.height, std::trunc(app.width * 0.8f));
  const f32 scale = height / 480;
  return {scale, (app.width - kPlayfieldWidth * scale) / 2,
          (app.height - height) / 2 +
              (height - kPlayfieldHeight * scale) * 0.75f - 16 * scale};
}

const Vec2 kPlayfieldCentre{kPlayfieldWidth / 2, kPlayfieldHeight / 2};

// ---- Clock ----

// Stable's offset, whole ms of song time between the music and the game:
// 24 for maps before v5, a ms back under Half Time, and the global offset
// scaled by the speed.
i32 song_offset(const App& app) {
  const auto& game = app.game;
  i32         offset = game.map->format_version < 5 ? 24 : 0;
  if (game.rate < 1)
    offset -= static_cast<i32>((100 - game.rate * 100) / 75.0f * 5);
  return offset + static_cast<i32>(game.rate * app.audio_offset);
}

// Stable's audio clock: it runs on frame time while the music plays, easing
// toward the music's reported position by an eighth each frame, catching up
// or slowing when off, and snapping when far off.
void advance_clock(App& app) {
  auto&     game = app.game;
  const f64 increase = app.dt * 1000 * game.rate;
  if (!game.audio_started || !music_playing(app.audio)) {
    // Before the music, stable's clock skips over hitches.
    if (increase < 60)
      game.clock_time += increase;
    return;
  }
  const f64 source = music_position(app.audio);
  bool      snap = game.clock_time == 0 || source == 0;
  if (!snap) {
    f64 proposed = game.clock_time + increase;
    proposed -= (proposed - source) / 8;
    const f64 diff = proposed - source;
    const f64 aim =
        app.now - game.clock_synced < 1.5 || game.rate < 1 ? 11 : 33;
    if (std::abs(diff) > aim * 2) {
      snap = true;
    } else if (diff < -aim) {
      game.clock_time += increase * 2;
      game.clock_synced = app.now;
    } else if (diff < aim) {
      game.clock_time = proposed;
    } else {
      game.clock_time += increase / 2;
      game.clock_synced = app.now;
    }
  }
  if (snap) {
    game.clock_time = source;
    game.clock_synced = app.now;
  }
}

// The game's whole-ms time, as stable's game loop sees it.
i32 game_time(const App& app) {
  // Halves round to even, as .NET's Math.Round does.
  return static_cast<i32>(std::nearbyint(app.game.clock_time)) -
         song_offset(app);
}

bool in_break(const Gameplay& game) {
  for (const auto& gap : game.breaks)
    if (game.time >= gap.start && game.time <= gap.end)
      return true;
  return false;
}

bool can_skip(const Gameplay& game) {
  return game.phase == GamePhase::Playing && game.skip_boundary &&
         game.time < *game.skip_boundary;
}

// Stable's countdown, which sets where skipping lands when a map has one:
// six of its beats before "go", the beat before the first object.
std::optional<i32> countdown_skip(const fosu::Beatmap&                  map,
                                  const std::vector<fosu::TimingPoint>& timing,
                                  f64                                   first) {
  const f64 offset = beat_offset_at(timing, first);
  f64       original = beat_length_at(timing, first);
  if (original <= 0)
    original = beat_length_at(timing, 0);
  if (original <= 0 || map.countdown == 0)
    return std::nullopt;
  f64 go = offset - 5;
  f64 beat = original <= 333 ? original * 2 : original;
  if (map.countdown == 3)
    beat /= 2;
  else if (map.countdown == 2)
    beat *= 2;
  first -= beat * map.countdown_offset;
  if (go >= first)
    while (go > first - beat)
      go -= beat;
  i32 divisor = 1;
  while (go < first - beat / divisor) {
    go += beat;
    const f64 in_bar = std::fmod(static_cast<f32>(go - offset) / original, 4.0);
    divisor = in_bar > 1.5 && in_bar < 3.5 ? 2 : 1;
  }
  go -= beat;
  if (go - 4 * beat <= 0)
    return std::nullopt;
  return static_cast<i32>(go) - 6 * static_cast<i32>(beat);
}

// ---- Phases ----

void pause(App& app) {
  music_pause(app.audio);
  app.game.phase = GamePhase::Paused;
  app.game.phase_changed = app.now;
  app.game.menu_selection = 0;
}

void resume(App& app) {
  auto& game = app.game;
  if (game.audio_started)
    music_play(app.audio);
  game.phase = GamePhase::Playing;
}

void fail(App& app) {
  auto& game = app.game;
  game.phase = GamePhase::Failed;
  game.phase_changed = app.now;
  game.menu_selection = 0;
  play_sound(app.audio, Sound::ComboBreak, 1);
}

// Easy's revive: the music waits while the bar refills from where it fell.
void recover(App& app) {
  auto& game = app.game;
  music_pause(app.audio);
  game.recovery_display = game.displayed_health * kHealthMax;
  game.phase = GamePhase::Recovering;
  game.phase_changed = app.now;
}

void skip_intro(App& app) {
  auto& game = app.game;
  if (!can_skip(game))
    return;
  // Stable lands a little either side of the boundary, at random.
  static std::mt19937 engine{std::random_device{}()};
  const i32           target =
      *game.skip_boundary + std::uniform_int_distribution<i32>(-80, 89)(engine);
  game.clock_time = target + song_offset(app);
  if (game.audio_ok && game.clock_time >= 0) {
    music_seek(app.audio, game.clock_time);
    music_play(app.audio);
    game.audio_started = true;
  }
  game.skip_boundary.reset();
  play_sound(app.audio, Sound::MenuClick, 0.6f);
}

// ---- Input and replays ----

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

// Shows a replay frame's keys on the key overlay.
void show_replay_keys(Gameplay& game, u32 keys) {
  for (i32 key = 0; key < kHitKeyCount; ++key) {
    const bool down = replay_holds(keys, key);
    game.press_counts[key] += down && !game.held[key];
    game.held[key] = down;
  }
}

// Where a replay's cursor is at `time`, between the frames either side.
// The frames must be in time order.
Vec2 frame_position(const std::vector<ReplayFrame>& frames, f64 time) {
  if (frames.empty())
    return kPlayfieldCentre;
  const auto next = std::upper_bound(frames.begin(), frames.end(), time,
                                     [](f64 t, const ReplayFrame& frame) {
                                       return t < frame.time;
                                     });
  if (next == frames.begin())
    return next->position;
  if (next == frames.end())
    return frames.back().position;
  const ReplayFrame& from = *(next - 1);
  const f64          span = next->time - from.time;
  return lerp(
      from.position, next->position,
      span > 0 ? clamp01(static_cast<f32>((time - from.time) / span)) : 1);
}

// The cursor while playing, in osu! pixels: Autopilot's, or the mouse's.
Vec2 live_cursor(const App& app, f64 time) {
  if (app.game.mods & kAutopilot)
    return frame_position(app.game.autopilot_frames, time);
  return playfield(app).to_playfield(app.cursor);
}

// Where a watched replay's cursor is drawn: between its frames.
void follow_replay_cursor(App& app) {
  auto&       game = app.game;
  const auto& frames = game.frames;
  if (frames.empty())
    return;
  const size_t next = std::min(game.next_frame, frames.size() - 1);
  const auto&  from = frames[game.next_frame > 0 ? game.next_frame - 1 : 0];
  const auto&  to = frames[next];
  const f64    span = to.time - from.time;
  const f32    t =
      span > 0 ? clamp01(static_cast<f32>((game.time - from.time) / span)) : 1;
  game.cursor = playfield(app).to_screen(lerp(from.position, to.position, t));
  game.replay_trail.push_back({app.now, game.cursor});
}

// Stable's 60 Hz "scorable" frames, by wall time, keeping their phase.
bool sixty_hz_frame(App& app) {
  auto&     game = app.game;
  const f64 wall = app.now * 1000;
  const f64 length = wall - game.sixty_last;
  if (length < kSixtyFrame)
    return false;
  const bool busy =
      game.active >= 0 && game.objects[game.active].kind != ObjectKind::Circle;
  game.sixty_last = !busy && length > 500 ? wall - kSixtyFrame
                                          : wall - (length - kSixtyFrame);
  return true;
}

// ---- Mods ----

// The hole narrows at 100 and 200 combo and opens wide during breaks. It
// trails the cursor slightly.
void update_flashlight(App& app) {
  auto&     game = app.game;
  const f32 target = in_break(game)     ? 8
                     : game.combo < 100 ? 3.2f
                     : game.combo < 200 ? 2.6f
                                        : 2;
  const f32 step = 6 * app.dt;
  game.flashlight_scale +=
      std::clamp(target - game.flashlight_scale, -step, step);
  const f32 t = std::min(1.0f, app.dt * 1000 / 120);
  game.flashlight_position =
      lerp(game.flashlight_position, game.cursor, t * (2 - t));
}

// Nightcore's drum track on the half beats of the timing point in effect: a
// kick and cymbal every eight bars' worth of half beats, then kick, hat,
// clap, hat. Hats play only on maps with an even slider tick rate.
void play_nightcore_beat(App& app) {
  auto&                    game = app.game;
  const fosu::TimingPoint* point = nullptr;
  for (const auto& candidate : game.timing) {
    if (candidate.time > game.time)
      break;
    if (candidate.uninherited && candidate.beat_length > 0)
      point = &candidate;
  }
  if (!point || !game.audio_started)
    return;
  const i64 beat = static_cast<i64>(
      std::floor((game.time - point->time) * 2 / point->beat_length));
  if (beat == game.nightcore_beat)
    return;
  game.nightcore_beat = beat;
  const i32     meter = std::max(1, point->meter);
  constexpr u32 kOmitFirstBarLine = 8;
  if (beat % (8 * meter) == 0) {
    play_sound(app.audio, Sound::NightcoreKick, 1);
    if (beat != 0 || !(point->effects & kOmitFirstBarLine))
      play_sound(app.audio, Sound::NightcoreFinish, 1);
    return;
  }
  const i64 in_bar = beat % meter % 4;
  if (in_bar == 0)
    play_sound(app.audio, Sound::NightcoreKick, 1);
  else if (in_bar == 2)
    play_sound(app.audio, Sound::NightcoreClap, 1);
  else if (static_cast<i32>(game.map->slider_tick_rate) % 2 == 0)
    play_sound(app.audio, Sound::NightcoreHat, 1);
}

// ---- Autoplay ----

enum class Easing : u8 { Out, In, InOutCubic };

f32 ease(Easing easing, f32 t) {
  switch (easing) {
    case Easing::Out:
      return -t * (t - 2);
    case Easing::In:
      return t * t;
    case Easing::InOutCubic:
      return t < 0.5f ? 4 * t * t * t : (t - 1) * (2 * t - 2) * (2 * t - 2) + 1;
  }
  return t;
}

// Inserts after any frames at the same time.
void add_frame(std::vector<ReplayFrame>& frames, ReplayFrame frame) {
  const auto at = std::upper_bound(frames.begin(), frames.end(), frame.time,
                                   [](f64 t, const ReplayFrame& f) {
                                     return t < f.time;
                                   });
  frames.insert(at, frame);
}

size_t insertion_index(const std::vector<ReplayFrame>& frames, f64 time) {
  return std::upper_bound(frames.begin(), frames.end(), time,
                          [](f64 t, const ReplayFrame& f) {
                            return t < f.time;
                          }) -
         frames.begin();
}

// Stable's autoplay: a perfect play as replay frames at 60 per second of
// real time. Auto and Cinema watch it; Autopilot's cursor follows it, with
// movements held back as late as allows.
std::vector<ReplayFrame> autoplay_frames(const Gameplay& game) {
  const bool   delayed = game.mods & kAutopilot;
  const Easing preferred = delayed ? Easing::InOutCubic : Easing::Out;
  const auto&  objects = game.objects;
  std::vector<ReplayFrame> replay;
  add_frame(replay, {-100000, {256, 500}, 0});
  add_frame(replay, {objects.front().time - 1500, {256, 500}, 0});
  add_frame(replay, {objects.front().time - 1000, {256, 192}, 0});
  const f32     frame_delay = static_cast<f32>(1000.0 / 60 * game.rate);
  const Vec2    centre = kPlayfieldCentre;
  constexpr f32 kSpinnerRadius = 50;
  const i32     reaction = static_cast<i32>(100 * game.rate);
  const f64     hit50 = game.windows.hit50, hit100 = game.windows.hit100;
  i32           button_index = 0;
  for (size_t i = 0; i < objects.size(); ++i) {
    const GameObject& h = objects[i];
    const bool        spinner = h.kind == ObjectKind::Spinner;
    if (delayed && i > 0) {
      // Linger on each object as long as possible.
      const GameObject& last = objects[i - 1];
      const bool        last_spinner = last.kind == ObjectKind::Spinner;
      for (const auto [lead, linger] :
           {std::pair{400.0, hit50}, {hit50, hit50}, {hit100, hit100}}) {
        if (h.time - lead > last.end_time + linger + 50) {
          if (!last_spinner && h.time - last.end_time < 1000)
            add_frame(replay, {last.end_time + linger, last.end_position, 0});
          if (!spinner)
            add_frame(replay, {h.time - lead, h.position, 0});
          break;
        }
      }
    }
    Vec2   target = h.position;
    Easing easing = preferred;
    f32    direction = -1;
    if (spinner) {
      // Enter the spinner's circle on a tangent.
      target = replay.back().position;
      Vec2      difference = centre - target;
      const f32 length = difference.length();
      if (length > kSpinnerRadius) {
        const f32 new_length =
            std::sqrt(length * length - kSpinnerRadius * kSpinnerRadius);
        const f32 angle = std::asin(kSpinnerRadius / length);
        direction = angle > 0 ? -1 : 1;
        difference.x =
            difference.x * std::cos(angle) - difference.y * std::sin(angle);
        difference.y =
            difference.x * std::sin(angle) + difference.y * std::cos(angle);
        const f32 inverse = 1.0f / difference.length();
        difference = difference * inverse * new_length;
        target = target + difference;
        easing = Easing::In;
      } else if (length > 0) {
        target = centre - difference * (kSpinnerRadius / length);
      } else {
        target = centre + Vec2{0, -kSpinnerRadius};
      }
    }

    {
      ReplayFrame last = replay.back();
      // Auto waits until it could see and react to the next object.
      const i32   wait =
          static_cast<i32>(h.time - std::max(0.0, game.preempt - reaction));
      if (wait > last.time) {
        last = {static_cast<f64>(wait), last.position, last.keys};
        add_frame(replay, last);
      }
      const f64 gap = (h.time - last.time) / game.rate;
      // Glide to objects far enough apart; stream through close ones.
      if (gap > 0 && ((last.position - target).length() >
                          game.radius * (1.5 + 100.0 / gap) ||
                      gap >= 266)) {
        for (f32 t = static_cast<f32>(last.time) + frame_delay; t < h.time;
             t += frame_delay) {
          const f32 progress =
              static_cast<f32>((t - last.time) / (h.time - last.time));
          add_frame(
              replay,
              {std::trunc(t),
               lerp(last.position, target, ease(easing, progress)), last.keys});
        }
        button_index = 0;
      } else {
        ++button_index;
      }
    }

    u32          button = button_index % 2 ? kReplayMouse2 : kReplayMouse1;
    ReplayFrame  press{h.time, target, button};
    ReplayFrame  release{h.end_time + (spinner ? 1 : 0), h.end_position, 0};
    const size_t before = insertion_index(replay, press.time);
    if (before > 0 && replay[before - 1].keys) {
      const u32 previous = replay[before - 1].keys;
      if (previous == button) {
        button = (kReplayMouse1 | kReplayMouse2) & ~button;
        press.keys = button;
      }
      // Follow only the newest slider or spinner: drop frames during it.
      const size_t end = insertion_index(replay, release.time);
      if (before < replay.size())
        replay.erase(replay.begin() + before,
                     replay.begin() + std::max(before, end));
      for (size_t j = before; j < replay.size(); ++j)
        if (j + 1 < replay.size() || replay[j].keys == previous)
          replay[j].keys = button;
    }
    add_frame(replay, press);

    if (spinner) {
      const Vec2 from = target - centre;
      const f32  radius = from.length();
      const f32  angle = radius == 0 ? 0 : std::atan2(from.y, from.x);
      auto       spin_point = [&](f64 elapsed) {
        const f32 t = static_cast<f32>(elapsed / game.rate) * direction;
        return centre +
               Vec2{
                   static_cast<f32>(std::cos(t / 20 + angle) * kSpinnerRadius),
                   static_cast<f32>(std::sin(t / 20 + angle) * kSpinnerRadius)};
      };
      for (f32 j = static_cast<f32>(h.time) + frame_delay; j < h.end_time;
           j += frame_delay)
        add_frame(replay, {std::trunc(j), spin_point(j - h.time), button});
      const Vec2 end = spin_point(h.end_time - h.time);
      add_frame(replay, {h.end_time, end, button});
      release.position = end;
    } else if (h.kind == ObjectKind::Slider) {
      f64 last_time = 0;
      for (const BallMove& move : h.moves) {
        if (last_time != 0 && move.start - last_time < frame_delay)
          continue;
        add_frame(replay, {static_cast<f64>(move.start), move.from, button});
        last_time = move.start;
      }
      add_frame(replay, {h.end_time, h.end_position, button});
    }
    // Let go only if nothing later is still going on.
    if (replay.back().time <= release.time)
      add_frame(replay, release);
  }
  return replay;
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
  auto& game = app.game;
  // Cinema is just for watching; it ends back at song select.
  if (game.mods & kCinema) {
    switch_screen(app, Screen::SongSelect);
    return;
  }
  std::string player = game.replay_player;
  std::string replay_path = game.replay_path;
  if (!game.watching) {
    const char* user = std::getenv("USER");
    player = user ? user : "player";
    // Stable starts every replay with two frames of a hidden cursor.
    game.frames.insert(
        game.frames.begin(),
        {{0, {256, -500}, 0},
         {static_cast<f64>(game.lead_frame_time), {256, -500}, 0}});
    replay_path =
        save_replay(app, game.map_index,
                    {.beatmap_md5 = app.library.maps[game.map_index].md5,
                     .player = player,
                     .counts = game.counts,
                     .score = game.score,
                     .max_combo = game.max_combo,
                     .perfect = game.max_combo >= game.health_rules.max_combo,
                     .mods = game.mods,
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
      .mods = game.mods,
  };
  switch_screen(app, Screen::Results);
}

// ---- Drawing ----

// A watched play's timeline, along the window's bottom edge, under the hit
// error meter.
struct Timeline {
  f32 x, y, w;
};

Timeline timeline(const App& app) {
  return {0, app.height - 3, app.width};
}

bool over_timeline(const App& app) {
  return app.cursor.y >= app.height - 20;
}

f64 timeline_time(const App& app) {
  const Timeline t = timeline(app);
  const f64      fraction = clamp01((app.cursor.x - t.x) / t.w);
  return app.game.timeline_start +
         fraction * (app.game.end_time - app.game.timeline_start);
}

bool can_scrub(const Gameplay& game) {
  return game.watching && game.phase == GamePhase::Playing;
}

// A press note lock blocked shakes its object 8 pixels side to side for
// 120 ms, as stable does.
f32 shake_offset(const App& app, const ObjectState& state) {
  const f32 ms = static_cast<f32>((app.now - state.shake_time) * 1000);
  if (ms < 0 || ms >= 120)
    return 0;
  constexpr f32 kStops[] = {0, 8, -8, 8, -8, -8, 0};
  const i32     step = static_cast<i32>(ms / 20);
  // Stable's fifth step starts from the right again.
  const f32     from = step == 4 ? 8 : kStops[step];
  return from + (kStops[step + 1] - from) * (ms - step * 20) / 20;
}

// Which pass along its path a slider's ball is on.
i32 current_span(const GameObject& object, f64 time) {
  if (object.span_duration <= 0)
    return 0;
  return std::clamp(
      static_cast<i32>(std::floor((time - object.time) / object.span_duration)),
      0, object.slides - 1);
}

f32 appear_alpha(const Gameplay& game, const GameObject& object) {
  return clamp01(static_cast<f32>((game.time - (object.time - game.preempt)) /
                                  game.fade_in));
}

// Hidden's circles fade in over the first 40% of the approach, then out over
// the next 30%.
f32 hidden_circle_alpha(const Gameplay& game, f64 time) {
  const f64 p = game.preempt;
  if (game.time < time - 0.6 * p)
    return clamp01(static_cast<f32>((game.time - (time - p)) / (0.4 * p)));
  return 1 -
         clamp01(static_cast<f32>((game.time - (time - 0.6 * p)) / (0.3 * p)));
}

// Hidden's slider bodies fade in over 400 ms, then out until the slider ends.
f32 hidden_body_alpha(const Gameplay& game, const GameObject& object) {
  const f64 shown = object.time - game.preempt + 400;
  if (game.time < shown)
    return clamp01(static_cast<f32>((game.time - (shown - 400)) / 400));
  const f32 x = clamp01(
      static_cast<f32>((game.time - shown) / (object.end_time - shown)));
  return 1 - x * (2 - x);
}

// The circle alpha before any hit animation, with Hidden's fading.
f32 circle_alpha(const Gameplay& game, const GameObject& object) {
  return game.mods & kHidden ? hidden_circle_alpha(game, object.time)
                             : appear_alpha(game, object);
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
  const f32        appear = circle_alpha(game, object);
  const CirclePart part = object.kind == ObjectKind::Slider
                              ? CirclePart::SliderStart
                              : CirclePart::Hit;
  // An unhit circle fades out between its 100 and 50 windows' ends.
  const f32        expiring =
      1 - clamp01(static_cast<f32>(
              (game.time - (object.time + game.windows.hit100)) /
              (game.windows.hit50 - game.windows.hit100)));
  if (state.head == Judgement::None) {
    draw_hit_circle(app, center, radius, colour,
                    appear * expiring * alpha_scale, object.combo_number, part);
  } else if (state.head == Judgement::Miss) {
    const f32 t =
        static_cast<f32>((game.time - state.head_time) / kMissAnimation);
    if (t < 1)
      draw_hit_circle(app, center, radius, colour,
                      (1 - t) * appear * expiring * alpha_scale,
                      object.combo_number, part);
  } else if (!(game.mods & kHidden)) {
    // A hit swells to 1.4 times its size as it fades; with Hidden it's
    // simply gone, as in osu!.
    const f32 t =
        static_cast<f32>((game.time - state.head_time) / kHitAnimation);
    if (t < 1)
      draw_hit_circle(app, center, radius * (1 + 0.4f * t * (2 - t)), colour,
                      (1 - t) * alpha_scale, object.combo_number, part);
  }
}

void draw_approach_circle(App&             app,
                          const Playfield& field,
                          size_t           index,
                          f32              alpha_scale) {
  const auto& game = app.game;
  const auto& object = game.objects[index];
  // Hidden shows only the first object's.
  if (object.kind == ObjectKind::Spinner ||
      game.states[index].head != Judgement::None || game.time >= object.time ||
      (game.mods & kHidden && index > 0))
    return;
  const Vec2 center = field.to_screen(object.position);
  const f32  radius = static_cast<f32>(game.radius) * field.scale;
  const f32  growth =
      1 + 3 * static_cast<f32>((object.time - game.time) / game.preempt);
  // It fades in to 0.9 over 800 ms, or the whole approach if shorter.
  const Color colour = fade(
      combo_colour(game, object),
      0.9f *
          clamp01(static_cast<f32>((game.time - (object.time - game.preempt)) /
                                   std::min(800.0, game.preempt))) *
          alpha_scale);
  if (const auto* approach = skin_element(app.skin, "approachcircle")) {
    draw_skin_image(app.renderer, approach->front(), center.x, center.y,
                    object_scale(radius) * growth, colour);
    return;
  }
  const f32 ring = radius * growth;
  draw_circle(app.renderer, center.x, center.y, ring, colour,
              1 - radius * 0.09f / ring);
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
  const bool  hidden = game.mods & kHidden;
  f32         alpha = alpha_scale;
  if (state.result != Judgement::None)
    alpha *= 1 - clamp01(static_cast<f32>((game.time - state.result_time) /
                                          kHitAnimation));
  const f32 appear = appear_alpha(game, object) * alpha;
  const f32 body_alpha =
      hidden ? hidden_body_alpha(game, object) * alpha : appear;
  // The body grows from the head over the first third of its approach, as
  // stable's snaking does.
  const f32 snake = std::min(
      1.0f, static_cast<f32>(game.time - (object.time - game.preempt)) /
                (static_cast<f32>(game.preempt) / 3));
  const i32 span = current_span(object, game.time);

  if (body_alpha > 0) {
    // Bodies are drawn for the playfield at rest; a shake or a fall moves
    // the finished texture.
    const Playfield rest = playfield(app);
    draw_slider_body(app, index, colour, snake, rest.scale, {rest.x, rest.y},
                     {field.x - rest.x, field.y - rest.y}, body_alpha);
  }
  if (appear > 0) {
    const auto* tick_image = skin_element(skin, "sliderscorepoint");
    for (const SliderPoint& point : object.points) {
      // Ticks on the first pass appear as the body reaches them.
      if (point.kind != PointKind::Tick || point.span != span ||
          point.time <= game.time ||
          (span == 0 &&
           (point.time - object.time) / object.span_duration > snake))
        continue;
      const Vec2 tick = field.to_screen(point.position);
      // Hidden fades each tick out over the second before it.
      f32        tick_alpha = appear;
      if (hidden) {
        const f64 from = std::max(object.time - game.preempt + game.fade_in,
                                  point.time - 1000.0);
        tick_alpha *=
            1 -
            clamp01(static_cast<f32>((game.time - from) / (point.time - from)));
      }
      if (tick_image) {
        draw_skin_image(r, tick_image->front(), tick.x, tick.y, scale,
                        fade(kWhite, tick_alpha));
        continue;
      }
      draw_circle(r, tick.x, tick.y, radius * 0.16f,
                  fade(kBlack, 0.4f * tick_alpha));
      draw_circle(r, tick.x, tick.y, radius * 0.11f, fade(kWhite, tick_alpha));
    }

    if (snake >= 1) {
      // Hidden shows the end only on the first pass, fading like a circle.
      const f32 end_alpha =
          hidden
              ? (span == 0 ? hidden_circle_alpha(game, object.time) * alpha : 0)
              : appear * 0.9f;
      draw_hit_circle(app, field.to_screen(object.path_end), radius, colour,
                      end_alpha, 0, CirclePart::SliderEnd);
      // While repeats remain, an arrow marks where the ball turns back.
      if (span < object.slides - 1 && state.result == Judgement::None) {
        const bool at_end = span % 2 == 0;
        const Vec2 turn = at_end ? object.path_end : object.position;
        const Vec2 toward = at_end ? object.tail_inward : object.head_inward;
        if (!at_end && !hidden)
          draw_hit_circle(app, field.to_screen(turn), radius, colour, appear, 0,
                          CirclePart::SliderEnd);
        draw_reverse_arrow(app, field.to_screen(turn), field.to_screen(toward),
                           radius, appear);
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
  // These images are drawn at 0.625 of their 1x size in playfield pixels,
  // starting at 0.8 of that and growing to full as the spinner fills.
  const f32   scale =
      0.625f * field.scale * (0.8f + 0.2f * completion * (2 - completion));
  const f32 elapsed =
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
  // A ring shrinking from 1.86 times size to almost nothing; not for Hidden.
  if (const auto* approach = skin_element(skin, "spinner-approachcircle");
      approach && !(game.mods & kHidden) && game.time >= object.time)
    draw_skin_image(r, approach->front(), c.x, c.y,
                    0.625f * field.scale * (1.86f - 1.76f * elapsed),
                    fade(kWhite, alpha));
  const f32 label_y = kPlayfieldHeight * 0.3f * field.scale;
  if (const auto* spin = skin_element(skin, "spinner-spin");
      spin && (state.scored_turns == 0 || game.time < object.time + 500))
    draw_skin_image(r, spin->front(), c.x, c.y + label_y, ui_scale(app),
                    fade(kWhite, alpha));
  if (const auto* clear = skin_element(skin, "spinner-clear");
      clear && state.scored_turns >= object.required_spins)
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
        1 - clamp01(static_cast<f32>((game.time - state.result_time) / 240));
  if (alpha <= 0)
    return;
  const Vec2 c = field.to_screen(kPlayfieldCentre);
  const f32  size = kPlayfieldHeight * 0.48f * field.scale;
  const f32  completion =
      clamp01(state.half_turns / std::max(1, object.required_spins));
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
  // Stable shows "SPIN!" until spinning starts, and "CLEAR!" once it's
  // enough for a 50.
  const char* label = state.scored_turns >= object.required_spins ? "CLEAR!"
                      : state.scored_turns == 0 || game.time < object.time + 500
                          ? "SPIN!"
                          : nullptr;
  if (label && !skinned)
    draw_text_shadowed(
        r, app.heavy, label, c.x,
        centered_baseline(app.heavy, label_size, c.y - size * 0.35f),
        label_size, fade(kWhite, alpha), Align::Center);
  char rpm[32];
  snprintf(rpm, sizeof(rpm), "%ld RPM", std::lround(state.rpm));
  draw_text_shadowed(r, app.bold, rpm, c.x, c.y + size * 0.62f,
                     label_size * 0.6f, fade(kWhite, 0.85f * alpha),
                     Align::Center);
  const f32 bonus_age = static_cast<f32>((game.time - state.bonus_time) / 800);
  if (bonus_age < 1) {
    char bonus[32];
    snprintf(bonus, sizeof(bonus), "%d", state.bonus);
    const f32 bonus_size = label_size * (2 - 0.72f * ease_out(bonus_age));
    draw_text_shadowed(
        r, app.heavy, bonus, c.x,
        centered_baseline(app.heavy, bonus_size, c.y + size * 0.4f), bonus_size,
        fade(kWhite, (1 - bonus_age) * alpha), Align::Center);
  }
}

// Dashes leading from one object to the next in a combo, vanishing as the
// next object's time arrives.
void draw_follow_points(App& app, const Playfield& field, f32 alpha_scale) {
  const auto& game = app.game;
  const auto* sprite = skin_element(app.skin, "followpoint");
  const f32   scale = object_scale(static_cast<f32>(game.radius) * field.scale);
  // As stable spaces them: from 48 pixels out, every 32, stopping 32 short.
  // Each fades in over 400 ms, from 800 ms before the cursor should pass it,
  // and out over 400 ms once it should have.
  for (size_t i = std::max<size_t>(game.first_active, 1);
       i < game.objects.size() &&
       game.objects[i - 1].end_time - 800 <= game.time;
       ++i) {
    const auto& from = game.objects[i - 1];
    const auto& to = game.objects[i];
    if (to.new_combo || from.kind == ObjectKind::Spinner ||
        to.kind == ObjectKind::Spinner)
      continue;
    const Vec2 delta = to.position - from.end_position;
    const i32  distance = static_cast<i32>(delta.length());
    const f32  angle = std::atan2(delta.y, delta.x);
    const f64  length = to.time - from.end_time;
    for (i32 j = 48; j < distance - 32; j += 32) {
      const f32 fraction = static_cast<f32>(j) / distance;
      const f64 passes = std::trunc(from.end_time + fraction * length);
      const f64 appears = passes - 800;
      const f32 in = clamp01(static_cast<f32>((game.time - appears) / 400));
      const f32 alpha =
          in * (1 - clamp01(static_cast<f32>((game.time - passes) / 400)));
      if (alpha <= 0)
        continue;
      const f32  slide = in * (2 - in);  // eased out
      const Vec2 at = field.to_screen(from.end_position +
                                      delta * (fraction - 0.1f * (1 - slide)));
      if (sprite) {
        draw_skin_image(
            app.renderer,
            skin_frame(app.skin, *sprite, (game.time - appears) / 1000), at.x,
            at.y, scale * (1.5f - 0.5f * slide),
            fade(kWhite, alpha * alpha_scale), angle);
        continue;
      }
      const Vec2 d = Vec2{std::cos(angle), std::sin(angle)} * (5 * field.scale);
      draw_line(app.renderer, at.x - d.x, at.y - d.y, at.x + d.x, at.y + d.y,
                2.2f * field.scale, fade(kWhite, 0.7f * alpha * alpha_scale));
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
    // As stable animates them: in over 120 ms, out from 500 ms to 1100.
    // Hits pop to 1.1 and settle; misses shrink from double size while
    // dropping and tilting.
    const bool miss = popup.judgement == Judgement::Miss;
    const f32  alpha =
        clamp01(age / 120) * (1 - clamp01((age - 500) / 600)) * alpha_scale;
    f32  pop = 1;
    f32  tilt = 0;
    Vec2 at = field.to_screen(popup.position);
    if (miss) {
      pop = game.mods & kSuddenDeath ? 2 + 4 * clamp01(age / 600)
                                     : 2 - clamp01(age / 120);
      const f32 drop = clamp01(age / 1100);
      at.y += (-5 + 45 * drop * drop) * field.scale;
      tilt = age < 120 ? popup.tilt * age / 120
                       : popup.tilt * (1 + clamp01((age - 120) / 980) *
                                               clamp01((age - 120) / 980));
    } else if (age < 96) {
      pop = 0.6f + 0.5f * age / 96;
    } else if (age < 120) {
      pop = 1.1f;
    } else {
      pop = 0.9f + 0.1f * clamp01((age - 120) / 48);
    }

    const char* element = popup.judgement == Judgement::Hit300   ? "hit300"
                          : popup.judgement == Judgement::Hit100 ? "hit100"
                          : popup.judgement == Judgement::Hit50  ? "hit50"
                                                                 : "hit0";
    if (const auto* frames = skin_element(app.skin, element)) {
      draw_skin_image(app.renderer, skin_frame(app.skin, *frames, age / 1000),
                      at.x, at.y, object_scale(radius) * pop,
                      fade(kWhite, alpha), tilt);
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

// The active mods' icons, as stable's Player shows them: at their own size
// from the top right, each further left and on top, fanned closely in play
// and spread apart in replays. Each grows in from double size after the last.
void draw_gameplay_mod_icons(App& app) {
  const auto& game = app.game;
  struct Icon {
    Mods             mod;  // stable's order is its bits'
    const ModButton* button;
    i32              state;
  };
  std::vector<Icon> icons;
  for (const ModRow& row : mod_rows())
    for (const ModButton& button : row.buttons)
      if (const i32 state = mod_button_state(game.mods, button))
        icons.push_back(
            {state == 2 ? button.mods[1] & ~button.mods[0] : button.mods[0],
             &button, state});
  std::sort(icons.begin(), icons.end(), [](const Icon& a, const Icon& b) {
    return a.mod < b.mod;
  });
  const f32 unit = app.height / 480;
  const f32 spacing = game.watching ? 50 : 10;
  for (size_t i = 0; i < icons.size(); ++i) {
    const auto* image = skin_element(
        app.skin, icons[i].button->skin_images[icons[i].state - 1]);
    const f32 height = (image ? image->front().height() : 68) * ui_scale(app);
    const f32 t = clamp01(
        static_cast<f32>(((app.now - game.started) * 1000 - 500.0 * i) / 400));
    const f32 grow = 2 - (1 - (1 - t) * (1 - t));
    draw_mod_icon(app, *icons[i].button, icons[i].state,
                  app.width - (30 + spacing * i) * unit, 94 * unit,
                  height * grow, t);
  }
}

void draw_hud(App& app) {
  auto&         r = app.renderer;
  const auto&   game = app.game;
  const auto&   skin = app.skin;
  const auto&   config = skin.config;
  constexpr f32 kMargin = 16;
  // Relax and Autopilot score nothing, so osu! hides the score and health.
  const bool    scoreless = game.mods & (kRelax | kAutopilot);

  // Health
  const f32     health = clamp01(game.displayed_health);
  if (scoreless) {
  } else if (const auto* bar = skin_element(skin, "scorebar-colour")) {
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

  // Score, accuracy and song progress, laid out as stable's ScoreDisplay:
  // in units of a 480-unit-tall window, from its top right.
  const f32 unit = app.height / 480;
  const f32 right = app.width - 6 * unit;
  char      score[32], accuracy_text[32];
  snprintf(score, sizeof(score), "%08lld",
           static_cast<long long>(std::llround(game.displayed_score)));
  snprintf(accuracy_text, sizeof(accuracy_text), "%05.2f%%",
           accuracy(game.counts) * 100);
  // The pie sits left of the accuracy as its "00.00%" placeholder measured.
  f32 pie_x, pie_y;
  if (const auto* digit = skin_element(skin, config.score_prefix + "-0")) {
    const f32 score_scale = ui_scale(app) * 0.96f;
    const f32 accuracy_scale = score_scale * 0.6f;
    const f32 score_height = digit->front().height() * score_scale;
    const f32 accuracy_height = digit->front().height() * accuracy_scale;
    pie_y = score_height + 3 * unit + accuracy_height / 2;
    pie_x = right - 24 * unit -
            skin_number_width(skin, config.score_prefix, "00.00%",
                              accuracy_scale, config.score_overlap)
                .value_or(0);
    if (!scoreless) {
      draw_skin_number(r, skin, config.score_prefix, score, right,
                       score_height / 2, score_scale, config.score_overlap,
                       kWhite, Align::Right);
      draw_skin_number(r, skin, config.score_prefix, accuracy_text, right,
                       pie_y, accuracy_scale, config.score_overlap, kWhite,
                       Align::Right);
    }
  } else {
    const f32 score_size = 40, accuracy_size = 22;
    const f32 score_baseline = kMargin + app.bold.cap_height * score_size;
    const f32 accuracy_baseline =
        score_baseline + 12 + app.bold.cap_height * accuracy_size;
    pie_x = right - 24 * unit - measure_text(app.bold, "00.00%", accuracy_size);
    pie_y = accuracy_baseline - app.bold.cap_height * accuracy_size / 2;
    if (!scoreless) {
      draw_text_shadowed(r, app.bold, score, right, score_baseline, score_size,
                         kWhite, Align::Right);
      draw_text_shadowed(r, app.bold, accuracy_text, right, accuracy_baseline,
                         accuracy_size, kWhite, Align::Right);
    }
  }
  // Like the score, stable hides the pie under Relax and Autopilot.
  if (!scoreless) {
    const f64  first = game.objects.front().time;
    const bool intro = game.time < first;
    const f32  progress = static_cast<f32>(
        intro ? 1 - (first - game.time) / std::max(1.0, first + 3000)
              : (game.time - first) / std::max(1.0, game.end_time - first));
    const f32 radius = 10 * unit;
    draw_circle(r, pie_x, pie_y, radius, fade(kWhite, 0.2f));
    draw_arc(
        r, pie_x, pie_y, radius, 0, clamp01(progress),
        intro ? Color{199 / 255.0f, 1, 47 / 255.0f, 0.6f} : fade(kWhite, 0.6f));
    draw_circle(r, pie_x, pie_y, radius, kWhite, 0.85f);
  }
  draw_gameplay_mod_icons(app);

  // Combo
  if (game.combo > 0 && !scoreless) {
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

// Darkness but for a hole around the cursor. Holding a slider dims the hole
// too, as in osu!stable.
void draw_flashlight(App& app, const Playfield& field) {
  auto&       r = app.renderer;
  const auto& game = app.game;
  for (size_t i = game.first_active; i < game.objects.size(); ++i) {
    if (game.states[i].tracking && game.states[i].result == Judgement::None) {
      draw_rect(r, 0, 0, app.width, app.height, fade(kBlack, 0.8f));
      break;
    }
  }
  // osu!stable's flashlight texture is clear out to 35 texels and dark from
  // 53, each texel `flashlight_scale` osu! pixels across.
  const f32 texel = game.flashlight_scale * field.scale;
  draw_spotlight(r, game.flashlight_position.x, game.flashlight_position.y,
                 35 * texel, 18 * texel, kBlack);
}

// A watched play's timeline, which a click or drag moves along, and the
// player's own pointer for it: the cursor drawn is the replay's.
void draw_timeline(App& app) {
  const auto& game = app.game;
  if (!can_scrub(game))
    return;
  auto&          r = app.renderer;
  const Timeline t = timeline(app);
  const bool     active = game.scrubbing || over_timeline(app);
  const f32      moved =
      1 - clamp01(static_cast<f32>((app.now - app.cursor_moved - 1.5) / 0.5));
  const f32 shown = std::max(active ? 1.0f : 0.0f, moved);
  const f32 alpha = 0.3f + 0.7f * shown;
  const f32 progress = clamp01(
      static_cast<f32>((game.time - game.timeline_start) /
                       std::max(1.0, game.end_time - game.timeline_start)));
  const f32 thickness = active ? 6 : 3;
  draw_rect(r, t.x, app.height - thickness, t.w, thickness,
            fade(kWhite, 0.25f * alpha));
  draw_rect(r, t.x, app.height - thickness, t.w * progress, thickness,
            fade(kWhite, 0.8f * alpha));
  if (active)
    draw_circle(r, t.x + t.w * progress, app.height - thickness / 2, 7, kWhite);
  const auto clock = [](f64 ms) {
    const i32 seconds = static_cast<i32>(std::max(0.0, ms) / 1000);
    char      text[16];
    snprintf(text, sizeof(text), "%d:%02d", seconds / 60, seconds % 60);
    return std::string(text);
  };
  draw_text_shadowed(r, app.regular,
                     clock(game.time) + " / " + clock(game.end_time), 10,
                     app.height - 12, 13, fade(kWhite, alpha));
  if (shown > 0) {
    draw_circle(r, app.cursor.x, app.cursor.y, 8, fade(kWhite, 0.9f * shown),
                0.65f);
    draw_circle(r, app.cursor.x, app.cursor.y, 2, fade(kWhite, 0.9f * shown));
  }
}

void draw_game_overlays(App& app) {
  auto&       r = app.renderer;
  const auto& game = app.game;

  if (game.phase == GamePhase::Playing && in_break(game)) {
    for (const auto& gap : game.breaks) {
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

  if (game.watching && !(game.mods & kCinema))
    draw_text_shadowed(
        r, app.bold,
        game.mods & kAutoplay ? "Autoplay" : "Watching " + game.replay_player,
        app.width / 2, 92, 20, fade(kWhite, 0.8f), Align::Center);

  if (game.phase == GamePhase::Recovering) {
    const f32 pulse =
        0.75f +
        0.25f * std::sin(static_cast<f32>(app.now - game.phase_changed) * 8);
    draw_text_shadowed(r, app.heavy, "Ready?", app.width / 2,
                       app.height * 0.45f, 64, fade(kWhite, pulse),
                       Align::Center);
    return;
  }
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

namespace {

// Stable judges a replay at each of its frames. Between them its game loop
// still runs every display frame, turning spinners and draining health, and
// for Auto scoring sliders, with the cursor eased between frames; here, every
// ms. Steps the replay up to `now`.
StepOutcome step_replay(App& app, i32 now) {
  auto&       game = app.game;
  StepOutcome outcome = StepOutcome::Continue;
  const auto& frames = game.frames;
  while (outcome == StepOutcome::Continue) {
    const bool frame_due =
        game.next_frame < frames.size() && frames[game.next_frame].time <= now;
    const f64 until = frame_due ? frames[game.next_frame].time : now + 1;
    while (game.replay_tick < until && outcome == StepOutcome::Continue) {
      const i32 time = static_cast<i32>(game.replay_tick);
      game.replay_tick += 1;
      if (game.next_frame == 0)
        continue;
      const ReplayFrame& from = frames[game.next_frame - 1];
      Vec2               cursor = from.position;
      if (game.next_frame < frames.size()) {
        const ReplayFrame& to = frames[game.next_frame];
        if (to.time != from.time)
          cursor = lerp(
              from.position, to.position,
              std::max(0.0f, 1 - static_cast<f32>(to.time - time) /
                                     static_cast<f32>(to.time - from.time)));
      }
      outcome = play_step(app, time, cursor, from.keys, false);
    }
    if (!frame_due || outcome != StepOutcome::Continue)
      break;
    const ReplayFrame& frame = frames[game.next_frame++];
    show_replay_keys(game, frame.keys);
    outcome = play_step(app, static_cast<i32>(frame.time), frame.position,
                        frame.keys, true);
    game.replay_tick = std::max(game.replay_tick, frame.time + 1);
  }
  return outcome;
}

// A play's setup: the map, its objects and judging, and a replay's frames.
// To be played, it loads the song; to be judged only, it reads its length.
bool load_gameplay(App&        app,
                   i32         map_index,
                   std::string replay_path,
                   bool        play) {
  if (map_index < 0)
    return false;
  const auto& info = app.library.maps[map_index];
  auto&       game = app.game;
  release_slider_bodies(game.bodies);
  game = Gameplay{};
  game.started = app.now;
  game.map_index = map_index;
  game.mods = app.mods;
  if (!replay_path.empty()) {
    auto replay = read_replay(replay_path);
    if (!replay)
      return false;
    game.watching = true;
    game.replay_path = std::move(replay_path);
    game.replay_player = std::move(replay->player);
    game.frames = std::move(replay->frames);
    game.mods = replay->mods;
    // Stable's fixes for its replays' first frames, which it never judges.
    auto& frames = game.frames;
    if (frames.size() >= 2 && frames[1].time < frames[0].time) {
      frames[1].time = frames[0].time;
      frames[0].time = 0;
    }
    if (frames.size() >= 3 && frames[0].time > frames[2].time)
      frames[0].time = frames[1].time = frames[2].time;
    game.next_frame = 1;
  }
  game.rate = mod_rate(game.mods);
  game.parser = std::make_unique<fosu::Parser>();
  game.map = game.parser->parse_file(info.osu_path.c_str(),
                                     stable_parse_options(game.mods));
  if (!game.map || game.map->hit_objects.empty())
    return false;
  const auto&      map = *game.map;
  // fosu has applied Hard Rock or Easy.
  const Difficulty difficulty{map.cs, map.ar, map.od, map.hp};
  game.radius = circle_radius(difficulty.cs);
  game.preempt = approach_preempt(difficulty.ar);
  game.fade_in = approach_fade_in(game.preempt);
  game.windows = hit_windows(difficulty.od);
  game.hp_setting = difficulty.hp;
  game.timing = sorted_timing_points(map);
  // A map's own colours win over the skin's, unless turned off in settings.
  if (app.beatmap_colours)
    for (u32 rgb : map.combo_colours)
      game.combo_colours.push_back(hex_color(rgb));
  if (game.combo_colours.empty())
    game.combo_colours = app.skin.config.combo_colours;
  if (game.combo_colours.empty())
    game.combo_colours.assign(std::begin(kDefaultComboColours),
                              std::end(kDefaultComboColours));
  game.default_sample_set =
      sample_set_or(static_cast<i32>(map.sample_set), SampleSet::Normal);

  game.breaks = stable_breaks(map);
  f64 audio_length = 0;
  if (play) {
    game.audio_ok = music_load(app.audio, info.audio_path);
    audio_length = game.audio_ok ? music_length(app.audio) : 0;
  } else {
    audio_length = audio_file_length(app.audio, info.audio_path);
  }
  game.objects = build_hit_objects(map, {difficulty.od, audio_length});
  if (game.objects.empty())
    return false;
  // Each sound takes its sample set and volume from the timing point just
  // after it, as stable looks them up.
  auto timing_at = [&](f64 time) -> std::pair<SampleSet, f32> {
    const auto* point = timing_point_at(game.timing, time + 5);
    return {sample_set_or(point ? static_cast<i32>(point->sample_set) : 0,
                          game.default_sample_set),
            point ? point->volume / 100.0f : 1};
  };
  for (GameObject& object : game.objects) {
    const auto& hit = map.hit_objects[object.source];
    const i32   sample_volume = hit_sample_volume(hit.hit_sample);
    const auto  fields = split(hit.hit_sample, ':');
    object.sample_set = fields.empty() ? 0 : field_value(fields[0]);
    const auto [set, volume] = timing_at(object.time);
    object.hitsound = resolve_hitsound(hit.hitsound, hit.hit_sample, set);
    object.volume = sample_volume > 0 ? sample_volume / 100.0f : volume;
    if (object.kind != ObjectKind::Slider)
      continue;
    const auto&      slider = map.sliders[object.slider];
    const i32        edges = object.slides + 1;
    std::vector<f64> edge_times{object.time};
    for (const SliderPoint& point : object.points)
      if (point.kind == PointKind::Repeat)
        edge_times.push_back(point.time);
    edge_times.push_back(object.end_time);
    for (i32 e = 0; e < edges; ++e) {
      const auto [edge_set, edge_volume] =
          timing_at(edge_times[std::min<size_t>(e, edge_times.size() - 1)]);
      const Hitsound base =
          resolve_hitsound(hit.hitsound, hit.hit_sample, edge_set);
      object.edge_hitsounds.push_back(edge_hitsounds(
          slider.edge_sounds, slider.edge_sets, edges, base, edge_set)[e]);
      object.edge_volumes.push_back(sample_volume > 0 ? sample_volume / 100.0f
                                                      : edge_volume);
    }
  }
  game.states.assign(game.objects.size(), ObjectState{});
  for (const auto& object : game.objects)
    game.end_time = std::max(game.end_time, object.end_time);

  // osu!stable rates the map before mods, over whole seconds between the
  // first and last objects' starts in file order, less breaks.
  i32 break_time = 0;
  for (const auto& gap : map.breaks)
    break_time += static_cast<i32>(gap.end) - static_cast<i32>(gap.start);
  const i32 drain_seconds =
      (static_cast<i32>(map.hit_objects.back().time) -
       static_cast<i32>(map.hit_objects.front().time) - break_time) /
      1000;
  game.mod_multiplier = mod_score_multiplier(game.mods);
  game.score_multiplier =
      difficulty_multiplier(map.hp, map.cs, map.od, map.hit_objects.size(),
                            drain_seconds) *
      game.mod_multiplier;
  start_judging(app, map);

  // Stable leads in long enough for the map's lead-in and for the first
  // object to appear, and offers a skip if the intro is long.
  const f64 first = game.objects.front().time;
  const f64 lead_in = std::max(static_cast<f64>(map.audio_lead_in),
                               std::max(1800.0, game.preempt) - first);
  game.clock_time = -lead_in;
  game.time = game.clock_time - song_offset(app);
  game.replay_tick = game.time;
  std::optional<i32> boundary = countdown_skip(map, game.timing, first);
  if (!boundary) {
    // The beat of the timing point in effect as the song starts.
    const fosu::TimingPoint* point = nullptr;
    for (const auto& candidate : game.timing)
      if (candidate.uninherited && (!point || candidate.time <= 0))
        point = &candidate;
    if (point && point->beat_length > 0)
      boundary = static_cast<i32>(
          first - point->beat_length * (point->beat_length < 500 ? 8 : 4));
    else
      boundary = static_cast<i32>(first - 2000);
  }
  if (*boundary + lead_in > 1200 + game.preempt)
    game.skip_boundary = boundary;
  game.lead_frame_time = (game.skip_boundary ? *game.skip_boundary : 0) - 1;

  // Auto and Cinema watch a perfect play made up on the spot, and
  // Autopilot's cursor follows one.
  if (!game.watching && game.mods & kAutoplay) {
    game.watching = true;
    game.replay_player = "osu!";
    game.frames = autoplay_frames(game);
  } else if (!game.watching && game.mods & kAutopilot) {
    game.autopilot_frames = autoplay_frames(game);
  }
  game.cursor = game.flashlight_position = app.cursor;
  game.timeline_start = game.time;
  return true;
}

// Shows a watched play at game time `target`: rebuilt from its start for an
// earlier time, judged silently up to it either way, with the music moved to
// match.
void seek(App& app, f64 target) {
  auto&     game = app.game;
  const i32 time =
      static_cast<i32>(std::clamp(target, game.timeline_start, game.end_time));
  if (time < game.time) {
    const f64 started = game.started;
    if (!load_gameplay(app, game.map_index, game.replay_path, true))
      return;
    game.started = started;
    game.dim = app.background_dim;
  }
  app.audio.effects_muted = true;
  StepOutcome outcome = StepOutcome::Continue;
  do
    outcome = step_replay(app, time);
  while (outcome == StepOutcome::Recover);
  app.audio.effects_muted = false;
  game.time = time;
  game.clock_time = time + song_offset(app);
  game.clock_synced = app.now;
  game.last_seek = app.now;
  game.popups.clear();
  game.recent_errors.clear();
  game.replay_trail.clear();
  game.displayed_score = static_cast<f64>(game.score);
  game.displayed_health = static_cast<f32>(game.health / kHealthMax);
  if (game.audio_ok) {
    game.audio_started = game.clock_time >= 0;
    music_seek(app.audio, std::max(0.0, game.clock_time));
    if (game.audio_started)
      music_play(app.audio);
    else
      music_pause(app.audio);
  }
  if (outcome == StepOutcome::Fail)
    fail(app);
}

}  // namespace

bool gameplay_start(App& app, i32 map_index, std::string replay_path) {
  if (!load_gameplay(app, map_index, std::move(replay_path), true))
    return false;
  auto& game = app.game;
  game.dim = app.background_dim;
  if (game.audio_ok) {
    music_pause(app.audio);
    music_set_rate(app.audio, game.rate, !mods_change_pitch(game.mods));
    music_set_speed(app.audio, 1);
    music_seek(app.audio, 0);
  } else {
    show_toast(app, "Audio file missing; playing silently");
  }
  select_map(app, map_index, false);
  return true;
}

bool judge_replay(App& app, i32 map_index, std::string replay_path) {
  if (!load_gameplay(app, map_index, std::move(replay_path), false))
    return false;
  auto& game = app.game;
  // Judging plays nothing; Easy's revive only pauses the music, so it goes on.
  app.audio.effects_muted = true;
  StepOutcome outcome = StepOutcome::Continue;
  do
    outcome = step_replay(app, static_cast<i32>(game.end_time) + 201);
  while (outcome == StepOutcome::Recover);
  app.audio.effects_muted = false;
  return outcome == StepOutcome::Continue;
}

void gameplay_update(App& app) {
  auto& game = app.game;
  if (game.phase == GamePhase::Paused || game.phase == GamePhase::Failed) {
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
  if (game.phase == GamePhase::Recovering) {
    // The bar refills at stable's rate, and play resumes once it's nearly
    // full.
    game.recovery_display += 0.08 * app.dt * 1000;
    game.displayed_health = static_cast<f32>(
        std::min(game.recovery_display, game.health) / kHealthMax);
    if (game.recovery_display > game.health - 5)
      resume(app);
    return;
  }

  // Dragging along the timeline seeks as it goes, up to 20 times a second.
  if (game.scrubbing && app.now - game.last_seek >= 0.05) {
    const f64 target = timeline_time(app);
    if (std::abs(target - game.time) >= 1)
      seek(app, target);
  }

  advance_clock(app);
  if (game.audio_ok && !game.audio_started && game.clock_time >= 0) {
    music_seek(app.audio, game.clock_time);
    music_play(app.audio);
    game.audio_started = true;
  }
  game.outro = game.audio_started && music_at_end(app.audio);
  const i32   now = game_time(app);
  StepOutcome outcome = StepOutcome::Continue;
  if (game.watching) {
    outcome = step_replay(app, now);
    game.time = now;
    follow_replay_cursor(app);
  } else {
    // As stable records: a frame each 60th of a second, and whenever the
    // keys change or something scores.
    const auto record = [&](i32 time, Vec2 cursor, u32 keys, bool scorable) {
      if (!game.audio_started && game.audio_ok)
        return;
      if (scorable || game.force_frame || game.frames.empty() ||
          keys != game.frames.back().keys)
        game.frames.push_back(
            {std::max<f64>(
                 time, game.frames.empty() ? time : game.frames.back().time),
             cursor, keys});
      game.force_frame = false;
    };
    // Stable reads the keys held at each frame. Here each press and release
    // counts when it happened, by macOS's timestamp: judging steps to that
    // moment, between frames, before the frame's own step.
    const i32 earliest = game.stepped ? std::min(game.step_time, now) : now;
    for (const auto& input : game.pending_keys) {
      const i32 ago = static_cast<i32>(
          std::lround(std::max(0.0, app.now - input.wall) * 1000 * game.rate));
      const i32  time = std::max(earliest, now - ago);
      const Vec2 at = live_cursor(app, time);
      outcome = play_step(app, time, at, input.keys, false);
      record(time, at, input.keys, false);
      if (outcome != StepOutcome::Continue)
        break;
    }
    game.pending_keys.clear();
    if (outcome == StepOutcome::Continue) {
      const Vec2 cursor = live_cursor(app, now);
      game.cursor = playfield(app).to_screen(cursor);
      if (game.mods & kAutopilot)
        game.replay_trail.push_back({app.now, game.cursor});
      const bool scorable = sixty_hz_frame(app);
      const u32  keys = replay_keys(game);
      outcome = play_step(app, now, cursor, keys, scorable);
      record(now, cursor, keys, scorable);
    }
  }
  switch (outcome) {
    case StepOutcome::Fail:
      fail(app);
      return;
    case StepOutcome::Recover:
      recover(app);
      return;
    case StepOutcome::Restart:
      gameplay_start(app, game.map_index);
      return;
    case StepOutcome::Continue:
      break;
  }
  if (game.mods & kFlashlight)
    update_flashlight(app);
  if (game.mods & kNightcore)
    play_nightcore_beat(app);

  while (game.first_active < game.objects.size() &&
         game.states[game.first_active].hit &&
         game.time > game.objects[game.first_active].end_time + 1000)
    ++game.first_active;
  game.displayed_score =
      game.score + (game.displayed_score - game.score) * std::exp(-12 * app.dt);
  game.displayed_health =
      approach(game.displayed_health,
               static_cast<f32>(game.health / kHealthMax), 10, app.dt);
  // Breaks show the background more, unless it's hidden.
  const f32 dim = app.background_dim;
  game.dim =
      approach(game.dim, in_break(game) && dim < 1 ? std::min(dim, 0.5f) : dim,
               3, app.dt);
  std::erase_if(game.popups, [&](const Popup& popup) {
    return game.time - popup.time > kPopupDuration;
  });
  std::erase_if(game.recent_errors, [&](const HitError& error) {
    return app.now - error.wall_time > 4;
  });

  // Stable passes the play 200 ms after the last object ends, and shows
  // results 3 seconds after.
  if (!game.finished && game.time > game.end_time + 200)
    game.finished = true;
  if (game.finished && game.time >= game.end_time + kResultsDelay &&
      app.transition_started < 0)
    finish(app);
}

void gameplay_draw(App& app) {
  const auto& game = app.game;
  // Cinema shows only the background, undimmed.
  if (game.mods & kCinema) {
    draw_backdrop(app, 0, 0);
    draw_timeline(app);
    draw_game_overlays(app);
    return;
  }
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
    draw_follow_points(app, field, alpha_scale);
    // Earlier objects draw on top of later ones.
    for (size_t i = end; i-- > game.first_active;) {
      Playfield shaken = field;
      shaken.x += shake_offset(app, game.states[i]) * field.scale;
      switch (game.objects[i].kind) {
        case ObjectKind::Circle:
          draw_head(app, shaken, game.objects[i], game.states[i], alpha_scale);
          break;
        case ObjectKind::Slider:
          draw_slider(app, shaken, i, alpha_scale);
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
  sweep_slider_bodies(app);
  if (game.mods & kFlashlight)
    draw_flashlight(app, field);
  draw_hud(app);
  draw_timeline(app);
  draw_game_overlays(app);
}

void gameplay_key(App& app, i32 key, i32 action, f64 wall) {
  auto& game = app.game;
  if (key == GLFW_KEY_Z || key == GLFW_KEY_X) {
    if (action != GLFW_REPEAT && !game.watching) {
      const i32 hit_key = key == GLFW_KEY_Z ? kKey1 : kKey2;
      game.press_counts[hit_key] += action == GLFW_PRESS;
      game.held[hit_key] = action == GLFW_PRESS;
      if (game.phase == GamePhase::Playing)
        game.pending_keys.push_back({wall, replay_keys(game)});
    }
    return;
  }
  if (action == GLFW_RELEASE || game.phase == GamePhase::Recovering)
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
  // A watched play skips back and forth 5 seconds, repeating while held.
  if ((key == GLFW_KEY_LEFT || key == GLFW_KEY_RIGHT) && can_scrub(game)) {
    seek(app, game.time + (key == GLFW_KEY_RIGHT ? 5000 : -5000));
    return;
  }
  if (action != GLFW_PRESS)
    return;
  switch (key) {
    case GLFW_KEY_ESCAPE:
      // Once the play has passed, stable skips straight to the results.
      if (!game.finished)
        pause(app);
      else if (app.transition_started < 0)
        finish(app);
      break;
    case GLFW_KEY_SPACE:
      skip_intro(app);
      break;
    case GLFW_KEY_GRAVE_ACCENT:  // quick retry, though not for Cinema
      if (!(game.mods & kCinema))
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
       (game.phase == GamePhase::Playing && app.mouse_buttons))) {
    const i32 hit_key = button == GLFW_MOUSE_BUTTON_LEFT ? kMouse1 : kMouse2;
    game.press_counts[hit_key] += action == GLFW_PRESS;
    game.held[hit_key] = action == GLFW_PRESS;
    if (game.phase == GamePhase::Playing)
      game.pending_keys.push_back({wall, replay_keys(game)});
  }
  if (button == GLFW_MOUSE_BUTTON_LEFT && game.watching) {
    if (action == GLFW_RELEASE && game.scrubbing) {
      game.scrubbing = false;
      seek(app, timeline_time(app));
    } else if (action == GLFW_PRESS && can_scrub(game) && over_timeline(app)) {
      game.scrubbing = true;
      seek(app, timeline_time(app));
    }
  }
  const bool menu =
      game.phase == GamePhase::Paused || game.phase == GamePhase::Failed;
  if (menu && button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_PRESS)
    activate_menu_item(app, menu_item_at(app, app.cursor));
}

void gameplay_focus_lost(App& app) {
  if (app.game.phase == GamePhase::Playing && !app.game.finished)
    pause(app);
}

}  // namespace circles
