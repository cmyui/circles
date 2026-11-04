#include <circles/gpu.h>

#import <AppKit/AppKit.h>
#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#include <simd/simd.h>
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <pthread.h>
#include <sstream>
#include <thread>

namespace circles {

struct GpuTexture {
  id<MTLTexture> texture;
};

namespace {

constexpr const char* kShaderPath = "shaders/circles.metal";
constexpr i32         kFramesInFlight = 3;
// Windowed frames. The compositor holds on to the last few it was handed for
// a while, and the game loop should always find one free to draw into.
constexpr i32         kSurfaces = 6;

struct Surface {
  IOSurfaceRef      surface = nullptr;
  id<MTLTexture>    texture;
  std::atomic<bool> busy = false;  // on the GPU
};

struct SpriteDraw {
  id<MTLBuffer>                 buffer;
  NSUInteger                    offset;
  u32                           count;
  std::array<id<MTLTexture>, 8> textures;
  simd_float2                   screen;
};

// A frame's vertex and instance data, one buffer per frame in flight.
struct Upload {
  id<MTLBuffer> buffer;
  NSUInteger    used = 0;
};

struct Gpu {
  id<MTLDevice>              device;
  id<MTLCommandQueue>        queue;
  id<MTLRenderPipelineState> sprites;
  id<MTLRenderPipelineState> pieces;
  id<MTLDepthStencilState>   nearest;  // depth test LEQUAL, writing depth
  id<MTLSamplerState>        sampler;
  id<MTLTexture>             blank;  // a white texel, for unused slots
  dispatch_semaphore_t       in_flight;
  std::array<Upload, kFramesInFlight> uploads;
  i32                                 upload_index = 0;

  NSWindow*                           window;
  NSView*                             view;
  CALayer*                            surface_layer;  // windowed
  CAMetalLayer*                       metal_layer;    // full screen
  bool                                full_screen = false;

  std::array<Surface, kSurfaces>      surfaces;
  i32                                 surface_width = 0, surface_height = 0;

  // Full screen, a thread of its own waits for the layer's next drawable;
  // the game loop draws once it has one.
  std::thread                         drawable_thread;
  std::mutex                          drawable_mutex;
  std::condition_variable             drawable_changed;
  id<CAMetalDrawable>                 next_drawable;
  bool                                want_drawables = false;
  bool                                stop = false;

  // The frame being recorded.
  id<MTLCommandBuffer>                commands;
  id<MTLTexture>                      target;
  Surface*                            surface = nullptr;
  id<CAMetalDrawable>                 drawable;
  f64                                 begun = 0;
  std::vector<SpriteDraw>             draws;

