#include <circles/gfx.h>
#include <circles/metal.h>
#include <circles/slider_body.h>
#include <tests/support/test.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <array>
#include <cstdio>

using namespace circles;

namespace {
constexpr i32 kSize = 128;
void pixel(const std::vector<u8>& image,
           i32                    x,
           i32                    y,
           std::array<i32, 3>     expected,
           i32                    tolerance = 2) {
  for (i32 channel = 0; channel < 3; ++channel)
    CHECK_NEAR(static_cast<i32>(image[(y * kSize + x) * 4 + channel]),
               expected[channel], tolerance);
}
void start(Renderer& renderer) {
  CHECK(gpu_begin_frame(true));
  begin_frame(renderer, kSize, kSize);
}
std::vector<u8> finish(Renderer& renderer) {
  flush(renderer);
  return gpu_readback_frame(kSize, kSize);
}
void show_target(Renderer& renderer, u32 target) {
  draw_image_region(renderer, {.id = target, .width = kSize, .height = kSize},
                    0, 0, kSize, kSize, 0, 0, 1, 1, kWhite);
}
}  // namespace

int main() {
  CHECK(glfwInit());
  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
  glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
  GLFWwindow* window = glfwCreateWindow(kSize, kSize, "Metal regression checks",
                                        nullptr, nullptr);
  CHECK(window);
  if (!window)
    return test_result();
  gpu_init(window);
  Renderer      renderer = create_renderer();

  // A 2x2 colour grid detects both vertical flips and wrong texture slots.
  const Texture grid =
      upload_texture({.pixels = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255,
                                 255, 255, 255, 255},
                      .width = 2,
                      .height = 2});
  start(renderer);
  draw_rect(renderer, 0, 0, kSize, kSize, {0, 0, 1, 1});
  draw_rect(renderer, 0, 0, 32, 32, {1, 0, 0, 0.5f});
  draw_image_region(renderer, grid, 64, 0, 64, 64, 0, 0, 1, 1, kWhite);
  draw_circle(renderer, 32, 96, 16, {0, 1, 0, 1});
  draw_rounded_rect(renderer, 64, 80, 48, 32, 8, {1, 1, 0, 1});
  auto image = finish(renderer);
  pixel(image, 16, 16, {128, 0, 127});
  pixel(image, 68, 4, {255, 0, 0});
  pixel(image, 124, 4, {0, 255, 0});
  pixel(image, 68, 60, {0, 0, 255});
  pixel(image, 124, 60, {255, 255, 255});
  pixel(image, 32, 96, {0, 255, 0});
  pixel(image, 12, 96, {0, 0, 255});
  pixel(image, 88, 96, {255, 255, 0});
  pixel(image, 64, 80, {0, 0, 255});

  // Exceed the eight texture slots; retire textures after encoding and reuse
  // their handles before submission. Recorded draws must keep their resources.
  start(renderer);
  std::vector<Texture> batch_textures;
  for (i32 i = 0; i < 12; ++i) {
    auto t = upload_texture({.pixels = {static_cast<u8>(i * 20), 0, 0, 255},
                             .width = 1,
                             .height = 1});
    draw_image_region(renderer, t, i * 10, 0, 10, 10, 0, 0, 1, 1, kWhite);
    batch_textures.push_back(t);
    // Keep the CPU batch intact until all texture slots have been exercised.
  }
  flush(renderer);
  image = finish(renderer);
  for (i32 i = 0; i < 12; ++i)
    pixel(image, i * 10 + 5, 5, {i * 20, 0, 0});
  for (auto& t : batch_textures)
    destroy_texture(t);
  start(renderer);
  auto red =
      upload_texture({.pixels = {255, 0, 0, 255}, .width = 1, .height = 1});
  draw_image_region(renderer, red, 0, 0, 64, 64, 0, 0, 1, 1, kWhite);
  flush(renderer);
  destroy_texture(red);
  auto green =
      upload_texture({.pixels = {0, 255, 0, 255}, .width = 1, .height = 1});
  // A fresh batch must see the replacement, even if the integer handle matches.
  begin_frame(renderer, kSize, kSize);
  draw_image_region(renderer, green, 64, 0, 64, 64, 0, 0, 1, 1, kWhite);
  image = finish(renderer);
  pixel(image, 16, 16, {255, 0, 0});
  pixel(image, 80, 16, {0, 255, 0});

  // Cache + changing tip: copy colour AND depth, then cross the cached line.
  // At the crossing, the nearest centre line must win, independently of order.
  Image ramp{.width = 128, .height = 1};
  for (i32 i = 0; i < 128; ++i)
    ramp.pixels.insert(ramp.pixels.end(), {static_cast<u8>(i * 2), 0, 0, 255});
  const u32 gradient = gpu_upload(ramp.pixels.data(), 128, 1, false, false);
  const u32 cached = gpu_create_target(kSize, kSize);
  const u32 scratch = gpu_create_target(kSize, kSize);
  const std::array<f32, 4> region{0, 0, kSize, kSize}, squash{0, 0, 1, 1};
  const PieceInstance horizontal{{16, 48}, {112, 48}, 1, 0, 96, 24, 0, 1, 0};
  const PieceInstance vertical{{64, 16}, {64, 112}, 0, 1, 96, 24, 0, 1, 0};
  start(renderer);
  gpu_clear_target(cached);
  gpu_draw_body(cached, {&horizontal, 1}, gradient, 12, region, squash);
  gpu_copy_target(cached, scratch, kSize, kSize);
  gpu_draw_body(scratch, {&vertical, 1}, gradient, 12, region, squash);
  show_target(renderer, scratch);
  auto forward = finish(renderer);
  pixel(forward, 24, 48, {240, 0, 0}, 15);
  pixel(forward, 64, 96, {240, 0, 0}, 15);
  pixel(forward, 64, 48, {240, 0, 0}, 15);
  pixel(forward, 4, 4, {0, 0, 0});
  start(renderer);
  gpu_clear_target(scratch);
  gpu_draw_body(scratch, {&vertical, 1}, gradient, 12, region, squash);
  gpu_draw_body(scratch, {&horizontal, 1}, gradient, 12, region, squash);
  show_target(renderer, scratch);
  auto reverse = finish(renderer);
  CHECK(forward == reverse);

  // Repeated reuse must preserve cached contents across all frame-ring slots.
  for (i32 i = 0; i < 10; ++i) {
    start(renderer);
    show_target(renderer, cached);
    image = finish(renderer);
    pixel(image, 24, 48, {240, 0, 0}, 15);
    pixel(image, 64, 96, {0, 0, 0});
  }
  // Exercise real asynchronous submission, ring reuse, size changes and mode
  // switches. API/shader validation checks the in-flight resource lifetimes.
  glfwShowWindow(window);
  glfwIconifyWindow(window);
  double deadline = glfwGetTime() + 2;
  while (!glfwGetWindowAttrib(window, GLFW_ICONIFIED) &&
         glfwGetTime() < deadline)
    glfwWaitEventsTimeout(0.01);
  CHECK(glfwGetWindowAttrib(window, GLFW_ICONIFIED));
  CHECK(!gpu_begin_frame());
  CHECK(!gpu_present());
  glfwRestoreWindow(window);
  deadline = glfwGetTime() + 2;
  while (glfwGetWindowAttrib(window, GLFW_ICONIFIED) &&
         glfwGetTime() < deadline)
    glfwWaitEventsTimeout(0.01);
  CHECK(!glfwGetWindowAttrib(window, GLFW_ICONIFIED));
  for (i32 frame = 0; frame < 150; ++frame) {
    if (frame == 40)
      glfwSetWindowSize(window, 192, 160);
    if (frame == 80)
      gpu_set_vsync(true);
    if (frame == 90)
      gpu_set_vsync(false);
    if (frame == 110)
      glfwSetWindowSize(window, kSize, kSize);
    if (frame == 120) {
      GLFWmonitor* monitor = glfwGetPrimaryMonitor();
      const auto*  mode = glfwGetVideoMode(monitor);
      glfwSetWindowMonitor(window, monitor, 0, 0, mode->width, mode->height,
                           mode->refreshRate);
    }
    if (frame == 130)
      glfwSetWindowMonitor(window, nullptr, 100, 100, kSize, kSize,
                           GLFW_DONT_CARE);
    glfwPollEvents();
    const double deadline = glfwGetTime() + 2;
    bool         began = false;
    while (!(began = gpu_begin_frame()) && glfwGetTime() < deadline)
      glfwWaitEventsTimeout(0.001);
    CHECK(began);
    if (!began)
      break;
    begin_frame(renderer, kSize, kSize);
    show_target(renderer, cached);
    draw_rect(renderer, frame % 100, 0, 10, 10, {0, 1, 0, 1});
    flush(renderer);
    CHECK(gpu_present());
    CHECK(!gpu_present());
  }
  start(renderer);
  show_target(renderer, cached);
  image = finish(renderer);
  pixel(image, 24, 48, {240, 0, 0}, 15);
  pixel(image, 64, 96, {0, 0, 0});
  gpu_destroy_texture(cached);
  gpu_destroy_texture(scratch);
  gpu_destroy_texture(gradient);
  gpu_shutdown();
  glfwDestroyWindow(window);
  glfwTerminate();
  return test_result();
}
