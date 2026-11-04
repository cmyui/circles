#pragma once

// Where a frame's time goes, and how input arrives. While profiling, each
// pass of the game loop records its CPU phases and, when it draws, its GPU
// time, draw calls and how long the drawing took to reach the display; each
// input event records its own timestamp from macOS, which GLFW doesn't pass
// on. Both are saved as CSV for tools/perf_summary.py.

#include <circles/gpu.h>
#include <circles/types.h>

#include <array>
#include <span>
#include <string>
#include <vector>

namespace circles {

// A loop pass's steps, in order; each records the time since the last ended.
// Draw and Present are empty in passes that don't draw.
enum class FramePhase : u8 { Poll, Update, Draw, Present, Sleep, Count };
inline constexpr i32 kFramePhases = static_cast<i32>(FramePhase::Count);

struct FrameRecord {
  f64                           start = 0;  // uptime seconds
  std::array<f32, kFramePhases> phase_ms{};
  f32 gpu_ms = -1;  // the frame's GPU work; -1 until it finishes
  // From starting to draw to the display showing it, where Metal reports it
  // (full screen); -1 otherwise.
  f32 shown_ms = -1;
  u32 draw_calls = 0;
  u32 vertices = 0;
  u32 inputs = 0;  // input events its poll delivered
  u8  screen = 0;
};

enum class InputKind : u8 { KeyDown, KeyUp, ButtonDown, ButtonUp, Move };
enum class InputDevice : u8 { Keyboard, Mouse, Tablet };

struct InputRecord {
  f64         event_time;  // uptime seconds, when macOS stamped the event
  f64         received;    // uptime seconds, when circles saw it
  u32         frame;       // index of the frame whose poll delivered it
  InputKind   kind;
  InputDevice device;
};

struct Profiler {
  bool                     active = false;   // recording
  bool                     overlay = false;  // shown while recording
  f64                      started = 0;      // uptime seconds
  std::vector<FrameRecord> frames;
  std::vector<InputRecord> inputs;
  FrameRecord              current;
  f64                      phase_start = 0;
  bool                     in_frame = false;
};

// Seconds since boot, on the clock macOS stamps input events with.
f64 uptime();

// Watches input events for their timestamps; call once.
void profiler_init(Profiler& profiler);
void profiler_start(Profiler& profiler, bool overlay);
// Stops and saves the recording to `directory`, returning the frames CSV's
// path, or nothing if it wasn't recording or couldn't be written.
std::string profiler_stop(Profiler& profiler, const std::string& directory);

void profile_frame_begin(Profiler& profiler, u8 screen);
void profile_phase(Profiler& profiler, FramePhase phase);
// What gpu_end_frame calls the frame being drawn, so its GPU times find it.
i64 profile_frame_tag(const Profiler& profiler);
// Puts the GPU's reports into the frames they're about.
void profile_gpu_times(Profiler& profiler, std::span<const GpuFrameTime> times);
void profile_frame_end(Profiler& profiler, u32 draw_calls, u32 vertices);

// macOS's timestamp (uptime seconds) for the key or button event being
// handled, or a negative value before any.
f64 last_input_event_time();

// An input event as macOS stamped it, and when circles read it; both uptime
// seconds.
struct InputTiming {
  f64 event_time;
  f64 received;
};
// Since the last call: presses of keys, modifiers included, and every mouse
// button, not key repeats; and cursor movements, from a mouse or tablet.
void take_input_presses(std::vector<InputTiming>& presses);
void take_input_moves(std::vector<InputTiming>& moves);

// Raw reports from pointing devices (mice, trackpads, tablets) as they reach
// macOS, before any driver turns them into cursor movement. Reading them
// needs the Input Monitoring permission: listen_to_pointers asks for it the
// first time, and listens once it's granted.
enum class PointerAccess : u8 { Asked, Denied, Listening };
PointerAccess listen_to_pointers();
// When reports arrived since the last call, in uptime seconds.
void          take_pointer_reports(std::vector<f64>& times);

// The slowest report interval, in ms, that a connected keyboard tells macOS
// it sends at; 0 if none says.
f64 keyboard_report_interval();

// Platform glue (profiler_macos.mm).
void install_input_monitor(Profiler& profiler);
void set_mouse_coalescing(bool enabled);

}  // namespace circles