  std::mutex                          times_mutex;
  std::vector<GpuFrameTime>           times;
};

Gpu* g = nullptr;

std::string read_text_file(const char* path) {
  std::ifstream file(path);
  if (!file)
    return {};
  std::stringstream ss;
  ss << file.rdbuf();
  return ss.str();
}

id<MTLRenderPipelineState> make_pipeline(id<MTLLibrary> library,
                                         NSString*      vertex,
                                         NSString*      fragment,
                                         MTLPixelFormat colour,
                                         MTLPixelFormat depth,
                                         bool           blend) {
  MTLRenderPipelineDescriptor* d = [[MTLRenderPipelineDescriptor alloc] init];
  d.vertexFunction = [library newFunctionWithName:vertex];
  d.fragmentFunction = [library newFunctionWithName:fragment];
  d.colorAttachments[0].pixelFormat = colour;
  d.depthAttachmentPixelFormat = depth;
  if (blend) {
    // Source over: source alpha and one minus it, on colour and alpha
    // alike.
    auto* a = d.colorAttachments[0];
    a.blendingEnabled = YES;
    a.sourceRGBBlendFactor = a.sourceAlphaBlendFactor =
        MTLBlendFactorSourceAlpha;
    a.destinationRGBBlendFactor = a.destinationAlphaBlendFactor =
        MTLBlendFactorOneMinusSourceAlpha;
  }
  NSError*                   error = nil;
  id<MTLRenderPipelineState> state =
      [g->device newRenderPipelineStateWithDescriptor:d error:&error];
  if (!state)
    NSLog(@"metal: %@", error);
  return state;
}

// Room for `size` bytes of this frame's data: where to write, and the buffer
// and offset the GPU reads them at.
void* upload(NSUInteger size,
             __strong id<MTLBuffer>& buffer,
             NSUInteger&             offset) {
  Upload&          u = g->uploads[g->upload_index];
  const NSUInteger at = (u.used + 255) & ~NSUInteger(255);
  if (!u.buffer || at + size > u.buffer.length) {
    // Draws recorded earlier keep the old buffer alive.
    const NSUInteger length =
        std::max<NSUInteger>(size, u.buffer ? u.buffer.length * 2 : 1 << 22);
    u.buffer = [g->device newBufferWithLength:length
                                      options:MTLResourceStorageModeShared];
    u.used = 0;
    return upload(size, buffer, offset);
  }
  u.used = at + size;
  buffer = u.buffer;
  offset = at;
  return static_cast<u8*>(u.buffer.contents) + at;
}

// Waits for the GPU to finish every frame sent, and for their completion
// handlers to run.
void wait_until_idle() {
  dispatch_semaphore_t done = dispatch_semaphore_create(0);
  id<MTLCommandBuffer> last = [g->queue commandBuffer];
  [last addCompletedHandler:^(id<MTLCommandBuffer>) {
    dispatch_semaphore_signal(done);
  }];
  [last commit];
  dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
}

void release_surfaces() {
  for (Surface& s : g->surfaces) {
    if (s.surface)
      CFRelease(s.surface);
    s.surface = nullptr;
    s.texture = nil;
    s.busy = false;
  }
}

void create_surfaces(i32 width, i32 height) {
  // Frames still on the GPU mark their surfaces done when they finish.
  wait_until_idle();
  release_surfaces();
  g->surface_width = width;
  g->surface_height = height;
  NSDictionary* properties = @{
    (id)kIOSurfaceWidth : @(width),
    (id)kIOSurfaceHeight : @(height),
    (id)kIOSurfaceBytesPerElement : @4,
    (id)kIOSurfacePixelFormat : @(static_cast<u32>('BGRA')),
  };
  MTLTextureDescriptor* d = [MTLTextureDescriptor
      texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                   width:width
                                  height:height
                               mipmapped:NO];
  d.usage = MTLTextureUsageRenderTarget;
  d.storageMode =
      g->device.hasUnifiedMemory ? MTLStorageModeShared : MTLStorageModeManaged;
  for (Surface& s : g->surfaces) {
    s.surface = IOSurfaceCreate((__bridge CFDictionaryRef)properties);
    if (s.surface)
      s.texture = [g->device newTextureWithDescriptor:d
                                            iosurface:s.surface
                                                plane:0];
  }
}

// One the compositor and GPU are done with.
Surface* free_surface() {
  for (Surface& s : g->surfaces)
    if (s.texture && !s.busy && !IOSurfaceIsInUse(s.surface))
      return &s;
  return nullptr;
}

// Full screen: waits for each drawable the layer frees, for the game loop
// to draw into next.
void wait_for_drawables() {
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
  while (true) {
    {
      std::unique_lock lock(g->drawable_mutex);
      g->drawable_changed.wait(lock, [] {
        return g->stop || (g->want_drawables && !g->next_drawable);
      });
      if (g->stop)
        return;
    }
    @autoreleasepool {
      id<CAMetalDrawable> drawable = [g->metal_layer nextDrawable];
      {
        std::lock_guard lock(g->drawable_mutex);
        if (g->want_drawables)
          g->next_drawable = drawable;
      }
      g->drawable_changed.notify_all();
    }
  }
}

// Follows the window in and out of full screen. Full screen, the Metal
// layer matches the view's pixels.
void follow_full_screen() {
  const bool full = (g->window.styleMask & NSWindowStyleMaskFullScreen) != 0;
  if (full) {
    const CGFloat scale = g->window.backingScaleFactor;
    const CGSize  size = [g->view convertSizeToBacking:g->view.bounds.size];
    if (g->metal_layer.contentsScale != scale)
      g->metal_layer.contentsScale = scale;
    if (!CGSizeEqualToSize(g->metal_layer.drawableSize, size))
      g->metal_layer.drawableSize = size;
  }
  if (full == g->full_screen)
    return;
  g->full_screen = full;
  g->view.layer = full ? g->metal_layer : g->surface_layer;
  {
    std::lock_guard lock(g->drawable_mutex);
    g->want_drawables = full;
    g->next_drawable = nil;
  }
  g->drawable_changed.notify_all();
}

id<MTLTexture> new_texture(i32           width,
                           i32           height,
                           TextureFormat format,
                           const void*   pixels,
                           bool          mipmaps,
                           bool          drawn_into) {
  MTLPixelFormat pixel_format = MTLPixelFormatRGBA8Unorm;
  NSUInteger     bytes = 4;
  if (format == TextureFormat::R8) {
    pixel_format = MTLPixelFormatR8Unorm;
    bytes = 1;
  } else if (format == TextureFormat::Depth16) {
    pixel_format = MTLPixelFormatDepth16Unorm;
    bytes = 2;
  }
  MTLTextureDescriptor* d =
      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pixel_format
                                                         width:width
                                                        height:height
                                                     mipmapped:mipmaps];
  d.usage = MTLTextureUsageShaderRead;
  if (drawn_into) {
    d.usage |= MTLTextureUsageRenderTarget;
    d.storageMode = MTLStorageModePrivate;
  } else {
    d.storageMode = g->device.hasUnifiedMemory ? MTLStorageModeShared
                                               : MTLStorageModeManaged;
  }
  id<MTLTexture> texture = [g->device newTextureWithDescriptor:d];
  if (texture && pixels) {
    [texture replaceRegion:MTLRegionMake2D(0, 0, width, height)
               mipmapLevel:0
                 withBytes:pixels
               bytesPerRow:width * bytes];
    if (texture.mipmapLevelCount > 1) {
      id<MTLCommandBuffer>      commands = [g->queue commandBuffer];
      id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
      [blit generateMipmapsForTexture:texture];
      [blit endEncoding];
      [commands commit];
    }
  }
  return texture;
}

id<MTLTexture> mtl(const GpuTexture* texture) {
  return texture ? texture->texture : nil;
}

}  // namespace

}  // namespace circles

