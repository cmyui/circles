#include <circles/app.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace circles {

namespace {

struct Button {
  f32 x, y, w, h;
};

enum ButtonIndex : i32 { kRetry, kWatch, kBack, kButtonCount };

// The screen is laid out for the default 1280x720 window. It scales with the
// window's height, as far as the width of its bottom row, the histogram and
// buttons, allows.
f32 layout_scale(const App& app) {
  return std::min(app.height / 720, app.width / 1112);
}

Button button(const App& app, i32 index) {
  const f32 s = layout_scale(app);
  const f32 width = 180 * s, height = 56 * s, gap = 16 * s;
  return {
      app.width - kButtonCount * (width + gap) - 24 * s + index * (width + gap),
      app.height - height - 40 * s, width, height};
}

i32 button_at(const App& app, Vec2 point) {
  for (i32 i = 0; i < kButtonCount; ++i) {
    const Button b = button(app, i);
    if (contains(point, b.x, b.y, b.w, b.h))
      return i;
  }
  return -1;
}

void activate(App& app, i32 index) {
  if (index < 0 || (index == kWatch && app.results.replay_path.empty()))
    return;
  play_sound(app.audio, Sound::MenuClick, 0.7f);
  if (index == kWatch)
    app.replay_to_watch = app.results.replay_path;
  switch_screen(app, index == kBack ? Screen::SongSelect : Screen::Gameplay);
}

struct Spread {
  f64 mean = 0;
  f64 unstable_rate = 0;  // ten times the standard deviation, as osu! shows
};

Spread spread(const std::vector<f64>& offsets) {
  if (offsets.empty())
    return {};
  Spread result;
  for (f64 offset : offsets)
    result.mean += offset;
  result.mean /= offsets.size();
  f64 variance = 0;
  for (f64 offset : offsets)
    variance += (offset - result.mean) * (offset - result.mean);
  result.unstable_rate = 10 * std::sqrt(variance / offsets.size());
  return result;
}

void draw_histogram(App& app, f32 x, f32 y, f32 w, f32 h, f32 s) {
  auto&         r = app.renderer;
  const auto&   results = app.results;
  constexpr i32 kBins = 41;
  i32           bins[kBins] = {};
  const f64     window = std::max(1.0, results.hit50_window);
  for (f64 offset : results.offsets) {
    const i32 bin =
        static_cast<i32>(std::floor((offset + window) / (2 * window) * kBins));
    ++bins[std::clamp(bin, 0, kBins - 1)];
  }
  const i32 peak = std::max(1, *std::max_element(bins, bins + kBins));
  const f32 bin_w = w / kBins;
  draw_rect(r, x, y + h, w, 1, fade(kWhite, 0.3f));
  for (i32 i = 0; i < kBins; ++i) {
    const f32 bar = h * bins[i] / peak;
    // Shade by how close the bin's centre is to perfect timing.
    const f32 closeness = 1 - std::abs((i + 0.5f) / kBins * 2 - 1);
    draw_rounded_rect(r, x + i * bin_w + 1, y + h - bar, bin_w - 2, bar, 2,
                      mix(hex_color(0xFFCC22), hex_color(0x66CCFF), closeness));
  }
  draw_rect(r, x + w / 2 - 0.5f, y - 6 * s, 1, h + 12 * s, fade(kWhite, 0.6f));
  draw_text(r, app.regular, "early", x, y + h + 18 * s, 12 * s,
            fade(kWhite, 0.6f));
  draw_text(r, app.regular, "late", x + w, y + h + 18 * s, 12 * s,
            fade(kWhite, 0.6f), Align::Right);
}

}  // namespace

void open_score(App& app, i32 map, const ReplayEntry& entry) {
  const Replay& replay = entry.replay;
  play_sound(app.audio, Sound::MenuClick, 0.7f);
  // An .osr holds the score but not its hit errors; judging it finds them.
  judge_replay(app, map, entry.path.string());
  app.results = Results{
      .map_index = map,
      .score = replay.score,
      .max_combo = replay.max_combo,
      .counts = replay.counts,
      .offsets = app.game.offsets,
      .hit50_window = app.game.windows.hit50,
      .player = replay.player,
      .replay_path = entry.path.string(),
      .mods = replay.mods,
  };
  switch_screen(app, Screen::Results);
}

void results_enter(App& app) {
  app.results.entered = app.now;
  app.results.hovered = -1;
}

void results_update(App& app) {
  const i32 hovered = button_at(app, app.cursor);
  if (hovered != app.results.hovered && hovered >= 0)
    play_sound(app.audio, Sound::MenuHover, 0.5f);
  app.results.hovered = hovered;
}

