#pragma once

#include <circles/audio.h>
#include <circles/gfx.h>
#include <circles/hitsounds.h>
#include <circles/library.h>
#include <circles/math.h>
#include <circles/rules.h>
#include <circles/skin.h>
#include <circles/types.h>

#include <fosu/beatmap.h>
#include <fosu/parser.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <deque>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace circles {

enum class Screen : u8 { MainMenu, SongSelect, Gameplay, Results };

enum class ObjectKind : u8 { Circle, Slider, Spinner };

// A hit object prepared for play. Positions are osu! pixels with stacking
// applied; times are ms.
struct GameObject {
  ObjectKind        kind;
  f64               time;
  f64               end_time;
  Vec2              position;
  Vec2              end_position;  // where a slider finishes, after its repeats
  i32               combo_number;
  i32               colour;  // index into Gameplay::combo_colours
  bool              new_combo;
  Hitsound          hitsound;
  f32               volume;  // 0 to 1, from the timing point at `time`

  // Sliders
  u32               slider = fosu::HitObject::kNoSlider;  // into map arrays
  i32               slides = 1;
  f64               span_duration = 0;
  std::vector<Vec2> path;                // evenly spaced points along the body
  std::vector<Hitsound> edge_hitsounds;  // the head, each repeat, then the end

  // Spinners
  i32                   required_spins = 0;
};

struct ObjectState {
  Judgement head = Judgement::None;  // a circle or slider head
  f64       head_time = 0;
  Judgement result = Judgement::None;  // the whole object
  f64       result_time = 0;

  // Sliders: the head, ticks, repeats and end are its parts.
  size_t    next_event = 1;  // into the map's slider events; 0 is the head
  i32       parts_hit = 0;
  i32       parts_total = 0;
  bool      tracking = false;
  f64       tracking_changed = -1e9;
  // osu! judges a slider's end slightly early but sounds it on time.
  bool      end_hit = false;

  // Spinners
  f32       rotation = 0;  // radians, signed
  f32       last_angle = 0;
  bool      has_angle = false;
  i32       spins_scored = 0;
  f32       rpm = 0;
  f64       bonus_time = -1e9;
};

// A hit key press on the gameplay clock.
struct Press {
  f64  time;
  Vec2 cursor;  // window points
};

struct Popup {
  f64       time;
  Vec2      position;
  Judgement judgement;
};

struct HitError {
  f64       wall_time;
  f64       offset;  // ms; negative is early
  Judgement judgement;
};

enum class GamePhase : u8 { Playing, Paused, Failed };

// Two keyboard keys, then two mouse buttons (a tablet pen taps as button 1).
enum HitKey : u8 { kKey1, kKey2, kMouse1, kMouse2, kHitKeyCount };

struct Gameplay {
  // The map stays parsed for its slider events and breaks.
  std::unique_ptr<fosu::Parser> parser;
  const fosu::Beatmap*          map = nullptr;
  i32                           map_index = -1;
  std::vector<GameObject>       objects;
  std::vector<ObjectState>      states;
  std::vector<Color>            combo_colours;
  f64                           radius = 0;
  f64                           preempt = 0;
  f64                           fade_in = 0;
  HitWindows                    windows{};
  f64                           hp = 0;
  f64                           score_multiplier = 1;
  f64                           end_time = 0;  // when the last object ends

  // The clock follows the music: audio time is clock_time plus the wall time
  // since clock_wall.
  f64                           clock_time = 0;
  f64                           clock_wall = 0;
  // The music's position settles shortly after starting or seeking.
  f64                           resync_after = 0;
  bool                          audio_ok = false;
  bool                          audio_started = false;
  f64    time = 0;  // gameplay time this frame: audio time minus the offset

  i64    score = 0;
  f64    displayed_score = 0;
  i32    combo = 0;
  i32    max_combo = 0;
  Counts counts;
  f64    health = 1;
  f32    displayed_health = 1;
  std::vector<f64>      offsets;
  std::vector<HitError> recent_errors;
  std::vector<Popup>    popups;
  f64                   combo_changed = -1e9;  // wall seconds
  size_t                judged = 0;
  size_t                first_active = 0;

  bool                  held[kHitKeyCount] = {};
  i32                   press_counts[kHitKeyCount] = {};
  std::vector<Press>    pending;

  GamePhase             phase = GamePhase::Playing;
  f64                   phase_changed = 0;  // wall seconds
  i32                   menu_selection = -1;
  f32                   dim = 0.8f;
  bool                  finished = false;
  f64                   finished_at = 0;
};

struct Results {
  i32              map_index = -1;
  i64              score = 0;
  i32              max_combo = 0;
  Counts           counts;
  std::vector<f64> offsets;
  f64              hit50_window = 0;
  f64              entered = 0;
  i32              hovered = -1;
};

struct Particle {
  f32 x, y, size, speed, alpha;
};

struct BeatRing {
  f64  start;
  bool kiai;
};

struct MainMenu {
  static constexpr i32  kBands = 64;
  std::vector<Particle> particles;
  f32                   bands[kBands] = {};
  i64                   last_beat = -1;
  std::vector<BeatRing> rings;
  i32                   hovered = -1;  // 0 logo, 1 play, 2 exit
  f32                   hover[3] = {};
};

struct SongSelect {
  std::string      query;
  std::vector<i32> sets;  // Library::sets matching the query
  // The carousel's position: points from the top of the list to the window's
  // centre line, easing toward scroll_target.
  f32              scroll = 0;
  f32              scroll_target = 0;
  i32              hovered_row = -1;
  // Dragging with the left button scrolls, and flings on release; holding the
  // right button on empty space jumps to the place in the list the cursor's
  // height marks.
  bool             left_down = false;
  bool             dragging = false;
  bool             right_down = false;
  f32              drag_start_y = 0;
  f32              drag_start_scroll = 0;
  f32              fling = 0;  // points per second
};

