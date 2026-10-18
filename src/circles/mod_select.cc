#include <circles/app.h>

#include <algorithm>
#include <cstdio>

namespace circles {

namespace {

constexpr f32 kButtonWidth = 112;
constexpr f32 kButtonHeight = 92;
constexpr f32 kButtonGap = 14;
constexpr f32 kRowHeight = kButtonHeight + 34;
constexpr f32 kPanelWidth = 860;
constexpr f32 kPanelHeight = 3 * kRowHeight + 150;

Color group_colour(ModGroup group) {
  switch (group) {
    case ModGroup::Reduction:
      return hex_color(0x66CC66);
    case ModGroup::Increase:
      return hex_color(0xEE5577);
    case ModGroup::Special:
      return hex_color(0x6688FF);
  }
  return kWhite;
}

struct Rect {
  f32 x, y, w, h;
};

Rect panel_rect(const App& app) {
  const f32 w = std::min(kPanelWidth, app.width - 40);
  return {(app.width - w) / 2, (app.height - kPanelHeight) / 2, w,
          kPanelHeight};
}

Rect button_rect(const App& app, size_t row, size_t column) {
  const Rect panel = panel_rect(app);
  return {panel.x + 210 + column * (kButtonWidth + kButtonGap),
          panel.y + 86 + row * kRowHeight, kButtonWidth, kButtonHeight};
}

// Reset, then close.
Rect action_rect(const App& app, i32 index) {
  const Rect    panel = panel_rect(app);
  constexpr f32 kWidth = 170, kHeight = 48;
  return {panel.x + panel.w - 2 * (kWidth + 16) + index * (kWidth + 16),
          panel.y + panel.h - kHeight - 22, kWidth, kHeight};
}

// Buttons are numbered row by row.
i32 button_at(const App& app, Vec2 point) {
  i32 index = 0;
  for (size_t row = 0; row < mod_rows().size(); ++row) {
    for (size_t column = 0; column < mod_rows()[row].buttons.size();
         ++column, ++index) {
      const Rect b = button_rect(app, row, column);
      if (contains(point, b.x, b.y, b.w, b.h))
        return index;
    }
  }
  return -1;
}

const ModButton* button_by_index(i32 index) {
  for (const ModRow& row : mod_rows()) {
    if (index < static_cast<i32>(row.buttons.size()))
      return &row.buttons[index];
    index -= static_cast<i32>(row.buttons.size());
  }
  return nullptr;
}

void set_mods(App& app, Mods mods) {
  app.mods = mods;
  save_settings(app);
  // The preview plays at the chosen speed, as the map will.
  music_set_rate(app.audio, mod_rate(mods), !mods_change_pitch(mods));
}

void press(App& app, const ModButton& button) {
  set_mods(app, press_mod_button(app.mods, button));
  play_sound(app.audio, Sound::MenuClick, 0.6f);
}

void close(App& app) {
  app.select.mod_select = false;
  play_sound(app.audio, Sound::MenuClick, 0.5f);
}

}  // namespace

void draw_mod_icon(App&             app,
                   const ModButton& button,
                   i32              state,
                   f32              cx,
                   f32              cy,
                   f32              height,
                   f32              alpha) {
  auto&     r = app.renderer;
  const i32 which = std::max(state, 1) - 1;
  if (const auto* image = skin_element(app.skin, button.skin_images[which])) {
    const SkinImage& frame = image->front();
    draw_skin_image(r, frame, cx, cy, height / frame.height(),
                    fade(kWhite, alpha));
    return;
  }
  ModGroup group = ModGroup::Special;
  for (const ModRow& row : mod_rows())
    for (const ModButton& b : row.buttons)
      if (&b == &button)
        group = row.group;
  const f32 w = height * 1.3f;
  draw_rounded_rect(r, cx - w / 2, cy - height / 2, w, height, height * 0.25f,
                    fade(group_colour(group), alpha));
  const f32 size = height * 0.5f;
  draw_text(r, app.heavy, button.acronyms[which], cx,
            centered_baseline(app.heavy, size, cy), size,
            fade(hex_color(0x1A1A2E), alpha), Align::Center);
}

f32 draw_mod_icons(App&  app,
                   Mods  mods,
                   f32   x,
                   f32   cy,
                   f32   height,
                   Align align) {
  const f32 step = height * 1.45f;  // an icon's width and the gap after it
  i32       count = 0;
  for (const ModRow& row : mod_rows())
    for (const ModButton& button : row.buttons)
      count += mod_button_state(mods, button) != 0;
  const f32 width = count ? count * step - height * 0.15f : 0;
  f32       left = align == Align::Left    ? x
                   : align == Align::Right ? x - width
                                           : x - width / 2;
  for (const ModRow& row : mod_rows()) {
    for (const ModButton& button : row.buttons) {
      if (const i32 state = mod_button_state(mods, button)) {
        draw_mod_icon(app, button, state, left + height * 0.65f, cy, height, 1);
        left += step;
      }
    }
  }
  return width;
}

void mod_select_update(App& app) {
  const i32 hovered = button_at(app, app.cursor);
  if (hovered != app.select.hovered_mod && hovered >= 0)
    play_sound(app.audio, Sound::MenuHover, 0.3f);
  app.select.hovered_mod = hovered;
}

void mod_select_draw(App& app) {
  auto&     r = app.renderer;
  const f32 appear = clamp01(
      static_cast<f32>((app.now - app.select.mod_select_changed) / 0.15));
  const Rect panel = panel_rect(app);
  draw_rect(r, 0, 0, app.width, app.height, fade(kBlack, 0.55f * appear));
  const f32 y_shift = (1 - ease_out(appear)) * 30;
  draw_rounded_rect(r, panel.x, panel.y + y_shift, panel.w, panel.h, 20,
                    fade(hex_color(0x12122A), 0.96f * appear));

  draw_text_shadowed(r, app.heavy, "Mods", panel.x + 32, panel.y + y_shift + 52,
                     34, fade(kWhite, appear));
  char multiplier[48];
  snprintf(multiplier, sizeof(multiplier), "Score multiplier  %.2fx",
           mod_score_multiplier(app.mods));
  draw_text(r, app.bold, multiplier, panel.x + panel.w - 32,
            panel.y + y_shift + 50, 18, fade(kWhite, 0.85f * appear),
            Align::Right);

  i32 index = 0;
  for (size_t row = 0; row < mod_rows().size(); ++row) {
    const ModRow& mod_row = mod_rows()[row];
    const Color   colour = group_colour(mod_row.group);
    const Rect    first = button_rect(app, row, 0);
    draw_text(r, app.bold, mod_row.title, panel.x + 32,
              first.y + y_shift + first.h / 2 + 6, 17, fade(colour, appear));
    for (size_t column = 0; column < mod_row.buttons.size();
         ++column, ++index) {
      const ModButton& button = mod_row.buttons[column];
      const i32        state = mod_button_state(app.mods, button);
      const bool       hovered = index == app.select.hovered_mod;
      Rect             b = button_rect(app, row, column);
      b.y += y_shift - (state ? 4 : 0);
      draw_rounded_rect(r, b.x, b.y, b.w, b.h, 14,
                        fade(state     ? shade(colour, hovered ? 1.15f : 1.0f)
                             : hovered ? hex_color(0x34345A)
                                       : hex_color(0x22223E),
                             appear));
      const Color text = state ? hex_color(0x12122A) : kWhite;
      const i32   which = std::max(state, 1) - 1;
      if (skin_element(app.skin, button.skin_images[which])) {
        draw_mod_icon(app, button, state, b.x + b.w / 2, b.y + 38, 50,
                      appear * (state ? 1 : 0.55f));
      } else {
        draw_text(r, app.heavy, button.acronyms[which], b.x + b.w / 2, b.y + 50,
                  32, fade(text, appear), Align::Center);
      }
      draw_text(r, app.regular, button.names[which], b.x + b.w / 2,
                b.y + b.h - 12, 13, fade(text, 0.85f * appear), Align::Center);
      const char key[2] = {button.key, 0};
      draw_text(r, app.bold, key, b.x + 10, b.y + 18, 12,
                fade(text, 0.5f * appear));
    }
  }

  const char* labels[] = {"Reset  (1)", "Close  (Esc)"};
  const Color accents[] = {hex_color(0xCC6633), hex_color(0x6655CC)};
  for (i32 i = 0; i < 2; ++i) {
    Rect b = action_rect(app, i);
    draw_button(app, b.x, b.y + y_shift, b.w, b.h, labels[i],
                contains(app.cursor, b.x, b.y, b.w, b.h) ? 1.0f : 0.0f,
                fade(accents[i], appear));
  }
}

void mod_select_key(App& app, i32 key, i32 action) {
  if (action != GLFW_PRESS)
    return;
  if (key == GLFW_KEY_ESCAPE || key == GLFW_KEY_F1 || key == GLFW_KEY_2) {
    close(app);
    return;
  }
  if (key == GLFW_KEY_1) {
    set_mods(app, 0);
    play_sound(app.audio, Sound::MenuClick, 0.6f);
    return;
  }
  for (const ModRow& row : mod_rows())
    for (const ModButton& button : row.buttons)
      if (key == button.key)  // GLFW letter keys are their capitals
        press(app, button);
}

void mod_select_click(App& app, i32 button, i32 action) {
  if (button != GLFW_MOUSE_BUTTON_LEFT || action != GLFW_PRESS)
    return;
  if (const ModButton* mod = button_by_index(button_at(app, app.cursor))) {
    press(app, *mod);
    return;
  }
  const Rect reset = action_rect(app, 0), done = action_rect(app, 1);
  const Rect panel = panel_rect(app);
  if (contains(app.cursor, reset.x, reset.y, reset.w, reset.h)) {
    set_mods(app, 0);
    play_sound(app.audio, Sound::MenuClick, 0.6f);
  } else if (contains(app.cursor, done.x, done.y, done.w, done.h) ||
             !contains(app.cursor, panel.x, panel.y, panel.w, panel.h)) {
    close(app);
  }
}

}  // namespace circles
