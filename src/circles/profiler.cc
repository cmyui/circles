#include <circles/gpu.h>
#include <circles/profiler.h>

#include <ctime>
#include <filesystem>
#include <fstream>
#include <time.h>

namespace circles {

f64 uptime() {
  return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) * 1e-9;
}

void profiler_init(Profiler& profiler) {
  install_input_monitor(profiler);
}

void profiler_start(Profiler& profiler, bool overlay) {
  profiler.active = true;
  profiler.overlay = overlay;
  profiler.started = uptime();
  profiler.frames.clear();
  profiler.inputs.clear();
  profiler.in_frame = false;
  // Every sample the devices send, not one per poll.
  set_mouse_coalescing(false);
}

namespace {

const char* const kPhaseNames[] = {"poll", "update", "draw", "present",
                                   "sleep"};
const char* const kInputKinds[] = {"key_down", "key_up", "button_down",
                                   "button_up", "move"};
const char* const kInputDevices[] = {"keyboard", "mouse", "tablet"};

// Puts finished frames' GPU times into their records. The GPU's reports
// are taken while not profiling too, so they don't pile up.
void collect_gpu_times(Profiler& profiler) {
  static std::vector<GpuFrameTime> times;
  times.clear();
  gpu_take_frame_times(times);
  for (const GpuFrameTime& t : times) {
    if (!profiler.active || t.tag < 0 ||
        t.tag >= static_cast<i64>(profiler.frames.size()))
      continue;
    FrameRecord& frame = profiler.frames[t.tag];
    if (t.gpu_ms >= 0)
      frame.gpu_ms = t.gpu_ms;
    if (t.shown_ms >= 0)
      frame.shown_ms = t.shown_ms;
  }
}

}  // namespace

std::string profiler_stop(Profiler& profiler, const std::string& directory) {
  if (!profiler.active)
    return {};
  collect_gpu_times(profiler);
  profiler.active = false;
  set_mouse_coalescing(true);
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  char       stamp[32];
  const auto now = std::time(nullptr);
  std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", std::localtime(&now));
  const auto    base = std::filesystem::path(directory) / stamp;
  const auto    frames_path = base.string() + "-frames.csv";
  const f64     origin = profiler.started;
  std::ofstream frames(frames_path);
  if (!frames)
    return {};
  frames << "frame,start_ms";
  for (const char* name : kPhaseNames)
    frames << ',' << name << "_ms";
  frames << ",gpu_ms,shown_ms,draw_calls,vertices,inputs,screen\n";
  for (size_t i = 0; i < profiler.frames.size(); ++i) {
    const FrameRecord& f = profiler.frames[i];
    frames << i << ',' << (f.start - origin) * 1000;
    for (f32 ms : f.phase_ms)
      frames << ',' << ms;
    frames << ',' << f.gpu_ms << ',' << f.shown_ms << ',' << f.draw_calls << ','
           << f.vertices << ',' << f.inputs << ',' << static_cast<i32>(f.screen)
           << '\n';
  }
  std::ofstream inputs(base.string() + "-input.csv");
  inputs << "event_ms,received_ms,frame,kind,device\n";
  for (const InputRecord& input : profiler.inputs)
    inputs << (input.event_time - origin) * 1000 << ','
           << (input.received - origin) * 1000 << ',' << input.frame << ','
           << kInputKinds[static_cast<i32>(input.kind)] << ','
           << kInputDevices[static_cast<i32>(input.device)] << '\n';
  return frames_path;
}

void profile_frame_begin(Profiler& profiler, u8 screen) {
  collect_gpu_times(profiler);
  if (!profiler.active)
    return;
  const f64 now = uptime();
  profiler.current = {};
  profiler.current.start = now;
  profiler.current.screen = screen;
  profiler.phase_start = now;
  profiler.in_frame = true;
}

void profile_phase(Profiler& profiler, FramePhase phase) {
  if (!profiler.active || !profiler.in_frame)
    return;
  const f64 now = uptime();
  profiler.current.phase_ms[static_cast<i32>(phase)] =
      static_cast<f32>((now - profiler.phase_start) * 1000);
  profiler.phase_start = now;
}

i64 profile_frame_tag(const Profiler& profiler) {
  return profiler.active && profiler.in_frame
             ? static_cast<i64>(profiler.frames.size())
             : -1;
}

void profile_frame_end(Profiler& profiler, u32 draw_calls, u32 vertices) {
  if (!profiler.active || !profiler.in_frame)
    return;
  profiler.current.draw_calls = draw_calls;
  profiler.current.vertices = vertices;
  profiler.frames.push_back(profiler.current);
  profiler.in_frame = false;
}

}  // namespace circles
