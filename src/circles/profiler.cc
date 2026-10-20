#include <circles/metal.h>
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
  profiler.query_frame.fill(-1);
  install_input_monitor(profiler);
}

void profiler_start(Profiler& profiler, bool overlay) {
  profiler.active = true;
  profiler.overlay = overlay;
  profiler.started = uptime();
  profiler.frames.clear();
  profiler.inputs.clear();
  profiler.query_frame.fill(-1);
  profiler.in_frame = false;
  profiler.gpu_frame_count = 0;
  // Every sample the devices send, not one per poll.
  set_mouse_coalescing(false);
}

namespace {

const char* const kPhaseNames[] = {"poll", "update", "draw", "swap", "sleep"};
const char* const kInputKinds[] = {"key_down", "key_up", "button_down",
                                   "button_up", "move"};
const char* const kInputDevices[] = {"keyboard", "mouse", "tablet"};

// Collects finished GPU timings into their frames.
void collect_gpu_times(Profiler& profiler, bool wait) {
  for (i32 slot = 0; slot < Profiler::kQueries; ++slot) {
    const i64 frame = profiler.query_frame[slot];
    if (frame < 0)
      continue;
    const f32 elapsed = gpu_query_time(slot, wait);
    if (elapsed < 0)
      continue;
    if (frame < static_cast<i64>(profiler.frames.size()))
      profiler.frames[frame].gpu_ms = elapsed;
    profiler.query_frame[slot] = -1;
  }
}

}  // namespace

std::string profiler_stop(Profiler& profiler, const std::string& directory) {
  if (!profiler.active)
    return {};
  collect_gpu_times(profiler, true);
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
  frames << ",gpu_ms,draw_calls,vertices,inputs,screen,rendered\n";
  for (size_t i = 0; i < profiler.frames.size(); ++i) {
    const FrameRecord& f = profiler.frames[i];
    frames << i << ',' << (f.start - origin) * 1000;
    for (f32 ms : f.phase_ms)
      frames << ',' << ms;
    frames << ',' << f.gpu_ms << ',' << f.draw_calls << ',' << f.vertices << ','
           << f.inputs << ',' << static_cast<i32>(f.screen) << ',' << f.rendered
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

void profile_gpu_begin(Profiler& profiler) {
  if (!profiler.active || !profiler.in_frame)
    return;
  if (profiler.gpu_frame_count++ % Profiler::kGpuSampleEvery)
    return;
  collect_gpu_times(profiler, false);
  const i32 slot = profiler.next_query;
  // A query still in flight a whole ring later is waited for.
  if (profiler.query_frame[slot] >= 0)
    collect_gpu_times(profiler, true);
  if (!gpu_query_begin(slot))
    return;
  profiler.query_frame[slot] = static_cast<i64>(profiler.frames.size());
  profiler.gpu_timing = true;
}

void profile_gpu_end(Profiler& profiler) {
  if (!profiler.gpu_timing)
    return;
  profiler.gpu_timing = false;
  profiler.next_query = (profiler.next_query + 1) % Profiler::kQueries;
}

void profile_frame_end(Profiler& profiler,
                       u32       draw_calls,
                       u32       vertices,
                       bool      rendered) {
  if (!profiler.active || !profiler.in_frame)
    return;
  profiler.current.draw_calls = draw_calls;
  profiler.current.vertices = vertices;
  profiler.current.rendered = rendered;
  profiler.frames.push_back(profiler.current);
  profiler.in_frame = false;
}

}  // namespace circles
