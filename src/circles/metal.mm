#include <circles/metal.h>
#include <circles/slider_body.h>

#include <metal_shader.h>

#define GLFW_INCLUDE_NONE
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace circles {
namespace {

[[noreturn]] void fail(const char* message) {
  std::fprintf(stderr, "Metal: %s\n", message);
  std::exit(1);
}

void check(id object, NSError* error) {
  if (!object)
    fail(error ? error.localizedDescription.UTF8String
               : "resource allocation failed");
}

struct TextureResource {
  id<MTLTexture> colour;
  id<MTLTexture> depth;
};
struct UploadBlock {
  id<MTLBuffer> buffer;
  size_t        used = 0;
};
struct Frame {
  id<MTLCommandBuffer>     command;
  std::vector<UploadBlock> blocks;
  size_t                   block = 0;
};
struct Slice {
  id<MTLBuffer> buffer;
  size_t        offset;
};
struct SpriteDraw {
  Slice                         vertices;
  u32                           count;
  std::array<id<MTLTexture>, 8> textures;
  std::array<f32, 2>            screen;
};

// nextDrawable can block on the window server even with displaySyncEnabled
// disabled. Only acquisition runs here: all game state and GPU encoding stay
// on the main thread. At most one unused drawable is held ahead of rendering.
class DrawableSource {
 public:
  explicit DrawableSource(CAMetalLayer* layer) : layer_(layer) {
    thread_ = std::thread([this] {
      for (;;) {
        @autoreleasepool {
          {
            std::unique_lock lock(mutex_);
            ready_.wait(lock, [this] {
              return stopping_ || requested_;
            });
            if (stopping_)
              return;
          }
          auto drawable = [layer_ nextDrawable];
          {
            std::lock_guard lock(mutex_);
            if (stopping_)
              return;
            drawable_ = drawable;
            requested_ = false;
          }
          // Wake glfwWaitEventsTimeout without touching any window/UI state.
          if (drawable)
            glfwPostEmptyEvent();
        }
      }
    });
  }

  ~DrawableSource() {
    {
      std::lock_guard lock(mutex_);
      stopping_ = true;
    }
    ready_.notify_one();
    thread_.join();
  }

  id<CAMetalDrawable> take() {
    std::lock_guard lock(mutex_);
    auto            drawable = drawable_;
    drawable_ = nil;
    requested_ = true;
    ready_.notify_one();
    return drawable;
  }

 private:
  CAMetalLayer*           layer_;
  id<CAMetalDrawable>     drawable_;
  std::mutex              mutex_;
  std::condition_variable ready_;
  bool                    requested_ = true;
  bool                    stopping_ = false;
  std::thread             thread_;
};

struct Context {
  GLFWwindow*                         window = nullptr;
  id<MTLDevice>                       device;
  id<MTLCommandQueue>                 queue;
  CAMetalLayer*                       layer;
  id<CAMetalDrawable>                 drawable;
  id<MTLRenderPipelineState>          sprites, bodies;
  id<MTLDepthStencilState>            depth;
  id<MTLBuffer>                       rim;
  id<MTLCommandBuffer>                command, uploads;
  std::array<Frame, 3>                frames;
  size_t                              frame = 0;
  std::vector<TextureResource>        textures{1};  // zero is the null handle
  std::vector<u32>                    free_textures;
  std::vector<SpriteDraw>             draws;
  std::array<id<MTLCommandBuffer>, 8> queries;
  bool                                active = false;

  std::unique_ptr<DrawableSource>     source;
};
Context g;

void completed(id<MTLCommandBuffer> command) {
  if (!command)
    return;
  [command waitUntilCompleted];
  if (command.status == MTLCommandBufferStatusError)
    fail(command.error.localizedDescription.UTF8String);
}

// Shared buffers are CPU-visible on both Apple silicon and Intel. Each slot is
// reused only after its last command buffer completes. Blocks grow on demand;
// normal frames only reset offsets, and never allocate a buffer per draw.
Slice upload(const void* data, size_t size) {
  Frame& frame = g.frames[g.frame];
  for (;;) {
    if (frame.block == frame.blocks.size()) {
      const size_t  capacity = std::max<size_t>(1024 * 1024, size);
      id<MTLBuffer> buffer =
          [g.device newBufferWithLength:capacity
                                options:MTLResourceStorageModeShared |
                                        MTLResourceCPUCacheModeWriteCombined];
      check(buffer, nil);
      frame.blocks.push_back({buffer});
    }
    auto&        block = frame.blocks[frame.block];
    const size_t offset = (block.used + 255) & ~size_t(255);
    if (offset + size <= block.buffer.length) {
      std::memcpy(static_cast<u8*>(block.buffer.contents) + offset, data, size);
      block.used = offset + size;
      return {block.buffer, offset};
    }
    ++frame.block;
  }
}

u32 store(TextureResource resource) {
  if (!g.free_textures.empty()) {
    const u32 id = g.free_textures.back();
    g.free_textures.pop_back();
    g.textures[id] = std::move(resource);
    return id;
  }
  g.textures.push_back(std::move(resource));
  return static_cast<u32>(g.textures.size() - 1);
}

id<MTLTexture> texture(i32             width,
                       i32             height,
                       MTLPixelFormat  format,
                       MTLTextureUsage usage,
                       bool            mipmaps = false) {
  auto descriptor =
      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                         width:width
                                                        height:height
                                                     mipmapped:mipmaps];
  descriptor.storageMode = MTLStorageModePrivate;
  descriptor.usage = usage;
  id<MTLTexture> result = [g.device newTextureWithDescriptor:descriptor];
  check(result, nil);
  return result;
}

id<MTLRenderPipelineState> pipeline(id<MTLLibrary> library,
                                    NSString*      vertex,
                                    NSString*      fragment,
                                    bool           body) {
  auto descriptor = [MTLRenderPipelineDescriptor new];
  descriptor.vertexFunction = [library newFunctionWithName:vertex];
  descriptor.fragmentFunction = [library newFunctionWithName:fragment];
  auto colour = descriptor.colorAttachments[0];
  colour.pixelFormat =
      body ? MTLPixelFormatRGBA8Unorm : MTLPixelFormatBGRA8Unorm;
  if (body) {
    descriptor.depthAttachmentPixelFormat = MTLPixelFormatDepth16Unorm;
  } else {
    colour.blendingEnabled = YES;
    colour.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
    colour.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    // Matches glBlendFunc's alpha factors as well as RGB.
    colour.sourceAlphaBlendFactor = MTLBlendFactorSourceAlpha;
    colour.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
  }
  NSError* error = nil;
  auto     result = [g.device newRenderPipelineStateWithDescriptor:descriptor
                                                             error:&error];
  check(result, error);
  return result;
}

MTLRenderPassDescriptor* target_pass(const TextureResource& target,
                                     bool                   clear) {
  auto pass = [MTLRenderPassDescriptor renderPassDescriptor];
  auto colour = pass.colorAttachments[0];
  colour.texture = target.colour;
  colour.loadAction = clear ? MTLLoadActionClear : MTLLoadActionLoad;
  colour.storeAction = MTLStoreActionStore;
  colour.clearColor = MTLClearColorMake(0, 0, 0, 0);
  pass.depthAttachment.texture = target.depth;
  pass.depthAttachment.loadAction =
      clear ? MTLLoadActionClear : MTLLoadActionLoad;
  pass.depthAttachment.storeAction = MTLStoreActionStore;
  pass.depthAttachment.clearDepth = 1;
  return pass;
}

void encode_sprites(id<MTLRenderCommandEncoder> encoder) {
  [encoder setRenderPipelineState:g.sprites];
  for (const auto& draw : g.draws) {
    [encoder setVertexBuffer:draw.vertices.buffer
                      offset:draw.vertices.offset
                     atIndex:0];
    [encoder setVertexBytes:draw.screen.data()
                     length:sizeof(draw.screen)
                    atIndex:1];
    for (size_t i = 0; i < draw.textures.size(); ++i)
      [encoder setFragmentTexture:draw.textures[i] atIndex:i];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                vertexStart:0
                vertexCount:draw.count];
  }
}

}  // namespace

