#include <circles/gl_upload.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace circles {
namespace {

[[noreturn]] void upload_failed(const char* message) {
  std::fprintf(stderr, "OpenGL upload: %s\n", message);
  std::abort();
}

void wait_for_frame(UploadFrame& frame) {
  if (!frame.fence)
    return;
  GLenum result = glClientWaitSync(frame.fence, GL_SYNC_FLUSH_COMMANDS_BIT, 0);
  while (result == GL_TIMEOUT_EXPIRED)
    result = glClientWaitSync(frame.fence, 0, 1000000000);
  if (result == GL_WAIT_FAILED)
    upload_failed("fence wait failed");
  glDeleteSync(frame.fence);
  frame.fence = nullptr;
}

}  // namespace

void begin_upload_frame(UploadArena& arena) {
  auto& frame = arena.frames[arena.frame];
  wait_for_frame(frame);
  frame.block = 0;
  for (auto& block : frame.blocks)
    block.used = 0;
}

size_t upload_vertices(UploadArena& arena, const void* data, size_t bytes) {
  auto& frame = arena.frames[arena.frame];
  for (;;) {
    if (frame.block == frame.blocks.size()) {
      UploadBlock block{.capacity = std::max<size_t>(1024 * 1024, bytes)};
      glGenBuffers(1, &block.buffer);
      glBindBuffer(GL_ARRAY_BUFFER, block.buffer);
      glBufferData(GL_ARRAY_BUFFER, block.capacity, nullptr, GL_STREAM_DRAW);
      frame.blocks.push_back(block);
    }
    auto&        block = frame.blocks[frame.block];
    const size_t offset = (block.used + 255) & ~size_t(255);
    if (offset + bytes > block.capacity) {
      ++frame.block;
      continue;
    }
    glBindBuffer(GL_ARRAY_BUFFER, block.buffer);
    void* mapped =
        glMapBufferRange(GL_ARRAY_BUFFER, offset, bytes,
                         GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_RANGE_BIT |
                             GL_MAP_UNSYNCHRONIZED_BIT);
    if (!mapped)
      upload_failed("mapping failed");
    std::memcpy(mapped, data, bytes);
    if (!glUnmapBuffer(GL_ARRAY_BUFFER))
      upload_failed("buffer contents invalidated");
    block.used = offset + bytes;
    return offset;
  }
}

void end_upload_frame(UploadArena& arena) {
  arena.frames[arena.frame].fence =
      glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
  if (!arena.frames[arena.frame].fence)
    upload_failed("fence creation failed");
  arena.frame = (arena.frame + 1) % arena.frames.size();
}

void destroy_upload_arena(UploadArena& arena) {
  for (auto& frame : arena.frames) {
    wait_for_frame(frame);
    for (auto& block : frame.blocks)
      glDeleteBuffers(1, &block.buffer);
  }
  arena = {};
}

}  // namespace circles
