#include <circles/md5.h>
#include <circles/replay.h>
#include <circles/strings.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <lzma.h>
#include <osu_replay_zip/osu_lzma.h>
#include <system_error>

namespace circles {

namespace {

namespace fs = std::filesystem;

constexpr u8  kModeStandard = 0;
// Any osu!stable version works; lazer's start at 30000001 and append data
// this game doesn't write.
constexpr i32 kGameVersion = 20251001;
// .NET ticks (100 ns since year 1) at the unix epoch.
constexpr i64 kUnixEpochTicks = 621355968000000000;
// A trailing frame with this delta holds osu!'s RNG seed, not input.
constexpr i64 kSeedFrame = -12345;

class Writer {
 public:
  void put_u8(u8 value) { bytes_.push_back(value); }
  void put_u16(u16 value) { little_endian(value, 2); }
  void put_i32(i32 value) { little_endian(static_cast<u32>(value), 4); }
  void put_i64(i64 value) { little_endian(static_cast<u64>(value), 8); }
  void put_bytes(std::span<const u8> data) {
    bytes_.insert(bytes_.end(), data.begin(), data.end());
  }
  // 0x0b, the length as ULEB128, then UTF-8; 0x00 alone for empty.
  void put_string(std::string_view text) {
    if (text.empty()) {
      put_u8(0);
      return;
    }
    put_u8(0x0b);
    for (size_t length = text.size();; length >>= 7) {
      const u8 low = length & 0x7F;
      if (length < 0x80) {
        put_u8(low);
        break;
      }
      put_u8(low | 0x80);
    }
    put_bytes({reinterpret_cast<const u8*>(text.data()), text.size()});
  }
  std::vector<u8> take() { return std::move(bytes_); }

 private:
  void little_endian(u64 value, i32 size) {
    for (i32 i = 0; i < size; ++i)
      bytes_.push_back(static_cast<u8>(value >> (8 * i)));
  }
  std::vector<u8> bytes_;
};

// Reads little-endian fields; once a read runs past the end, every read
// fails and returns 0.
class Reader {
 public:
  explicit Reader(std::span<const u8> data) : data_(data) {}
  bool ok() const { return ok_; }
  u8 get_u8() { return static_cast<u8>(little_endian(1)); }
  u16 get_u16() { return static_cast<u16>(little_endian(2)); }
  i32 get_i32() { return static_cast<i32>(little_endian(4)); }
  i64 get_i64() { return static_cast<i64>(little_endian(8)); }
  std::span<const u8> get_bytes(size_t count) {
    if (!ok_ || count > data_.size() - pos_) {
      ok_ = false;
      return {};
    }
    pos_ += count;
    return data_.subspan(pos_ - count, count);
  }
  std::string get_string() {
    const u8 marker = get_u8();
    if (marker == 0)
      return {};
    if (marker != 0x0b) {
      ok_ = false;
      return {};
    }
    size_t length = 0;
    for (i32 shift = 0; ok_ && shift < 64; shift += 7) {
      const u8 byte = get_u8();
      length |= static_cast<size_t>(byte & 0x7F) << shift;
      if (!(byte & 0x80))
        break;
    }
    const auto text = get_bytes(length);
    return {reinterpret_cast<const char*>(text.data()), text.size()};
  }

