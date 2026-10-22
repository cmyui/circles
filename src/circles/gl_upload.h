#pragma once

#include <OpenGL/gl3.h>
#include <array>
#include <cstddef>
#include <vector>

namespace circles {

// OpenGL 4.1 has no persistent buffer mapping. Map only unused ranges, and
// wait for a frame's fence before recycling its ranges.
struct UploadBlock {
  GLuint buffer = 0;
  size_t capacity = 0, used = 0;
};

struct UploadFrame {
  GLsync                   fence = nullptr;
  std::vector<UploadBlock> blocks;
  size_t                   block = 0;
};

struct UploadArena {
  std::array<UploadFrame, 3> frames;
  size_t                     frame = 0;
};

void begin_upload_frame(UploadArena& arena);
// Binds the selected GL_ARRAY_BUFFER and returns the byte offset of the data.
size_t upload_vertices(UploadArena& arena, const void* data, size_t bytes);
void end_upload_frame(UploadArena& arena);
void destroy_upload_arena(UploadArena& arena);

}  // namespace circles
