#pragma once

// The GPU, through Metal. Textures live there, and each frame's drawing is
// recorded here and run as one command buffer: slider bodies' passes first,
// then the screen's.
//
// Frames reach the display one of two ways, whichever is quicker:
// - Windowed, each frame is drawn into a shared surface (an IOSurface) that
//   the window compositor is handed the moment the GPU finishes it. The
//   compositor shows the newest at each refresh, so frames are drawn as often
//   as the game loop runs: the one it takes is at most a pass old.
// - Full screen, frames bypass the compositor and go straight to the display
//   through a CAMetalLayer. With vsync, the layer has two drawables, one on
//   screen and one to draw: as soon as the display frees one, the game loop
//   draws into it, and it shows whole at the next refresh. Without vsync,
//   each frame shows the moment it's drawn, from wherever the display is in
//   drawing the screen: newer, but split where it replaces the last
//   (tearing).
//
// Either way, colour values reach the display unconverted, as its own colour
// space: a wide-gamut display shows its full range.

#include <circles/types.h>

#include <chrono>
#include <vector>

struct GLFWwindow;

namespace circles {

struct GpuTexture;

enum class TextureFormat : u8 { Rgba8, R8, Depth16 };

// Sets up Metal and takes over the window's view; the window is made with
// GLFW_NO_API. False if Metal can't be used.
bool gpu_init(GLFWwindow* window);
void gpu_shutdown();

// `pixels` holds rows top first, or is null for a texture only drawn into.
// Mipmaps are made from the pixels. Releasing is safe while frames that use
// the texture are still on the GPU.
GpuTexture* gpu_create_texture(i32           width,
                               i32           height,
                               TextureFormat format,
                               const void*   pixels,
                               bool          mipmaps,
                               bool          drawn_into);
void gpu_release_texture(GpuTexture* texture);

// Whether full screen waits for the display's refresh (vsync), and without
// it, how many frames to draw per refresh, windowed too; 0 draws as often
// as there's somewhere to draw.
void gpu_set_frame_limit(bool vsync, i32 per_refresh);
// Whether to draw a frame now: the limit allows it and, full screen, the
// display has freed a drawable.
bool gpu_frame_due();
// Sleeps until `wake`, or until a frame is due.
void gpu_sleep_until(std::chrono::steady_clock::time_point wake);

// Starts recording a frame of the window's size in pixels. False if there's
// nothing to draw into, as while minimized.
bool gpu_begin_frame(i32 width, i32 height);
// Screen drawing, in order: triangles of sprite vertices (12 floats each,
// laid out as gfx.cc writes them) sampling up to 8 textures by slot.
void gpu_draw_sprites(const f32*               vertices,
                      u32                      vertex_count,
                      const GpuTexture* const* textures,
                      i32                      texture_count,
                      f32                      screen_width,
                      f32                      screen_height);

// Slider bodies' drawing, run before the screen's.
struct PieceUniforms {
  f32 region[4];  // the framebuffer area drawn: x, y, width, height
  f32 squash[4];  // origin xy, scale zw; see BodySquash
  f32 radius;     // pixels
  f32 padding[3];
};
void gpu_clear_target(GpuTexture* colour, GpuTexture* depth);
void gpu_copy_target(GpuTexture* from_colour,
                     GpuTexture* from_depth,
                     GpuTexture* to_colour,
                     GpuTexture* to_depth);
// Draws `count` pieces of `stride` bytes each, `vertices` vertices apiece,
// into the target, shaded by `gradient`: the depth test keeps the nearest.
void gpu_draw_pieces(GpuTexture*          colour,
                     GpuTexture*          depth,
                     const void*          pieces,
                     u32                  count,
                     u32                  stride,
                     u32                  vertices,
                     const PieceUniforms& uniforms,
                     const f32*           rim,  // 25 points, xy
                     GpuTexture*          gradient);

// Sends the frame to the GPU and on to the display. `tag` names the frame in
// gpu_take_frame_times.
void gpu_end_frame(i64 tag);

// Times are seconds of uptime, the clock macOS stamps input events with.
struct GpuFrameTime {
  i64 tag;
  f32 gpu_ms;  // the GPU's time on the frame, or -1 in a report of `shown`
  f64 drawn;   // when its drawing started
  // When the display showed it, as Metal reports it; -1 until then, and
  // windowed, where the compositor doesn't say.
  f64 shown;
};
// Reports since the last call: one when the GPU finishes a frame, and full
// screen, another once it's shown.
void gpu_take_frame_times(std::vector<GpuFrameTime>& times);

}  // namespace circles
