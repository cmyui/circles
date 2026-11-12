#pragma once

// Replays: a play's result and its input, frame by frame. They are kept as
// .orz files (orz.h), which hold what osu!'s .osr does, so a replay can be
// exchanged with osu! as one.

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

// A replay as an .orz. Empty if it can't be made: a frame further than
// 131072 osu! pixels out.
std::vector<u8> encode_replay(const Replay& replay);
// Nothing if the data is malformed or not an osu!standard replay. Without
// `with_frames` the input isn't decoded, for listing replays quickly.
std::optional<Replay> decode_replay(std::span<const u8> data,
                                    bool                with_frames = true);

std::optional<Replay> read_replay(const std::filesystem::path& path,
                                  bool with_frames = true);
bool write_replay(const std::filesystem::path& path, const Replay& replay);

// Exchanging replays with osu! and other copies of the game. An .osr goes in
// as the .orz that gives it back byte for byte, and comes out the same way.

// Brings the replay at `source`, an .osr or an .orz, into `directory` as an
// .orz of the same name. Returns its path there, or nothing if it isn't an
// osu!standard replay or one of that name is already there.
std::filesystem::path import_replay(const std::filesystem::path& source,
                                    const std::filesystem::path& directory);
// Imports each .osr lying in `directory`, removing it once its .orz gives it
// back byte for byte. Returns how many came in.
i32 import_osr_replays(const std::filesystem::path& directory);
// Writes the replay at `path`, an .orz, into `directory` as both an .osr and
// an .orz of its name. False if either can't be written.
bool export_replay(const std::filesystem::path& path,
                   const std::filesystem::path& directory);

// A replay file's result, without its frames.
struct ReplayEntry {
  std::filesystem::path path;
  Replay                replay;
};

std::vector<ReplayEntry> scan_replays(const std::filesystem::path& directory);

}  // namespace circles
