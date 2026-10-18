#include <circles/app.h>
#include <circles/strings.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace circles {

namespace {

constexpr f32 kSetHeight = 76;
constexpr f32 kDifficultyHeight = 54;
constexpr f32 kRowGap = 8;
// Difficulties sit this far right of their set, on a rail joined to it.
constexpr f32 kDifficultyIndent = 56;
// How far a press may move before it scrolls instead of clicking.
constexpr f32 kDragThreshold = 6;
constexpr f32 kWheelStep = 90;  // points per scroll wheel notch
constexpr f32 kReplayRowHeight = 46;

// A carousel row: a set, or one of the selected set's difficulties.
struct Row {
  i32 set;
  i32 map;  // negative for a set
  f32 top;  // points from the top of the list
  f32 height;

  f32 center() const { return top + height / 2; }
};

std::vector<Row> build_rows(const App& app) {
  std::vector<Row> rows;
  const i32        selected_set =
      app.selected_map >= 0 ? app.library.set_of_map[app.selected_map] : -1;
  f32 top = 0;
  for (i32 set : app.select.sets) {
    rows.push_back({set, -1, top, kSetHeight});
    top += kSetHeight + kRowGap;
    if (set != selected_set)
      continue;
    for (i32 map : app.library.sets[set].maps) {
      rows.push_back({set, map, top, kDifficultyHeight});
      top += kDifficultyHeight + kRowGap;
    }
  }
  return rows;
}

// The furthest the list scrolls: its last row at the centre line.
f32 scroll_limit(const std::vector<Row>& rows) {
  return rows.empty() ? 0 : rows.back().center();
}

f32 center_line(const App& app) {
  return app.height * 0.5f + 24;
}

// Rows curve away to the right above and below the centre line.
f32 curve_x(const App& app, f32 y) {
  const f32 offset = (y - center_line(app)) / app.height;
  return app.width * 0.5f + offset * offset * 260;
}

struct Rect {
  f32 x, y, w, h;
};

Rect row_rect(const App& app, const Row& row) {
  const f32 y = center_line(app) + row.top - app.select.scroll;
  f32       x = curve_x(app, y + row.height / 2);
  if (row.map >= 0)
    x += kDifficultyIndent - (row.map == app.selected_map ? 20 : 0);
  return {x, y, app.width - x + 40, row.height};
}

void recenter(App& app) {
  for (const Row& row : build_rows(app))
    if (row.map == app.selected_map)
      app.select.scroll_target = row.center();
  app.select.fling = 0;
}

bool in_carousel(const App& app) {
  return app.cursor.x >= app.width * 0.5f - 20 && app.cursor.y > 76;
}

// Every word of the query appears in one of the set's difficulties.
bool matches(const App& app, i32 set, const std::vector<std::string>& words) {
  for (i32 map : app.library.sets[set].maps) {
    const auto& text = app.library.maps[map].search_text;
    if (std::all_of(words.begin(), words.end(), [&](const std::string& word) {
          return text.find(word) != std::string::npos;
        }))
      return true;
  }
  return false;
}

void apply_filter(App& app) {
  auto&                    select = app.select;
  std::istringstream       stream(lowercase(select.query));
  std::vector<std::string> words;
  for (std::string word; stream >> word;)
    words.push_back(word);
  select.sets.clear();
  for (i32 set = 0; set < static_cast<i32>(app.library.sets.size()); ++set)
    if (matches(app, set, words))
      select.sets.push_back(set);
  if (select.sets.empty())
    return;
  const bool visible =
      app.selected_map >= 0 &&
      std::find(select.sets.begin(), select.sets.end(),
                app.library.set_of_map[app.selected_map]) != select.sets.end();
  if (!visible)
    select_map(app, app.library.sets[select.sets.front()].maps.front(), true);
  recenter(app);
}

void choose(App& app, i32 map) {
  if (map == app.selected_map)
    return;
  select_map(app, map, true);
  recenter(app);
  play_sound(app.audio, Sound::MenuHover, 0.5f);
}

// Steps through every difficulty of the visible sets in order.
void move_difficulty(App& app, i32 step) {
  std::vector<i32> order;
  for (i32 set : app.select.sets)
    for (i32 map : app.library.sets[set].maps)
      order.push_back(map);
  if (order.empty())
    return;
  const auto current = std::find(order.begin(), order.end(), app.selected_map);
  const i32  index = current == order.end()
                         ? 0
                         : static_cast<i32>(current - order.begin()) + step;
  choose(app, order[std::clamp(index, 0, static_cast<i32>(order.size()) - 1)]);
}

void move_set(App& app, i32 step) {
  const auto& sets = app.select.sets;
  if (sets.empty())
    return;
  const i32  current = app.library.set_of_map[std::max(app.selected_map, 0)];
  const auto found = std::find(sets.begin(), sets.end(), current);
  const i32  index =
      found == sets.end() ? 0 : static_cast<i32>(found - sets.begin()) + step;
  const i32 count = static_cast<i32>(sets.size());
  choose(app,
         app.library.sets[sets[(index % count + count) % count]].maps.front());
}

void start(App& app) {
  if (app.selected_map < 0)
    return;
  play_sound(app.audio, Sound::MenuClick, 0.7f);
  switch_screen(app, Screen::Gameplay);
}

void watch(App& app, const ReplayEntry& entry) {
  app.replay_to_watch = entry.path.string();
  start(app);
}

// The selected map's replays sit in a panel under its details.
f32 panel_width(const App& app) {
  return std::min(app.width * 0.44f, 560.0f);
}

constexpr f32 kReplayPanelTop = 292;

// As many replays as fit above the footer.
size_t visible_replays(const App& app, size_t count) {
  const f32 room = app.height - 80 - kReplayPanelTop - 44;
  return std::min(count,
                  static_cast<size_t>(std::max(0.0f, room / kReplayRowHeight)));
}

Rect replay_row(const App& app, size_t index) {
  return {32, kReplayPanelTop + 40 + index * kReplayRowHeight,
          panel_width(app) - 16, kReplayRowHeight - 4};
}

void draw_replays(App& app) {
  auto&      r = app.renderer;
  const auto replays = map_replays(app, app.selected_map);
  const auto count = visible_replays(app, replays.size());
  const f32  w = panel_width(app);
  draw_rounded_rect(r, 24, kReplayPanelTop, w,
                    48 + std::max<size_t>(count, 1) * kReplayRowHeight, 16,
                    fade(hex_color(0x101020), 0.8f));
  draw_text(r, app.bold, "Replays", 44, kReplayPanelTop + 28, 16, kWhite);
  if (replays.empty()) {
    draw_text(r, app.regular, "None yet. Every play you finish is saved.", 44,
              kReplayPanelTop + 66, 14, fade(kWhite, 0.55f));
    return;
  }
  for (size_t i = 0; i < count; ++i) {
    const Replay& replay = replays[i]->replay;
    const Rect    row = replay_row(app, i);
    if (static_cast<i32>(i) == app.select.hovered_replay)
      draw_rounded_rect(r, row.x, row.y, row.w, row.h, 10, fade(kWhite, 0.12f));
    const Grade grade_ = grade(replay.counts);
    draw_text_shadowed(r, app.heavy, grade_name(grade_), row.x + 24,
                       centered_baseline(app.heavy, 24, row.y + row.h / 2), 24,
                       grade_colour(grade_), Align::Center);
    draw_text(r, app.bold, fit_text(app.bold, replay.player, 16, row.w - 170),
              row.x + 52, row.y + 18, 16, kWhite);
    char stats[96];
    snprintf(stats, sizeof(stats), "%lld    %.2f%%    %dx",
             static_cast<long long>(replay.score),
             accuracy(replay.counts) * 100, replay.max_combo);
    draw_text(r, app.regular, stats, row.x + 52, row.y + 36, 13,
              fade(kWhite, 0.75f));
    char       date[32];
    const auto timestamp = static_cast<std::time_t>(replay.timestamp);
    std::strftime(date, sizeof(date), "%Y-%m-%d", std::localtime(&timestamp));
    draw_text(r, app.regular, date, row.x + row.w - 12, row.y + 18, 13,
              fade(kWhite, 0.55f), Align::Right);
  }
}

void draw_stat_bar(App&        app,
                   f32         x,
                   f32         y,
                   f32         width,
                   const char* label,
                   f64         value) {
  auto& r = app.renderer;
  draw_text(r, app.bold, label, x, y, 13, fade(kWhite, 0.75f));
  const f32 bar_x = x + 34, bar_w = width - 80;
  draw_rounded_rect(r, bar_x, y - 8, bar_w, 6, 3, fade(kWhite, 0.15f));
  draw_rounded_rect(r, bar_x, y - 8,
                    bar_w * clamp01(static_cast<f32>(value / 10)), 6, 3,
                    fade(kWhite, 0.85f));
  char text[16];
  snprintf(text, sizeof(text), "%g", value);
  draw_text(r, app.bold, text, x + width, y, 13, kWhite, Align::Right);
}

void draw_info_panel(App& app) {
  if (app.selected_map < 0)
    return;
  auto&       r = app.renderer;
  const auto& info = app.library.maps[app.selected_map];
  const f32   x = 24, y = 24, w = panel_width(app), h = 252;
  const f32   text_x = x + 20, text_w = w - 40;
  draw_rounded_rect(r, x, y, w, h, 16, fade(hex_color(0x101020), 0.8f));
  f32 line = y + 46;
  draw_text_shadowed(r, app.bold, fit_text(app.bold, info.title, 30, text_w),
                     text_x, line, 30, kWhite);
  line += 28;
  draw_text(r, app.regular, fit_text(app.regular, info.artist, 19, text_w),
            text_x, line, 19, fade(kWhite, 0.85f));
  line += 24;
  draw_text(r, app.regular,
            fit_text(app.regular, "mapped by " + info.creator, 15, text_w),
            text_x, line, 15, fade(kWhite, 0.6f));
  line += 28;
  draw_text(r, app.bold, fit_text(app.bold, info.version, 18, text_w), text_x,
            line, 18, difficulty_colour(info));
  line += 26;

  char bpm[32];
  if (std::lround(info.bpm_min) == std::lround(info.bpm_max))
    snprintf(bpm, sizeof(bpm), "%ld", std::lround(info.bpm_max));
  else
    snprintf(bpm, sizeof(bpm), "%ld-%ld", std::lround(info.bpm_min),
             std::lround(info.bpm_max));
  char stats[160];
  snprintf(stats, sizeof(stats),
           "Length %s    BPM %s    Circles %d    Sliders %d    Spinners %d",
           format_duration(info.end_time).c_str(), bpm, info.circles,
           info.sliders, info.spinners);
  draw_text(r, app.regular, fit_text(app.regular, stats, 14, text_w), text_x,
            line, 14, fade(kWhite, 0.8f));
  line += 24;
  draw_stat_bar(app, text_x, line, text_w, "CS", info.cs);
  draw_stat_bar(app, text_x, line + 16, text_w, "AR", info.ar);
  draw_stat_bar(app, text_x, line + 32, text_w, "OD", info.od);
  draw_stat_bar(app, text_x, line + 48, text_w, "HP", info.hp);
}

// Sets are dark raised cards with a dot per difficulty, coloured by
// difficulty.
void draw_set_row(App&              app,
                  const Rect&       rect,
                  const BeatmapSet& set,
                  bool              expanded,
                  bool              hovered) {
  auto&     r = app.renderer;
  const f32 lift = hovered ? 1.12f : 1.0f;
  draw_rounded_rect(r, rect.x + 3, rect.y + 5, rect.w, rect.h, 14,
                    fade(kBlack, 0.4f));
  draw_rounded_rect_gradient(
      r, rect.x, rect.y, rect.w, rect.h, 14,
      fade(shade(hex_color(expanded ? 0x6A2C7A : 0x2E2546), lift), 0.96f),
      fade(shade(hex_color(expanded ? 0x3C1A4E : 0x1C1830), lift), 0.96f));
  draw_rounded_rect(r, rect.x + 10, rect.y + 12, 6, rect.h - 24, 3,
                    fade(hex_color(0xFF66AA), expanded ? 1.0f : 0.7f));
  const f32 dots_width = std::min<size_t>(set.maps.size(), 16) * 15.0f;
  const f32 text_width = app.width - rect.x - dots_width - 70;
  draw_text_shadowed(r, app.bold, fit_text(app.bold, set.title, 23, text_width),
                     rect.x + 30, rect.y + 33, 23, kWhite);
  draw_text(r, app.regular,
            fit_text(app.regular, set.artist + "  //  " + set.creator, 15,
                     text_width),
            rect.x + 30, rect.y + 56, 15, fade(kWhite, 0.7f));
  f32 dot_x = app.width - 28;
  for (size_t i = std::min<size_t>(set.maps.size(), 16); i-- > 0;) {
    const auto& info = app.library.maps[set.maps[i]];
    draw_circle(r, dot_x, rect.y + 26, 5.5f, difficulty_colour(info));
    dot_x -= 15;
  }
}

// Difficulties are smaller translucent rows with a badge in their
// difficulty's colour.
void draw_difficulty_row(App&               app,
                         const Rect&        rect,
                         const BeatmapInfo& info,
                         bool               selected,
                         bool               hovered) {
  auto& r = app.renderer;
  draw_rounded_rect(r, rect.x, rect.y, rect.w, rect.h, 10,
                    selected  ? fade(kWhite, 0.96f)
                    : hovered ? fade(hex_color(0x2A2A48), 0.85f)
                              : fade(hex_color(0x14142A), 0.65f));
  const Color text = selected ? hex_color(0x1A1A2E) : kWhite;
  const f32   badge_y = rect.y + rect.h / 2;
  draw_circle(r, rect.x + 26, badge_y, 11, difficulty_colour(info));
  draw_circle(r, rect.x + 26, badge_y, 11, fade(text, 0.35f), 0.82f);
  const auto label = selected ? draw_text : draw_text_shadowed;
  label(r, app.bold, fit_text(app.bold, info.version, 19, rect.w - 90),
        rect.x + 48, rect.y + 24, 19, text, Align::Left);
  char stats[96];
  snprintf(stats, sizeof(stats), "AR %g    OD %g    CS %g    %s", info.ar,
           info.od, info.cs, format_duration(info.end_time).c_str());
  label(r, app.regular, stats, rect.x + 48, rect.y + 43, 13, fade(text, 0.75f),
        Align::Left);
}

// Joins the expanded set to its difficulties: a rail down from the set's
// accent bar with a branch to each difficulty.
void draw_rail(App& app, const std::vector<Row>& rows) {
  auto&      r = app.renderer;
  const auto line = fade(kWhite, 0.3f);
  Vec2       previous{};
  bool       started = false;
  for (const Row& row : rows) {
    const Rect rect = row_rect(app, row);
    if (row.map < 0) {
      if (row.set == app.library.set_of_map[app.selected_map]) {
        previous = {rect.x + 13, rect.y + rect.h - 10};
        started = true;
      } else if (started) {
        return;
      }
      continue;
    }
    if (!started)
      continue;
    const Vec2 branch{curve_x(app, rect.y + rect.h / 2) + 13,
                      rect.y + rect.h / 2};
    draw_line(r, previous.x, previous.y, branch.x, branch.y, 2, line);
    draw_line(r, branch.x, branch.y, rect.x - 6, branch.y, 2, line);
    draw_circle(r, branch.x, branch.y, 3, line);
    previous = branch;
  }
}

void draw_search_box(App& app) {
  auto&       r = app.renderer;
  const auto& select = app.select;
  const f32   x = app.width * 0.5f + 24, y = 24, w = app.width * 0.5f - 48,
              h = 44;
  draw_rounded_rect(r, x, y, w, h, 22, fade(hex_color(0x101020), 0.8f));
  const f32  baseline = centered_baseline(app.regular, 17, y + h / 2);
  const bool caret = std::fmod(app.now, 1.0) < 0.5;
  if (select.query.empty())
    draw_text(r, app.regular, "type to search", x + 22, baseline, 17,
              fade(kWhite, 0.45f));
  else
    draw_text(
        r, app.regular,
        fit_text(app.regular, select.query, 17, w - 140) + (caret ? "_" : ""),
        x + 22, baseline, 17, kWhite);
  const std::string count = std::to_string(select.sets.size()) +
                            (select.sets.size() == 1 ? " set" : " sets");
  draw_text(r, app.regular, count, x + w - 22, baseline, 14, fade(kWhite, 0.6f),
            Align::Right);
}

}  // namespace

