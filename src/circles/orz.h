#pragma once

// .orz: an osu! replay (.osr) compressed losslessly by osu_replay_zip
// (third_party/osu_replay_zip), smaller than the .osr's own LZMA leaves it.
// An .orz gives back its .osr byte for byte.

#include <circles/types.h>

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace circles {

// Whether the data is an .orz, by its first byte.
bool is_orz(std::span<const u8> data);

// Empty if the data isn't an .osr.
std::vector<u8> orz_from_osr(std::span<const u8> osr);

// What an .orz holds, as a replay is read from it: its .osr without the
// input, and the input as the .osr has it before compression (replay.h).
// Rebuilding the whole .osr would compress that text only for it to be
// decompressed again.
struct OrzReplay {
  std::vector<u8> osr;
  std::string     frames;
};
// Nothing if the .orz is damaged, or doesn't hold a whole .osr. Without
// `with_frames` the input isn't decoded, for listing replays quickly.
std::optional<OrzReplay> read_orz(std::span<const u8> orz,
                                  bool                with_frames = true);

}  // namespace circles
