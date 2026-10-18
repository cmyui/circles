#include <circles/app.h>

#include <chrono>
#include <memory>
#include <thread>

namespace {

circles::App& app_of(GLFWwindow* window) {
  return *static_cast<circles::App*>(glfwGetWindowUserPointer(window));
}

}  // namespace

int main() {
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

  // GLFW delivers input when a frame polls for it, so a hit's time is only as
  // precise as the frame rate. Frames run up to 1000 per second, sleeping
  // between them rather than spinning a core.
  constexpr auto kFrameInterval = std::chrono::microseconds(1000);
  auto           next_frame = std::chrono::steady_clock::now();
  while (!glfwWindowShouldClose(window) && !app->quit) {
    glfwPollEvents();
    circles::app_frame(*app, glfwGetTime());
    glfwSwapBuffers(window);
    next_frame += kFrameInterval;
    const auto now = std::chrono::steady_clock::now();
    if (next_frame < now)
      next_frame = now;
    else
      std::this_thread::sleep_until(next_frame);
  }
  circles::app_shutdown(*app);
}