// Draws slider bodies; see slider.frag.
struct SliderRenderer {
  GLuint program;
  GLuint vao;
  GLuint corner_vbo;
  GLuint center_vbo;
  GLint  screen_size;
  GLint  radius;
  GLint  body_colour;
  GLint  border_colour;
  GLint  border_width;
  GLint  alpha;
};

// The background image, loaded off the main thread and cross-faded in.
struct Backdrop {
  std::string        path;  // requested
  std::string        loading_path;
  std::future<Image> loading;
  Texture            current;
  Texture            previous;
  f64                changed = -1e9;  // wall seconds
};

struct TrailPoint {
  f64  time;
  Vec2 position;
};

// Not movable: Audio must stay put.
struct App {
  GLFWwindow*              window = nullptr;
  f32                      width = 0;  // window size in points
  f32                      height = 0;
  f64                      now = 0;  // wall seconds
  f32                      dt = 0;
  Renderer                 renderer;
  SliderRenderer           slider_renderer;
  Font                     heavy;
  Font                     bold;
  Font                     regular;
  Audio                    audio;
  Library                  library;
  Backdrop                 backdrop;
  Skin                     skin;
  // Folder names in skins/; skin_index is into them, or -1 for the built-in
  // look.
  std::vector<std::string> skins;
  i32                      skin_index = -1;

  Screen                   screen = Screen::MainMenu;
  Screen                   next_screen = Screen::MainMenu;
  f64        transition_started = -1;  // wall seconds; negative when none runs

  // Shared by the menu music, song select and gameplay.
  i32        selected_map = -1;
  // ms; positive when sound reaches the player late.
  f64        audio_offset = 0;
  // Whether mouse buttons hit in gameplay. Tablet players who tap keys turn
  // them off so pen taps don't count.
  bool       mouse_buttons = true;

  MainMenu   menu;
  SongSelect select;
  Gameplay   game;
  Results    results;

  Vec2       cursor{};
  std::deque<TrailPoint> trail;
  std::vector<Vec2>      scratch;  // a slider body's points this frame
  bool                   fullscreen = false;
  i32                    windowed[4] = {};  // x, y, width, height to restore

  i32                    fps = 0;
  i32                    fps_frames = 0;
  f64                    fps_window_start = 0;
  std::string            toast;
  f64                    toast_time = -1e9;
  bool                   quit = false;
};

// app.cc
bool app_init(App& app);
void app_frame(App& app, f64 now);
void app_shutdown(App& app);
void app_key(App& app, i32 key, i32 action);
void app_char(App& app, u32 codepoint);
void app_mouse_button(App& app, i32 button, i32 action);
void app_scroll(App& app, f64 dy);
void app_cursor(App& app, f64 x, f64 y);
void app_focus(App& app, bool focused);

// Fades out, switches, and fades back in.
void switch_screen(App& app, Screen screen);
// Shows the map's background and, with `preview`, plays its song from the
// preview point unless it's already playing.
void select_map(App& app, i32 map, bool preview);
void play_preview(App& app);
// `parallax` (0 to 1) shifts the image slightly against the cursor.
void draw_backdrop(App& app, f32 dim, f32 parallax);
void show_toast(App& app, std::string text);
// Shifts the global audio offset by `ms` and saves it.
void change_audio_offset(App& app, f64 ms);
// Switches to skins[index], or the built-in look for -1, and saves the choice.
void use_skin(App& app, i32 index);
// osu! lays out its interface for a 768-point-tall window; skin images for
// it scale by this.
inline f32 ui_scale(const App& app) {
  return app.height / 768;
}
void save_settings(const App& app);

bool contains(Vec2 point, f32 x, f32 y, f32 w, f32 h);
// `hover` (0 to 1) animates the highlight.
void draw_button(App&             app,
                 f32              x,
                 f32              y,
                 f32              w,
                 f32              h,
                 std::string_view label,
                 f32              hover,
                 Color            accent);
// `text`, cut short with "..." to fit `max_width`.
std::string fit_text(const Font&      font,
                     std::string_view text,
                     f32              size,
                     f32              max_width);
std::string format_duration(f64 ms);  // m:ss
Color difficulty_colour(const BeatmapInfo& info);
Color grade_colour(Grade grade);
const char* grade_name(Grade grade);

// menu.cc
void menu_enter(App& app);
void menu_update(App& app);
void menu_draw(App& app);
void menu_key(App& app, i32 key, i32 action);
void menu_click(App& app, i32 button, i32 action);

// song_select.cc
void select_enter(App& app);
void select_update(App& app);
void select_draw(App& app);
void select_key(App& app, i32 key, i32 action);
void select_char(App& app, u32 codepoint);
void select_click(App& app, i32 button, i32 action);
void select_scroll(App& app, f64 dy);

// gameplay.cc
SliderRenderer create_slider_renderer();
bool gameplay_start(App& app, i32 map);
void gameplay_update(App& app);
void gameplay_draw(App& app);
// `wall` is when the input arrived, for timing the hit.
void gameplay_key(App& app, i32 key, i32 action, f64 wall);
void gameplay_click(App& app, i32 button, i32 action, f64 wall);
void gameplay_focus_lost(App& app);

// results.cc
void results_enter(App& app);
void results_update(App& app);
void results_draw(App& app);
void results_key(App& app, i32 key, i32 action);
void results_click(App& app, i32 button, i32 action);

}  // namespace circles
