// The settings panel, opened with Ctrl+O as osu! opens its options, anywhere
// but in a map. Two pages, switched with Tab or by their names:
// - General: the gameplay background dim, where 100% hides the background;
//   the cursor's size, as osu!'s multiple of the skin's; whether the game
//   loop is capped at 1000 updates per second; the frame limiter, whether
//   full screen frames wait for the display's refresh or show as soon as
//   drawn, where they can tear; and whether maps' own combo colours replace
//   the skin's.
// - Tablet: whether circles reads the pen itself (also F5); whether its area
//   maps onto the whole display or the window; the area; the tip's pressure
//   that clicks; and importing OpenTabletDriver's area.
#include <circles/app.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <span>

namespace circles {

namespace {

enum Page : i32 { kGeneral, kTablet, kPageCount };
constexpr const char* kPageNames[kPageCount] = {"General", "Tablet"};

// A row sets a number with a slider, a choice with a switch, or one of a few
// named options; or it's a button.
struct RowSpec {
  const char* label;
  i32         page = kGeneral;
  f32 App::* number = nullptr;
  f32        min = 0, max = 1;
  f32 (*max_of)() = nullptr;  // a maximum that depends, instead of max
  f32         step = 0.05f;   // per arrow key
  const char* format = "%.2fx";
  bool App::* choice = nullptr;
  i32 App::*                   option = nullptr;  // an index into names
  std::span<const char* const> names;
  // What follows a change to the row; all a button does.
  void (*changed)(App&) = nullptr;
  bool half = false;  // half a line; half rows come in pairs
};

enum Row : i32 {
  kDimRow,
  kCursorRow,
  kFrameCapRow,
  kFrameLimiterRow,
  kComboColourRow,
  kPenRow,
  kFullScreenRow,
  kAreaWidthRow,
  kAreaHeightRow,
  kAreaXRow,
  kAreaYRow,
  kRotationRow,
  kTipRow,
  kImportRow,
  kRowCount
};

// The area's limits: the connected tablet's surface, or while none is,
// kTablets[0]'s.
const TabletModel& surface_tablet() {
  const TabletModel* connected = connected_tablet();
  return connected ? *connected : kTablets[0];
}

f32 row_max(const RowSpec& spec) {
  return spec.max_of ? spec.max_of() : spec.max;
}

void pen_changed(App& app) {
  set_pen_direct(app, app.pen_direct);
}

void area_changed(App& app) {
  app.tablet_area_set = true;
  update_tablet_mapping(app);
}

void import_area(App& app) {
  const TabletModel* tablet = connected_tablet();
  show_toast(app, import_tablet_settings(app)
                      ? std::string("Imported OpenTabletDriver's area")
                      : std::string("OpenTabletDriver has no area for ") +
                            (tablet ? tablet->name : "a supported tablet"));
}

constexpr RowSpec kRows[kRowCount] = {
    {.label = "Background dim", .number = &App::background_dim},
    // osu!'s range.
    {.label = "Cursor size",
     .number = &App::cursor_size,
     .min = 0.1f,
     .max = 2},
    {.label = "Cap updates at 1000 per second", .choice = &App::frame_cap},
    // All but VSync show frames the moment they're drawn, which can tear.
    {.label = "Frame limiter",
     .option = &App::frame_limiter,
     .names = kFrameLimiterNames,
     .changed = apply_frame_limiter},
    {.label = "Beatmap combo colours", .choice = &App::beatmap_colours},
    {.label = "Read the pen directly (F5)",
     .page = kTablet,
     .choice = &App::pen_direct,
     .changed = pen_changed},
    {.label = "Map to full screen",
     .page = kTablet,
     .choice = &App::tablet_full_screen},
    {.label = "Width",
     .page = kTablet,
     .number = &App::tablet_width,
     .min = 1,
     .max_of =
         [] {
           return surface_tablet().width;
         },
     .step = 1,
     .format = "%.1f mm",
     .changed = area_changed,
     .half = true},
    {.label = "Height",
     .page = kTablet,
     .number = &App::tablet_height,
     .min = 1,
     .max_of =
         [] {
           return surface_tablet().height;
         },
     .step = 1,
     .format = "%.1f mm",
     .changed = area_changed,
     .half = true},
    {.label = "Centre X",
     .page = kTablet,
     .number = &App::tablet_x,
     .max_of =
         [] {
           return surface_tablet().width;
         },
     .step = 1,
     .format = "%.1f mm",
     .changed = area_changed,
     .half = true},
    {.label = "Centre Y",
     .page = kTablet,
     .number = &App::tablet_y,
     .max_of =
         [] {
           return surface_tablet().height;
         },
     .step = 1,
     .format = "%.1f mm",
     .changed = area_changed,
     .half = true},
    {.label = "Rotation",
     .page = kTablet,
     .number = &App::tablet_rotation,
     .max = 360,
     .step = 1,
     .format = "%.0f deg",
     .changed = area_changed,
     .half = true},
    {.label = "Tip pressure",
     .page = kTablet,
     .number = &App::tablet_tip,
     .max = 100,
     .step = 1,
     .format = "%.0f%%",
     .half = true},
    {.label = "Import area from OpenTabletDriver",
     .page = kTablet,
     .changed = import_area},
};

// Sliders are a label over a track; switches and buttons are one line.
constexpr f32 kSliderHeight = 66, kSwitchHeight = 44, kRowGap = 12;
constexpr f32 kRowsTop = 72, kFooter = 56, kPanelWidth = 460, kInset = 16;

f32 row_height(i32 row) {
  return kRows[row].number ? kSliderHeight : kSwitchHeight;
}

struct Box {
  f32 x, y, w, h;
};

// A page's rows, from the panel's top-left, top to bottom with half rows
// in pairs; and the panel's height for it.
struct PageLayout {
  std::array<Box, kRowCount> rows{};
  f32                        height = 0;
};

PageLayout page_layout(i32 page) {
  PageLayout layout;
  const f32  full = kPanelWidth - 2 * kInset, half = (full - kRowGap) / 2;
  f32        top = kRowsTop;
  bool       beside = false;  // the next half row goes beside the last
  for (i32 row = 0; row < kRowCount; ++row) {
    const RowSpec& spec = kRows[row];
    if (spec.page != page)
      continue;
    const f32 x = beside ? kInset + half + kRowGap : kInset;
    layout.rows[row] = {x, top, spec.half ? half : full, row_height(row)};
    beside = spec.half && !beside;
    if (!beside)
      top += row_height(row) + kRowGap;
  }
  layout.height = top - kRowGap + kFooter;
  return layout;
}

struct PanelLayout {
  f32        x, y, w, h;
  PageLayout page;

