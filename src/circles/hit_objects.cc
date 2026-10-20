#include <circles/hit_objects.h>
#include <circles/rules.h>

#include <algorithm>
#include <cmath>
#include <optional>

namespace circles {

namespace {

f32 bpm_multiplier_of(const fosu::TimingPoint& point) {
  if (point.beat_length >= 0)
    return 1;
  return std::clamp(static_cast<f32>(-point.beat_length), 10.0f, 1000.0f) /
         100.0f;
}

}  // namespace

f64 beat_length_at(const std::vector<fosu::TimingPoint>& points, f64 time) {
  if (points.empty())
    return 0;
  size_t point = 0, sample = 0;
  for (size_t i = 0; i < points.size(); ++i) {
    if (points[i].time > time)
      continue;
    (points[i].uninherited ? point : sample) = i;
  }
  f64 multiplier = 1;
  if (sample > point && points[sample].beat_length < 0)
    multiplier = bpm_multiplier_of(points[sample]);
  return points[point].beat_length * multiplier;
}

f64 beat_offset_at(const std::vector<fosu::TimingPoint>& points, f64 time) {
  if (points.empty())
    return 0;
  size_t point = 0;
  for (size_t i = 0; i < points.size(); ++i)
    if (points[i].uninherited && points[i].time <= time)
      point = i;
  return points[point].time;
}

std::vector<fosu::Break> stable_breaks(const fosu::Beatmap& map) {
  const f64                shift = fosu_time_shift(map);
  std::vector<fosu::Break> breaks;
  for (const auto& gap : map.breaks) {
    const fosu::Break shifted{gap.start - shift, gap.end - shift};
    if (static_cast<i32>(shifted.end) - static_cast<i32>(shifted.start) >= 650)
      breaks.push_back(shifted);
  }
  std::stable_sort(breaks.begin(), breaks.end(),
                   [](const fosu::Break& a, const fosu::Break& b) {
                     return a.start < b.start;
                   });
  return breaks;
}

std::vector<fosu::TimingPoint> sorted_timing_points(const fosu::Beatmap& map) {
  std::vector<fosu::TimingPoint> points(map.timing_points.begin(),
                                        map.timing_points.end());
  for (auto& point : points)
    point.time -= fosu_time_shift(map);
  std::stable_sort(points.begin(), points.end(),
                   [](const fosu::TimingPoint& a, const fosu::TimingPoint& b) {
                     if (a.time != b.time)
                       return a.time < b.time;
                     return a.uninherited && !b.uninherited;
                   });
  return points;
}

const fosu::TimingPoint* timing_point_at(
    const std::vector<fosu::TimingPoint>& points,
    f64                                   time) {
  const fosu::TimingPoint* found = nullptr;
  for (const auto& point : points)
    if (point.time <= time)
      found = &point;
  return found || points.empty() ? found : &points.front();
}

fosu::ParseOptions stable_parse_options(Mods mods) {
  fosu::Mods difficulty = fosu::Mods::None;
  if (mods & kHardRock)
    difficulty = fosu::Mods::HardRock;
  else if (mods & kEasy)
    difficulty = fosu::Mods::Easy;
  return {.calculate_slider_events = true,
          .apply_stacking = true,
          .mods = difficulty,
          .client = fosu::Client::Stable};
}

namespace {

// The ball's moves along each line of the path, pass by pass, as stable times
// them: whole ms, from the time each line takes at the slider's velocity.
// `velocity` is in osu! pixels per second.
void add_ball_moves(GameObject&             object,
                    const fosu::SliderPath& path,
                    f64                     velocity) {
  const auto& lengths = path.cumulative_lengths;
  const auto  line_count = static_cast<i32>(path.points.size()) - 1;
  auto        at = [&](i32 point) {
    return object.position + Vec2{path.points[point].x, path.points[point].y};
  };
  f64 current_time = object.time;
  for (i32 span = 0; span < object.slides; ++span) {
    const bool reverse = span % 2 == 1;
    for (i32 j = reverse ? line_count - 1 : 0; j != (reverse ? -1 : line_count);
         j += reverse ? -1 : 1) {
      const f32 distance = static_cast<f32>(lengths[j + 1] - lengths[j]);
      const f64 duration = 1000.0f * distance / velocity;
      object.moves.push_back({at(reverse ? j + 1 : j), at(reverse ? j : j + 1),
                              static_cast<i32>(current_time),
                              static_cast<i32>(current_time + duration)});
      current_time += duration;
    }
  }
}

PointKind point_kind(fosu::SliderEventType type) {
  switch (type) {
    case fosu::SliderEventType::Repeat:
      return PointKind::Repeat;
    case fosu::SliderEventType::LegacyLastTick:
      return PointKind::End;
    default:
      return PointKind::Tick;
  }
}

}  // namespace

std::vector<GameObject> build_hit_objects(const fosu::Beatmap&  map,
                                          const ObjectSettings& settings) {
  const f64               shift = fosu_time_shift(map);
  std::vector<GameObject> objects;
  bool                    after_spinner = true;  // the first object, too
  for (u32 h = 0; h < map.hit_objects.size(); ++h) {
    const auto& hit = map.hit_objects[h];
    // Stable plays hold notes, from converted maps, as circles.
    GameObject  object{
        .kind = hit.is_slider()    ? ObjectKind::Slider
                : hit.is_spinner() ? ObjectKind::Spinner
                                   : ObjectKind::Circle,
        .time = hit.time - shift,
        .end_time = hit.time - shift,
        .position = {hit.x, hit.y},
        .stack_count = map.stacking.empty() ? 0 : map.stacking[h].stack_height,
        .source = h,
    };
    // A spinner starts a new combo only when marked; the object after one
    // always does.
    object.new_combo = hit.new_combo || (!hit.is_spinner() && after_spinner);
    object.combo_offset = hit.new_combo ? hit.combo_skip : 0;
    after_spinner = hit.is_spinner();
    object.end_position = object.path_end = object.position;
    if (object.kind == ObjectKind::Spinner) {
      object.end_time = hit.end_time - shift;
      if (settings.audio_length > 0)
        object.end_time =
            std::min(object.end_time, std::trunc(settings.audio_length) - 50);
      const f64 ratio = map_difficulty_range(settings.od, 3, 5, 7.5);
      object.required_spins = static_cast<i32>(
          static_cast<f32>(object.end_time - object.time) / 1000 * ratio);
    } else if (object.kind == ObjectKind::Slider) {
      const auto& path = map.slider_paths[hit.slider];
      const auto  events = map.slider_events[hit.slider];
      const auto  offset = [&](fosu::PathPoint point) {
        return object.position + Vec2{point.x, point.y};
      };
      object.slider = hit.slider;
      object.slides = map.sliders[hit.slider].slides;
      object.end_time = hit.end_time - shift;
      object.span_duration = (object.end_time - object.time) / object.slides;
      object.path_end = offset(path.points.back());
      object.end_position = offset(events.back().position);
      // Between the head and the end.
      for (const auto& event : events.subspan(1, events.size() - 2))
        object.points.push_back({point_kind(event.type),
                                 static_cast<i32>(event.time - shift),
                                 event.span_index, offset(event.position)});
      if (path.points.size() > 1)
        add_ball_moves(object, path, map.slider_velocities[hit.slider] * 1000);
      object.body = build_body_shape(path, object.position,
                                     map.sliders[hit.slider].length);
      // About as far in as a reverse arrow reaches.
      const f64 inward = path.distance() > 0
                             ? std::min(18.0, path.distance()) / path.distance()
                             : 0;
      object.head_inward = offset(fosu::slider_position_at(path, inward));
      object.tail_inward = offset(fosu::slider_position_at(path, 1 - inward));
    }
    objects.push_back(std::move(object));
  }

  // Combo numbers and colours. The first object after a break starts a new
  // combo; spinners take no number and, from v9, pass their colour skip on.
  const auto breaks = stable_breaks(map);
  i32        combo = 0, number = 0;
  bool       force_new = false;
  size_t     next_break = 0;
  for (size_t i = 0; i < objects.size(); ++i) {
    GameObject& object = objects[i];
    while (next_break < breaks.size() && breaks[next_break].end < object.time) {
      ++next_break;
      object.new_combo = true;
    }
    if (object.kind == ObjectKind::Spinner) {
      if (map.format_version <= 8) {
        force_new = true;
      } else if (object.new_combo) {
        combo += object.combo_offset;
        force_new = true;
      }
    } else if (force_new || object.new_combo || i == 0) {
      number = 1;
      combo += object.combo_offset + 1;
      force_new = false;
    } else {
      ++number;
    }
    object.combo_number = number;
    object.colour = combo;
  }
  return objects;
}

namespace {

Vec2 along(const BallMove& move, f64 time) {
  if (move.end == move.start)
    return move.to;
  return move.from +
         (move.to - move.from) *
             (1 - static_cast<f32>(move.end - time) / (move.end - move.start));
}

}  // namespace

Vec2 ball_position(const GameObject& object, f64 time) {
  if (object.moves.empty())
    return object.end_position;
  for (const BallMove& move : object.moves)
    if (move.start <= time && move.end >= time)
      return along(move, time);
  return time > object.moves.back().end ? object.moves.back().to
                                        : object.moves.front().from;
}

std::optional<Vec2> tracked_ball_position(const GameObject& object, i32 time) {
  if (object.moves.empty())
    return std::nullopt;
  for (const BallMove& move : object.moves)
    if (move.start <= time && move.end >= time)
      return along(move, time);
  if (time > object.moves.back().end)
    return along(object.moves.back(), time);
  return std::nullopt;
}

}  // namespace circles
