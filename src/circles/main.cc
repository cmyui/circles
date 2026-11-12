#include <circles/app.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>

#include <unistd.h>

namespace {

circles::App& app_of(GLFWwindow* window) {
  return *static_cast<circles::App*>(glfwGetWindowUserPointer(window));
}

// For measuring: `--benchmark <replay>` (an .osr or .orz) watches it silently
// while profiling and exits at its results, saving the profile to perf/;
// given a .osu, it watches Auto play the map.
// `--size WxH` sets the window, and `--uncapped` lifts the cap on game loop
// passes. `--working-dir DIR` runs in DIR, which holds beatmaps/, skins/,
// replays/ and settings.txt; without it, the game runs in its game folder,
// made if need be (macOS starts an opened app in /).
struct Options {
  std::string working_dir;
  std::string benchmark;
  int         width = 0;
  int         height = 0;
  bool        uncapped = false;
};

bool parse_options(int argc, char** argv, Options& options) {
  for (int i = 1; i < argc; ++i) {
    const bool has_value = i + 1 < argc;
    if (!std::strcmp(argv[i], "--working-dir") && has_value)
      options.working_dir = argv[++i];
    else if (!std::strcmp(argv[i], "--benchmark") && has_value)
      options.benchmark = argv[++i];
    else if (!std::strcmp(argv[i], "--size") && has_value &&
             std::sscanf(argv[++i], "%dx%d", &options.width, &options.height) ==
                 2)
      continue;
    else if (!std::strcmp(argv[i], "--uncapped"))
      options.uncapped = true;
    else {
      std::fprintf(stderr,
                   "usage: circles [--working-dir DIR] [--benchmark "
                   "replay.osr|map.osu] [--size WxH] [--uncapped]\n");
      return false;
    }
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!parse_options(argc, argv, options))
    return 2;
  if (options.working_dir.empty()) {
    options.working_dir = circles::game_folder();
    std::error_code error;
    std::filesystem::create_directories(options.working_dir, error);
  }
  if (chdir(options.working_dir.c_str()) != 0) {
    std::perror(options.working_dir.c_str());
    return 2;
  }
  // On the heap so the audio engine inside never moves.
  auto app = std::make_unique<circles::App>();
  if (!circles::app_init(*app))
    return 1;
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
  glfwSetWindowFocusCallback(window, [](GLFWwindow* w, int focused) {
    circles::app_focus(app_of(w), focused);
  });
  glfwSetDropCallback(window, [](GLFWwindow* w, int count, const char** paths) {
    circles::app_drop(app_of(w), {paths, static_cast<size_t>(count)});
  });
  if (options.width > 0 && options.height > 0)
    glfwSetWindowSize(window, options.width, options.height);
  if (!options.benchmark.empty() &&
      !circles::app_start_benchmark(*app, options.benchmark)) {
    std::fprintf(stderr, "can't benchmark %s: no such replay or map\n",
                 options.benchmark.c_str());
    circles::app_shutdown(*app);
    return 1;
  }

  // The game loop runs as often as it can, for the least latency, unless
  // the cap is turned on in settings: then up to 1000 times per second,
  // sleeping between passes rather than spinning a core (--uncapped lifts it
  // for a benchmark). Each pass handles input and advances
  // the game; it draws when gpu_frame_due says, and in full screen the loop
  // wakes early when the display frees a buffer. Presses are judged by their
  // own timestamps, so the loop's rate doesn't limit their precision.
  if (options.uncapped)
    app->cap_update_frequency = false;
  using Clock = std::chrono::steady_clock;
  auto  next_update = Clock::now();
  auto& profiler = app->profiler;
  using circles::FramePhase;
  while (!glfwWindowShouldClose(window) && !app->quit) {
    circles::profile_frame_begin(profiler,
                                 static_cast<circles::u8>(app->screen));
    glfwPollEvents();
    circles::profile_phase(profiler, FramePhase::Poll);
    circles::app_update(*app, glfwGetTime());
    circles::profile_phase(profiler, FramePhase::Update);
    const bool draw = circles::gpu_frame_due() && circles::app_draw(*app);
    next_update +=
        std::chrono::microseconds(app->cap_update_frequency ? 1000 : 0);
    const auto now = Clock::now();
    if (next_update > now)
      circles::gpu_sleep_until(next_update);
    else
      next_update = now;
    circles::profile_phase(profiler, FramePhase::Sleep);
    circles::profile_frame_end(profiler, draw ? app->renderer.draw_calls : 0,
                               draw ? app->renderer.vertices_drawn : 0);
    if (!options.benchmark.empty() && app->screen == circles::Screen::Results) {
      const std::string path = circles::app_finish_benchmark(*app);
      std::printf("profile: %s\n", path.empty()
                                       ? "stopped early (saved with F8)"
                                       : path.c_str());
      break;
    }
  }
  circles::app_shutdown(*app);
}
