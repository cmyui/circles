// The settings panel, opened with Ctrl+O as osu! opens its options, anywhere
// but in a map: the gameplay background dim, where 100% hides the
// background; the cursor's size, as osu!'s multiple of the skin's; whether
// frames are capped at 1000 per second; and whether maps' own combo colours
// replace the skin's.
#include <circles/app.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace circles {

namespace {

// A row sets a number with a slider, or a choice with a switch.
struct RowSpec {
  const char* label;
  f32 App::* number = nullptr;
  f32        min = 0, max = 1;
  bool App::* choice = nullptr;
};

enum Row : i32 {
  kDimRow,
  kCursorRow,
  kFrameCapRow,
  kComboColourRow,
  kRowCount
};

constexpr RowSpec kRows[kRowCount] = {
    {"Background dim", &App::background_dim, 0, 1},
    // osu!'s range.
    {"Cursor size", &App::cursor_size, 0.1f, 2},
    {"Cap frames at 1000 fps", nullptr, 0, 1, &App::frame_cap},
    {"Beatmap combo colours", nullptr, 0, 1, &App::beatmap_colours},
};

constexpr f32 kKeyStep = 0.05f;
// Sliders are a label over a track; switches are one line.
constexpr f32 kSliderHeight = 66, kSwitchHeight = 44, kRowGap = 12;
constexpr f32 kRowsTop = 72, kFooter = 56, kPanelWidth = 460;

f32 row_height(i32 row) {
  return kRows[row].number ? kSliderHeight : kSwitchHeight;
}

// From the panel's top.
f32 row_top(i32 row) {
  f32 top = kRowsTop;
  for (i32 i = 0; i < row; ++i)
    top += row_height(i) + kRowGap;
  return top;
}

struct PanelLayout {
  f32 x, y, w, h;
  f32 track_x, track_w;
  f32 track_y(i32 row) const { return y + row_top(row) + 46; }
};

PanelLayout panel_layout(const App& app) {
  const f32 h = row_top(kRowCount) - kRowGap + kFooter;
  const f32 x = (app.width - kPanelWidth) / 2, y = (app.height - h) / 2;
  return {x, y, kPanelWidth, h, x + 32, kPanelWidth - 64};
}

// The row under the cursor, -1 for none: a slider's track or a switch's line.
i32 row_at(const App& app) {
  const PanelLayout p = panel_layout(app);
  for (i32 row = 0; row < kRowCount; ++row)
    if (kRows[row].number ? contains(app.cursor, p.track_x - 10,
                                     p.track_y(row) - 16, p.track_w + 20, 32)
                          : contains(app.cursor, p.x + 16, p.y + row_top(row),
                                     p.w - 32, kSwitchHeight))
      return row;
  return -1;
}

// In hundredths, so round values like 100% and 1.00x are easy to land on.
void set_number(App& app, i32 row, f32 value) {
  const RowSpec& spec = kRows[row];
  value = std::clamp(std::round(value * 100) / 100, spec.min, spec.max);
  if (value == app.*spec.number)
    return;
  app.*spec.number = value;
  save_settings(app);
}

void number_from_cursor(App& app, i32 row) {
  const PanelLayout p = panel_layout(app);
  const RowSpec&    spec = kRows[row];
  const f32         t = clamp01((app.cursor.x - p.track_x) / p.track_w);
  set_number(app, row, spec.min + t * (spec.max - spec.min));
}

void toggle_choice(App& app, i32 row) {
  bool& choice = app.*kRows[row].choice;
  choice = !choice;
  save_settings(app);
  play_sound(app.audio, Sound::MenuClick, 0.5f);
}

}  // namespace

void toggle_settings(App& app) {
  auto& menu = app.settings_menu;
  menu.open = !menu.open;
  menu.changed = app.now;
  menu.dragging = false;
  play_sound(app.audio, Sound::MenuClick, 0.6f);
}

void settings_key(App& app, i32 key, i32 action) {
  auto& menu = app.settings_menu;
  if (action == GLFW_RELEASE)
    return;
  const bool     press = action == GLFW_PRESS;
  const RowSpec& spec = kRows[menu.selected];
  if (key == GLFW_KEY_ESCAPE && press) {
    toggle_settings(app);
  } else if (key == GLFW_KEY_UP || key == GLFW_KEY_DOWN) {
    menu.selected = std::clamp(menu.selected + (key == GLFW_KEY_DOWN ? 1 : -1),
                               0, kRowCount - 1);
  } else if (spec.number && (key == GLFW_KEY_LEFT || key == GLFW_KEY_RIGHT)) {
    set_number(
        app, menu.selected,
        app.*spec.number + (key == GLFW_KEY_RIGHT ? kKeyStep : -kKeyStep));
  } else if (spec.choice && press &&
             (key == GLFW_KEY_LEFT || key == GLFW_KEY_RIGHT ||
              key == GLFW_KEY_ENTER || key == GLFW_KEY_SPACE)) {
    toggle_choice(app, menu.selected);
  }
}

