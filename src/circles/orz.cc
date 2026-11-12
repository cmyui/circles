#include <circles/orz.h>

// The library is headers, compiled here alone: they define functions that
// aren't inline, and the tables of its models' starting states are large.
#include <osu_replay_zip/orz_read.h>
#include <osu_replay_zip/orz_write.h>

namespace circles {

bool is_orz(std::span<const u8> data) {
  return !data.empty() && data[0] == orz::ORZ_MAGIC;
}

std::vector<u8> orz_from_osr(std::span<const u8> osr) {
  orz::orz_state  state;
  std::vector<u8> out;
  orz::osr_to_orz(osr, out, state);
  return out;
}

std::optional<OrzReplay> read_orz(std::span<const u8> data, bool with_frames) {
  orz::_decoded_orz decoded;
  if (with_frames ? !orz::orz_decode_frames(data, decoded)
                  : !decoded.file.decode(data))
    return std::nullopt;
  const auto& file = decoded.file;
  if (!(file.encode_info & orz::ORZ_ENCODE_INFO::has_osr_header))
    return std::nullopt;
  // The .osr's fields either side of its input, with no life bar and no
  // input between them.
  const auto parts = orz::osr_parts::decode(
      orz::container_data(file, orz::orz_tag::osr_meta_data));
  OrzReplay replay;
  orz::add_to_u8(replay.osr, parts.header);
  replay.osr.push_back(0);
  orz::add_to_u8(replay.osr, parts.after_life);
  orz::add_to_u8(replay.osr, orz::u32{0});
  orz::add_to_u8(replay.osr, parts.tail);
  if (with_frames)
    replay.frames = orz::replay_text(decoded.frames, file);
  return replay;
}

}  // namespace circles