  Box row(i32 r) const {
    const Box b = page.rows[r];
    return {x + b.x, y + b.y, b.w, b.h};
  }
  // A slider's track, across its row.
  f32 track_x(i32 r) const { return row(r).x + 16; }
  f32 track_w(i32 r) const { return row(r).w - 32; }
  f32 track_y(i32 r) const { return row(r).y + 46; }
};

PanelLayout panel_layout(const App& app) {
  PageLayout page = page_layout(app.settings_menu.page);
  const f32  x = (app.width - kPanelWidth) / 2,
             y = (app.height - page.height) / 2;
  return {x, y, kPanelWidth, page.height, page};
}

// The page names in the header, right-aligned: where each is.
Box tab_box(const App& app, const PanelLayout& p, i32 page) {
  f32 right = p.x + p.w - 32;
  for (i32 i = kPageCount - 1; i > page; --i)
    right -= measure_text(app.bold, kPageNames[i], 17) + 20;
  const f32 w = measure_text(app.bold, kPageNames[page], 17);
  return {right - w, p.y + 30, w, 28};
}

bool on_page(const App& app, i32 row) {
  return kRows[row].page == app.settings_menu.page;
}

// The row under the cursor, -1 for none: a slider's track, or a switch's or
// button's line.
i32 row_at(const App& app) {
  const PanelLayout p = panel_layout(app);
  for (i32 row = 0; row < kRowCount; ++row) {
    if (!on_page(app, row))
      continue;
    const Box b = p.row(row);
    if (kRows[row].number
            ? contains(app.cursor, p.track_x(row) - 10, p.track_y(row) - 16,
                       p.track_w(row) + 20, 32)
            : contains(app.cursor, b.x, b.y, b.w, b.h))
      return row;
  }
  return -1;
}

void show_page(App& app, i32 page) {
  auto& menu = app.settings_menu;
  menu.page = page;
  menu.dragging = false;
  menu.selected = 0;
  while (!on_page(app, menu.selected))
    ++menu.selected;
  play_sound(app.audio, Sound::MenuClick, 0.5f);
}

// In hundredths, so round values like 100% and 1.00x are easy to land on.
void set_number(App& app, i32 row, f32 value) {
  const RowSpec& spec = kRows[row];
  value = std::clamp(std::round(value * 100) / 100, spec.min, row_max(spec));
  if (value == app.*spec.number)
    return;
  app.*spec.number = value;
  if (spec.changed)
    spec.changed(app);
  save_settings(app);
}

void number_from_cursor(App& app, i32 row) {
  const PanelLayout p = panel_layout(app);
  const RowSpec&    spec = kRows[row];
  const f32 t = clamp01((app.cursor.x - p.track_x(row)) / p.track_w(row));
  set_number(app, row, spec.min + t * (row_max(spec) - spec.min));
}

// Flips a switch, steps an option by `step`, or presses a button.
void use_row(App& app, i32 row, i32 step = 1) {
  const RowSpec& spec = kRows[row];
  if (spec.choice) {
    bool& choice = app.*spec.choice;
    choice = !choice;
  }
  if (spec.option) {
    const i32 count = static_cast<i32>(spec.names.size());
    i32&      option = app.*spec.option;
    option = (option + step + count) % count;
  }
  if (spec.changed)
    spec.changed(app);
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
  } else if (key == GLFW_KEY_TAB && press) {
    show_page(app, (menu.page + 1) % kPageCount);
  } else if (key == GLFW_KEY_UP || key == GLFW_KEY_DOWN) {
    const i32 step = key == GLFW_KEY_DOWN ? 1 : -1;
    for (i32 row = menu.selected + step; row >= 0 && row < kRowCount;
         row += step)
      if (on_page(app, row)) {
        menu.selected = row;
        break;
      }
  } else if (spec.number && (key == GLFW_KEY_LEFT || key == GLFW_KEY_RIGHT)) {
    set_number(
        app, menu.selected,
        app.*spec.number + (key == GLFW_KEY_RIGHT ? spec.step : -spec.step));
  } else if (!spec.number && press &&
             (key == GLFW_KEY_ENTER || key == GLFW_KEY_SPACE ||
              ((spec.choice || spec.option) &&
               (key == GLFW_KEY_LEFT || key == GLFW_KEY_RIGHT)))) {
    use_row(app, menu.selected, key == GLFW_KEY_LEFT ? -1 : 1);
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
  for (i32 page = 0; page < kPageCount; ++page) {
    const Box tab = tab_box(app, p, page);
    if (page != menu.page && contains(app.cursor, tab.x, tab.y, tab.w, tab.h)) {
      show_page(app, page);
      return;
    }
  }
  const i32 row = row_at(app);
  if (row >= 0) {
    menu.selected = row;
    if (kRows[row].number) {
      menu.dragging = true;
      number_from_cursor(app, row);
    } else {
      use_row(app, row);
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
  for (i32 page = 0; page < kPageCount; ++page) {
    const Box tab = tab_box(app, p, page);
    draw_text(r, app.bold, kPageNames[page], tab.x, p.y + lift + 50, 17,
              fade(kWhite, (page == menu.page ? 1.0f : 0.45f) * appear));
  }

  const i32 hovered = row_at(app);
  for (i32 row = 0; row < kRowCount; ++row) {
    if (!on_page(app, row))
      continue;
    const RowSpec& spec = kRows[row];
    Box            box = p.row(row);
    box.y += lift;
    // The row the keys change.
    if (row == menu.selected)
      draw_rounded_rect(r, box.x, box.y, box.w, box.h, 10,
                        fade(kWhite, 0.06f * appear));

    if (spec.number) {
      const f32 value = app.*spec.number;
      draw_text(r, app.bold, spec.label, box.x + 16, box.y + 22, 17,
                fade(kWhite, appear));
      char text[32];
      if (row == kDimRow && value >= 1)
        snprintf(text, sizeof(text), "100%%  (hidden)");
      else if (row == kDimRow)
        snprintf(text, sizeof(text), "%.0f%%", value * 100);
      else
        snprintf(text, sizeof(text), spec.format, value);
      draw_text(r, app.bold, text, box.x + box.w - 16, box.y + 22, 17,
                fade(kWhite, 0.85f * appear), Align::Right);
      const f32 track_x = p.track_x(row), track_w = p.track_w(row);
      const f32 track_y = p.track_y(row) + lift;
      const f32 hover =
          (menu.dragging && row == menu.selected) || hovered == row ? 1.0f
                                                                    : 0.0f;
      draw_rounded_rect(r, track_x, track_y - 3, track_w, 6, 3,
                        fade(kWhite, 0.2f * appear));
      const f32 fill =
          track_w * (value - spec.min) / (row_max(spec) - spec.min);
      draw_rounded_rect(r, track_x, track_y - 3, fill, 6, 3,
                        fade(hex_color(0x8844CC), appear));
      draw_circle(r, track_x + fill, track_y, 9 + 2 * hover,
                  fade(kWhite, appear));
      continue;
    }

    const f32 centre = box.y + box.h / 2;
    if (spec.option) {
      const f32 baseline = centered_baseline(app.bold, 17, centre);
      draw_text(r, app.bold, spec.label, box.x + 16, baseline, 17,
                fade(kWhite, appear));
      draw_text(r, app.bold, spec.names[app.*spec.option], box.x + box.w - 16,
                baseline, 17, fade(kWhite, 0.85f * appear), Align::Right);
      continue;
    }
    if (!spec.choice) {
      // A button.
      draw_rounded_rect(
          r, box.x, box.y, box.w, box.h, 10,
          fade(hex_color(0x8844CC), (hovered == row ? 0.55f : 0.35f) * appear));
      draw_text(r, app.bold, spec.label, box.x + box.w / 2,
                centered_baseline(app.bold, 17, centre), 17,
                fade(kWhite, appear), Align::Center);
      continue;
    }
    const bool on = app.*spec.choice;
    draw_text(r, app.bold, spec.label, box.x + 16,
              centered_baseline(app.bold, 17, centre), 17,
              fade(kWhite, appear));
    // An on/off switch.
    constexpr f32 kSwitchW = 46, kSwitchH = 26;
    const f32     switch_x = box.x + box.w - 16 - kSwitchW;
    draw_rounded_rect(
        r, switch_x, centre - kSwitchH / 2, kSwitchW, kSwitchH, kSwitchH / 2,
        fade(on ? hex_color(0x8844CC) : hex_color(0x3A3A55), appear));
    const f32 knob_x =
        on ? switch_x + kSwitchW - kSwitchH / 2 : switch_x + kSwitchH / 2;
    draw_circle(r, knob_x, centre, kSwitchH / 2 - 3, fade(kWhite, appear));
  }

  draw_text(r, app.regular,
            "Up / Down choose  ·  Left / Right change  ·  Tab page  ·  Esc "
            "closes",
            p.x + 32, p.y + lift + p.h - 22, 13, fade(kWhite, 0.5f * appear));
}

}  // namespace circles