namespace circles {

bool gpu_init(GLFWwindow* window) {
  g = new Gpu;
  g->device = MTLCreateSystemDefaultDevice();
  if (!g->device)
    return false;
  g->queue = [g->device newCommandQueue];
  const std::string source = read_text_file(kShaderPath);
  if (source.empty()) {
    fprintf(stderr, "failed to open %s\n", kShaderPath);
    return false;
  }
  MTLCompileOptions* options = [[MTLCompileOptions alloc] init];
  // Fast math, except where circles.metal asks for exact operations, as for
  // stable's arithmetic in slider bodies.
  options.mathMode = MTLMathModeFast;
  NSError*       error = nil;
  id<MTLLibrary> library = [g->device newLibraryWithSource:@(source.c_str())
                                                   options:options
                                                     error:&error];
  if (!library) {
    NSLog(@"%s: %@", kShaderPath, error);
    return false;
  }
  g->sprites =
      make_pipeline(library, @"sprite_vertex", @"sprite_fragment",
                    MTLPixelFormatBGRA8Unorm, MTLPixelFormatInvalid, true);
  g->pieces = make_pipeline(library, @"piece_vertex", @"piece_fragment",
                            MTLPixelFormatRGBA8Unorm,
                            MTLPixelFormatDepth16Unorm, false);
  if (!g->sprites || !g->pieces)
    return false;
  MTLDepthStencilDescriptor* depth = [[MTLDepthStencilDescriptor alloc] init];
  depth.depthCompareFunction = MTLCompareFunctionLessEqual;
  depth.depthWriteEnabled = YES;
  g->nearest = [g->device newDepthStencilStateWithDescriptor:depth];
  // Linear filtering, between mipmaps too, clamped at the edges.
  MTLSamplerDescriptor* sampler = [[MTLSamplerDescriptor alloc] init];
  sampler.minFilter = sampler.magFilter = MTLSamplerMinMagFilterLinear;
  sampler.mipFilter = MTLSamplerMipFilterLinear;
  sampler.sAddressMode = sampler.tAddressMode =
      MTLSamplerAddressModeClampToEdge;
  g->sampler = [g->device newSamplerStateWithDescriptor:sampler];
  const u8 white[4] = {255, 255, 255, 255};
  g->blank = new_texture(1, 1, TextureFormat::Rgba8, white, false, false);
  g->in_flight = dispatch_semaphore_create(kFramesInFlight);

  g->window = glfwGetCocoaWindow(window);
  g->view = g->window.contentView;
  g->surface_layer = [CALayer layer];
  g->surface_layer.opaque = YES;
  g->metal_layer = [CAMetalLayer layer];
  g->metal_layer.device = g->device;
  g->metal_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
  // Opaque, as direct-to-display needs.
  g->metal_layer.opaque = YES;
  // One drawable on screen and one being drawn: a frame waits for at most
  // one refresh.
  g->metal_layer.maximumDrawableCount = 2;
  g->view.layer = g->surface_layer;
  g->view.wantsLayer = YES;

  g->drawable_thread = std::thread(wait_for_drawables);
  return true;
}

void gpu_shutdown() {
  if (!g)
    return;
  if (g->drawable_thread.joinable()) {
    {
      std::lock_guard lock(g->drawable_mutex);
      g->stop = true;
    }
    g->drawable_changed.notify_all();
    g->drawable_thread.join();
  }
  if (g->queue)
    wait_until_idle();
  release_surfaces();
  delete g;
  g = nullptr;
}

GpuTexture* gpu_create_texture(i32           width,
                               i32           height,
                               TextureFormat format,
                               const void*   pixels,
                               bool          mipmaps,
                               bool          drawn_into) {
  id<MTLTexture> texture =
      new_texture(width, height, format, pixels, mipmaps, drawn_into);
  return texture ? new GpuTexture{texture} : nullptr;
}

void gpu_release_texture(GpuTexture* texture) {
  // Frames still on the GPU hold their own references.
  delete texture;
}

bool gpu_frame_due() {
  follow_full_screen();
  if (!g->full_screen)
    return true;
  std::lock_guard lock(g->drawable_mutex);
  return g->next_drawable != nil;
}

void gpu_sleep_until(std::chrono::steady_clock::time_point wake) {
  std::unique_lock lock(g->drawable_mutex);
  g->drawable_changed.wait_until(lock, wake, [] {
    return g->want_drawables && g->next_drawable;
  });
}

bool gpu_begin_frame(i32 width, i32 height) {
  if (width <= 0 || height <= 0)
    return false;
  g->surface = nullptr;
  g->drawable = nil;
  if (g->full_screen) {
    {
      std::lock_guard lock(g->drawable_mutex);
      g->drawable = g->next_drawable;
      g->next_drawable = nil;
    }
    g->drawable_changed.notify_all();
    if (!g->drawable)
      return false;
    g->target = g->drawable.texture;
  } else {
    const CGFloat scale = g->window.backingScaleFactor;
    if (g->surface_layer.contentsScale != scale)
      g->surface_layer.contentsScale = scale;
    if (width != g->surface_width || height != g->surface_height)
      create_surfaces(width, height);
    g->surface = free_surface();
    if (!g->surface)
      return false;
    g->target = g->surface->texture;
  }
  dispatch_semaphore_wait(g->in_flight, DISPATCH_TIME_FOREVER);
  g->upload_index = (g->upload_index + 1) % kFramesInFlight;
  g->uploads[g->upload_index].used = 0;
  g->begun = CACurrentMediaTime();
  g->commands = [g->queue commandBuffer];
  g->draws.clear();
  return true;
}

void gpu_draw_sprites(const f32*               vertices,
                      u32                      vertex_count,
                      const GpuTexture* const* textures,
                      i32                      texture_count,
                      f32                      screen_width,
                      f32                      screen_height) {
  SpriteDraw       draw;
  const NSUInteger size = vertex_count * 12 * sizeof(f32);
  memcpy(upload(size, draw.buffer, draw.offset), vertices, size);
  draw.count = vertex_count;
  for (i32 i = 0; i < 8; ++i)
    draw.textures[i] = i < texture_count ? mtl(textures[i]) : g->blank;
  draw.screen = simd_make_float2(screen_width, screen_height);
  g->draws.push_back(std::move(draw));
}

void gpu_clear_target(GpuTexture* colour, GpuTexture* depth) {
  MTLRenderPassDescriptor* pass =
      [MTLRenderPassDescriptor renderPassDescriptor];
  pass.colorAttachments[0].texture = mtl(colour);
  pass.colorAttachments[0].loadAction = MTLLoadActionClear;
  pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
  pass.colorAttachments[0].storeAction = MTLStoreActionStore;
  pass.depthAttachment.texture = mtl(depth);
  pass.depthAttachment.loadAction = MTLLoadActionClear;
  pass.depthAttachment.clearDepth = 1;
  pass.depthAttachment.storeAction = MTLStoreActionStore;
  [[g->commands renderCommandEncoderWithDescriptor:pass] endEncoding];
}

void gpu_copy_target(GpuTexture* from_colour,
                     GpuTexture* from_depth,
                     GpuTexture* to_colour,
                     GpuTexture* to_depth) {
  id<MTLBlitCommandEncoder> blit = [g->commands blitCommandEncoder];
  [blit copyFromTexture:mtl(from_colour) toTexture:mtl(to_colour)];
  [blit copyFromTexture:mtl(from_depth) toTexture:mtl(to_depth)];
  [blit endEncoding];
}

void gpu_draw_pieces(GpuTexture*          colour,
                     GpuTexture*          depth,
                     const void*          pieces,
                     u32                  count,
                     u32                  stride,
                     u32                  vertices,
                     const PieceUniforms& uniforms,
                     const f32*           rim,
                     GpuTexture*          gradient) {
  id<MTLBuffer>    buffer;
  NSUInteger       offset;
  const NSUInteger size = NSUInteger(count) * stride;
  memcpy(upload(size, buffer, offset), pieces, size);
  MTLRenderPassDescriptor* pass =
      [MTLRenderPassDescriptor renderPassDescriptor];
  pass.colorAttachments[0].texture = mtl(colour);
  pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
  pass.colorAttachments[0].storeAction = MTLStoreActionStore;
  pass.depthAttachment.texture = mtl(depth);
  pass.depthAttachment.loadAction = MTLLoadActionLoad;
  pass.depthAttachment.storeAction = MTLStoreActionStore;
  id<MTLRenderCommandEncoder> encoder =
      [g->commands renderCommandEncoderWithDescriptor:pass];
  [encoder setRenderPipelineState:g->pieces];
  [encoder setDepthStencilState:g->nearest];
  [encoder setVertexBuffer:buffer offset:offset atIndex:0];
  [encoder setVertexBytes:&uniforms length:sizeof(uniforms) atIndex:1];
  [encoder setVertexBytes:rim length:25 * 2 * sizeof(f32) atIndex:2];
  [encoder setFragmentTexture:mtl(gradient) atIndex:0];
  [encoder setFragmentSamplerState:g->sampler atIndex:0];
  [encoder drawPrimitives:MTLPrimitiveTypeTriangle
              vertexStart:0
              vertexCount:vertices
            instanceCount:count];
  [encoder endEncoding];
}

void gpu_end_frame(i64 tag) {
  MTLRenderPassDescriptor* pass =
      [MTLRenderPassDescriptor renderPassDescriptor];
  pass.colorAttachments[0].texture = g->target;
  pass.colorAttachments[0].loadAction = MTLLoadActionClear;
  pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
  pass.colorAttachments[0].storeAction = MTLStoreActionStore;
  id<MTLRenderCommandEncoder> encoder =
      [g->commands renderCommandEncoderWithDescriptor:pass];
  [encoder setRenderPipelineState:g->sprites];
  [encoder setFragmentSamplerState:g->sampler atIndex:0];
  for (const SpriteDraw& draw : g->draws) {
    [encoder setVertexBuffer:draw.buffer offset:draw.offset atIndex:0];
    [encoder setVertexBytes:&draw.screen length:sizeof(draw.screen) atIndex:1];
    __unsafe_unretained id<MTLTexture> textures[8];
    std::copy(draw.textures.begin(), draw.textures.end(), textures);
    [encoder setFragmentTextures:textures withRange:NSMakeRange(0, 8)];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                vertexStart:0
                vertexCount:draw.count];
  }
  [encoder endEncoding];
  g->draws.clear();