void settings_click(App& app, i32 button, i32 action) {
  auto& menu = app.settings_menu;
  if (button != GLFW_MOUSE_BUTTON_LEFT)
    return;
  if (action == GLFW_RELEASE) {
    menu.dragging = false;
    return;
  }
  const PanelLayout p = panel_layout(app);
  const i32         row = row_at(app);
  if (row >= 0) {
    menu.selected = row;
    if (kRows[row].number) {
      menu.dragging = true;
      number_from_cursor(app, row);
    } else {
      toggle_choice(app, row);
    }
  } else if (!contains(app.cursor, p.x, p.y, p.w, p.h)) {
    toggle_settings(app);
  }
}

void settings_cursor(App& app) {
  if (app.settings_menu.dragging)
    number_from_cursor(app, app.settings_menu.selected);
}

void settings_draw(App& app) {
  auto&       r = app.renderer;
  const auto& menu = app.settings_menu;
  const f32   appear =
      ease_out(clamp01(static_cast<f32>((app.now - menu.changed) / 0.15)));
  draw_rect(r, 0, 0, app.width, app.height, fade(kBlack, 0.55f * appear));
  const PanelLayout p = panel_layout(app);
  const f32         lift = (1 - appear) * 20;
  draw_rounded_rect(r, p.x, p.y + lift, p.w, p.h, 18,
                    fade(hex_color(0x12122A), 0.96f * appear));
  draw_text_shadowed(r, app.heavy, "Settings", p.x + 32, p.y + lift + 50, 28,
                     fade(kWhite, appear));

  const i32 hovered = row_at(app);
  for (i32 row = 0; row < kRowCount; ++row) {
    const RowSpec& spec = kRows[row];
    const f32      top = p.y + lift + row_top(row);
    // The row the keys change.
    if (row == menu.selected)
      draw_rounded_rect(r, p.x + 16, top, p.w - 32, row_height(row), 10,
                        fade(kWhite, 0.06f * appear));

    if (spec.number) {
      const f32 value = app.*spec.number;
      draw_text(r, app.bold, spec.label, p.x + 32, top + 22, 17,
                fade(kWhite, appear));
      char text[32];
      if (row == kDimRow && value >= 1)
        snprintf(text, sizeof(text), "100%%  (hidden)");
      else if (row == kDimRow)
        snprintf(text, sizeof(text), "%.0f%%", value * 100);
      else
        snprintf(text, sizeof(text), "%.2fx", value);
      draw_text(r, app.bold, text, p.x + p.w - 32, top + 22, 17,
                fade(kWhite, 0.85f * appear), Align::Right);
      const f32 track_y = p.track_y(row) + lift;
      const f32 hover =
          (menu.dragging && row == menu.selected) || hovered == row ? 1.0f
                                                                    : 0.0f;
      draw_rounded_rect(r, p.track_x, track_y - 3, p.track_w, 6, 3,
                        fade(kWhite, 0.2f * appear));
      const f32 fill = p.track_w * (value - spec.min) / (spec.max - spec.min);
      draw_rounded_rect(r, p.track_x, track_y - 3, fill, 6, 3,
                        fade(hex_color(0x8844CC), appear));
      draw_circle(r, p.track_x + fill, track_y, 9 + 2 * hover,
                  fade(kWhite, appear));
      continue;
    }

    const bool on = app.*spec.choice;
    const f32  centre = top + kSwitchHeight / 2;
    draw_text(r, app.bold, spec.label, p.x + 32,
              centered_baseline(app.bold, 17, centre), 17,
              fade(kWhite, appear));
    // An on/off switch.
    constexpr f32 kSwitchW = 46, kSwitchH = 26;
    const f32     switch_x = p.x + p.w - 32 - kSwitchW;
    draw_rounded_rect(
        r, switch_x, centre - kSwitchH / 2, kSwitchW, kSwitchH, kSwitchH / 2,
        fade(on ? hex_color(0x8844CC) : hex_color(0x3A3A55), appear));
    const f32 knob_x =
        on ? switch_x + kSwitchW - kSwitchH / 2 : switch_x + kSwitchH / 2;
    draw_circle(r, knob_x, centre, kSwitchH / 2 - 3, fade(kWhite, appear));
  }

  draw_text(r, app.regular,
            "Up / Down choose   ·   Left / Right change   ·   Esc closes",
            p.x + 32, p.y + lift + p.h - 22, 13, fade(kWhite, 0.5f * appear));
}

}  // namespace circles