void select_enter(App& app) {
  apply_filter(app);
  if (app.selected_map >= 0)
    select_map(app, app.selected_map, true);
  recenter(app);
  app.select.scroll = app.select.scroll_target;
}

void select_update(App& app) {
  auto&      select = app.select;
  const auto rows = build_rows(app);
  const f32  limit = scroll_limit(rows);
  if (select.right_down) {
    select.scroll_target =
        clamp01((app.cursor.y - 80) / (app.height - 160)) * limit;
  } else if (select.left_down) {
    const f32 moved = app.cursor.y - select.drag_start_y;
    if (std::abs(moved) > kDragThreshold)
      select.dragging = true;
    if (select.dragging) {
      // The list follows the cursor exactly, tracking its speed for the fling.
      const f32 target =
          std::clamp(select.drag_start_scroll - moved, 0.0f, limit);
      if (app.dt > 0)
        select.fling = approach(select.fling, (target - select.scroll) / app.dt,
                                15, app.dt);
      select.scroll = select.scroll_target = target;
    }
  } else if (select.fling != 0) {
    const f32 target = select.scroll_target + select.fling * app.dt;
    select.scroll_target = std::clamp(target, 0.0f, limit);
    select.fling *= std::exp(-4 * app.dt);
    if (std::abs(select.fling) < 30 || target != select.scroll_target)
      select.fling = 0;
  }
  select.scroll = approach(select.scroll, select.scroll_target, 12, app.dt);
  // Keep the preview looping from its start point.
  if (music_at_end(app.audio))
    play_preview(app);

  i32 hovered = -1;
  for (i32 i = 0; i < static_cast<i32>(rows.size()); ++i) {
    const Rect rect = row_rect(app, rows[i]);
    if (contains(app.cursor, rect.x, rect.y, rect.w, rect.h))
      hovered = i;
  }
  if (hovered != select.hovered_row && hovered >= 0 && !select.dragging)
    play_sound(app.audio, Sound::MenuHover, 0.3f);
  select.hovered_row = hovered;

  i32        hovered_replay = -1;
  const auto replays = map_replays(app, app.selected_map);
  for (size_t i = 0; i < visible_replays(app, replays.size()); ++i) {
    const Rect row = replay_row(app, i);
    if (contains(app.cursor, row.x, row.y, row.w, row.h))
      hovered_replay = static_cast<i32>(i);
  }
  if (hovered_replay != select.hovered_replay && hovered_replay >= 0)
    play_sound(app.audio, Sound::MenuHover, 0.3f);
  select.hovered_replay = hovered_replay;
}