void results_draw(App& app) {
  auto&       r = app.renderer;
  const auto& results = app.results;
  const auto& info = app.library.maps[results.map_index];
  const f32   t = static_cast<f32>(app.now - results.entered);
  const f32   s = layout_scale(app);
  const f32   left = 40 * s;
  draw_backdrop(app, 0.72f, 0.4f);

  draw_rect_gradient(r, 0, 0, app.width, 120 * s, fade(kBlack, 0.6f),
                     fade(kBlack, 0));
  draw_text_shadowed(r, app.bold,
                     fit_text(app.bold, info.artist + " - " + info.title,
                              28 * s, app.width - 2 * left),
                     left, 52 * s, 28 * s, kWhite);
  draw_text_shadowed(r, app.regular,
                     "[" + info.version + "]   mapped by " + info.creator +
                         "   played by " + results.player,
                     left, 80 * s, 17 * s, fade(kWhite, 0.8f));

  // Totals count up as the screen opens.
  const f32 count_up = ease_out(t / 1.0f);
  char      text[64];
  snprintf(text, sizeof(text), "%08lld",
           static_cast<long long>(std::llround(results.score * count_up)));
  draw_text(r, app.regular, "SCORE", left, 140 * s, 14 * s, fade(kWhite, 0.6f));
  draw_text_shadowed(r, app.bold, text, left, 200 * s, 64 * s, kWhite);

  struct Line {
    const char* label;
    i32         count;
    Color       colour;
  };
  const Line lines[] = {
      {"300", results.counts.hit300, hex_color(0x66CCFF)},
      {"100", results.counts.hit100, hex_color(0x88EE55)},
      {"50", results.counts.hit50, hex_color(0xFFCC22)},
      {"MISS", results.counts.miss, hex_color(0xFF4466)},
  };
  for (i32 i = 0; i < 4; ++i) {
    const f32 x = left + (i % 2) * 200 * s, y = (260 + (i / 2) * 44) * s;
    draw_text_shadowed(r, app.bold, lines[i].label, x, y, 20 * s,
                       lines[i].colour);
    snprintf(text, sizeof(text), "%dx",
             static_cast<i32>(std::lround(lines[i].count * count_up)));
    draw_text_shadowed(r, app.bold, text, x + 70 * s, y, 24 * s, kWhite);
  }

  draw_text(r, app.regular, "ACCURACY", left, 360 * s, 14 * s,
            fade(kWhite, 0.6f));
  snprintf(text, sizeof(text), "%.2f%%",
           accuracy(results.counts) * 100 * count_up);
  draw_text_shadowed(r, app.bold, text, left, 396 * s, 32 * s, kWhite);
  draw_text(r, app.regular, "MAX COMBO", left + 200 * s, 360 * s, 14 * s,
            fade(kWhite, 0.6f));
  snprintf(text, sizeof(text), "%dx",
           static_cast<i32>(std::lround(results.max_combo * count_up)));
  draw_text_shadowed(r, app.bold, text, left + 200 * s, 396 * s, 32 * s,
                     kWhite);

  const Spread timing = spread(results.offsets);
  snprintf(text, sizeof(text), "Unstable rate %.1f    Average %+.1f ms %s",
           timing.unstable_rate, timing.mean,
           std::abs(timing.mean) < 1 ? ""
           : timing.mean < 0         ? "(early)"
                                     : "(late)");
  draw_text(r, app.regular, text, left, 436 * s, 15 * s, fade(kWhite, 0.85f));
  if (!results.offsets.empty())
    draw_histogram(app, left, app.height - 170 * s,
                   std::min(app.width * 0.42f, 460 * s), 80 * s, s);

  // The grade lands after the totals.
  const Grade grade_ = grade(results.counts);
  const f32   land = clamp01((t - 0.6f) / 0.35f);
  if (land > 0) {
    constexpr const char* kGradeImages[] = {"ranking-x", "ranking-s",
                                            "ranking-a", "ranking-b",
                                            "ranking-c", "ranking-d"};
    const bool            silver = silver_grade(grade_, results.mods);
    const char*           image_name =
        silver ? (grade_ == Grade::SS ? "ranking-xh" : "ranking-sh")
               : kGradeImages[static_cast<i32>(grade_)];
    const Color colour = grade_colour(grade_, results.mods);
    const f32   overshoot = 1 + 0.6f * (1 - ease_out(land));
    const f32   cx = app.width * 0.74f, cy = app.height * 0.46f;
    if (const auto* image = skin_element(app.skin, image_name)) {
      draw_skin_image(r, image->front(), cx, cy, ui_scale(app) * overshoot,
                      fade(kWhite, land));
    } else {
      const f32 size = app.height * 0.42f * overshoot;
      draw_circle(r, cx, cy, app.height * 0.27f, fade(colour, 0.25f * land), 0,
                  1);
      draw_text_shadowed(r, app.heavy, grade_name(grade_), cx,
                         centered_baseline(app.heavy, size, cy), size,
                         fade(colour, land), Align::Center);
    }
    draw_mod_icons(app, results.mods, cx, cy + app.height * 0.3f,
                   mod_icon_height(app), Align::Center, t - 0.6f);
  }

  const char* labels[] = {"RETRY", "WATCH", "BACK"};
  const Color accents[] = {hex_color(0xDD9933), hex_color(0x33AA88),
                           hex_color(0x6655CC)};
  for (i32 i = 0; i < kButtonCount; ++i) {
    const Button b = button(app, i);
    draw_button(app, b.x, b.y, b.w, b.h, labels[i],
                results.hovered == i ? 1.0f : 0.0f, accents[i]);
  }
}

void results_key(App& app, i32 key, i32 action) {
  if (action != GLFW_PRESS)
    return;
  if (key == GLFW_KEY_R || key == GLFW_KEY_GRAVE_ACCENT)
    activate(app, kRetry);
  else if (key == GLFW_KEY_W)
    activate(app, kWatch);
  else if (key == GLFW_KEY_ESCAPE || key == GLFW_KEY_ENTER ||
           key == GLFW_KEY_KP_ENTER)
    activate(app, kBack);
}

void results_click(App& app, i32 button, i32 action) {
  if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_PRESS)
    activate(app, button_at(app, app.cursor));
}

}  // namespace circles
