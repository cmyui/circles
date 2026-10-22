#include <circles/app.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

namespace {

circles::App& app_of(GLFWwindow* window) {
  return *static_cast<circles::App*>(glfwGetWindowUserPointer(window));
}

// For measuring: `--benchmark <replay.osr>` watches a replay silently while
// profiling and exits at its results, saving the profile to perf/; given a
// .osu, it watches Auto play the map.
// `--size WxH` sets the window, `--uncapped` lifts the frame cap and
// `--vsync` waits for the display.
struct Options {
  std::string benchmark;
  int         benchmark_seconds = 0;
  int         width = 0;
  int         height = 0;
  bool        uncapped = false;
  bool        vsync = false;
  bool        skip_intro = false;
  bool        batch_sliders = false;
  bool        reuse_uploads = false;
};

bool parse_options(int argc, char** argv, Options& options) {
  for (int i = 1; i < argc; ++i) {
    const bool has_value = i + 1 < argc;
    if (!std::strcmp(argv[i], "--benchmark") && has_value)
      options.benchmark = argv[++i];
    else if (!std::strcmp(argv[i], "--benchmark-seconds") && has_value) {
      char extra;
      if (std::sscanf(argv[++i], "%d%c", &options.benchmark_seconds, &extra) !=
              1 ||
          options.benchmark_seconds <= 0)
        return false;
    } else if (!std::strcmp(argv[i], "--size") && has_value &&
               std::sscanf(argv[++i], "%dx%d", &options.width,
                           &options.height) == 2)
      continue;
    else if (!std::strcmp(argv[i], "--uncapped"))
      options.uncapped = true;
    else if (!std::strcmp(argv[i], "--vsync"))
      options.vsync = true;
    else if (!std::strcmp(argv[i], "--skip-intro"))
      options.skip_intro = true;
    else if (!std::strcmp(argv[i], "--batch-sliders"))
      options.batch_sliders = true;
    else if (!std::strcmp(argv[i], "--reuse-uploads"))
      options.reuse_uploads = true;
    else {
      std::fprintf(
          stderr,
          "usage: circles [--benchmark replay.osr|map.osu] [--size WxH] "
          "[--uncapped] [--vsync] [--benchmark-seconds N] [--skip-intro] "
          "[--batch-sliders] [--reuse-uploads]\n");
      return false;
    }
  }
  return options.benchmark_seconds == 0 || !options.benchmark.empty();
}

}  // namespace

int main(int argc, char** argv) {
  const auto process_started = std::chrono::steady_clock::now();
  Options    options;
  if (!parse_options(argc, argv, options))
    return 2;
  // On the heap so the audio engine inside never moves.
  auto app = std::make_unique<circles::App>();
  if (!circles::app_init(*app))
    return 1;
  app->renderer.batch_sliders = options.batch_sliders;
  app->renderer.reuse_uploads = options.reuse_uploads;
  GLFWwindow* window = app->window;
  glfwSetWindowUserPointer(window, app.get());
  glfwSetKeyCallback(window, [](GLFWwindow* w, int key, int, int action, int) {
    circles::app_key(app_of(w), key, action);
  });
  glfwSetCharCallback(window, [](GLFWwindow* w, unsigned codepoint) {
    circles::app_char(app_of(w), codepoint);
  });
  glfwSetMouseButtonCallback(
      window, [](GLFWwindow* w, int button, int action, int) {
        circles::app_mouse_button(app_of(w), button, action);
      });
  glfwSetScrollCallback(window, [](GLFWwindow* w, double, double dy) {
    circles::app_scroll(app_of(w), dy);
  });
  glfwSetCursorPosCallback(window, [](GLFWwindow* w, double x, double y) {
    circles::app_cursor(app_of(w), x, y);
  });
  // Benchmarks keep running when the window loses focus; controls stay live.
  if (options.benchmark.empty())
    glfwSetWindowFocusCallback(window, [](GLFWwindow* w, int focused) {
      circles::app_focus(app_of(w), focused);
    });
  glfwSetDropCallback(window, [](GLFWwindow* w, int count, const char** paths) {
    circles::app_drop(app_of(w), {paths, static_cast<size_t>(count)});
  });
  if (options.width > 0 && options.height > 0)
    glfwSetWindowSize(window, options.width, options.height);
  app->vsync = options.vsync;
  glfwSwapInterval(app->vsync ? 1 : 0);
  if (!options.benchmark.empty() &&
      !circles::app_start_benchmark(*app, options.benchmark)) {
    std::fprintf(stderr, "can't benchmark %s: no such replay or map\n",
                 options.benchmark.c_str());
    circles::app_shutdown(*app);
    return 1;
  }

  // Frames run up to 1000 per second, sleeping between them rather than
  // spinning a core, unless the cap is turned off in settings (or for a
  // benchmark, --uncapped). Presses are judged by their own timestamps, so
  // the frame rate doesn't limit their precision.
  if (options.uncapped)
    app->frame_cap = false;
  auto  next_frame = std::chrono::steady_clock::now();
  auto& profiler = app->profiler;
  bool  skipped_intro = false;
  using circles::FramePhase;
  while (!glfwWindowShouldClose(window) && !app->quit) {
    if (options.skip_intro && !skipped_intro &&
        app->screen == circles::Screen::Gameplay) {
      circles::gameplay_skip_intro(*app, false);
      skipped_intro = true;
      if (!options.benchmark.empty())
        circles::profiler_start(profiler, false);
      std::printf("benchmark start: %.0f ms; first object: %.0f ms\n",
                  app->game.clock_time, app->game.objects.front().time);
    }
    circles::profile_frame_begin(profiler,
                                 static_cast<circles::u8>(app->screen));
    glfwPollEvents();
    circles::profile_phase(profiler, FramePhase::Poll);
    circles::app_frame(*app, glfwGetTime());
    glfwSwapBuffers(window);
    circles::profile_phase(profiler, FramePhase::Swap);
    next_frame += std::chrono::microseconds(app->frame_cap ? 1000 : 0);
    const auto now = std::chrono::steady_clock::now();
    if (next_frame < now)
      next_frame = now;
    else
      std::this_thread::sleep_until(next_frame);
    circles::profile_phase(profiler, FramePhase::Sleep);
    circles::profile_frame_end(profiler, app->renderer.draw_calls,
                               app->renderer.vertices_drawn);
    const bool timed_out = options.benchmark_seconds > 0 &&
                           std::chrono::steady_clock::now() - process_started >=
                               std::chrono::seconds(options.benchmark_seconds);
    if (!options.benchmark.empty() &&
        (app->screen == circles::Screen::Results || timed_out)) {
      const std::string path = circles::app_finish_benchmark(*app);
      std::printf("profile: %s\n", path.empty()
                                       ? "stopped early (saved with F8)"
                                       : path.c_str());
      break;
    }
  }
  circles::app_shutdown(*app);
}
