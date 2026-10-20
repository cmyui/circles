// Requires a logged-in macOS graphics session; run separately from CPU tests.
#include <circles/app.h>
#include <tests/support/test.h>

#include <algorithm>
#include <memory>

using namespace circles;

namespace {

std::vector<u8> render(bool batching, bool uploads) {
  auto app = std::make_unique<App>();
  app->window =
      glfwCreateWindow(128, 96, "OpenGL pixel checks", nullptr, nullptr);
  if (!app->window) {
    CHECK(false);
    return {};
  }
  glfwMakeContextCurrent(app->window);
  glfwSwapInterval(0);
  app->width = 128;
  app->height = 96;
  glfwGetFramebufferSize(app->window, &app->pixel_width, &app->pixel_height);
  app->renderer = create_renderer();
  app->renderer.batch_sliders = batching;
  app->renderer.reuse_uploads = uploads;
  app->slider_bodies = create_slider_body_renderer();
  app->game.radius = 4;
  app->game.objects.resize(12);
  for (size_t i = 0; i < 12; ++i) {
    const Vec2 p{10.0f + (i % 3) * 40.0f, 12.0f + (i / 3) * 20.0f};
    app->game.objects[i].body = {.lines = {{p, {p.x + 30, p.y}, 0}},
                                 .lengths = {30},
                                 .length = 30,
                                 .piece_ends = {0}};
  }
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  std::vector<u8> all;
  for (int frame = 0; frame < 36; ++frame) {
    glViewport(0, 0, app->pixel_width, app->pixel_height);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    begin_frame(app->renderer, app->width, app->height);
    const f32 snake = frame == 0 ? 0.5f : (frame % 9 + 1) / 9.0f;
    for (size_t i = 0; i < 12; ++i) {
      draw_slider_body(*app, i, {1, 0.1f * (i % 3), 0, 1}, snake, 1, {0, 0},
                       {0, 0}, 1);
      // Painter order must survive batch breaks at eight texture slots.
      draw_rect(app->renderer, 20 + (i % 3) * 40, 9 + (i / 3) * 20, 4, 6,
                {0, 0, 1, 0.5f});
    }
    if (frame == 4) {
      // Exceed both the sprite batch and the reusable upload block capacity.
      for (int i = 0; i < 9000; ++i)
        draw_rect(app->renderer, 124, 92, 1, 1, {0, 1, 0, 1});
    }
    flush(app->renderer);
    end_frame(app->renderer);
    // Leave several frames in flight between captures to exercise the fences.
    if (frame == 0 || frame == 4 || frame == 8 || frame == 17 || frame == 35) {
      std::vector<u8> pixels(app->pixel_width * app->pixel_height * 4);
      glReadPixels(0, 0, app->pixel_width, app->pixel_height, GL_RGBA,
                   GL_UNSIGNED_BYTE, pixels.data());
      // A snaking body must reach the screen, not be drawn into itself.
      const size_t at = ((96 - 12) * app->pixel_height / 96 * app->pixel_width +
                         14 * app->pixel_width / 128) *
                        4;
      CHECK(pixels[at] > 30);
      CHECK_EQ(glGetError(), GL_NO_ERROR);
      all.insert(all.end(), pixels.begin(), pixels.end());
    }
    glfwSwapBuffers(app->window);
    sweep_slider_bodies(*app);
  }
  release_slider_bodies(app->game.bodies);
  destroy_upload_arena(app->renderer.uploads);
  glfwDestroyWindow(app->window);
  return all;
}

}  // namespace

int main() {
  if (!glfwInit())
    return 1;
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
  glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
  const auto baseline = render(false, false);
  for (const auto [batching, uploads] :
       {std::pair{true, false}, std::pair{false, true},
        std::pair{true, true}}) {
    const auto pixels = render(batching, uploads);
    CHECK_EQ(pixels.size(), baseline.size());
    size_t different = 0;
    int    max_error = 0;
    for (size_t i = 0; i < std::min(pixels.size(), baseline.size()); ++i) {
      const int error = std::abs(int(pixels[i]) - int(baseline[i]));
      different += error > 1;
      max_error = std::max(max_error, error);
    }
    std::printf("batch=%d reuse=%d: channels differing >1=%zu, max=%d\n",
                batching, uploads, different, max_error);
    CHECK_EQ(different, 0u);
  }
  glfwTerminate();
  return test_result();
}
