// The settings panel, opened with Ctrl+O as osu! opens its options, anywhere
// but in a map: the gameplay background dim, where 100% hides the
// background, and whether frames are capped at 1000 per second.
#include <circles/app.h>

#include <algorithm>
#include <cstdio>

namespace circles {

namespace {

constexpr f32 kDimStep = 0.05f;

enum Row : i32 { kDimRow, kFrameCapRow, kRowCount };

struct PanelLayout {
  f32 x, y, w, h;
  f32 track_x, track_y, track_w;
  f32 cap_y;  // the frame cap row's centre
};

PanelLayout panel_layout(const App& app) {
  constexpr f32 w = 460, h = 250;
  const f32     x = (app.width - w) / 2, y = (app.height - h) / 2;
  return {x, y, w, h, x + 32, y + 118, w - 64, y + 172};
}

bool over_track(const App& app) {
  const PanelLayout p = panel_layout(app);
  return contains(app.cursor, p.track_x - 10, p.track_y - 16, p.track_w + 20,
                  32);
}

bool over_frame_cap(const App& app) {
  const PanelLayout p = panel_layout(app);
  return contains(app.cursor, p.x + 16, p.cap_y - 20, p.w - 32, 40);
}

void set_dim(App& app, f32 dim) {
  dim = std::clamp(dim, 0.0f, 1.0f);
  if (dim == app.background_dim)
    return;
  app.background_dim = dim;
  save_settings(app);
}

void step_dim(App& app, f32 step) {
  set_dim(app, std::round((app.background_dim + step) * 100) / 100);
}

void dim_from_cursor(App& app) {
  const PanelLayout p = panel_layout(app);
  // Whole percents, so 100% is easy to land on.
  set_dim(app, std::round((app.cursor.x - p.track_x) / p.track_w * 100) / 100);
}

void toggle_frame_cap(App& app) {
  app.frame_cap = !app.frame_cap;
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
  const bool press = action == GLFW_PRESS;
  if (key == GLFW_KEY_ESCAPE && press) {
    toggle_settings(app);
  } else if (key == GLFW_KEY_UP || key == GLFW_KEY_DOWN) {
    menu.selected = std::clamp(menu.selected + (key == GLFW_KEY_DOWN ? 1 : -1),
                               0, kRowCount - 1);
  } else if (menu.selected == kDimRow &&
             (key == GLFW_KEY_LEFT || key == GLFW_KEY_RIGHT)) {
    step_dim(app, key == GLFW_KEY_RIGHT ? kDimStep : -kDimStep);
  } else if (menu.selected == kFrameCapRow && press &&
             (key == GLFW_KEY_LEFT || key == GLFW_KEY_RIGHT ||
              key == GLFW_KEY_ENTER || key == GLFW_KEY_SPACE)) {
    toggle_frame_cap(app);
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
  if (over_track(app)) {
    menu.selected = kDimRow;
    menu.dragging = true;
    dim_from_cursor(app);
  } else if (over_frame_cap(app)) {
    menu.selected = kFrameCapRow;
    toggle_frame_cap(app);
  } else if (!contains(app.cursor, p.x, p.y, p.w, p.h)) {
    toggle_settings(app);
  }
}

void settings_cursor(App& app) {
  if (app.settings_menu.dragging)
    dim_from_cursor(app);
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
  // The row the keys change.
  const f32 selected_y = menu.selected == kDimRow ? p.y + 72 : p.cap_y - 22;
  draw_rounded_rect(r, p.x + 16, selected_y + lift, p.w - 32,
                    menu.selected == kDimRow ? 66 : 44, 10,
                    fade(kWhite, 0.06f * appear));

  draw_text(r, app.bold, "Background dim", p.x + 32, p.y + lift + 94, 17,
            fade(kWhite, appear));
  char value[32];
  if (app.background_dim >= 1)
    snprintf(value, sizeof(value), "100%%  (hidden)");
  else
    snprintf(value, sizeof(value), "%.0f%%", app.background_dim * 100);
  draw_text(r, app.bold, value, p.x + p.w - 32, p.y + lift + 94, 17,
            fade(kWhite, 0.85f * appear), Align::Right);
  const f32 track_y = p.track_y + lift;
  const f32 hover = menu.dragging || over_track(app) ? 1.0f : 0.0f;
  draw_rounded_rect(r, p.track_x, track_y - 3, p.track_w, 6, 3,
                    fade(kWhite, 0.2f * appear));
  const f32 fill = p.track_w * app.background_dim;
  draw_rounded_rect(r, p.track_x, track_y - 3, fill, 6, 3,
                    fade(hex_color(0x8844CC), appear));
  draw_circle(r, p.track_x + fill, track_y, 9 + 2 * hover,
              fade(kWhite, appear));

  const f32 cap_y = p.cap_y + lift;
  draw_text(r, app.bold, "Cap frames at 1000 fps", p.x + 32,
            centered_baseline(app.bold, 17, cap_y), 17, fade(kWhite, appear));
  // An on/off switch.
  constexpr f32 kSwitchW = 46, kSwitchH = 26;
  const f32     switch_x = p.x + p.w - 32 - kSwitchW;
  draw_rounded_rect(
      r, switch_x, cap_y - kSwitchH / 2, kSwitchW, kSwitchH, kSwitchH / 2,
      fade(app.frame_cap ? hex_color(0x8844CC) : hex_color(0x3A3A55), appear));
  const f32 knob_x = app.frame_cap ? switch_x + kSwitchW - kSwitchH / 2
                                   : switch_x + kSwitchH / 2;
  draw_circle(r, knob_x, cap_y, kSwitchH / 2 - 3, fade(kWhite, appear));

  draw_text(r, app.regular,
            "Up / Down choose   ·   Left / Right change   ·   Esc closes",
            p.x + 32, p.y + lift + p.h - 22, 13, fade(kWhite, 0.5f * appear));
}

}  // namespace circles