  const f64 begun = g->begun;
  Gpu*      gpu = g;
  if (g->surface) {
    // Handed to the compositor the moment it's drawn.
    Surface* surface = g->surface;
    surface->busy = true;
    CALayer*     layer = g->surface_layer;
    IOSurfaceRef shown = surface->surface;
    CFRetain(shown);
    [g->commands addCompletedHandler:^(id<MTLCommandBuffer>) {
      [CATransaction begin];
      [CATransaction setDisableActions:YES];
      layer.contents = (__bridge id)shown;
      [CATransaction commit];
      CFRelease(shown);
      surface->busy = false;
    }];
  } else {
    [g->drawable addPresentedHandler:^(id<MTLDrawable> drawable) {
      if (drawable.presentedTime <= 0)
        return;  // dropped
      std::lock_guard lock(gpu->times_mutex);
      gpu->times.push_back(
          {tag, -1, static_cast<f32>((drawable.presentedTime - begun) * 1000)});
    }];
    [g->commands presentDrawable:g->drawable];
  }
  dispatch_semaphore_t in_flight = g->in_flight;
  [g->commands addCompletedHandler:^(id<MTLCommandBuffer> commands) {
    {
      std::lock_guard lock(gpu->times_mutex);
      gpu->times.push_back(
          {tag,
           static_cast<f32>((commands.GPUEndTime - commands.GPUStartTime) *
                            1000),
           -1});
    }
    dispatch_semaphore_signal(in_flight);
  }];
  [g->commands commit];
  g->commands = nil;
  g->target = nil;
  g->drawable = nil;
}

void gpu_take_frame_times(std::vector<GpuFrameTime>& times) {
  std::lock_guard lock(g->times_mutex);
  times.insert(times.end(), g->times.begin(), g->times.end());
  g->times.clear();
}

}  // namespace circles
