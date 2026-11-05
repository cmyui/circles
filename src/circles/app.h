#pragma once

#include <circles/audio.h>
#include <circles/gfx.h>
#include <circles/health.h>
#include <circles/hit_objects.h>
#include <circles/hitsounds.h>
#include <circles/library.h>
#include <circles/math.h>
#include <circles/mods.h>
#include <circles/profiler.h>
#include <circles/replay.h>
#include <circles/rules.h>
#include <circles/skin.h>
#include <circles/tablet.h>
#include <circles/types.h>

#include <fosu/beatmap.h>
#include <fosu/parser.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <deque>
#include <future>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace circles {

enum class Screen : u8 { MainMenu, SongSelect, Gameplay, Results };

// An object's progress in play, as osu!stable tracks it.
struct ObjectState {
  // Stable's IsHit: the whole object has been judged.
  bool      hit = false;
  Judgement result = Judgement::None;
  f64       result_time = 0;
  Judgement head = Judgement::None;  // a circle, or a slider's head
  f64       head_time = 0;
  f64       shake_time = -1e9;  // when a blocked press last shook it

  // Sliders
  i32       points_hit = 0;  // ticks, repeats and end followed
  i32       points_missed = 0;
  bool      tracking = false;  // stable's IsSliding
  f64       tracking_changed = -1e9;
  i32       init_time = 0;      // when tracking last began
  u8        down_button = 0;    // the side holding it: 1 left, 2 right
  i32       last_sounded = -1;  // the last point whose sound played

  // Spinners
  f32       half_turns = 0;  // stable's floatRotationCount
  i32       last_half_turns = 0;
  i32       scored_turns = 0;  // stable's scoringRotationCount
  f64       velocity = 0;      // radians per ms, eased toward the target
  f64       target_velocity = 0;
  i32       zero_count = 0;
  f64       last_angle = 0;
  f64       frame_variance = 1000.0 / 60;
  f32       rotation = 0;  // the spinner's drawn angle
  f64       rpm = 0;
  f64       bonus_time = -1e9;
  i32       bonus = 0;  // the bonus shown
};

struct Popup {
  f64       time;
  Vec2      position;
  Judgement judgement;
  f32       tilt = 0;  // a miss's random rotation, radians
};

struct HitError {
  f64       wall_time;
  f64       offset;  // ms; negative is early
  Judgement judgement;
};

// Recovering is Easy's revive: the music waits while health refills.
enum class GamePhase : u8 { Playing, Paused, Failed, Recovering };

struct TrailPoint {
  f64  time;
  Vec2 position;
};

// Two keyboard keys, then two mouse buttons (a tablet pen taps as button 1).
enum HitKey : u8 { kKey1, kKey2, kMouse1, kMouse2, kHitKeyCount };

struct Gameplay {
  Mods                           mods = 0;
  f64                            rate = 1;  // song speed from the mods
  // The map stays parsed for its slider events and breaks.
  std::unique_ptr<fosu::Parser>  parser;
  const fosu::Beatmap*           map = nullptr;
  i32                            map_index = -1;
  std::vector<GameObject>        objects;
  std::vector<ObjectState>       states;
  SliderBodies                   bodies;  // the visible sliders'
  std::vector<Color>             combo_colours;
  f64                            radius = 0;
  f64                            preempt = 0;
  f64                            fade_in = 0;
  HitWindows                     windows{};
  f64                            hp_setting = 0;  // the map's HP, after mods
  std::vector<fosu::TimingPoint> timing;          // sorted as stable sorts them
  std::vector<fosu::Break>       breaks;          // as stable keeps them
  SampleSet                      default_sample_set = SampleSet::Normal;
  // Score V1's combo bonus scales by score_multiplier: the map's difficulty
  // times the mods' multiplier. Score V2 scales by the mods' alone.
  f64                            score_multiplier = 1;
  f64                            mod_multiplier = 1;
  f64                            end_time = 0;  // when the last object ends