void gpu_init(GLFWwindow* window) {
  @autoreleasepool {
    g.window = window;
    g.device = MTLCreateSystemDefaultDevice();
    check(g.device, nil);
    g.queue = [g.device newCommandQueue];
    check(g.queue, nil);
    NSWindow* native = glfwGetCocoaWindow(window);
    g.layer = [CAMetalLayer layer];
    g.layer.device = g.device;
    g.layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    g.layer.framebufferOnly = YES;
    g.layer.opaque = YES;
    g.layer.displaySyncEnabled = NO;
    g.layer.maximumDrawableCount = 3;
    g.layer.allowsNextDrawableTimeout = YES;
    g.layer.presentsWithTransaction = NO;
    native.contentView.layer = g.layer;
    native.contentView.wantsLayer = YES;
    g.layer.frame = native.contentView.bounds;
    g.layer.autoresizingMask = kCALayerWidthSizable | kCALayerHeightSizable;

    NSError*  error = nil;
    NSString* source = [NSString stringWithUTF8String:kMetalShaderSource];
    auto      options = [MTLCompileOptions new];
    // Preserve stable's operation-by-operation placement, including no FMA.
    options.mathMode = MTLMathModeSafe;
    id<MTLLibrary> library = [g.device newLibraryWithSource:source
                                                    options:options
                                                      error:&error];
    check(library, error);
    g.sprites = pipeline(library, @"sprite_vertex", @"sprite_fragment", false);
    g.bodies = pipeline(library, @"body_vertex", @"body_fragment", true);
    auto depth = [MTLDepthStencilDescriptor new];
    depth.depthCompareFunction = MTLCompareFunctionLessEqual;
    depth.depthWriteEnabled = YES;
    g.depth = [g.device newDepthStencilStateWithDescriptor:depth];
    check(g.depth, nil);
    std::array<f32, 50> rim{};
    rim[1] = -1;
    for (i32 i = 1; i < 24; ++i) {
      const f32 angle = static_cast<f32>(i) * (3.1415927f / 24.0f);
      rim[2 * i] = static_cast<f32>(std::sin(static_cast<f64>(angle)));
      rim[2 * i + 1] = -static_cast<f32>(std::cos(static_cast<f64>(angle)));
    }
    rim[49] = 1;
    g.rim = [g.device newBufferWithBytes:rim.data()
                                  length:sizeof(rim)
                                 options:MTLResourceStorageModeShared];
    check(g.rim, nil);
    gpu_set_vsync(false);
    g.source = std::make_unique<DrawableSource>(g.layer);
    std::printf("Metal: %s\n", g.device.name.UTF8String);
  }
}

