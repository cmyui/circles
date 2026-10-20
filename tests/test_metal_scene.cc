#include <circles/app.h>
#include <circles/metal.h>
#include <tests/support/test.h>

#include <algorithm>
#include <cstdlib>
#include <memory>
using namespace circles;

int main() {
  if (!glfwInit())
    return 1;
  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
  glfwWindowHint(GLFW_VISIBLE, 0);
  auto app = std::make_unique<App>();
  app->window =
      glfwCreateWindow(640, 480, "visual comparison", nullptr, nullptr);
  CHECK(app->window);
  if (!app->window) {
    glfwTerminate();
    return test_result();
  }
  app->width = app->pixel_width = 640;
  app->height = app->pixel_height = 480;
  gpu_init(app->window);
  app->renderer = create_renderer();
  auto& renderer = app->renderer;
  auto  font = load_font("/System/Library/Fonts/Avenir Next.ttc", 5);
  auto  image = upload_texture({.pixels = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0,
                                           255, 255, 255, 255, 255, 255},
                                .width = 2,
                                .height = 2});
  app->game.radius = 9;
  app->game.objects.resize(12);
  for (size_t i = 0; i < 12; ++i) {
    Vec2 p{25.0f + (i % 4) * 150, 240.0f + (i / 4) * 60};
    app->game.objects[i].body = {.lines = {{p, {p.x + 100, p.y}, 0}},
                                 .lengths = {100},
                                 .length = 100,
                                 .piece_ends = {0}};
  }
  // Reference: OpenGL at 06838a5, 640x480 RGBA8. Images cover text, sprite
  // orientation, alpha blending, all shape kinds, slider scratch targets, batch
  // overflow and cache resets.
  for (int frame = 0; frame < 12; ++frame) {
    if (!gpu_begin_frame(true))
      return 2;
    begin_frame(renderer, 640, 480);
    draw_rect_gradient(renderer, 0, 0, 640, 480, {.05f, .1f, .2f, 1},
                       {.2f, .1f, .05f, 1});
    draw_text(renderer, font, "circles: Aa 0123456789!", 15, 35, 27, kWhite);
    draw_circle(renderer, 45, 90, 24, {1, 0, 0, .7f});
    draw_circle(renderer, 110, 90, 24, {0, 1, 0, 1}, .7f, .05f);
    draw_rounded_rect(renderer, 150, 65, 70, 50, 10, {0, 0, 1, .8f});
    draw_arc(renderer, 260, 90, 25, .6f, .73f, {1, 1, 0, 1});
    draw_image_region(renderer, image, 310, 60, 100, 60, 0, 0, 1, 1, kWhite);
    draw_sprite(renderer, image, 470, 90, 80, 50, .4f, {1, 1, 1, .8f});
    draw_line(renderer, 30, 170, 230, 195, 7, kWhite);
    draw_triangle(renderer, 290, 150, 320, 200, 260, 200, {1, .3f, .7f, 1});
    for (size_t i = 0; i < 12; ++i) {
      draw_slider_body(*app, i, {1, .1f * float(i % 4), .2f, 1},
                       float(frame % 6 + 1) / 6, 1, {0, 0}, {0, 0}, 1);
      draw_rect(renderer, 60 + (i % 4) * 150, 235 + (i / 4) * 60, 10, 20,
                {0, 0, 1, .5f});
    }
    if (frame == 5)
      for (int i = 0; i < 9000; ++i)
        draw_rect(renderer, 630, 470, 1, 1, {0, 1, 0, 1});
    draw_spotlight(renderer, 320, 240, 250, 30, {0, 0, 0, .25f});
    flush(renderer);
    auto pixels = gpu_readback_frame(640, 480);
    if (frame == 0 || frame == 5 || frame == 6 || frame == 11) {
      const auto reference =
          load_image(std::string(CIRCLES_TEST_DIR) + "/metal-scene-" +
                     std::to_string(frame) + ".png");
      CHECK_EQ(reference.width, 640);
      CHECK_EQ(reference.height, 480);
      CHECK_EQ(reference.pixels.size(), pixels.size());
      int    max_error = 0;
      size_t different = 0;
      for (size_t i = 0; i < std::min(pixels.size(), reference.pixels.size());
           ++i) {
        const int error = std::abs(int(pixels[i]) - int(reference.pixels[i]));
        max_error = std::max(max_error, error);
        different += error > 2;
      }
      std::printf("scene %d: channels differing >2=%zu, max=%d\n", frame,
                  different, max_error);
      CHECK_EQ(different, 0u);
    }
    sweep_slider_bodies(*app);
  }
  release_slider_bodies(app->game.bodies);
  destroy_texture(image);
  destroy_texture(font.atlas);
  gpu_shutdown();
  glfwDestroyWindow(app->window);
  glfwTerminate();
  return test_result();
}
