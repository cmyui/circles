#include <circles/app.h>
#include <circles/strings.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace circles {

namespace {

constexpr f32 kRowHeight = 64;
constexpr f32 kRowStride = 72;

// A carousel row: a set's header, or one of the selected set's difficulties.
struct Row {
  i32 set;
  i32 map;  // negative for a header
};

std::vector<Row> build_rows(const App& app) {
  std::vector<Row> rows;
  const i32        selected_set =
      app.selected_map >= 0 ? app.library.set_of_map[app.selected_map] : -1;
  for (i32 set : app.select.sets) {
    rows.push_back({set, -1});
    if (set == selected_set)
      for (i32 map : app.library.sets[set].maps)
        rows.push_back({set, map});
  }
  return rows;
}

i32 row_of_map(const std::vector<Row>& rows, i32 map) {
  for (size_t i = 0; i < rows.size(); ++i)
    if (rows[i].map == map)
      return static_cast<i32>(i);
  return 0;
}

struct Rect {
  f32 x, y, w, h;
};

// Rows curve away to the right above and below the centre.
Rect row_rect(const App& app, i32 index, const Row& row) {
  const f32 center = app.height * 0.5f + 24;
  const f32 y =
      center + (index - app.select.scroll) * kRowStride - kRowHeight / 2;
  const f32 offset = (y + kRowHeight / 2 - center) / app.height;
  f32       x = app.width * 0.5f + offset * offset * 260;
  if (row.map >= 0)
    x += row.map == app.selected_map ? 8 : 36;
  return {x, y, app.width - x + 40, kRowHeight};
}

void recenter(App& app) {
  app.select.scroll_target =
      static_cast<f32>(row_of_map(build_rows(app), app.selected_map));
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
  const f32   x = 24, y = 24, w = std::min(app.width * 0.44f, 560.0f), h = 252;
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
  auto& select = app.select;
  select.scroll = approach(select.scroll, select.scroll_target, 12, app.dt);
  // Keep the preview looping from its start point.
  if (music_at_end(app.audio))
    play_preview(app);

  const auto rows = build_rows(app);
  i32        hovered = -1;
  for (i32 i = 0; i < static_cast<i32>(rows.size()); ++i) {
    const Rect rect = row_rect(app, i, rows[i]);
    if (contains(app.cursor, rect.x, rect.y, rect.w, rect.h))
      hovered = i;
  }
  if (hovered != select.hovered_row && hovered >= 0)
    play_sound(app.audio, Sound::MenuHover, 0.3f);
  select.hovered_row = hovered;
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
  for (i32 i = 0; i < static_cast<i32>(rows.size()); ++i) {
    const Row& row = rows[i];
    const Rect rect = row_rect(app, i, row);
    if (rect.y > app.height || rect.y + rect.h < 0)
      continue;
    const bool hovered = i == select.hovered_row;
    if (row.map < 0) {
      const auto& set = app.library.sets[row.set];
      draw_rounded_rect(r, rect.x, rect.y, rect.w, rect.h, 12,
                        fade(hex_color(0x1A1A2E), hovered ? 0.95f : 0.82f));
      draw_rounded_rect(r, rect.x + 10, rect.y + 12, 5, rect.h - 24, 2.5f,
                        hex_color(0xFF66AA));
      draw_text(r, app.bold, fit_text(app.bold, set.title, 22, rect.w - 80),
                rect.x + 28, rect.y + 30, 22, kWhite);
      draw_text(r, app.regular,
                fit_text(app.regular, set.artist + "  //  " + set.creator, 15,
                         rect.w - 80),
                rect.x + 28, rect.y + 52, 15, fade(kWhite, 0.7f));
      continue;
    }
    const auto& info = app.library.maps[row.map];
    const bool  selected = row.map == app.selected_map;
    draw_rounded_rect(r, rect.x, rect.y, rect.w, rect.h, 12,
                      selected
                          ? fade(kWhite, 0.95f)
                          : fade(hex_color(0x262640), hovered ? 0.95f : 0.85f));
    draw_rounded_rect(r, rect.x + 10, rect.y + 12, 6, rect.h - 24, 3,
                      difficulty_colour(info));
    const Color text = selected ? hex_color(0x1A1A2E) : kWhite;
    draw_text(r, app.bold, fit_text(app.bold, info.version, 21, rect.w - 80),
              rect.x + 30, rect.y + 29, 21, text);
    char stats[96];
    snprintf(stats, sizeof(stats), "AR %g    OD %g    CS %g    %s", info.ar,
             info.od, info.cs, format_duration(info.end_time).c_str());
    draw_text(r, app.regular, stats, rect.x + 30, rect.y + 51, 14,
              fade(text, 0.75f));
  }

  draw_info_panel(app);
  draw_search_box(app);
  draw_rect_gradient(r, 0, app.height - 64, app.width, 64, fade(kBlack, 0),
                     fade(kBlack, 0.7f));
  draw_text(r, app.regular,
            "Enter  play        Esc  back        F2  random        Arrows  "
            "choose        Type  search",
            app.width / 2, app.height - 22, 14, fade(kWhite, 0.75f),
            Align::Center);
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
  if (button != GLFW_MOUSE_BUTTON_LEFT || action != GLFW_PRESS)
    return;
  const auto rows = build_rows(app);
  const i32  hovered = app.select.hovered_row;
  if (hovered < 0 || hovered >= static_cast<i32>(rows.size()))
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
  const auto rows = build_rows(app);
  app.select.scroll_target =
      std::clamp(app.select.scroll_target - static_cast<f32>(dy) * 1.5f, 0.0f,
                 static_cast<f32>(std::max<size_t>(rows.size(), 1) - 1));
}

}  // namespace circles
