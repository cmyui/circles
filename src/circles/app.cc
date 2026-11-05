#include <circles/app.h>
#include <circles/archive.h>
#include <circles/strings.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <utility>

namespace circles {

namespace {

constexpr const char* kFontPath = "/System/Library/Fonts/Avenir Next.ttc";
// Faces within Avenir Next.ttc.
constexpr i32         kFaceDemiBold = 2;
constexpr i32         kFaceMedium = 5;
constexpr i32         kFaceHeavy = 8;

constexpr const char* kBeatmapDirectory = "beatmaps";
constexpr const char* kSkinDirectory = "skins";
constexpr const char* kReplayDirectory = "replays";
constexpr const char* kSettingsPath = "settings.txt";

constexpr f64 kTransitionHalf = 0.18;  // seconds to fade out, then back in
constexpr f64 kBackdropFade = 0.4;
constexpr f64 kTrailLength = 0.12;
constexpr f32 kVolumeStep = 0.05f;  // per scroll wheel notch
constexpr f32 kTau = 6.2831853f;

// The frame limiter's settings, as settings.txt names them, and frames per
// refresh without vsync for each.
constexpr const char* kFrameLimiters[] = {"vsync", "2x", "4x", "8x",
                                          "unlimited"};
constexpr i32         kFramesPerRefresh[] = {0, 2, 4, 8, 0};

// Saved preferences, as written by save_settings.
struct Settings {
  f64         audio_offset = 0;
  f32         volume = 1;
  std::string skin;  // folder name in skins/; empty for the built-in look
  bool        mouse_buttons = true;
  Mods        mods = 0;
  f32         background_dim = 0.8f;
  bool        frame_cap = true;
  i32         frame_limiter = 0;
  f32         cursor_size = 1;
  bool        beatmap_colours = true;
  i32         readout = 1;  // Readout::mode
  bool        pen_direct = false;
  bool        tablet_full_screen = true;
  TabletArea  tablet_area;
  bool        tablet_area_set = false;
  f32         tablet_tip = 1;
  std::array<TabletProfile, 2> tablet_profiles;
};

Settings load_settings() {
  Settings      settings;
  std::ifstream file(kSettingsPath);
  std::string   line;
  while (std::getline(file, line)) {
    const size_t equals = line.find('=');
    if (equals == std::string::npos)
      continue;
    const std::string_view key(line.data(), equals);
    const char*            value = line.c_str() + equals + 1;
    if (key == "audio_offset")
      settings.audio_offset = std::strtod(value, nullptr);
    else if (key == "volume")
      settings.volume = static_cast<f32>(std::strtod(value, nullptr));
    else if (key == "skin")
      settings.skin = value;
    else if (key == "mouse_buttons")
      settings.mouse_buttons = std::strtol(value, nullptr, 10) != 0;
    else if (key == "mods")
      settings.mods = static_cast<Mods>(std::strtoul(value, nullptr, 10));
    else if (key == "frame_cap")
      settings.frame_cap = std::strtol(value, nullptr, 10) != 0;
    else if (key == "frame_limiter") {
      for (i32 i = 0; i < static_cast<i32>(std::size(kFrameLimiters)); ++i)
        if (!std::strcmp(value, kFrameLimiters[i]))
          settings.frame_limiter = i;
    }
    else if (key == "background_dim")
      settings.background_dim =
          std::clamp(static_cast<f32>(std::strtod(value, nullptr)), 0.0f, 1.0f);
    else if (key == "cursor_size")
      settings.cursor_size =
          std::clamp(static_cast<f32>(std::strtod(value, nullptr)), 0.1f, 2.0f);
    else if (key == "beatmap_colours")
      settings.beatmap_colours = std::strtol(value, nullptr, 10) != 0;
    else if (key == "pen_direct")
      settings.pen_direct = std::strtol(value, nullptr, 10) != 0;
    else if (key == "tablet_full_screen")
      settings.tablet_full_screen = std::strtol(value, nullptr, 10) != 0;
    else if (key == "tablet_tip")
      settings.tablet_tip = std::clamp(
          static_cast<f32>(std::strtod(value, nullptr)), 0.0f, 100.0f);
    else if (key == "tablet_area") {
      TabletArea& area = settings.tablet_area;
      settings.tablet_area_set =
          std::sscanf(value, "%f %f %f %f %f", &area.x, &area.y, &area.width,
                      &area.height, &area.rotation) == 5 &&
          area.width > 0 && area.height > 0;
      if (!settings.tablet_area_set)
        area = {};
    }
    else if (key.starts_with("tablet_profile_")) {
      // tablet_profile_N=x y width height rotation name, N from 1.
      const size_t  slot = std::strtoul(line.c_str() + 15, nullptr, 10) - 1;
      TabletProfile profile;
      TabletArea&   area = profile.area;
      i32           name = 0;
      if (slot < settings.tablet_profiles.size() &&
          std::sscanf(value, "%f %f %f %f %f %n", &area.x, &area.y,
                      &area.width, &area.height, &area.rotation, &name) == 5 &&
          area.width > 0 && area.height > 0 && value[name]) {
        profile.name = value + name;
        settings.tablet_profiles[slot] = std::move(profile);
      }
    }
    else if (key == "readout")
      settings.readout =
          std::clamp(static_cast<i32>(std::strtol(value, nullptr, 10)), 0, 2);
  }
  return settings;
}

i32 skin_index_of(const App& app, std::string_view name) {
  const auto found = std::find(app.skins.begin(), app.skins.end(), name);
  return found == app.skins.end() ? -1
                                  : static_cast<i32>(found - app.skins.begin());
}

// Characters some file systems reject become underscores.
std::string file_name_safe(std::string text) {
  for (char& c : text)
    if (std::string_view("/\\:*?\"<>|").find(c) != std::string_view::npos ||
        static_cast<unsigned char>(c) < ' ')
      c = '_';
  return text;
}

// Imports any new .osk files and lists the skins again. Returns the name of
// the last skin imported, if any.
std::string refresh_skins(App& app) {
  const auto        imported = import_archives(kSkinDirectory, ".osk");
  const std::string current =
      app.skin_index >= 0 ? app.skins[app.skin_index] : std::string();
  app.skins = list_skins(kSkinDirectory);
  app.skin_index = skin_index_of(app, current);
  return imported.empty() ? std::string() : imported.back().filename().string();
}

// F9 steps through the built-in look and each skin, picking up skins added
// while the game runs.
void cycle_skin(App& app) {
  refresh_skins(app);
  const i32 next = app.skin_index + 1;
  use_skin(app, next < static_cast<i32>(app.skins.size()) ? next : -1);
  show_toast(app, "Skin: " + (app.skin_index < 0 ? std::string("built-in")
                                                 : app.skin.config.name));
}

void change_volume(App& app, f64 notches) {
  set_master_volume(app.audio, app.audio.master_volume +
                                   static_cast<f32>(notches) * kVolumeStep);
  save_settings(app);
  show_toast(app,
             "Volume " +
                 std::to_string(std::lround(app.audio.master_volume * 100)) +
                 "%");
}

bool accepting_input(const App& app) {
  // Input waits while the old screen fades out.
  return app.transition_started < 0 || app.screen == app.next_screen;
}

void enter_screen(App& app, Screen screen) {
  // Slider bodies keep their textures only while they're drawn.
  if (screen != Screen::Gameplay)
    release_slider_bodies(app.game.bodies);
  app.screen = screen;
  switch (screen) {
    case Screen::MainMenu:
      menu_enter(app);
      break;
    case Screen::SongSelect:
      select_enter(app);
      break;
    case Screen::Gameplay:
      if (!gameplay_start(app, app.selected_map,
                          std::exchange(app.replay_to_watch, {}))) {
        show_toast(app, "Couldn't load that beatmap");
        app.screen = Screen::SongSelect;
        select_enter(app);
      }
      break;
    case Screen::Results:
      results_enter(app);
      break;
  }
}

void start_backdrop_load(Backdrop& backdrop) {
  backdrop.loading_path = backdrop.path;
  backdrop.loading = std::async(std::launch::async, [path = backdrop.path] {
    return load_image(path);
  });
}

// Only one image loads at a time: replacing an in-flight std::async future
// would block until it finished.
void request_backdrop(App& app, const std::string& path) {
  auto& backdrop = app.backdrop;
  if (path == backdrop.path)
    return;
  backdrop.path = path;
  if (path.empty()) {
    destroy_texture(backdrop.previous);
    backdrop.previous = backdrop.current;
    backdrop.current = {};
    backdrop.changed = app.now;
  } else if (!backdrop.loading.valid()) {
    start_backdrop_load(backdrop);
  }
}

void update_backdrop(App& app) {
  auto& backdrop = app.backdrop;
  if (backdrop.loading.valid() &&
      backdrop.loading.wait_for(std::chrono::seconds(0)) ==
          std::future_status::ready) {
    const Image image = backdrop.loading.get();
    if (backdrop.loading_path == backdrop.path) {
      destroy_texture(backdrop.previous);
      backdrop.previous = backdrop.current;
      backdrop.current = upload_texture(image);
      backdrop.changed = app.now;
    } else if (!backdrop.path.empty()) {
      start_backdrop_load(backdrop);
    }
  }
  if (backdrop.previous.id && app.now - backdrop.changed > kBackdropFade)
    destroy_texture(backdrop.previous);
}

void draw_skin_cursor(App&                          app,
                      const SkinImage&              cursor,
                      Vec2                          position,
                      const std::deque<TrailPoint>& trail,
                      bool                          pressed) {
  auto&       r = app.renderer;
  const auto& config = app.skin.config;
  // The cursor size setting scales the cursor, its middle and its trail, as
  // in osu!.
  const f32   scale = ui_scale(app) * app.cursor_size;
  if (const auto* trail_image = skin_element(app.skin, "cursortrail")) {
    for (const auto& point : trail) {
      const f32 age = static_cast<f32>((app.now - point.time) / kTrailLength);
      draw_skin_image(r, trail_image->front(), point.position.x,
                      point.position.y, scale, fade(kWhite, 0.6f * (1 - age)));
    }
  }
  const f32 cursor_scale = scale * (pressed && config.cursor_expand ? 1.3f : 1);
  // Cursors not marked as centred hang from their top-left corner.
  Vec2      at = position;
  if (!config.cursor_centre)
    at = at + Vec2{cursor.width(), cursor.height()} * (cursor_scale / 2);
  const f32 angle =
      config.cursor_rotate ? static_cast<f32>(app.now) * kTau / 10 : 0;
  draw_skin_image(r, cursor, at.x, at.y, cursor_scale, kWhite, angle);
  if (const auto* middle = skin_element(app.skin, "cursormiddle"))
    draw_skin_image(r, middle->front(), at.x, at.y, scale, kWhite);
}

void draw_cursor(App& app) {
  auto&       r = app.renderer;
  const auto& game = app.game;
  bool        pressed = false;
  if (app.screen == Screen::Gameplay)
    for (bool held : game.held)
      pressed |= held;
  // A replay being watched shows its cursor, until a pause needs the mouse.
  // Autopilot's cursor moves by itself; Cinema shows none.
  const bool replay = app.screen == Screen::Gameplay &&
                      (game.watching || game.mods & kAutopilot) &&
                      game.phase == GamePhase::Playing;
  if (replay && game.mods & kCinema)
    return;
  const Vec2  position = replay ? game.cursor : app.cursor;
  const auto& trail = replay ? game.replay_trail : app.trail;
  if (const auto* cursor = skin_element(app.skin, "cursor")) {
    draw_skin_cursor(app, cursor->front(), position, trail, pressed);
    return;
  }
  const f32 size = app.height * 0.022f * app.cursor_size * (pressed ? 1.2f : 1);
  const Color warm = hex_color(0xFFC860);
  for (const auto& point : trail) {
    const f32 age = static_cast<f32>((app.now - point.time) / kTrailLength);
    draw_circle(r, point.position.x, point.position.y,
                size * 0.55f * (1 - age * 0.5f), fade(warm, 0.25f * (1 - age)));
  }
  draw_circle(r, position.x, position.y, size * 1.7f, fade(warm, 0.25f), 0, 1);
  draw_circle(r, position.x, position.y, size, fade(kWhite, 0.95f), 0.72f);
  draw_circle(r, position.x, position.y, size * 0.3f, warm);
}

constexpr const char* kProfileDirectory = "perf";

void toggle_profiling(App& app) {
  auto& profiler = app.profiler;
  if (!profiler.active) {
    profiler_start(profiler, true);
    show_toast(app, "Profiling; F8 again saves it");
    return;
  }
  const std::string path = profiler_stop(profiler, kProfileDirectory);
  show_toast(app, path.empty() ? "Couldn't save the profile" : "Saved " + path);
}

f32 percentile(std::vector<f32> values, f32 fraction) {
  if (values.empty())
    return 0;
  const auto nth = values.begin() +
                   static_cast<std::ptrdiff_t>(fraction * (values.size() - 1));
  std::nth_element(values.begin(), nth, values.end());
  return *nth;
}

// The last second's frame phases and input, and a graph of recent frames.
void draw_profiler_overlay(App& app) {
  auto&                         r = app.renderer;
  const auto&                   profiler = app.profiler;
  const auto&                   frames = profiler.frames;
  const f64                     now = uptime();
  constexpr Color               kPhaseColours[] = {{0.6f, 0.6f, 0.6f, 1},
                                                   {1, 0.6f, 0.2f, 1},
                                                   {0.3f, 0.6f, 1, 1},
                                                   {1, 0.3f, 0.3f, 1},
                                                   {0.25f, 0.25f, 0.3f, 1}};
  std::array<f64, kFramePhases> sums{};
  f64                           gpu = 0;
  i32                           count = 0, gpu_count = 0, drawn = 0;
  u64                           draws = 0, vertices = 0;
  std::vector<f32>              frame_ms, shown_ms;
  for (size_t i = frames.size(); i-- > 0 && now - frames[i].start < 1;) {
    f32 total = 0;
    for (i32 p = 0; p < kFramePhases; ++p) {
      sums[p] += frames[i].phase_ms[p];
      total += frames[i].phase_ms[p];
    }
    frame_ms.push_back(total);
    if (frames[i].gpu_ms >= 0) {
      gpu += frames[i].gpu_ms;
      ++gpu_count;
    }
    if (frames[i].shown_ms >= 0)
      shown_ms.push_back(frames[i].shown_ms);
    drawn += frames[i].draw_calls > 0;
    draws += frames[i].draw_calls;
    vertices += frames[i].vertices;
    ++count;
  }
  std::array<i32, 3> device_events{};
  std::vector<f32>   delays;
  for (size_t i = profiler.inputs.size();
       i-- > 0 && now - profiler.inputs[i].received < 1;) {
    ++device_events[static_cast<i32>(profiler.inputs[i].device)];
    delays.push_back(static_cast<f32>(
        (profiler.inputs[i].received - profiler.inputs[i].event_time) * 1000));
  }

  const f32 x = 12, y = 56, w = 420, graph_h = 60;
  draw_rounded_rect(r, x, y, w, 120 + graph_h, 10, fade(kBlack, 0.75f));
  char      line[256];
  const f32 n = static_cast<f32>(std::max(count, 1));
  snprintf(line, sizeof(line),
           "Profiling (F8 saves)   %d updates/s  %d drawn/s   loop p50 %.2f  "
           "p99 %.2f ms",
           count, drawn, percentile(frame_ms, 0.5f),
           percentile(frame_ms, 0.99f));
  draw_text(r, app.regular, line, x + 10, y + 20, 12, kWhite);
  snprintf(line, sizeof(line),
           "poll %.3f  update %.3f  draw %.3f  present %.3f  sleep %.3f ms",
           sums[0] / n, sums[1] / n, sums[2] / n, sums[3] / n, sums[4] / n);
  draw_text(r, app.regular, line, x + 10, y + 40, 12, kWhite);
  snprintf(line, sizeof(line),
           "gpu %.3f ms   %.0f draws   %.0f vertices   shown after %.2f ms",
           gpu_count ? gpu / gpu_count : -1.0, draws / n, vertices / n,
           shown_ms.empty() ? -1.0f : percentile(shown_ms, 0.5f));
  draw_text(r, app.regular, line, x + 10, y + 60, 12, kWhite);
  snprintf(
      line, sizeof(line),
      "input/s: keys %d  mouse %d  tablet %d   delay p50 %.2f  p99 %.2f ms",
      device_events[0], device_events[1], device_events[2],
      percentile(delays, 0.5f), percentile(delays, 0.99f));
  draw_text(r, app.regular, line, x + 10, y + 80, 12, kWhite);

  // Each recent frame's phases, stacked; 4 ms fills the graph.
  const f32    graph_y = y + 100, bar = 4, scale = graph_h / 4;
  const size_t shown =
      std::min<size_t>(frames.size(), static_cast<size_t>((w - 20) / bar));
  for (size_t k = 0; k < shown; ++k) {
    const FrameRecord& f = frames[frames.size() - shown + k];
    f32                top = graph_y + graph_h;
    for (i32 p = 0; p < kFramePhases; ++p) {
      const f32 h = std::min(f.phase_ms[p] * scale, top - graph_y);
      top -= h;
      draw_rect(r, x + 10 + k * bar, top, bar, h, kPhaseColours[p]);
    }
    if (f.gpu_ms >= 0)
      draw_rect(r, x + 10 + k * bar,
                graph_y + graph_h - std::min(f.gpu_ms * scale, graph_h), bar,
                1.5f, kWhite);
  }
  constexpr const char* kPhaseNames[] = {"poll", "update", "draw", "present",
                                         "sleep"};
  f32                   legend_x = x + 10;
  for (i32 p = 0; p < kFramePhases; ++p) {
    draw_rect(r, legend_x, graph_y + graph_h + 8, 8, 8, kPhaseColours[p]);
    draw_text(r, app.regular, kPhaseNames[p], legend_x + 12,
              graph_y + graph_h + 16, 11, fade(kWhite, 0.8f));
    legend_x += 62;
  }
  draw_text(r, app.regular, "— gpu", legend_x, graph_y + graph_h + 16, 11,
            fade(kWhite, 0.8f));
}

// Imports the dropped archives into beatmaps/ and selects the first new map.
// Rescanning renumbers the library's maps, so it waits until no screen holds
// one: the main menu or song select.
void import_dropped(App& app) {
  namespace fs = std::filesystem;
  std::error_code error;
  for (const auto& path : app.dropped) {
    const fs::path source(path);
    const fs::path destination =
        fs::path(kBeatmapDirectory) / source.filename();
    if (!fs::equivalent(source, destination, error))
      fs::copy_file(source, destination, fs::copy_options::skip_existing,
                    error);
  }
  app.dropped.clear();
  const auto imported = import_archives(kBeatmapDirectory, ".osz");
  if (imported.empty()) {
    show_toast(app, "Already imported, or not a valid .osz");
    return;
  }
  const std::string previous =
      app.selected_map >= 0 ? app.library.maps[app.selected_map].md5 : "";
  app.library = scan_library(kBeatmapDirectory);
  i32 select = -1, kept = -1;
  for (size_t m = 0; m < app.library.maps.size(); ++m) {
    const auto& info = app.library.maps[m];
    if (select < 0 && fs::path(info.osu_path).parent_path() == imported.front())
      select = static_cast<i32>(m);
    if (info.md5 == previous)
      kept = static_cast<i32>(m);
  }
  char text[64];
  snprintf(text, sizeof(text), "Imported %zu beatmap set%s", imported.size(),
           imported.size() == 1 ? "" : "s");
  show_toast(app,
             select >= 0 ? text : "Imported, but it has no osu!standard maps");
  // The old selection, renumbered, so selecting the new map compares songs
  // and starts its preview.
  app.selected_map = kept;
  if (select >= 0)
    select_map(app, select, true);
  if (app.screen == Screen::SongSelect)
    select_enter(app);
}

// Whether the pen, read directly, is aiming: while it's in range, its
// driver's cursor, which the same pen moves, is ignored.
bool pen_aiming(const App& app) {
  return app.pen_direct && app.pen.in_range && uptime() - app.pen.last < 0.1;
}

void mouse_button(App& app, i32 button, i32 action, f64 time) {
  if (app.settings_menu.open) {
    settings_click(app, button, action);
    return;
  }
  if (!accepting_input(app))
    return;
  switch (app.screen) {
    case Screen::MainMenu:
      menu_click(app, button, action);
      break;
    case Screen::SongSelect:
      select_click(app, button, action);
      break;
    case Screen::Gameplay:
      gameplay_click(app, button, action, time);
      break;
    case Screen::Results:
      results_click(app, button, action);
      break;
  }
}

void set_cursor(App& app, f64 x, f64 y) {
  app.cursor = {static_cast<f32>(x), static_cast<f32>(y)};
  app.cursor_moved = app.now;
  app.trail.push_back({glfwGetTime(), app.cursor});
  settings_cursor(app);
}

// Aims and clicks with the pen's reports since the last pass, each at its
// own time. The tip is the left button, as OpenTabletDriver binds it.
void read_pen(App& app) {
  auto& pen = app.pen;
  if (!app.pen_direct)
    return;
  // A tablet connected whose surface the area goes past: said once.
  const TabletModel* tablet = connected_tablet();
  if (tablet != pen.tablet) {
    pen.tablet = tablet;
    const std::string warning = tablet_area_warning(app);
    if (!warning.empty())
      show_toast(app, "Pen: " + warning, kWarningSeconds);
  }
  pen.reports.clear();
  take_pen_reports(pen.reports);
  if (pen.reports.empty())
    return;
  const f64 now = uptime();
  const f64 to_glfw = glfwGetTime() - now;
  // Where the area maps, in the window's points.
  Vec2 origin{0, 0}, size{app.width, app.height};
  if (app.tablet_full_screen)
    window_screen(app, origin, size);
  for (const PenReport& report : pen.reports) {
    pen.last = report.arrived;
    pen.in_range = report.in_range;
    if (report.in_range) {
      const Vec2 across = map_pen(pen.mapping, report.x, report.y);
      set_cursor(app,
                 std::clamp(origin.x + across.x * size.x, 0.0f, app.width),
                 std::clamp(origin.y + across.y * size.y, 0.0f, app.height));
      app.readout.pen_moves.push_back({report.arrived, now});
    }
    const bool tip = report.in_range && report.pressure > app.tablet_tip;
    if (tip != pen.tip) {
      pen.tip = tip;
      mouse_button(app, GLFW_MOUSE_BUTTON_LEFT,
                   tip ? GLFW_PRESS : GLFW_RELEASE, report.arrived + to_glfw);
    }
  }
}

// How many recent samples the readout's medians cover.
constexpr size_t kReadoutSamples = 30;
// Input read this long after it happened, or a frame drawn this long after
// it was read, met a stalled game loop, as while a map loads: it says
// nothing about the usual delay.
constexpr f64    kStall = 0.1;

// A figure with no sample this recent says nothing about now: it reads "-".
constexpr f64 kFresh = 3;

void add_sample(LatencyStat& stat, f64 seconds) {
  stat.samples.push_back(static_cast<f32>(seconds * 1000));
  if (stat.samples.size() > kReadoutSamples)
    stat.samples.pop_front();
  stat.last = uptime();
}

void refresh(LatencyStat& stat, f64 now) {
  if (now - stat.last > kFresh)
    stat.samples.clear();
  stat.ms = stat.samples.empty()
                ? -1
                : percentile({stat.samples.begin(), stat.samples.end()}, 0.5f);
}

// The driver's time on each cursor movement: from the newest pointing
// device report before it to the movement's own timestamp. A mouse's
// movement is stamped with its report's arrival, so it reads about 0; a
// tablet driver such as OpenTabletDriver makes its own, later. Movements are
// timed once 20 ms old, when every report before them has been taken.
void time_driver(Readout& readout, f64 now) {
  readout.new_reports.clear();
  take_pointer_reports(readout.new_reports);
  std::sort(readout.new_reports.begin(), readout.new_reports.end());
  readout.pointer_reports.insert(readout.pointer_reports.end(),
                                 readout.new_reports.begin(),
                                 readout.new_reports.end());
  auto& reports = readout.pointer_reports;
  while (!readout.driver_moves.empty() &&
         now - readout.driver_moves.front().received > 0.02) {
    // Stamps of the same moment can differ by rounding.
    const f64 stamped = readout.driver_moves.front().event_time + 0.0001;
    readout.driver_moves.pop_front();
    while (reports.size() > 1 && reports[1] <= stamped)
      reports.pop_front();
    if (!reports.empty() && reports.front() <= stamped &&
        stamped - reports.front() < 0.05)
      add_sample(readout.driver,
                 std::max(0.0, stamped - 0.0001 - reports.front()));
  }
  while (reports.size() > 1 && now - reports.front() > 1)
    reports.pop_front();
}

// Takes the readout's samples. From the input monitor: how long presses and
// cursor movements took to be read, and how often movements come. From the
// GPU's reports: for each frame drawn, how long input read before its
// drawing started waited for it; for frames shown (full screen), how long
// each took from drawing to the display, and which presses and movements it
// was the first to show. From the audio: how long sounds waited for their
// first cycle.
// The GPU's reports go to the profiler too.
void take_samples(App& app) {
  auto&     readout = app.readout;
  const f64 now = uptime();
  readout.new_presses.clear();
  take_input_presses(readout.new_presses);
  for (const InputTiming& press : readout.new_presses)
    if (press.received - press.event_time <= kStall) {
      add_sample(readout.key_read, press.received - press.event_time);
      readout.presses.push_back(press);
      readout.waiting_presses.push_back(press);
    }
  readout.new_moves.clear();
  take_input_moves(readout.new_moves);
  if (pen_aiming(app))
    readout.new_moves.clear();
  // The driver's movements, then the pen's own, which no driver made.
  const size_t from_driver = readout.new_moves.size();
  readout.new_moves.insert(readout.new_moves.end(), readout.pen_moves.begin(),
                           readout.pen_moves.end());
  readout.pen_moves.clear();
  for (size_t i = 0; i < readout.new_moves.size(); ++i) {
    const InputTiming& move = readout.new_moves[i];
    if (move.received - move.event_time > kStall)
      continue;
    add_sample(readout.aim_read, move.received - move.event_time);
    // Between movements while the cursor keeps moving.
    if (readout.last_move >= 0 && move.event_time > readout.last_move &&
        move.event_time - readout.last_move < 0.05)
      add_sample(readout.aim_interval, move.event_time - readout.last_move);
    readout.last_move = std::max(readout.last_move, move.event_time);
    readout.moves.push_back(move);
    readout.waiting_moves.push_back(move);
    if (i < from_driver && readout.pointer_access == PointerAccess::Listening)
      readout.driver_moves.push_back(move);
  }
  time_driver(readout, now);

  readout.reports.clear();
  gpu_take_frame_times(readout.reports);
  profile_gpu_times(app.profiler, readout.reports);
  for (const GpuFrameTime& report : readout.reports) {
    if (report.shown < 0) {
      // A frame drawn, windowed or full screen: input read before its drawing
      // started waited for it. A moving cursor's wait is its newest
      // movement's.
      while (!readout.waiting_presses.empty() &&
             readout.waiting_presses.front().received <= report.drawn) {
        const f64 wait =
            report.drawn - readout.waiting_presses.front().received;
        if (wait <= kStall)
          add_sample(readout.frame_wait, wait);
        readout.waiting_presses.pop_front();
      }
      f64 read = -1;
      while (!readout.waiting_moves.empty() &&
             readout.waiting_moves.front().received <= report.drawn) {
        read = readout.waiting_moves.front().received;
        readout.waiting_moves.pop_front();
      }
      if (read >= 0 && report.drawn - read <= kStall)
        add_sample(readout.aim_wait, report.drawn - read);
      continue;
    }
    ++readout.shown;
    add_sample(readout.display, report.shown - report.drawn);
    while (!readout.presses.empty() &&
           readout.presses.front().received <= report.drawn) {
      const InputTiming& press = readout.presses.front();
      if (report.drawn - press.received <= kStall)
        add_sample(readout.key_screen, report.shown - press.event_time);
      readout.presses.pop_front();
    }
    // The newest movement drawn into it, if the cursor was moving.
    f64 newest = -1, read = -1;
    while (!readout.moves.empty() &&
           readout.moves.front().received <= report.drawn) {
      newest = readout.moves.front().event_time;
      read = readout.moves.front().received;
      readout.moves.pop_front();
    }
    if (newest >= 0 && report.drawn - read <= kStall)
      add_sample(readout.aim_screen, report.shown - newest);
  }
  // Windowed, no frame reports being shown; minimized, none drawn.
  for (auto* pending : {&readout.presses, &readout.moves,
                        &readout.waiting_presses, &readout.waiting_moves})
    while (!pending->empty() && now - pending->front().received > 1)
      pending->pop_front();

  readout.waits.clear();
  sound_waits(app.audio, readout.waits);
  for (f64 wait : readout.waits)
    add_sample(readout.mixer, wait / 1000);
}

// The readout's medians, refreshed with the frame rates.
void refresh_readout(App& app, f64 span) {
  auto&     readout = app.readout;
  const f64 now = uptime();
  readout.shown_rate = static_cast<i32>(std::lround(readout.shown / span));
  readout.shown = 0;
  // Listening needs a permission the stage panel asks for; it can be
  // granted while the game runs.
  if (readout.mode == 2 && readout.pointer_access != PointerAccess::Listening)
    readout.pointer_access = listen_to_pointers();
  for (LatencyStat* stat :
       {&readout.key_read, &readout.driver, &readout.aim_interval,
        &readout.aim_read, &readout.frame_wait, &readout.aim_wait,
        &readout.display, &readout.mixer, &readout.key_screen,
        &readout.aim_screen})
    refresh(*stat, now);
}

// "12.3", or "-" when not known. Under 1 ms, to hundredths: the input stages
// are that small, and the clocks resolve them.
std::string ms_text(f32 ms) {
  char text[16] = "-";
  if (ms >= 0)
    snprintf(text, sizeof(text), ms < 1 ? "%.2f" : "%.1f", ms);
  return text;
}

// "12.3 ms", or "-" when not known.
std::string with_ms(std::string text) {
  return text == "-" ? text : text + " ms";
}

// Bottom right: rates and each path's total on a line, or (mode 2) a table
// of each path's stages, on a dark backing. Words are dim, numbers bright.
void draw_readout(App& app) {
  const auto& readout = app.readout;
  if (readout.mode == 0)
    return;
  // Full screen, frames shown; windowed, macOS doesn't report what the
  // compositor shows, so frames drawn, and nothing about the screen.
  const bool full = readout.shown_rate > 0;
  const auto screen = [&](f32 ms) {
    return ms_text(full ? ms : -1);
  };
  // A press's hitsound: read, then waiting for the mixer, then the output
  // device's delay.
  const f32 device =
      app.audio.ready ? static_cast<f32>(app.audio.output_latency) : -1;
  const f32 key_sound =
      readout.key_read.ms >= 0 && readout.mixer.ms >= 0 && device >= 0
          ? readout.key_read.ms + readout.mixer.ms + device
          : -1;
  // The aiming hand's whole path: the driver's time, when known, then the
  // movement's from its timestamp to the frame showing it.
  const f32 aim_screen =
      readout.aim_screen.ms >= 0
          ? readout.aim_screen.ms + std::max(readout.driver.ms, 0.0f)
          : -1;

  // Each line alternates words and numbers, starting with words.
  using Line = std::vector<std::string>;
  std::vector<Line>                       lines;
  std::vector<std::array<std::string, 6>> table;
  char                                    rate[16], updates[16];
  snprintf(rate, sizeof(rate), "%d", full ? readout.shown_rate : app.fps);
  snprintf(updates, sizeof(updates), "%d", app.update_rate);
  Line rates = {"", rate, full ? " fps   " : " drawn/s   ", updates,
                " updates/s"};
  if (readout.mode == 1) {
    rates.insert(rates.end(),
                 {"   key to screen ", with_ms(screen(readout.key_screen.ms)),
                  "   aim to screen ", with_ms(screen(aim_screen)),
                  "   key to sound ", with_ms(ms_text(key_sound))});
    lines.push_back(rates);
  } else {
    table = {
        {"", "driver", "read", "wait", "output", "total"},
        {"key to screen", "-", ms_text(readout.key_read.ms),
         ms_text(readout.frame_wait.ms), screen(readout.display.ms),
         with_ms(screen(readout.key_screen.ms))},
        {"aim to screen", ms_text(readout.driver.ms),
         ms_text(readout.aim_read.ms), ms_text(readout.aim_wait.ms),
         screen(readout.display.ms), with_ms(screen(aim_screen))},
        {"key to sound", "-", ms_text(readout.key_read.ms),
         ms_text(readout.mixer.ms), ms_text(device),
         with_ms(ms_text(key_sound))},
    };
    const f64 keyboard = readout.keyboard_interval_ms;
    lines.push_back({"keyboard reports every ",
                     with_ms(ms_text(keyboard > 0 ? keyboard : -1)),
                     "   aim every ",
                     with_ms(ms_text(readout.aim_interval.ms))});
    if (readout.pointer_access == PointerAccess::Asked)
      lines.push_back({"driver: allow Input Monitoring when macOS asks"});
    else if (readout.pointer_access == PointerAccess::Denied)
      lines.push_back({"driver: allow circles in Input Monitoring settings"});
    lines.push_back({"pen ",
                     app.pen_direct ? "read by circles" : "through its driver",
                     "   (F5 switches)"});
    lines.push_back(rates);
  }

  auto&         r = app.renderer;
  const Font&   font = app.regular;
  constexpr f32 kSize = 13, kRow = 18, kGap = 16, kMargin = 8;
  const f32     pad = table.empty() ? 0 : 10;  // the backing's
  const auto    width = [&](std::string_view text) {
    return measure_text(font, text, kSize);
  };
  std::array<f32, 6> columns{};
  for (const auto& row : table)
    for (size_t c = 0; c < row.size(); ++c)
      columns[c] = std::max(columns[c], width(row[c]));
  f32 block = 0;
  for (f32 column : columns)
    block += column;
  if (!table.empty())
    block += kGap * (columns.size() - 1);
  for (const Line& line : lines) {
    f32 line_width = 0;
    for (const std::string& piece : line)
      line_width += width(piece);
    block = std::max(block, line_width);
  }
  // Baselines: half a row between the table and the lines.
  const f32 left = app.width - kMargin - pad - block;
  const f32 last = app.height - kMargin - pad;
  f32 y = last - kRow * static_cast<f32>(lines.size() - 1) -
          (table.empty() ? 0 : kRow * (static_cast<f32>(table.size()) + 0.5f));
  if (!table.empty()) {
    const f32 top = y - font.cap_height * kSize - pad;
    draw_rounded_rect(r, left - pad, top, block + 2 * pad,
                      app.height - kMargin - top, 6, fade(kBlack, 0.55f));
  }
  const Color dim = fade(kWhite, 0.5f), bright = fade(kWhite, 0.85f);
  for (size_t i = 0; i < table.size(); ++i, y += kRow) {
    f32 x = left;
    for (size_t c = 0; c < columns.size(); ++c) {
      const std::string& text = table[i][c];
      if (c == 0)
        draw_text_shadowed(r, font, text, x, y, kSize, dim);
      else
        draw_text_shadowed(r, font, text, x + columns[c], y, kSize,
                           i > 0 && text != "-" ? bright : dim, Align::Right);
      x += columns[c] + kGap;
    }
  }
  if (!table.empty())
    y += kRow / 2;
  for (const Line& line : lines) {
    f32 x = left;
    for (size_t k = 0; k < line.size(); ++k) {
      draw_text_shadowed(r, font, line[k], x, y, kSize,
                         k % 2 == 1 && line[k] != "-" ? bright : dim);
      x += width(line[k]);
    }
    y += kRow;
  }
}

void draw_overlays(App& app) {
  auto& r = app.renderer;
  if (app.settings_menu.open)
    settings_draw(app);
  if (app.profiler.active && app.profiler.overlay)
    draw_profiler_overlay(app);
  if (app.now - app.toast_time < app.toast_seconds) {
    const f32 alpha = clamp01(static_cast<f32>(
        std::min(app.now - app.toast_time,
                 app.toast_seconds - (app.now - app.toast_time)) /
        0.25));
    const f32 width = measure_text(app.bold, app.toast, 17) + 40;
    draw_rounded_rect(r, (app.width - width) / 2, 18, width, 40, 20,
                      fade(hex_color(0x15152A), 0.9f * alpha));
    draw_text(r, app.bold, app.toast, app.width / 2,
              centered_baseline(app.bold, 17, 38), 17, fade(kWhite, alpha),
              Align::Center);
  }

  draw_readout(app);

  if (app.transition_started >= 0) {
    const f64 t = app.now - app.transition_started;
    const f32 alpha = static_cast<f32>(
        t < kTransitionHalf ? t / kTransitionHalf : 2 - t / kTransitionHalf);
    draw_rect(r, 0, 0, app.width, app.height, fade(kBlack, clamp01(alpha)));
  }
  draw_cursor(app);
}

}  // namespace

TabletArea tablet_area(const App& app) {
  return {app.tablet_x, app.tablet_y, app.tablet_width, app.tablet_height,
          app.tablet_rotation};
}

void update_tablet_mapping(App& app) {
  app.pen.mapping = tablet_mapping(tablet_area(app));
}

void set_tablet_area(App& app, const TabletArea& area) {
  app.tablet_x = area.x, app.tablet_y = area.y;
  app.tablet_width = area.width, app.tablet_height = area.height;
  app.tablet_rotation = area.rotation;
  update_tablet_mapping(app);
}

bool import_tablet_settings(App& app) {
  TabletArea area;
  if (!import_tablet_area(area, app.tablet_tip))
    return false;
  set_tablet_area(app, area);
  app.tablet_area_set = true;
  save_settings(app);
  return true;
}

std::string tablet_area_warning(const App& app) {
  const TabletModel* tablet = connected_tablet();
  const TabletArea   area = tablet_area(app);
  if (!tablet || fit_area(area, *tablet) == area)
    return {};
  return std::string("the area goes past the ") + tablet->name +
         "'s edge, so part of the screen is out of reach (Ctrl+O to fit it)";
}

void set_pen_direct(App& app, bool on) {
  app.pen_direct = on;
  app.pen.in_range = app.pen.tip = false;
  read_tablet(on);
  // The aim's figures start over, each describing one way of reading it,
  // and movements the other way made aren't timed.
  auto& readout = app.readout;
  for (LatencyStat* stat :
       {&readout.driver, &readout.aim_interval, &readout.aim_read,
        &readout.aim_wait, &readout.aim_screen})
    *stat = {};
  readout.moves.clear();
  readout.waiting_moves.clear();
  readout.driver_moves.clear();
  if (!on) {
    show_toast(app, "Pen: through its driver");
    return;
  }
  // The first time, the area OpenTabletDriver has.
  const bool imported = !app.tablet_area_set && import_tablet_settings(app);
  const TabletModel* tablet = connected_tablet();
  if (listen_to_pointers() != PointerAccess::Listening)
    show_toast(app, "Pen: read by circles once Input Monitoring is allowed");
  else if (!tablet)
    show_toast(app,
               "Pen: no supported tablet connected, so through its driver");
  else
    show_toast(app, std::string("Pen: read by circles, ") + tablet->name +
                        (imported ? ", with OpenTabletDriver's area" : ""));
}

void apply_frame_limiter(App& app) {
  gpu_set_frame_limit(app.frame_limiter == 0,
                      kFramesPerRefresh[app.frame_limiter]);
}

bool app_init(App& app) {
  // GLFW would move into the app's Resources; the game stays in its folder.
  glfwInitHint(GLFW_COCOA_CHDIR_RESOURCES, GLFW_FALSE);
  if (!glfwInit())
    return false;
  // GLFW makes the window but no graphics context: Metal draws into it.
  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
  app.window = glfwCreateWindow(1280, 720, "circles", nullptr, nullptr);
  if (!app.window || !gpu_init(app.window)) {
    fprintf(stderr, "can't draw: no window or no Metal device\n");
    glfwTerminate();
    return false;
  }
  profiler_init(app.profiler);
  glfwSetInputMode(app.window, GLFW_CURSOR, GLFW_CURSOR_HIDDEN);
  glfwSetWindowSizeLimits(app.window, 800, 500, GLFW_DONT_CARE, GLFW_DONT_CARE);

  app.renderer = create_renderer();
  app.slider_bodies = create_slider_body_renderer();
  app.heavy = load_font(kFontPath, kFaceHeavy);
  app.bold = load_font(kFontPath, kFaceDemiBold);
  app.regular = load_font(kFontPath, kFaceMedium);

  const bool     audio = audio_init(app.audio);
  const Settings settings = load_settings();
  app.audio_offset = settings.audio_offset;
  app.mouse_buttons = settings.mouse_buttons;
  app.mods = settings.mods;
  app.background_dim = settings.background_dim;
  app.frame_cap = settings.frame_cap;
  app.frame_limiter = settings.frame_limiter;
  apply_frame_limiter(app);
  app.cursor_size = settings.cursor_size;
  app.beatmap_colours = settings.beatmap_colours;
  app.readout.mode = settings.readout;
  app.readout.keyboard_interval_ms = keyboard_report_interval();
  app.tablet_full_screen = settings.tablet_full_screen;
  app.tablet_tip = settings.tablet_tip;
  app.tablet_area_set = settings.tablet_area_set;
  set_tablet_area(app, settings.tablet_area);
  app.tablet_profiles = settings.tablet_profiles;
  if (settings.pen_direct)
    set_pen_direct(app, true);
  set_master_volume(app.audio, settings.volume);
  // A new game folder starts empty.
  std::error_code error;
  for (const char* folder :
       {kBeatmapDirectory, kSkinDirectory, kReplayDirectory})
    std::filesystem::create_directories(folder, error);
  const i32 imported =
      static_cast<i32>(import_archives(kBeatmapDirectory, ".osz").size());
  // The skin waits for skins/ to be scanned, and a newly added skin takes
  // over from the saved one.
  const std::string new_skin = refresh_skins(app);
  use_skin(app,
           skin_index_of(app, new_skin.empty() ? settings.skin : new_skin));
  app.library = scan_library(kBeatmapDirectory);
  app.replays = scan_replays(kReplayDirectory);
  printf("%zu beatmaps in %zu sets, %zu replays\n", app.library.maps.size(),
         app.library.sets.size(), app.replays.size());

  app.now = glfwGetTime();
  app.fps_window_start = app.now;
  f64 x, y;
  glfwGetCursorPos(app.window, &x, &y);
  app.cursor = {static_cast<f32>(x), static_cast<f32>(y)};
  menu_enter(app);
  if (imported)
    show_toast(app, "Imported " + std::to_string(imported) + " beatmap set" +
                        (imported == 1 ? "" : "s"));
  if (!new_skin.empty())
    show_toast(app, "Skin: " + app.skin.config.name);
  if (!audio)
    show_toast(app, "No audio output device");
  return true;
}

void app_update(App& app, f64 now) {
  app.dt = static_cast<f32>(std::clamp(now - app.now, 0.0, 0.1));
  app.now = now;
  update_output_latency(app.audio);
  i32 width, height, pixels_wide, pixels_high;
  glfwGetWindowSize(app.window, &width, &height);
  glfwGetFramebufferSize(app.window, &pixels_wide, &pixels_high);
  app.width = static_cast<f32>(width);
  app.height = static_cast<f32>(height);
  app.pixel_width = pixels_wide;
  app.pixel_height = pixels_high;

  ++app.updates;
  read_pen(app);
  take_samples(app);
  if (now - app.fps_window_start >= 0.25) {
    const f64 span = now - app.fps_window_start;
    app.fps = static_cast<i32>(std::lround(app.fps_frames / span));
    app.update_rate = static_cast<i32>(std::lround(app.updates / span));
    refresh_readout(app, span);
    app.fps_frames = 0;
    app.updates = 0;
    app.fps_window_start = now;
  }
  update_backdrop(app);
  if (!app.dropped.empty() && app.transition_started < 0 &&
      (app.screen == Screen::MainMenu || app.screen == Screen::SongSelect))
    import_dropped(app);
  for (auto* trail : {&app.trail, &app.game.replay_trail})
    while (!trail->empty() && now - trail->front().time > kTrailLength)
      trail->pop_front();

  if (app.transition_started >= 0) {
    const f64 t = now - app.transition_started;
    if (app.screen != app.next_screen && t >= kTransitionHalf)
      enter_screen(app, app.next_screen);
    if (t >= 2 * kTransitionHalf)
      app.transition_started = -1;
  }

  switch (app.screen) {
    case Screen::MainMenu:
      menu_update(app);
      break;
    case Screen::SongSelect:
      select_update(app);
      break;
    case Screen::Gameplay:
      gameplay_update(app);
      break;
    case Screen::Results:
      results_update(app);
      break;
  }
}

bool app_draw(App& app) {
  if (!gpu_begin_frame(app.pixel_width, app.pixel_height))
    return false;
  ++app.fps_frames;
  begin_frame(app.renderer, app.width, app.height);
  switch (app.screen) {
    case Screen::MainMenu:
      menu_draw(app);
      break;
    case Screen::SongSelect:
      select_draw(app);
      break;
    case Screen::Gameplay:
      gameplay_draw(app);
      break;
    case Screen::Results:
      results_draw(app);
      break;
  }
  draw_overlays(app);
  flush(app.renderer);
  profile_phase(app.profiler, FramePhase::Draw);
  gpu_end_frame(profile_frame_tag(app.profiler));
  profile_phase(app.profiler, FramePhase::Present);
  return true;
}

void app_shutdown(App& app) {
  read_tablet(false);
  if (app.backdrop.loading.valid())
    app.backdrop.loading.wait();
  release_slider_bodies(app.game.bodies);
  destroy_skin(app.skin);
  audio_shutdown(app.audio);
  gpu_shutdown();
  glfwDestroyWindow(app.window);
  glfwTerminate();
}

// When the input being handled happened, on glfwGetTime's clock: macOS's own
// timestamp for it, rather than when this frame's poll got to it.
f64 input_time() {
  const f64 now = glfwGetTime();
  const f64 event = last_input_event_time();
  return event < 0 ? now : std::min(now, event + (now - uptime()));
}

void app_key(App& app, i32 key, i32 action) {
  const bool control =
      glfwGetKey(app.window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS ||
      glfwGetKey(app.window, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS;
  if (key == GLFW_KEY_O && control && action == GLFW_PRESS &&
      app.screen != Screen::Gameplay) {
    toggle_settings(app);
    return;
  }
  if (app.settings_menu.open) {
    settings_key(app, key, action);
    return;
  }
  if (key == GLFW_KEY_F8 && action == GLFW_PRESS) {
    toggle_profiling(app);
    return;
  }
  if (key == GLFW_KEY_F5 && action == GLFW_PRESS) {
    set_pen_direct(app, !app.pen_direct);
    save_settings(app);
    return;
  }
  if (key == GLFW_KEY_F6 && action == GLFW_PRESS) {
    // The readout: hidden, a line, then with its stages.
    app.readout.mode = (app.readout.mode + 1) % 3;
    save_settings(app);
    return;
  }
  if (key == GLFW_KEY_F7 && action == GLFW_PRESS) {
    // The frame limiter's next choice, as osu!'s F7 steps through them.
    app.frame_limiter = (app.frame_limiter + 1) %
                        static_cast<i32>(std::size(kFrameLimiterNames));
    apply_frame_limiter(app);
    save_settings(app);
    show_toast(app, std::string("Frame limiter: ") +
                        kFrameLimiterNames[app.frame_limiter]);
    return;
  }
  // On release: macOS drops the release of a key held as the window changes
  // to or from full screen, and GLFW then takes its next press for a repeat.
  if (key == GLFW_KEY_F11 && action == GLFW_RELEASE) {
    toggle_fullscreen(app);
    return;
  }
  if (key == GLFW_KEY_F10 && action == GLFW_PRESS) {
    app.mouse_buttons = !app.mouse_buttons;
    save_settings(app);
    show_toast(app,
               app.mouse_buttons ? "Mouse buttons on" : "Mouse buttons off");
    return;
  }
  // Loading a skin takes a moment, so not mid-map.
  if (key == GLFW_KEY_F9 && action == GLFW_PRESS &&
      app.screen != Screen::Gameplay) {
    cycle_skin(app);
    return;
  }
  // The audio offset is set while listening: on the main menu or mid-map.
  // Song select takes these keys as search text. Shift moves in bigger steps
  // for speakers with a long delay; holding repeats.
  const bool offset_screen =
      app.screen == Screen::MainMenu || app.screen == Screen::Gameplay;
  if ((key == GLFW_KEY_MINUS || key == GLFW_KEY_EQUAL) && offset_screen &&
      action != GLFW_RELEASE) {
    const bool shift =
        glfwGetKey(app.window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ||
        glfwGetKey(app.window, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS;
    change_audio_offset(app,
                        (key == GLFW_KEY_EQUAL ? 1 : -1) * (shift ? 25 : 5));
    return;
  }
  if (!accepting_input(app))
    return;
  switch (app.screen) {
    case Screen::MainMenu:
      menu_key(app, key, action);
      break;
    case Screen::SongSelect:
      select_key(app, key, action);
      break;
    case Screen::Gameplay:
      gameplay_key(app, key, action, input_time());
      break;
    case Screen::Results:
      results_key(app, key, action);
      break;
  }
}

void app_char(App& app, u32 codepoint) {
  if (app.settings_menu.open)
    settings_char(app, codepoint);
  else if (accepting_input(app) && app.screen == Screen::SongSelect)
    select_char(app, codepoint);
}

void app_mouse_button(App& app, i32 button, i32 action) {
  if (!pen_aiming(app))
    mouse_button(app, button, action, input_time());
}

void app_scroll(App& app, f64 dy) {
  // Alt (Option on Mac keyboards) + scroll sets the volume anywhere; on the
  // main menu, any scroll does.
  const bool alt = glfwGetKey(app.window, GLFW_KEY_LEFT_ALT) == GLFW_PRESS ||
                   glfwGetKey(app.window, GLFW_KEY_RIGHT_ALT) == GLFW_PRESS;
  if (alt || app.screen == Screen::MainMenu)
    change_volume(app, dy);
  else if (accepting_input(app) && !app.settings_menu.open &&
           app.screen == Screen::SongSelect)
    select_scroll(app, dy);
}

void app_cursor(App& app, f64 x, f64 y) {
  if (!pen_aiming(app))
    set_cursor(app, x, y);
}

void app_drop(App& app, std::span<const char* const> paths) {
  size_t queued = 0;
  for (const char* path : paths) {
    if (lowercase(std::filesystem::path(path).extension().string()) != ".osz")
      continue;
    app.dropped.emplace_back(path);
    ++queued;
  }
  if (!queued)
    show_toast(app, "Drop .osz beatmap archives to import them");
  else if (app.screen == Screen::Gameplay || app.screen == Screen::Results)
    show_toast(app, "Importing once you're back at song select");
}

void app_focus(App& app, bool focused) {
  if (!focused && app.screen == Screen::Gameplay)
    gameplay_focus_lost(app);
}

bool app_start_benchmark(App& app, const std::string& path) {
  namespace fs = std::filesystem;
  const bool auto_play =
      lowercase(fs::path(path).extension().string()) == ".osu";
  std::error_code error;
  std::string     md5;
  if (!auto_play) {
    const auto replay = read_replay(path, false);
    if (!replay)
      return false;
    md5 = replay->beatmap_md5;
  }
  i32 map = -1;
  for (size_t m = 0; m < app.library.maps.size(); ++m)
    if (auto_play ? fs::equivalent(app.library.maps[m].osu_path, path, error)
                  : app.library.maps[m].md5 == md5)
      map = static_cast<i32>(m);
  if (map < 0)
    return false;
  // Silent, without touching the saved volume.
  set_master_volume(app.audio, 0);
  select_map(app, map, false);
  if (auto_play)
    app.mods = kAutoplay;
  else
    app.replay_to_watch = path;
  switch_screen(app, Screen::Gameplay);
  profiler_start(app.profiler, false);
  return true;
}

std::string app_finish_benchmark(App& app) {
  return profiler_stop(app.profiler, kProfileDirectory);
}

void switch_screen(App& app, Screen screen) {
  if (app.transition_started >= 0)
    return;
  app.next_screen = screen;
  app.transition_started = app.now;
}

void select_map(App& app, i32 map, bool preview) {
  if (map < 0 || map >= static_cast<i32>(app.library.maps.size()))
    return;
  const bool same_song =
      app.selected_map >= 0 && app.library.maps[app.selected_map].audio_path ==
                                   app.library.maps[map].audio_path;
  app.selected_map = map;
  request_backdrop(app, app.library.maps[map].background_path);
  if (preview && !(same_song && music_playing(app.audio)))
    play_preview(app);
}

void play_preview(App& app) {
  if (app.selected_map < 0)
    return;
  const auto& info = app.library.maps[app.selected_map];
  if (!music_load(app.audio, info.audio_path))
    return;
  music_set_rate(app.audio, mod_rate(app.mods), !mods_change_pitch(app.mods));
  music_set_speed(app.audio, 1);
  music_seek(app.audio,
             info.preview_time >= 0 ? info.preview_time : info.end_time * 0.4);
  music_fade_in(app.audio, 800);
  music_play(app.audio);
}

void draw_backdrop(App& app, f32 dim, f32 parallax) {
  auto&       r = app.renderer;
  const auto& backdrop = app.backdrop;
  const f32   appear =
      clamp01(static_cast<f32>((app.now - backdrop.changed) / kBackdropFade));
  // Each full-screen layer costs the GPU, so only what shows is drawn: the
  // dim tints the images rather than covering them, and an image that has
  // faded in hides the black and the image before it.
  const bool previous_shows = appear < 1 && backdrop.previous.id;
  const bool covered =
      dim < 1 && ((backdrop.current.id && appear >= 1) || previous_shows);
  if (!covered)
    draw_rect(r, 0, 0, app.width, app.height, kBlack);
  if (dim >= 1)
    return;
  const f32 zoom = 1 + 0.03f * parallax;
  const f32 shift_x =
      -parallax * std::clamp(app.cursor.x / app.width * 2 - 1, -1.0f, 1.0f);
  const f32 shift_y =
      -parallax * std::clamp(app.cursor.y / app.height * 2 - 1, -1.0f, 1.0f);
  const Color tint{1 - dim, 1 - dim, 1 - dim, 1};
  if (previous_shows)
    draw_image_cover(r, backdrop.previous, 0, 0, app.width, app.height, tint,
                     zoom, shift_x, shift_y);
  draw_image_cover(r, backdrop.current, 0, 0, app.width, app.height,
                   fade(tint, appear), zoom, shift_x, shift_y);
}

void show_toast(App& app, std::string text, f64 seconds) {
  app.toast = std::move(text);
  app.toast_time = app.now;
  app.toast_seconds = seconds;
}

void save_settings(const App& app) {
  std::ofstream file(kSettingsPath);
  file << "audio_offset=" << app.audio_offset << '\n';
  file << "volume=" << app.audio.master_volume << '\n';
  file << "skin=" << (app.skin_index >= 0 ? app.skins[app.skin_index] : "")
       << '\n';
  file << "mouse_buttons=" << app.mouse_buttons << '\n';
  file << "mods=" << app.mods << '\n';
  file << "background_dim=" << app.background_dim << '\n';
  file << "frame_cap=" << app.frame_cap << '\n';
  file << "frame_limiter=" << kFrameLimiters[app.frame_limiter] << '\n';
  file << "cursor_size=" << app.cursor_size << '\n';
  file << "beatmap_colours=" << app.beatmap_colours << '\n';
  file << "pen_direct=" << app.pen_direct << '\n';
  file << "tablet_full_screen=" << app.tablet_full_screen << '\n';
  file << "tablet_tip=" << app.tablet_tip << '\n';
  if (app.tablet_area_set)
    file << "tablet_area=" << app.tablet_x << ' ' << app.tablet_y << ' '
         << app.tablet_width << ' ' << app.tablet_height << ' '
         << app.tablet_rotation << '\n';
  for (size_t i = 0; i < app.tablet_profiles.size(); ++i) {
    const auto& [name, area] = app.tablet_profiles[i];
    if (!name.empty())
      file << "tablet_profile_" << i + 1 << '=' << area.x << ' ' << area.y
           << ' ' << area.width << ' ' << area.height << ' ' << area.rotation
           << ' ' << name << '\n';
  }
  file << "readout=" << app.readout.mode << '\n';
}

std::string save_replay(App& app, i32 map, Replay replay) {
  const auto& info = app.library.maps[map];
  char        date[32];
  const auto  timestamp = static_cast<std::time_t>(replay.timestamp);
  std::strftime(date, sizeof(date), "%Y-%m-%d %H-%M-%S",
                std::localtime(&timestamp));
  // Named as osu! names exported replays.
  const auto path =
      std::filesystem::path(kReplayDirectory) /
      file_name_safe(replay.player + " - " + info.artist + " - " + info.title +
                     " [" + info.version + "] (" + date + ").osr");
  if (!write_replay(path, replay))
    return {};
  replay.frames.clear();
  app.replays.push_back({path, std::move(replay)});
  return path.string();
}

std::vector<const ReplayEntry*> map_replays(const App& app, i32 map) {
  std::vector<const ReplayEntry*> replays;
  if (map < 0)
    return replays;
  for (const auto& entry : app.replays)
    if (entry.replay.beatmap_md5 == app.library.maps[map].md5)
      replays.push_back(&entry);
  std::stable_sort(replays.begin(), replays.end(),
                   [](const ReplayEntry* a, const ReplayEntry* b) {
                     return a->replay.score > b->replay.score;
                   });
  return replays;
}

void change_audio_offset(App& app, f64 ms) {
  app.audio_offset += ms;
  save_settings(app);
  char text[64];
  snprintf(text, sizeof(text),
           "Audio offset %+.0f ms, plus %.0f ms output delay", app.audio_offset,
           app.audio.output_latency);
  show_toast(app, text);
}

void use_skin(App& app, i32 index) {
  destroy_skin(app.skin);
  app.skin_index =
      index >= 0 && index < static_cast<i32>(app.skins.size()) ? index : -1;
  if (app.skin_index >= 0)
    app.skin = load_skin(std::filesystem::path(kSkinDirectory) /
                         app.skins[app.skin_index]);
  load_sounds(app.audio, [&](const std::string& stem) {
    return app.skin_index < 0
               ? std::string()
               : skin_file(app.skin, stem, {".wav", ".mp3", ".ogg"}).string();
  });
  save_settings(app);
}

bool contains(Vec2 point, f32 x, f32 y, f32 w, f32 h) {
  return point.x >= x && point.x < x + w && point.y >= y && point.y < y + h;
}

void draw_button(App&             app,
                 f32              x,
                 f32              y,
                 f32              w,
                 f32              h,
                 std::string_view label,
                 f32              hover,
                 Color            accent) {
  auto&     r = app.renderer;
  const f32 slide = hover * 8;
  draw_rounded_rect(r, x + slide + 3, y + 5, w, h, h * 0.3f,
                    fade(kBlack, 0.35f));
  draw_rounded_rect(r, x + slide, y, w, h, h * 0.3f,
                    mix(shade(accent, 0.75f), shade(accent, 1.1f), hover));
  const f32 size = h * 0.4f;
  draw_text_shadowed(r, app.bold, label, x + slide + w / 2,
                     centered_baseline(app.bold, size, y + h / 2), size, kWhite,
                     Align::Center);
}

std::string fit_text(const Font&      font,
                     std::string_view text,
                     f32              size,
                     f32              max_width) {
  if (measure_text(font, text, size) <= max_width)
    return std::string(text);
  std::string cut(text);
  while (!cut.empty() && measure_text(font, cut + "...", size) > max_width)
    cut.pop_back();
  return cut + "...";
}

std::string format_duration(f64 ms) {
  const i32 seconds = static_cast<i32>(std::max(0.0, ms) / 1000);
  char      text[16];
  snprintf(text, sizeof(text), "%d:%02d", seconds / 60, seconds % 60);
  return text;
}

Color difficulty_colour(const BeatmapInfo& info) {
  // From green for easy to purple for the hardest, by approach rate and
  // accuracy demands.
  constexpr f32 kStops[] = {1.5f, 3.5f, 5.5f, 7.5f, 9};
  const Color   colours[] = {hex_color(0x7BD67B), hex_color(0x4FA8FF),
                             hex_color(0xFFD34F), hex_color(0xFF5A8C),
                             hex_color(0xB06BFF)};
  const f32     rating = static_cast<f32>((info.ar + info.od) / 2);
  if (rating <= kStops[0])
    return colours[0];
  for (i32 i = 1; i < 5; ++i)
    if (rating <= kStops[i])
      return mix(colours[i - 1], colours[i],
                 (rating - kStops[i - 1]) / (kStops[i] - kStops[i - 1]));
  return colours[4];
}

Color grade_colour(Grade grade, Mods mods) {
  switch (grade) {
    case Grade::SS:
    case Grade::S:
      return silver_grade(grade, mods) ? hex_color(0xDDE3EE)
                                       : hex_color(0xFFD452);
    case Grade::A:
      return hex_color(0x88DA20);
    case Grade::B:
      return hex_color(0x4BB7FF);
    case Grade::C:
      return hex_color(0xE86CF0);
    case Grade::D:
      return hex_color(0xFF5A5A);
  }
  return kWhite;
}

const char* grade_name(Grade grade) {
  constexpr const char* kNames[] = {"SS", "S", "A", "B", "C", "D"};
  return kNames[static_cast<i32>(grade)];
}

}  // namespace circles