void select_draw(App& app) {
  auto&       r = app.renderer;
  const auto& select = app.select;
  draw_backdrop(app, 0.5f, 0.5f);

  if (app.library.sets.empty()) {
    draw_text_shadowed(r, app.bold, "No beatmaps found", app.width / 2,
                       app.height * 0.45f, 32, kWhite, Align::Center);
    draw_text_shadowed(r, app.regular,
                       "Put .osz files or extracted beatmap folders in "
                       "beatmaps/, then restart",
                       app.width / 2, app.height * 0.45f + 36, 17,
                       fade(kWhite, 0.8f), Align::Center);
    return;
  }

  const auto rows = build_rows(app);
  draw_rail(app, rows);
  for (i32 i = 0; i < static_cast<i32>(rows.size()); ++i) {
    const Row& row = rows[i];
    const Rect rect = row_rect(app, row);
    if (rect.y > app.height || rect.y + rect.h < 0)
      continue;
    const bool hovered = i == select.hovered_row;
    if (row.map < 0)
      draw_set_row(app, rect, app.library.sets[row.set],
                   row.set == app.library.set_of_map[app.selected_map],
                   hovered);
    else
      draw_difficulty_row(app, rect, app.library.maps[row.map],
                          row.map == app.selected_map, hovered);
  }

  // Rows fade out under the search box rather than showing through it.
  draw_rect_gradient(r, 0, 0, app.width, 96, fade(kBlack, 0.75f),
                     fade(kBlack, 0));
  draw_info_panel(app);
  if (app.selected_map >= 0)
    draw_replays(app);
  draw_search_box(app);
  draw_rect_gradient(r, 0, app.height - 64, app.width, 64, fade(kBlack, 0),
                     fade(kBlack, 0.7f));
  draw_text(
      r, app.regular,
      fit_text(
          app.regular,
          "Enter  play      Esc  back      Left / Right  set      "
          "Up / Down  difficulty      Drag, or right-click empty space  scroll"
          "      F2  random      Type  search",
          14, app.width - 40),
      app.width / 2, app.height - 22, 14, fade(kWhite, 0.75f), Align::Center);
}

