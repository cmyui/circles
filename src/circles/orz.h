#pragma once

// .orz: an osu! replay compressed losslessly by osu_replay_zip
// (third_party/osu_replay_zip). It holds an .osr's fields and the replay's
// frames, and gives back the .osr those make byte for byte.

#include <circles/types.h>

#include <optional>
#include <span>
#include <vector>

namespace circles {

// A frame as an .osr lists it: the ms since the frame before, the cursor in
// osu! pixels, and the keys held.
struct OrzFrame {
  i32 delta;
  f32 x, y;
  u32 keys;
};

// A replay in the parts an .orz is made from and read into: `osr_fields`,
// an .osr with no input (a length of 0 where it would be), and its frames.
struct OrzReplay {
  std::vector<u8>       osr_fields;
  std::vector<OrzFrame> frames;
};

// Empty if the fields aren't an .osr's, or a frame is one the format can't
// hold: a coordinate past 131072 osu! pixels.
std::vector<u8> encode_orz(std::span<const u8>       osr_fields,
                           std::span<const OrzFrame> frames);
// Nothing if the .orz is damaged or holds no .osr fields. The cursor of each
// frame is the float its .osr's text reads back as. Without `with_frames`
// the frames aren't decoded, for listing replays quickly.
std::optional<OrzReplay> decode_orz(std::span<const u8> orz,
                                    bool                with_frames = true);

// To and from a whole .osr, for exchanging replays with osu!. Each is empty
// if the data isn't what it should be.
std::vector<u8> orz_from_osr(std::span<const u8> osr);
std::vector<u8> osr_from_orz(std::span<const u8> orz);

}  // namespace circles
