#pragma once

// A map's hit objects as osu!stable plays them: fosu's stable mode works out
// slider paths, timing, scoring points and stacking, and these add combo
// numbers and the slider ball's moves. Times are whole ms and positions osu!
// pixels, as stable keeps them.

#include <circles/hitsounds.h>
#include <circles/math.h>
#include <circles/mods.h>
#include <circles/slider_body.h>
#include <circles/types.h>

#include <fosu/beatmap.h>
#include <fosu/parse_options.h>
#include <optional>
#include <vector>

namespace circles {

enum class ObjectKind : u8 { Circle, Slider, Spinner };

// A slider's tick, repeat or end: a time its ball must be followed.
enum class PointKind : u8 { Tick, Repeat, End };

struct SliderPoint {
  PointKind kind;
  i32       time;
  i32       span;      // which pass along the path it's on
  Vec2      position;  // where the ball is then
};

// The ball crossing one line of a slider's path on one pass. Like stable's
// slider ball, it moves linearly between whole-ms times.
struct BallMove {
  Vec2 from;
  Vec2 to;
  i32  start;
  i32  end;
};

struct GameObject {
  ObjectKind kind;
  f64        time;
  f64        end_time;
  Vec2       position;  // stacked
  // Where the object finishes: a slider's head or the end of its path,
  // depending on its repeats.
  Vec2       end_position;
  Vec2       path_end;  // the far end of a slider's path
  i32        stack_count = 0;
  bool       new_combo = false;
  i32        combo_offset = 0;  // colours to skip at a new combo
  i32        combo_number = 0;
  i32        colour = 0;  // a running count; modulo the colour count
  u32        source = 0;  // index into the parsed map's hit objects

  // Filled in by gameplay.
  Hitsound   hitsound{};
  f32        volume = 1;      // 0 to 1, from the timing point at `time`
  i32        sample_set = 0;  // its own sample set; 0 follows timing points

  // Sliders
  u32        slider = fosu::HitObject::kNoSlider;
  i32        slides = 1;
  f64        span_duration = 0;
  BodyShape  body;
  // Along the path from each end, for the reverse arrows to point at.
  Vec2       head_inward{}, tail_inward{};
  std::vector<BallMove>    moves;
  std::vector<SliderPoint> points;  // in time order; the last is the end
  std::vector<Hitsound>    edge_hitsounds;  // the head, each repeat, the end
  std::vector<f32>         edge_volumes;

  // Spinners: the half turns that earn a 50; a 300 needs two more.
  i32                      required_spins = 0;
};

struct ObjectSettings {
  f64 od;  // after mods
  // ms of audio; stable ends spinners 50 ms before it. 0 when unknown.
  f64 audio_length = 0;
};

// fosu, like osu!lazer, moves pre-v5 maps' times 24 ms later as it parses;
// osu!stable keeps the file's times and delays its clock instead, so these
// undo the shift.
inline f64 fosu_time_shift(const fosu::Beatmap& map) {
  return map.format_version < 5 ? 24 : 0;
}

// The map's breaks as stable keeps them: at least 650 ms long, in order.
std::vector<fosu::Break> stable_breaks(const fosu::Beatmap& map);

// What build_hit_objects needs from fosu: stable's calculations, with Hard
// Rock or Easy applied to the map's difficulty and, for Hard Rock, flipping
// the playfield.
fosu::ParseOptions stable_parse_options(Mods mods);

// `map` is parsed with stable_parse_options.
std::vector<GameObject> build_hit_objects(const fosu::Beatmap&  map,
                                          const ObjectSettings& settings);

// Where a slider's ball is drawn at `time`, following its moves.
Vec2 ball_position(const GameObject& object, f64 time);
// Where stable checks a slider's ball at whole-ms `time`: nothing before it
// starts moving, and past the last move's end, carried on along it.
std::optional<Vec2> tracked_ball_position(const GameObject& object, i32 time);

// Timing points as stable orders them: by time, uninherited first.
std::vector<fosu::TimingPoint> sorted_timing_points(const fosu::Beatmap& map);
// The last timing point at or before `time`, or the first if none is.
const fosu::TimingPoint* timing_point_at(
    const std::vector<fosu::TimingPoint>& points,
    f64                                   time);
// Stable's BeatLengthAt: the uninherited point's beat length in effect at
// `time`, scaled by a later inherited point's speed.
f64 beat_length_at(const std::vector<fosu::TimingPoint>& points, f64 time);
// Stable's BeatOffsetAt: where that uninherited point starts.
f64 beat_offset_at(const std::vector<fosu::TimingPoint>& points, f64 time);

}  // namespace circles