void select_key(App& app, i32 key, i32 action) {
  if (action == GLFW_RELEASE)
    return;
  auto& select = app.select;
  switch (key) {
    case GLFW_KEY_UP:
      move_difficulty(app, -1);
      break;
    case GLFW_KEY_DOWN:
      move_difficulty(app, 1);
      break;
    case GLFW_KEY_LEFT:
      move_set(app, -1);
      break;
    case GLFW_KEY_RIGHT:
      move_set(app, 1);
      break;
    case GLFW_KEY_ENTER:
    case GLFW_KEY_KP_ENTER:
      start(app);
      break;
    case GLFW_KEY_ESCAPE:
      if (action != GLFW_PRESS)
        break;
      if (select.query.empty()) {
        switch_screen(app, Screen::MainMenu);
      } else {
        select.query.clear();
        apply_filter(app);
      }
      break;
    case GLFW_KEY_BACKSPACE:
      if (!select.query.empty()) {
        select.query.pop_back();
        apply_filter(app);
      }
      break;
    case GLFW_KEY_F2:
      if (!select.sets.empty()) {
        static std::mt19937 engine{std::random_device{}()};
        const i32 set = select.sets[std::uniform_int_distribution<size_t>(
            0, select.sets.size() - 1)(engine)];
        choose(app, app.library.sets[set].maps.front());
      }
      break;
  }
}