  // The clock follows the music: audio time is clock_time plus the wall time
  // since clock_wall.
  f64                            clock_time = 0;
  f64                            clock_synced = 0;  // wall seconds
  // Skipping the intro is offered until this time.
  std::optional<i32>             skip_boundary;
  i32    lead_frame_time = -1;  // a replay's second frame, as stable records it
  bool   audio_ok = false;
  bool   audio_started = false;
  f64    time = 0;  // gameplay time this frame: audio time minus the offset
  // Stable's game loop runs in whole ms: the last frame's time, and the last
  // of its 60 Hz "scorable" frames.
  i32    step_time = 0;
  bool   stepped = false;
  i32    last_scorable_time = 0;
  f64    sixty_last = 0;  // wall ms of the last 60 Hz frame
  i32    active = -1;     // the slider or spinner being scored, if any

  i64    score = 0;
  f64    displayed_score = 0;
  i32    combo = 0;
  i32    max_combo = 0;
  Counts counts;
  // Health in stable's units, 0 to 200, and its rules for this map: passive
  // drain per ms, and multipliers on what hits restore.
  f64    health = kHealthMax;
  HealthRules     health_rules;
  bool            draining = false;
  i32             resume_after_break = -1;
  std::deque<i32> drain_stops;  // where drain pauses before each break
  bool            before_gameplay = true;
  bool            after_gameplay = false;
  f32             displayed_health = 1;  // 0 to 1, for the bar
  f64             recovery_display = 0;  // the bar refilling in Easy's revive
  // Hits within the current combo below 300, for its end-of-combo bonus.
  i32             combo_katu = 0;
  i32             combo_bad = 0;
  bool            outro = false;  // the song has ended; presses no longer count
  // Score V2's tallies; see update_score_v2.
  f64             combo_portion = 0;
  f64             bonus_portion = 0;
  i32             recoveries = 0;  // Easy's revives left
  std::vector<f64>      offsets;
  std::vector<HitError> recent_errors;
  std::vector<Popup>    popups;
  f64                   combo_changed = -1e9;  // wall seconds
  size_t                first_active = 0;

  bool                  held[kHitKeyCount] = {};
  // Presses and releases since the last frame: when they happened (wall
  // seconds) and the keys held after each, as replay bits.
  struct TimedKeys {
    f64 wall;
    u32 keys;
  };
  std::vector<TimedKeys>   pending_keys;
  i32                      press_counts[kHitKeyCount] = {};
  // Stable's two sides: left is key 1 or mouse 1, right key 2 or mouse 2.
  bool                     left = false;
  bool                     right = false;
  u8                       down_sides = 0;  // 1 left, 2 right
  u8                       last_sides = 0;
  u8                       last_sides2 = 0;
  bool                     game_down = false;
  bool                     press_left = false;  // this frame's new presses
  bool                     press_right = false;
  bool                     force_frame = false;  // record this frame

  // Every play records its input as replay frames. Watching a replay plays
  // its frames back instead of taking input.
  std::vector<ReplayFrame> frames;
  bool                     watching = false;
  // Scrubbing a watched play: where its timeline starts (game ms), and a
  // drag along it.
  f64                      timeline_start = 0;
  bool                     scrubbing = false;
  f64                      last_seek = -1e9;  // wall seconds
  std::string              replay_path;       // the replay being watched
  std::string              replay_player;
  size_t                   next_frame = 0;   // watching: the next to apply
  f64                      replay_tick = 0;  // watching: the next 60 Hz frame
  // The cursor that hits, in window points: the mouse's, the replay's or
  // Autopilot's.
  Vec2                     cursor{};
  std::deque<TrailPoint>   replay_trail;
  // Autopilot's cursor follows a perfect play's.
  std::vector<ReplayFrame> autopilot_frames;
  // Flashlight's hole follows the cursor, in window points, and shrinks as
  // the combo grows; its scale is osu!stable's for its texture.
  Vec2                     flashlight_position{};
  f32                      flashlight_scale = 3.2f;
  i64                      nightcore_beat = -1;  // the last half beat played

