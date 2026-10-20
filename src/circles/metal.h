#pragma once

#include <circles/types.h>

#include <array>
#include <span>
#include <vector>

struct GLFWwindow;

namespace circles {
struct PieceInstance;

// Call this API on the main thread. Only drawable acquisition uses a worker;
// rendering and presentation submission use the latest main-thread game state.
// Command buffers retain resources through GPU completion.
void gpu_init(GLFWwindow* window);
void gpu_shutdown();
void gpu_set_vsync(bool enabled);
// Nonblocking for onscreen rendering; false means continue updating the game
// and try again later. Offscreen rendering is reserved for readback tests.
bool gpu_begin_frame(bool offscreen = false);
bool gpu_present();
// Render the queued frame into RGBA8 pixels instead of presenting. For renderer
// regression checks; waits for the GPU and does not affect the normal frame
// path.
std::vector<u8> gpu_readback_frame(i32 width, i32 height);
u32 gpu_upload(const u8* pixels, i32 width, i32 height, bool red, bool mipmaps);
void gpu_destroy_texture(u32 texture);
u32 gpu_create_target(i32 width, i32 height);
void gpu_clear_target(u32 target);
void gpu_copy_target(u32 source, u32 destination, i32 width, i32 height);
void gpu_draw_sprites(std::span<const f32>      vertices,
                      const std::array<u32, 8>& textures,
                      f32                       width,
                      f32                       height);
void gpu_draw_body(u32                            target,
                   std::span<const PieceInstance> pieces,
                   u32                            gradient,
                   f32                            radius,
                   std::array<f32, 4>             region,
                   std::array<f32, 4>             squash);
bool gpu_query_begin(i32 slot);
f32 gpu_query_time(i32 slot, bool wait);
}  // namespace circles