void gpu_set_vsync(bool enabled) {
  g.layer.displaySyncEnabled = enabled;
}

bool gpu_begin_frame(bool offscreen) {
  @autoreleasepool {
    if (g.uploads) {
      [g.uploads commit];
      g.uploads = nil;
    }
    int width, height;
    glfwGetFramebufferSize(g.window, &width, &height);
    if (width <= 0 || height <= 0 ||
        glfwGetWindowAttrib(g.window, GLFW_ICONIFIED))
      return false;
    CGSize        size = CGSizeMake(width, height);
    const CGFloat scale =
        ((NSWindow*)glfwGetCocoaWindow(g.window)).backingScaleFactor;
    if (g.layer.contentsScale != scale)
      g.layer.contentsScale = scale;
    if (!CGSizeEqualToSize(g.layer.drawableSize, size)) {
      g.layer.drawableSize = size;
    }
    Frame& frame = g.frames[g.frame];
    if (offscreen)
      completed(frame.command);
    else if (frame.command &&
             frame.command.status != MTLCommandBufferStatusCompleted) {
      if (frame.command.status == MTLCommandBufferStatusError)
        fail(frame.command.error.localizedDescription.UTF8String);
      return false;
    }
    if (!offscreen) {
      g.drawable = g.source->take();
      if (!g.drawable)
        return false;
      // A resize may have occurred while acquisition was in progress. Never
      // stretch a frame prepared for another backing size into this drawable.
      if (g.drawable.texture.width != static_cast<NSUInteger>(width) ||
          g.drawable.texture.height != static_cast<NSUInteger>(height)) {
        g.drawable = nil;
        return false;
      }
    }
    frame.command = nil;
    frame.block = 0;
    for (auto& block : frame.blocks)
      block.used = 0;
    g.draws.clear();
    g.command = [g.queue commandBuffer];
    check(g.command, nil);
    g.active = true;
    return true;
  }
}