  GamePhase                phase = GamePhase::Playing;
  f64                      phase_changed = 0;  // wall seconds
  f64                      started = 0;        // wall seconds
  i32                      menu_selection = -1;
  f32                      dim = 0.8f;
  bool                     finished = false;  // passed; results follow
};

struct Results {
  i32              map_index = -1;
  i64              score = 0;
  i32              max_combo = 0;
  Counts           counts;
  std::vector<f64> offsets;
  f64              hit50_window = 0;
  std::string      player;
  std::string      replay_path;  // empty if the replay wasn't saved
  Mods             mods = 0;
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
  f32              fling = 0;            // points per second
  i32              hovered_replay = -1;  // into the selected map's replays
  // The mod select panel, over the list while open.
  bool             mod_select = false;
  f64              mod_select_changed = -1e9;  // wall seconds
  i32              hovered_mod = -1;           // buttons numbered row by row
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

// Recent samples of one latency, in ms, and their median.
struct LatencyStat {
  std::deque<f32> samples;
  f64             last = -1;  // when the newest was taken, uptime
  f32             ms = -1;    // the median; -1 while none is recent
};

// The corner readout: latency from the hands to the eyes and ears, by stage
// and end to end. Times are seconds of uptime, the clock macOS stamps input
// events with.
struct Readout {
  i32                       mode = 1;  // 0 hidden, 1 a line, 2 with stages
  // The keyboards' slowest report interval, read at launch.
  f64                       keyboard_interval_ms = 0;
  // Being handled.
  std::vector<InputTiming>  new_presses, new_moves;
  std::vector<InputTiming>  pen_moves;  // the pen's, read directly
  std::vector<GpuFrameTime> reports;
  std::vector<f64>          waits;
  // Read, until a frame drawn after them is shown, and until a frame starts
  // drawing after them.
  std::deque<InputTiming>   presses, moves, waiting_presses, waiting_moves;
  f64                       last_move = -1;  // event time
  // keys
  LatencyStat               key_read;
  // aim: from a pointing device's report reaching macOS to its cursor
  // movement (the driver), how often movements come, and reading them
  PointerAccess             pointer_access = PointerAccess::Asked;
  std::vector<f64>          new_reports;
  std::deque<f64>           pointer_reports;  // when they arrived
  std::deque<InputTiming>   driver_moves;     // for timing the driver
  LatencyStat               driver, aim_interval, aim_read;
  // screen: from reading a press, or the newest cursor movement, to the next
  // frame starting to draw, and from drawing to being shown
  LatencyStat               frame_wait, aim_wait, display;
  // sound: from starting a sound to the audio cycle that mixes it
  LatencyStat               mixer;
  // End to end: a press, or the newest cursor movement, to the frame
  // showing it.
  LatencyStat               key_screen, aim_screen;
  i32                       shown = 0;  // frames since shown_rate was set
  // Frames per second the display showed: only known full screen, else 0.
  i32                       shown_rate = 0;
};

// Not movable: Audio must stay put.
struct App {
  Profiler                 profiler;
  GLFWwindow*              window = nullptr;
  f32                      width = 0;  // window size in points
  f32                      height = 0;
  i32                      pixel_width = 0;  // framebuffer size
  i32                      pixel_height = 0;
  f64                      now = 0;  // wall seconds
  f32                      dt = 0;
  Renderer                 renderer;
  SliderBodyRenderer       slider_bodies;
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
  f64  transition_started = -1;  // wall seconds; negative when none runs

