#pragma once

// osu! replays (.osr): a play's result and its input, frame by frame. The
// input is LZMA-compressed text, "w|x|y|keys," per frame, where w is the ms
// since the previous frame and x, y are in osu! pixels.

#include <circles/math.h>
#include <circles/rules.h>
#include <circles/types.h>

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace circles {

// Input bits in a frame. osu! writes keyboard key 1 as kReplayKey1 |
// kReplayMouse1, and key 2 likewise.
inline constexpr u32 kReplayMouse1 = 1;
inline constexpr u32 kReplayMouse2 = 2;
inline constexpr u32 kReplayKey1 = 4;
inline constexpr u32 kReplayKey2 = 8;

struct ReplayFrame {
  f64  time;      // ms on the gameplay clock
  Vec2 position;  // osu! pixels
  u32  keys;      // kReplay* bits held
};

struct Replay {
  std::string              beatmap_md5;
  std::string              player;
  Counts                   counts;
  i64                      score = 0;
  i32                      max_combo = 0;
  bool                     perfect = false;  // no misses or slider breaks
  u32                      mods = 0;
  i64                      timestamp = 0;  // unix seconds
  std::vector<ReplayFrame> frames;
};

std::vector<u8> encode_replay(const Replay& replay);
// Nothing if the data is malformed or not an osu!standard replay. Without
// `with_frames` the input isn't decompressed, for listing replays quickly.
std::optional<Replay> decode_replay(std::span<const u8> data,
                                    bool                with_frames = true);

std::optional<Replay> read_replay(const std::filesystem::path& path,
                                  bool with_frames = true);
bool write_replay(const std::filesystem::path& path, const Replay& replay);

// A replay file's result, without its frames.
struct ReplayEntry {
  std::filesystem::path path;
  Replay                replay;
};

std::vector<ReplayEntry> scan_replays(const std::filesystem::path& directory);

}  // namespace circles