bool gpu_present() {
  if (!g.active)
    return false;
  @autoreleasepool {
    Frame&         frame = g.frames[g.frame];
    id<MTLTexture> target;
    target = g.drawable.texture;
    if (target) {
      auto pass = [MTLRenderPassDescriptor renderPassDescriptor];
      auto colour = pass.colorAttachments[0];
      colour.texture = target;
      colour.loadAction = MTLLoadActionClear;
      colour.storeAction = MTLStoreActionStore;
      colour.clearColor = MTLClearColorMake(0, 0, 0, 1);
      auto encoder = [g.command renderCommandEncoderWithDescriptor:pass];
      encode_sprites(encoder);
      [encoder endEncoding];
      [g.command presentDrawable:g.drawable];
    }
    [g.command commit];
    frame.command = g.command;
    g.command = nil;
    g.drawable = nil;
    g.draws.clear();
    g.active = false;
    g.frame = (g.frame + 1) % g.frames.size();
    return target != nil;
  }
}

std::vector<u8> gpu_readback_frame(i32 width, i32 height) {
  @autoreleasepool {
    auto target = texture(width, height, MTLPixelFormatBGRA8Unorm,
                          MTLTextureUsageRenderTarget);
    auto pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    auto encoder = [g.command renderCommandEncoderWithDescriptor:pass];
    encode_sprites(encoder);
    [encoder endEncoding];
    const size_t stride = (static_cast<size_t>(width) * 4 + 255) & ~size_t(255);
    auto buffer = [g.device newBufferWithLength:stride * height
                                        options:MTLResourceStorageModeShared];
    check(buffer, nil);
    auto blit = [g.command blitCommandEncoder];
    [blit copyFromTexture:target
                     sourceSlice:0
                     sourceLevel:0
                    sourceOrigin:MTLOriginMake(0, 0, 0)
                      sourceSize:MTLSizeMake(width, height, 1)
                        toBuffer:buffer
               destinationOffset:0
          destinationBytesPerRow:stride
        destinationBytesPerImage:stride * height];
    [blit endEncoding];
    [g.command commit];
    completed(g.command);
    std::vector<u8> pixels(static_cast<size_t>(width) * height * 4);
    const auto*     source = static_cast<const u8*>(buffer.contents);
    for (i32 y = 0; y < height; ++y)
      for (i32 x = 0; x < width; ++x) {
        const auto from = source + y * stride + x * 4;
        auto to = pixels.data() + (static_cast<size_t>(y) * width + x) * 4;
        to[0] = from[2];
        to[1] = from[1];
        to[2] = from[0];
        to[3] = from[3];
      }
    g.command = nil;
    g.draws.clear();
    g.active = false;
    g.frame = (g.frame + 1) % g.frames.size();
    return pixels;
  }
}

u32 gpu_upload(const u8* pixels,
               i32       width,
               i32       height,
               bool      red,
               bool      mipmaps) {
  @autoreleasepool {
    const size_t  row = static_cast<size_t>(width) * (red ? 1 : 4);
    const size_t  stride = (row + 255) & ~size_t(255);
    id<MTLBuffer> staging =
        [g.device newBufferWithLength:stride * height
                              options:MTLResourceStorageModeShared |
                                      MTLResourceCPUCacheModeWriteCombined];
    check(staging, nil);
    for (i32 y = 0; y < height; ++y)
      std::memcpy(static_cast<u8*>(staging.contents) + y * stride,
                  pixels + y * row, row);
    id<MTLTexture> result = texture(
        width, height, red ? MTLPixelFormatR8Unorm : MTLPixelFormatRGBA8Unorm,
        MTLTextureUsageShaderRead, mipmaps);
    if (!g.active && !g.uploads)
      g.uploads = [g.queue commandBuffer];
    auto encoder = [(g.active ? g.command : g.uploads) blitCommandEncoder];
    [encoder copyFromBuffer:staging
               sourceOffset:0
          sourceBytesPerRow:stride
        sourceBytesPerImage:stride * height
                 sourceSize:MTLSizeMake(width, height, 1)
                  toTexture:result
           destinationSlice:0
           destinationLevel:0
          destinationOrigin:MTLOriginMake(0, 0, 0)];
    if (result.mipmapLevelCount > 1)
      [encoder generateMipmapsForTexture:result];
    [encoder endEncoding];
    return store({result, nil});
  }
}

void gpu_destroy_texture(u32 id) {
  if (!id)
    return;
  // Command buffers and queued screen draws retain every resource they use.
  // Reusing this integer handle cannot change those already-recorded
  // references.
  g.textures[id] = {};
  g.free_textures.push_back(id);
}