  // Shared by the menu music, song select and gameplay.
  i32  selected_map = -1;
  Mods mods = 0;  // chosen in song select
  // ms; positive when sound reaches the player late.
  f64  audio_offset = 0;
  // Replays in replays/, and the one gameplay should play back next.
  std::vector<ReplayEntry> replays;
  // Beatmap archives dropped on the window, awaiting import.
  std::vector<std::string> dropped;
  std::string              replay_to_watch;
  // Whether mouse buttons hit in gameplay. Tablet players who tap keys turn
  // them off so pen taps don't count.
  bool                     mouse_buttons = true;
  // Gameplay's background dim, 0 to 1; 1 hides the background.
  f32                      background_dim = 0.8f;
  // The game loop sleeps to run at most 1000 passes per second, rather than
  // as fast as it can.
  bool                     frame_cap = true;
  // Full screen frames: with vsync (0), whole at each refresh; without, as
  // soon as drawn, where they can tear, at most 2, 4 or 8 per refresh (1-3)
  // or unlimited (4). See gpu_set_frame_limit.
  i32                      frame_limiter = 0;
  // The cursor's size, a multiple of the skin's, 0.1 to 2 as in osu!.
  f32                      cursor_size = 1;
  // Whether a map's own combo colours replace the skin's.
  bool                     beatmap_colours = true;
  // The pen, read by circles itself rather than through its driver's
  // cursor (F5; see tablet.h). Its area maps onto the whole display, or
  // onto just the window; the cursor stays in the window either way.
  bool                     pen_direct = false;
  bool                     tablet_full_screen = true;
  f32                      tablet_x = kTabletWidth / 2;  // the area, in mm
  f32                      tablet_y = kTabletHeight / 2;
  f32                      tablet_width = kTabletWidth;
  f32                      tablet_height = kTabletHeight;
  f32                      tablet_rotation = 0;  // degrees clockwise
  f32                      tablet_tip = 1;  // pressure percent that clicks
  // Whether the area was set, by hand or by importing OpenTabletDriver's;
  // until then, reading the pen imports it.
  bool                     tablet_area_set = false;
  struct Pen {
    TabletMapping          mapping;
    std::vector<PenReport> reports;  // being handled
    bool                   in_range = false, tip = false;
    f64                    last = -1;  // when its last report arrived
  } pen;
  struct SettingsMenu {
    bool open = false;
    f64  changed = -1e9;  // wall seconds
    bool dragging = false;
    i32  selected = 0;  // the row the keys change
    i32  page = 0;
  } settings_menu;

  MainMenu               menu;
  SongSelect             select;
  Gameplay               game;
  Results                results;

  Vec2                   cursor{};
  f64                    cursor_moved = -1e9;  // wall seconds
  std::deque<TrailPoint> trail;

