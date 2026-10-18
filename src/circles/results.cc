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

// Retry, then back.
Button button(const App& app, i32 index) {
  constexpr f32 kWidth = 200, kHeight = 56, kGap = 16;
  return {app.width - 2 * kWidth - kGap - 40 + index * (kWidth + kGap),
          app.height - kHeight - 40, kWidth, kHeight};
}

i32 button_at(const App& app, Vec2 point) {
  for (i32 i = 0; i < 2; ++i) {
    const Button b = button(app, i);
    if (contains(point, b.x, b.y, b.w, b.h))
      return i;
  }
  return -1;
}

void activate(App& app, i32 index) {
  if (index < 0)
    return;
  play_sound(app.audio, Sound::MenuClick, 0.7f);
  switch_screen(app, index == 0 ? Screen::Gameplay : Screen::SongSelect);
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

void draw_histogram(App& app, f32 x, f32 y, f32 w, f32 h) {
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
  draw_rect(r, x + w / 2 - 0.5f, y - 6, 1, h + 12, fade(kWhite, 0.6f));
  draw_text(r, app.regular, "early", x, y + h + 18, 12, fade(kWhite, 0.6f));
  draw_text(r, app.regular, "late", x + w, y + h + 18, 12, fade(kWhite, 0.6f),
            Align::Right);
}

}  // namespace

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
  draw_backdrop(app, 0.72f, 0.4f);

  draw_rect_gradient(r, 0, 0, app.width, 120, fade(kBlack, 0.6f),
                     fade(kBlack, 0));
  draw_text_shadowed(
      r, app.bold,
      fit_text(app.bold, info.artist + " - " + info.title, 28, app.width - 80),
      40, 52, 28, kWhite);
  draw_text_shadowed(r, app.regular,
                     "[" + info.version + "]   mapped by " + info.creator, 40,
                     80, 17, fade(kWhite, 0.8f));

  // Totals count up as the screen opens.
  const f32 count_up = ease_out(t / 1.0f);
  char      text[64];
  snprintf(text, sizeof(text), "%08lld",
           static_cast<long long>(std::llround(results.score * count_up)));
  draw_text(r, app.regular, "SCORE", 40, 140, 14, fade(kWhite, 0.6f));
  draw_text_shadowed(r, app.bold, text, 40, 200, 64, kWhite);

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
    const f32 x = 40 + (i % 2) * 200, y = 260 + (i / 2) * 44;
    draw_text_shadowed(r, app.bold, lines[i].label, x, y, 20, lines[i].colour);
    snprintf(text, sizeof(text), "%dx",
             static_cast<i32>(std::lround(lines[i].count * count_up)));
    draw_text_shadowed(r, app.bold, text, x + 70, y, 24, kWhite);
  }

  draw_text(r, app.regular, "ACCURACY", 40, 360, 14, fade(kWhite, 0.6f));
  snprintf(text, sizeof(text), "%.2f%%",
           accuracy(results.counts) * 100 * count_up);
  draw_text_shadowed(r, app.bold, text, 40, 396, 32, kWhite);
  draw_text(r, app.regular, "MAX COMBO", 240, 360, 14, fade(kWhite, 0.6f));
  snprintf(text, sizeof(text), "%dx",
           static_cast<i32>(std::lround(results.max_combo * count_up)));
  draw_text_shadowed(r, app.bold, text, 240, 396, 32, kWhite);

  const Spread timing = spread(results.offsets);
  snprintf(text, sizeof(text), "Unstable rate %.1f    Average %+.1f ms %s",
           timing.unstable_rate, timing.mean,
           std::abs(timing.mean) < 1 ? ""
           : timing.mean < 0         ? "(early)"
                                     : "(late)");
  draw_text(r, app.regular, text, 40, 436, 15, fade(kWhite, 0.85f));
  if (!results.offsets.empty())
    draw_histogram(app, 40, app.height - 170,
                   std::min(app.width * 0.42f, 460.0f), 80);

  // The grade lands after the totals.
  const Grade grade_ = grade(results.counts);
  const f32   land = clamp01((t - 0.6f) / 0.35f);
  if (land > 0) {
    constexpr const char* kGradeImages[] = {"ranking-x", "ranking-s",
                                            "ranking-a", "ranking-b",
                                            "ranking-c", "ranking-d"};
    const Color           colour = grade_colour(grade_);
    const f32             overshoot = 1 + 0.6f * (1 - ease_out(land));
    const f32             cx = app.width * 0.74f, cy = app.height * 0.46f;
    if (const auto* image =
            skin_element(app.skin, kGradeImages[static_cast<i32>(grade_)])) {
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
  }

  const char* labels[] = {"RETRY", "BACK"};
  const Color accents[] = {hex_color(0xDD9933), hex_color(0x6655CC)};
  for (i32 i = 0; i < 2; ++i) {
    const Button b = button(app, i);
    draw_button(app, b.x, b.y, b.w, b.h, labels[i],
                results.hovered == i ? 1.0f : 0.0f, accents[i]);
  }
}

void results_key(App& app, i32 key, i32 action) {
  if (action != GLFW_PRESS)
    return;
  if (key == GLFW_KEY_R || key == GLFW_KEY_GRAVE_ACCENT)
    activate(app, 0);
  else if (key == GLFW_KEY_ESCAPE || key == GLFW_KEY_ENTER ||
           key == GLFW_KEY_KP_ENTER)
    activate(app, 1);
}

void results_click(App& app, i32 button, i32 action) {
  if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_PRESS)
    activate(app, button_at(app, app.cursor));
}

}  // namespace circles