 private:
  u64 little_endian(size_t size) {
    const auto field = get_bytes(size);
    u64        value = 0;
    for (size_t i = 0; i < field.size(); ++i)
      value |= static_cast<u64>(field[i]) << (8 * i);
    return value;
  }
  std::span<const u8> data_;
  size_t              pos_ = 0;
  bool                ok_ = true;
};

// LZMA in the "alone" (.lzma) container, byte for byte as osu! compresses
// the same text: its encoder isn't liblzma's, and the two choose different
// matches.
std::vector<u8> compress(std::string_view text) {
  return orz::osu_lzma::compress(
      {reinterpret_cast<const u8*>(text.data()), text.size()});
}

std::optional<std::string> decompress(std::span<const u8> data) {
  lzma_stream stream = LZMA_STREAM_INIT;
  if (lzma_alone_decoder(&stream, UINT64_MAX) != LZMA_OK)
    return std::nullopt;
  stream.next_in = data.data();
  stream.avail_in = data.size();
  std::string out;
  char        buffer[1 << 16];
  lzma_ret    result;
  do {
    stream.next_out = reinterpret_cast<u8*>(buffer);
    stream.avail_out = sizeof(buffer);
    result = lzma_code(&stream, LZMA_FINISH);
    out.append(buffer, sizeof(buffer) - stream.avail_out);
  } while (result == LZMA_OK);
  lzma_end(&stream);
  if (result != LZMA_STREAM_END)
    return std::nullopt;
  return out;
}

// A coordinate as osu!stable prints a float (.NET Framework's default, "G7"):
// 7 significant digits, a half rounding away from zero, trailing zeros
// dropped, and an exponent ("1.5E-05") below 0.0001 or from 10 million up.
void append_coordinate(std::string& text, f32 value) {
  if (value == 0) {
    text += '0';
    return;
  }
  // "d.ddd...e+xx", with every digit the float has, so the 8th is exact.
  char digits[64];
  snprintf(digits, sizeof(digits), "%.40e", static_cast<f64>(std::abs(value)));
  i32 exponent = std::atoi(std::strchr(digits, 'e') + 1);
  i32 mantissa = digits[0] - '0';
  for (i32 i = 2; i < 8; ++i)
    mantissa = mantissa * 10 + (digits[i] - '0');
  if (digits[8] >= '5' && ++mantissa == 10000000) {
    mantissa = 1000000;
    ++exponent;
  }
  char d[8];
  snprintf(d, sizeof(d), "%d", mantissa);
  i32 count = 7;
  while (count > 1 && d[count - 1] == '0')
    --count;
  if (value < 0)
    text += '-';
  if (exponent < -4 || exponent > 6) {
    text += d[0];
    if (count > 1)
      text.append(".").append(d + 1, count - 1);
    char suffix[8];
    snprintf(suffix, sizeof(suffix), "E%c%02d", exponent < 0 ? '-' : '+',
             std::abs(exponent));
    text += suffix;
  } else if (exponent < 0) {
    text.append("0.").append(-exponent - 1, '0').append(d, count);
  } else {
    const i32 whole = exponent + 1;
    text.append(d, std::min(count, whole));
    if (count < whole)
      text.append(whole - count, '0');
    else if (count > whole)
      text.append(".").append(d + whole, count - whole);
  }
}

}  // namespace

std::string replay_frames_text(const std::vector<ReplayFrame>& frames) {
  std::string text;
  i64         previous = 0;
  for (const auto& f : frames) {
    // Whole-ms deltas from whole-ms times, so rounding never accumulates.
    const i64 time = std::llround(f.time);
    text += std::to_string(time - previous);
    text += '|';
    append_coordinate(text, f.position.x);
    text += '|';
    append_coordinate(text, f.position.y);
    text += '|';
    text += std::to_string(f.keys);
    text += ',';
    previous = time;
  }
  return text + std::to_string(kSeedFrame) + "|0|0|0,";
}

namespace {

std::vector<ReplayFrame> decode_frames(const std::string& text) {
  std::vector<ReplayFrame> frames;
  f64                      time = 0;
  for (std::string_view entry : split(text, ',')) {
    const auto fields = split(entry, '|');
    if (fields.size() < 4)
      continue;
    const i64 delta = std::strtoll(std::string(fields[0]).c_str(), nullptr, 10);
    if (delta == kSeedFrame)
      continue;
    time += static_cast<f64>(delta);
    frames.push_back({time,
                      {std::strtof(std::string(fields[1]).c_str(), nullptr),
                       std::strtof(std::string(fields[2]).c_str(), nullptr)},
                      static_cast<u32>(std::strtoul(
                          std::string(fields[3]).c_str(), nullptr, 10))});
  }
  return frames;
}

}  // namespace

std::vector<u8> encode_replay(const Replay& replay) {
  const std::string     frames = replay_frames_text(replay.frames);
  const std::vector<u8> compressed = compress(frames);
  const auto            count = [](i32 n) {
    return static_cast<u16>(std::clamp(n, 0, 0xFFFF));
  };
  Writer out;
  out.put_u8(kModeStandard);
  out.put_i32(kGameVersion);
  out.put_string(replay.beatmap_md5);
  out.put_string(replay.player);
  out.put_string(
      md5_hex({reinterpret_cast<const u8*>(frames.data()), frames.size()}));
  out.put_u16(count(replay.counts.hit300));
  out.put_u16(count(replay.counts.hit100));
  out.put_u16(count(replay.counts.hit50));
  out.put_u16(count(replay.counts.geki));
  out.put_u16(count(replay.counts.katu));
  out.put_u16(count(replay.counts.miss));
  out.put_i32(static_cast<i32>(std::clamp<i64>(replay.score, 0, INT32_MAX)));
  out.put_u16(count(replay.max_combo));
  out.put_u8(replay.perfect);
  out.put_i32(static_cast<i32>(replay.mods));
  out.put_string("");  // the life bar graph
  out.put_i64(replay.timestamp * 10000000 + kUnixEpochTicks);
  out.put_i32(static_cast<i32>(compressed.size()));
  out.put_bytes(compressed);
  out.put_i64(0);  // online score id
  return out.take();
}

std::optional<Replay> decode_replay(std::span<const u8> data,
                                    bool                with_frames) {
  Reader in(data);
  if (in.get_u8() != kModeStandard)
    return std::nullopt;
  in.get_i32();  // game version
  Replay replay;
  replay.beatmap_md5 = in.get_string();
  replay.player = in.get_string();
  in.get_string();  // replay hash
  replay.counts.hit300 = in.get_u16();
  replay.counts.hit100 = in.get_u16();
  replay.counts.hit50 = in.get_u16();
  replay.counts.geki = in.get_u16();
  replay.counts.katu = in.get_u16();
  replay.counts.miss = in.get_u16();
  replay.score = static_cast<u32>(in.get_i32());
  replay.max_combo = in.get_u16();
  replay.perfect = in.get_u8() != 0;
  replay.mods = static_cast<u32>(in.get_i32());
  in.get_string();
  replay.timestamp = (in.get_i64() - kUnixEpochTicks) / 10000000;
  const i32  compressed_size = in.get_i32();
  const auto compressed =
      in.get_bytes(compressed_size > 0 ? compressed_size : 0);
  if (!in.ok())
    return std::nullopt;
  if (with_frames && !compressed.empty()) {
    const auto text = decompress(compressed);
    if (!text)
      return std::nullopt;
    replay.frames = decode_frames(*text);
  }
  return replay;
}

std::optional<Replay> read_replay(const fs::path& path, bool with_frames) {
  std::ifstream         file(path, std::ios::binary);
  const std::vector<u8> data((std::istreambuf_iterator<char>(file)),
                             std::istreambuf_iterator<char>());
  return decode_replay(data, with_frames);
}

bool write_replay(const fs::path& path, const Replay& replay) {
  const std::vector<u8> data = encode_replay(replay);
  std::error_code       error;
  fs::create_directories(path.parent_path(), error);
  std::ofstream file(path, std::ios::binary);
  file.write(reinterpret_cast<const char*>(data.data()),
             static_cast<std::streamsize>(data.size()));
  return static_cast<bool>(file);
}

std::vector<ReplayEntry> scan_replays(const fs::path& directory) {
  std::vector<ReplayEntry> entries;
  std::error_code          error;
  for (const auto& entry : fs::directory_iterator(directory, error)) {
    if (!entry.is_regular_file() ||
        lowercase(entry.path().extension().string()) != ".osr")
      continue;
    if (auto replay = read_replay(entry.path(), false))
      entries.push_back({entry.path(), std::move(*replay)});
  }
  return entries;
}

}  // namespace circles