  i32                    fps = 0;          // frames drawn per second
  i32                    update_rate = 0;  // game loop passes per second
  i32                    fps_frames = 0;
  i32                    updates = 0;
  f64                    fps_window_start = 0;
  Readout                readout;
  std::string            toast;
  f64                    toast_time = -1e9;
  bool                   quit = false;
};

// app.cc
bool app_init(App& app);
// A pass of the game loop: input handled, the game advanced. Every pass
// updates; app_draw draws when gpu_frame_due says.
void app_update(App& app, f64 now);
// False if there was nothing to draw into yet.
bool app_draw(App& app);
void app_shutdown(App& app);
void app_key(App& app, i32 key, i32 action);
void app_char(App& app, u32 codepoint);
void app_mouse_button(App& app, i32 button, i32 action);
void app_scroll(App& app, f64 dy);
void app_cursor(App& app, f64 x, f64 y);
void app_focus(App& app, bool focused);
// Files dropped on the window: .osz beatmap archives are imported.
void app_drop(App& app, std::span<const char* const> paths);

// window_macos.mm
void toggle_fullscreen(App& app);
// The display the window is on, in the window's points: its top-left
// relative to the window's content, and its size.
void window_screen(App& app, Vec2& origin, Vec2& size);

// Fades out, switches, and fades back in.
void switch_screen(App& app, Screen screen);
// Watches `path` silently while profiling: a replay, or Auto playing a .osu.
// False if it or its map can't be found.
bool app_start_benchmark(App& app, const std::string& path);
// Stops profiling and saves it, returning the frames CSV's path.
std::string app_finish_benchmark(App& app);
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
// Writes `replay` of `map` to replays/ and lists it. Returns its path, or
// nothing if it couldn't be written.
std::string save_replay(App& app, i32 map, Replay replay);
// The local leaderboard of `map`: its replays, best score first. The
// replays folder is the score store, each .osr holding its score. If it
// outgrows scanning at startup, or scores need data an .osr can't hold, a
// database (e.g. SQLite) may be worth adding.
std::vector<const ReplayEntry*> map_replays(const App& app, i32 map);

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
// Hidden and Flashlight make SS and S grades silver, as in osu!.
inline bool silver_grade(Grade grade, Mods mods) {
  return (grade == Grade::SS || grade == Grade::S) &&
         mods & (kHidden | kFlashlight);
}
Color grade_colour(Grade grade, Mods mods = 0);
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

// mod_select.cc
void mod_select_update(App& app);
void mod_select_draw(App& app);
void mod_select_key(App& app, i32 key, i32 action);
void mod_select_click(App& app, i32 button, i32 action);
// The button's icon centred on (cx, cy): its skin image, or its acronym on
// its group's colour. `state` is as mod_button_state gives it.
void draw_mod_icon(App&             app,
                   const ModButton& button,
                   i32              state,
                   f32              cx,
                   f32              cy,
                   f32              height,
                   f32              alpha);
// An icon for each of `mods` in a row; `x` is its left edge, centre or right
// edge, per `align`. Returns the row's width.
// A row of the mods' icons. With `elapsed`, seconds since they began to
// appear, each grows in from double size half a second after the last.
f32 draw_mod_icons(App&  app,
                   Mods  mods,
                   f32   x,
                   f32   cy,
                   f32   height,
                   Align align,
                   f32   elapsed = 1e9f);
// A mod icon's height at the skin's own size, as osu!stable draws it.
f32 mod_icon_height(const App& app);

// gameplay.cc
// Plays the map, or watches the replay at `replay_path` on it.
bool gameplay_start(App& app, i32 map, std::string replay_path = {});
// Judges the replay at `replay_path` silently, start to end, without touching
// the music. False if it couldn't be loaded or the replay failed.
bool judge_replay(App& app, i32 map, std::string replay_path);
void gameplay_update(App& app);
void gameplay_draw(App& app);
// `wall` is when the input arrived, for timing the hit.
void gameplay_key(App& app, i32 key, i32 action, f64 wall);
void gameplay_click(App& app, i32 button, i32 action, f64 wall);
void gameplay_focus_lost(App& app);

// The pen read directly or through its driver; its area changed; and
// OpenTabletDriver's area and tip threshold imported, false if it has none.
void set_pen_direct(App& app, bool on);
void update_tablet_mapping(App& app);
bool import_tablet_settings(App& app);

// The frame limiter's choices, as the settings show them, in
// app.frame_limiter's order.
inline constexpr const char* kFrameLimiterNames[] = {
    "VSync", "2x refresh", "4x refresh", "8x refresh", "Unlimited"};
// Applies app.frame_limiter.
void apply_frame_limiter(App& app);

// settings_menu.cc
void toggle_settings(App& app);
void settings_key(App& app, i32 key, i32 action);
void settings_click(App& app, i32 button, i32 action);
void settings_cursor(App& app);
void settings_draw(App& app);

// results.cc
// Shows a saved score's results, its hit errors from judging its replay.
void open_score(App& app, i32 map, const ReplayEntry& entry);
void results_enter(App& app);
void results_update(App& app);
void results_draw(App& app);
void results_key(App& app, i32 key, i32 action);
void results_click(App& app, i32 button, i32 action);

}  // namespace circles
