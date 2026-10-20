#pragma once

// Where a frame's time goes, and how input arrives. While profiling, each
// frame records its CPU phases, its GPU time and its draw calls, and each
// input event its own timestamp from macOS, which GLFW doesn't pass on. Both
// are saved as CSV for tools/perf_summary.py.

#include <circles/types.h>

#include <array>
#include <string>
#include <vector>

namespace circles {

// A frame's steps, in order; each records the time since the last ended.
enum class FramePhase : u8 { Poll, Update, Draw, Swap, Sleep, Count };
inline constexpr i32 kFramePhases = static_cast<i32>(FramePhase::Count);

struct FrameRecord {
  f64                           start = 0;  // uptime seconds
  std::array<f32, kFramePhases> phase_ms{};
  f32  gpu_ms = -1;  // the frame's GPU work; -1 until its query reports
  u32  draw_calls = 0;
  u32  vertices = 0;
  u32  inputs = 0;  // input events its poll delivered
  u8   screen = 0;
  bool rendered = false;  // a new image was submitted for presentation
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
  bool                      active = false;   // recording
  bool                      overlay = false;  // shown while recording
  f64                       started = 0;      // uptime seconds
  std::vector<FrameRecord>  frames;
  std::vector<InputRecord>  inputs;
  FrameRecord               current;
  f64                       phase_start = 0;
  bool                      in_frame = false;

  u64                       gpu_frame_count = 0;

  static constexpr i32      kQueries = 8;
  static constexpr i32      kGpuSampleEvery = 16;
  std::array<i64, kQueries> query_frame{};
  i32                       next_query = 0;
  bool                      gpu_timing = false;
};

// Seconds since boot, on the clock macOS stamps input events with.
f64 uptime();

// Watches input events for their timestamps; call once after renderer
// initialization.
void profiler_init(Profiler& profiler);
void profiler_start(Profiler& profiler, bool overlay);
// Stops and saves the recording to `directory`, returning the frames CSV's
// path, or nothing if it wasn't recording or couldn't be written.
std::string profiler_stop(Profiler& profiler, const std::string& directory);

void profile_frame_begin(Profiler& profiler, u8 screen);
void profile_phase(Profiler& profiler, FramePhase phase);
void profile_gpu_begin(Profiler& profiler);
void profile_gpu_end(Profiler& profiler);
void profile_frame_end(Profiler& profiler,
                       u32       draw_calls,
                       u32       vertices,
                       bool      rendered);

// macOS's timestamp (uptime seconds) for the key or button event being
// handled, or a negative value before any.
f64 last_input_event_time();

// Platform glue (profiler_macos.mm).
void install_input_monitor(Profiler& profiler);
void set_mouse_coalescing(bool enabled);

}  // namespace circles