u32 gpu_create_target(i32 width, i32 height) {
  @autoreleasepool {
    return store(
        {texture(width, height, MTLPixelFormatRGBA8Unorm,
                 MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead),
         texture(width, height, MTLPixelFormatDepth16Unorm,
                 MTLTextureUsageRenderTarget)});
  }
}

void gpu_clear_target(u32 target) {
  @autoreleasepool {
    auto encoder = [g.command
        renderCommandEncoderWithDescriptor:target_pass(g.textures[target],
                                                       true)];
    [encoder endEncoding];
  }
}

void gpu_copy_target(u32 source, u32 destination, i32 width, i32 height) {
  @autoreleasepool {
    auto        encoder = [g.command blitCommandEncoder];
    const auto& from = g.textures[source];
    const auto& to = g.textures[destination];
    for (bool depth : {false, true})
      [encoder copyFromTexture:depth ? from.depth : from.colour
                   sourceSlice:0
                   sourceLevel:0
                  sourceOrigin:MTLOriginMake(0, 0, 0)
                    sourceSize:MTLSizeMake(width, height, 1)
                     toTexture:depth ? to.depth : to.colour
              destinationSlice:0
              destinationLevel:0
             destinationOrigin:MTLOriginMake(0, 0, 0)];
    [encoder endEncoding];
  }
}

void gpu_draw_sprites(std::span<const f32>      vertices,
                      const std::array<u32, 8>& textures,
                      f32                       width,
                      f32                       height) {
  SpriteDraw draw{.vertices = upload(vertices.data(), vertices.size_bytes()),
                  .count = static_cast<u32>(vertices.size() / 12),
                  .screen = {width, height}};
  for (size_t i = 0; i < textures.size(); ++i)
    draw.textures[i] = g.textures[textures[i]].colour;
  g.draws.push_back(std::move(draw));
}

void gpu_draw_body(u32                            target,
                   std::span<const PieceInstance> pieces,
                   u32                            gradient,
                   f32                            radius,
                   std::array<f32, 4>             region,
                   std::array<f32, 4>             squash) {
  static_assert(sizeof(PieceInstance) == 32 &&
                offsetof(PieceInstance, end_cap) == 28);
  @autoreleasepool {
    const Slice data = upload(pieces.data(), pieces.size_bytes());
    const struct {
      std::array<f32, 4> region, squash;
      f32                radius;
      f32                padding[3];
    } params{region, squash, radius, {}};
    auto encoder = [g.command
        renderCommandEncoderWithDescriptor:target_pass(g.textures[target],
                                                       false)];
    [encoder setViewport:MTLViewport{0, 0, region[2], region[3], 0, 1}];
    [encoder setRenderPipelineState:g.bodies];
    [encoder setDepthStencilState:g.depth];
    [encoder setVertexBuffer:data.buffer offset:data.offset atIndex:0];
    [encoder setVertexBytes:&params length:sizeof(params) atIndex:1];
    [encoder setVertexBuffer:g.rim offset:0 atIndex:2];
    [encoder setFragmentTexture:g.textures[gradient].colour atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                vertexStart:0
                vertexCount:156
              instanceCount:pieces.size()];
    [encoder endEncoding];
  }
}

// Command-buffer timestamps measure the whole GPU frame, without introducing
// extra submissions or a CPU wait on sampled frames.
bool gpu_query_begin(i32 slot) {
  if (!g.active)
    return false;
  g.queries[slot] = g.command;
  return true;
}
f32 gpu_query_time(i32 slot, bool wait) {
  auto command = g.queries[slot];
  if (!command || command.status < MTLCommandBufferStatusCommitted)
    return -1;
  if (wait)
    completed(command);
  if (command.status == MTLCommandBufferStatusError)
    fail(command.error.localizedDescription.UTF8String);
  if (command.status != MTLCommandBufferStatusCompleted)
    return -1;
  const f32 ms =
      static_cast<f32>((command.GPUEndTime - command.GPUStartTime) * 1000);
  g.queries[slot] = nil;
  return ms;
}

void gpu_shutdown() {
  @autoreleasepool {
    g.drawable = nil;
    if (g.uploads) {
      [g.uploads commit];
      completed(g.uploads);
    }
    for (auto& frame : g.frames) {
      completed(frame.command);
      frame.command = nil;
    }
    g.queries.fill(nil);
    g.source.reset();
    ((NSWindow*)glfwGetCocoaWindow(g.window)).contentView.layer = nil;
    g = {};
  }
}

}  // namespace circles
