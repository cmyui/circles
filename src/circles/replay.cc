#include <circles/md5.h>
#include <circles/replay.h>
#include <circles/strings.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <lzma.h>
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

// LZMA in the "alone" (.lzma) container osu! uses.
std::vector<u8> compress(std::string_view text) {
  lzma_options_lzma options;
  lzma_lzma_preset(&options, LZMA_PRESET_DEFAULT);
  lzma_stream stream = LZMA_STREAM_INIT;
  if (lzma_alone_encoder(&stream, &options) != LZMA_OK)
    return {};
  stream.next_in = reinterpret_cast<const u8*>(text.data());
  stream.avail_in = text.size();
  std::vector<u8> out;
  u8              buffer[1 << 16];
  lzma_ret        result;
  do {
    stream.next_out = buffer;
    stream.avail_out = sizeof(buffer);
    result = lzma_code(&stream, LZMA_FINISH);
    out.insert(out.end(), buffer, buffer + sizeof(buffer) - stream.avail_out);
  } while (result == LZMA_OK);
  lzma_end(&stream);
  return result == LZMA_STREAM_END ? out : std::vector<u8>();
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

std::string encode_frames(const std::vector<ReplayFrame>& frames) {
  std::string text;
  i64         previous = 0;
  char        frame[96];
  for (const auto& f : frames) {
    // Whole-ms deltas from whole-ms times, so rounding never accumulates.
    const i64 time = std::llround(f.time);
    snprintf(frame, sizeof(frame), "%lld|%.2f|%.2f|%u,",
             static_cast<long long>(time - previous), f.position.x,
             f.position.y, f.keys);
    text += frame;
    previous = time;
  }
  snprintf(frame, sizeof(frame), "%lld|0|0|0,",
           static_cast<long long>(kSeedFrame));
  return text + frame;
}

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
  const std::string     frames = encode_frames(replay.frames);
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
  out.put_u16(0);  // geki, for other modes
  out.put_u16(0);  // katu, likewise
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
  in.get_u16();
  in.get_u16();
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