void select_char(App& app, u32 codepoint) {
  if (codepoint < ' ' || codepoint > '~')
    return;
  app.select.query.push_back(static_cast<char>(codepoint));
  apply_filter(app);
}

void select_click(App& app, i32 button, i32 action) {
  auto& select = app.select;
  // Holding the right button on empty space jumps through the list, as in
  // osu!, which keeps right-clicks on a row for its beatmap options.
  if (button == GLFW_MOUSE_BUTTON_RIGHT) {
    select.right_down = action == GLFW_PRESS && select.hovered_row < 0 &&
                        select.hovered_replay < 0;
    select.fling = 0;
    return;
  }
  if (button != GLFW_MOUSE_BUTTON_LEFT)
    return;
  if (action == GLFW_PRESS && select.hovered_replay >= 0) {
    watch(app, *map_replays(app, app.selected_map)[select.hovered_replay]);
    return;
  }
  if (action == GLFW_PRESS) {
    if (!in_carousel(app))
      return;
    select.left_down = true;
    select.dragging = false;
    select.drag_start_y = app.cursor.y;
    select.drag_start_scroll = select.scroll;
    select.fling = 0;
    return;
  }
  // A press that didn't become a drag is a click.
  const bool clicked = select.left_down && !select.dragging;
  select.left_down = false;
  select.dragging = false;
  const auto rows = build_rows(app);
  const i32  hovered = select.hovered_row;
  if (!clicked || hovered < 0 || hovered >= static_cast<i32>(rows.size()))
    return;
  const Row& row = rows[hovered];
  if (row.map < 0)
    choose(app, app.library.sets[row.set].maps.front());
  else if (row.map == app.selected_map)
    start(app);
  else
    choose(app, row.map);
}

void select_scroll(App& app, f64 dy) {
  auto& select = app.select;
  select.fling = 0;
  select.scroll_target =
      std::clamp(select.scroll_target - static_cast<f32>(dy) * kWheelStep, 0.0f,
                 scroll_limit(build_rows(app)));
}

}  // namespace circles
