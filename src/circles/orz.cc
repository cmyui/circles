#include <circles/orz.h>

// The library is headers, compiled here alone: they define functions that
// aren't inline, and the tables of its models' starting states are large.
#include <cstring>
#include <osu_replay_zip/orz_read.h>
#include <osu_replay_zip/orz_write.h>

namespace circles {

static_assert(sizeof(OrzFrame) == sizeof(orz::_osr_frame));

std::vector<u8> encode_orz(std::span<const u8>       osr_fields,
                           std::span<const OrzFrame> frames) {
  std::vector<orz::_osr_frame> list(frames.size());
  if (!frames.empty())
    std::memcpy(list.data(), frames.data(), frames.size_bytes());
  orz::orz_state  state;
  std::vector<u8> out;
  orz::osr_frames_to_orz(osr_fields, list, out, state);
  return out;
}

std::optional<OrzReplay> decode_orz(std::span<const u8> data,
                                    bool                with_frames) {
  orz::_decoded_orz decoded;
  if (with_frames ? !orz::orz_decode_frames(data, decoded)
                  : !decoded.file.decode(data))
    return std::nullopt;
  OrzReplay replay{.osr_fields = orz::osr_fields(decoded.file)};
  if (replay.osr_fields.empty())
    return std::nullopt;
  // Frames coded from 7-digit text come back as floats that print the same
  // 7 digits, not always the one the text reads as. Others are exact.
  const bool seven_digits =
      !(decoded.file.encode_info & (orz::ORZ_ENCODE_INFO::text_shortest |
                                    orz::ORZ_ENCODE_INFO::replay_verbatim));
  replay.frames.reserve(decoded.frames.size());
  for (const auto& f : decoded.frames)
    replay.frames.push_back(
        {f.delta, seven_digits ? orz::dotnet::g7_float(f.x) : f.x,
         seven_digits ? orz::dotnet::g7_float(f.y) : f.y, f.keys});
  return replay;
}

std::vector<u8> orz_from_osr(std::span<const u8> osr) {
  orz::orz_state  state;
  std::vector<u8> out;
  orz::osr_to_orz(osr, out, state);
  return out;
}

std::vector<u8> osr_from_orz(std::span<const u8> data) {
  std::vector<u8> out;
  if (!orz::orz_to_osr(data, out))
    out.clear();
  return out;
}

}  // namespace circles
