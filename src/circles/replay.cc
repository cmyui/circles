#include <circles/md5.h>
#include <circles/mods.h>
#include <circles/orz.h>
#include <circles/replay.h>
#include <circles/strings.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iterator>
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

// The frame that closes an .osr's list, holding osu!'s RNG seed, not input.
constexpr OrzFrame kClosingFrame{static_cast<i32>(kSeedFrame), 0, 0, 0};

// osu!stable's name for the grade: SS is X, and an H marks a silver one.
const char* stable_rank(const Counts& counts, u32 mods) {
  const bool silver = mods & (kHidden | kFlashlight);
  switch (grade(counts)) {
    case Grade::SS:
      return silver ? "XH" : "X";
    case Grade::S:
      return silver ? "SH" : "S";
    case Grade::A:
      return "A";
    case Grade::B:
      return "B";
    case Grade::C:
      return "C";
    case Grade::D:
      return "D";
  }
  return "D";
}

// The .osr of a replay, with no input.
std::vector<u8> osr_fields(const Replay& replay) {
  const auto count = [](i32 n) {
    return static_cast<u16>(std::clamp(n, 0, 0xFFFF));
  };
  const i32 score =
      static_cast<i32>(std::clamp<i64>(replay.score, 0, INT32_MAX));
  // The score's checksum, as osu!stable makes it.
  const std::string checked = std::to_string(count(replay.max_combo)) + "osu" +
                              replay.player + replay.beatmap_md5 +
                              std::to_string(score) +
                              stable_rank(replay.counts, replay.mods);
  Writer            out;
  out.put_u8(kModeStandard);
  out.put_i32(kGameVersion);
  out.put_string(replay.beatmap_md5);
  out.put_string(replay.player);
  out.put_string(
      md5_hex({reinterpret_cast<const u8*>(checked.data()), checked.size()}));
  out.put_u16(count(replay.counts.hit300));
  out.put_u16(count(replay.counts.hit100));
  out.put_u16(count(replay.counts.hit50));
  out.put_u16(count(replay.counts.geki));
  out.put_u16(count(replay.counts.katu));
  out.put_u16(count(replay.counts.miss));
  out.put_i32(score);
  out.put_u16(count(replay.max_combo));
  out.put_u8(replay.perfect);
  out.put_i32(static_cast<i32>(replay.mods));
  out.put_string("");  // the life bar graph
  out.put_i64(replay.timestamp * 10000000 + kUnixEpochTicks);
  out.put_i32(0);  // no input
  out.put_i64(0);  // online score id
  return out.take();
}

// Nothing if the fields are malformed or not an osu!standard replay's.
std::optional<Replay> read_osr_fields(std::span<const u8> data) {
  Reader in(data);
  if (in.get_u8() != kModeStandard)
    return std::nullopt;
  in.get_i32();  // game version
  Replay replay;
  replay.beatmap_md5 = in.get_string();
  replay.player = in.get_string();
  in.get_string();  // the score's checksum
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
  in.get_string();  // the life bar graph
  replay.timestamp = (in.get_i64() - kUnixEpochTicks) / 10000000;
  if (!in.ok())
    return std::nullopt;
  return replay;
}

}  // namespace

std::vector<u8> encode_replay(const Replay& replay) {
  std::vector<OrzFrame> frames;
  frames.reserve(replay.frames.size() + 1);
  i64 previous = 0;
  for (const auto& f : replay.frames) {
    // Whole-ms deltas from whole-ms times, so rounding never accumulates.
    const i64 time = std::llround(f.time);
    frames.push_back({static_cast<i32>(time - previous), f.position.x,
                      f.position.y, f.keys});
    previous = time;
  }
  frames.push_back(kClosingFrame);
  return encode_orz(osr_fields(replay), frames);
}

std::optional<Replay> decode_replay(std::span<const u8> data,
                                    bool                with_frames) {
  const auto orz = decode_orz(data, with_frames);
  if (!orz)
    return std::nullopt;
  auto replay = read_osr_fields(orz->osr_fields);
  if (!replay)
    return std::nullopt;
  f64 time = 0;
  for (const OrzFrame& f : orz->frames) {
    if (f.delta == kSeedFrame)
      continue;
    time += f.delta;
    replay->frames.push_back({time, {f.x, f.y}, f.keys});
  }
  return replay;
}

namespace {

std::vector<u8> read_file(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file),
          std::istreambuf_iterator<char>()};
}

bool write_file(const fs::path& path, std::span<const u8> data) {
  std::error_code error;
  fs::create_directories(path.parent_path(), error);
  std::ofstream file(path, std::ios::binary);
  file.write(reinterpret_cast<const char*>(data.data()),
             static_cast<std::streamsize>(data.size()));
  return static_cast<bool>(file);
}

}  // namespace

std::optional<Replay> read_replay(const fs::path& path, bool with_frames) {
  return decode_replay(read_file(path), with_frames);
}

bool write_replay(const fs::path& path, const Replay& replay) {
  const std::vector<u8> data = encode_replay(replay);
  return !data.empty() && write_file(path, data);
}

fs::path import_replay(const fs::path& source, const fs::path& directory) {
  std::vector<u8> data = read_file(source);
  if (lowercase(source.extension().string()) == ".osr")
    data = orz_from_osr(data);
  const fs::path  path = directory / source.stem().concat(".orz");
  std::error_code error;
  if (!decode_replay(data, false) || fs::exists(path, error) ||
      !write_file(path, data))
    return {};
  return path;
}

i32 import_osr_replays(const fs::path& directory) {
  std::vector<fs::path> sources;
  std::error_code       error;
  for (const auto& entry : fs::directory_iterator(directory, error))
    if (entry.is_regular_file() &&
        lowercase(entry.path().extension().string()) == ".osr")
      sources.push_back(entry.path());
  i32 imported = 0;
  for (const auto& source : sources) {
    const fs::path path = import_replay(source, directory);
    imported += !path.empty();
    // One whose .orz was already there goes too, if that .orz is its own.
    const auto osr = read_file(source);
    if (!osr.empty() && osr_from_orz(read_file(
                            fs::path(source).replace_extension(".orz"))) == osr)
      fs::remove(source, error);
  }
  return imported;
}

bool export_replay(const fs::path& path, const fs::path& directory) {
  const std::vector<u8> orz = read_file(path);
  const std::vector<u8> osr = osr_from_orz(orz);
  const fs::path        name = directory / path.stem();
  return !osr.empty() && write_file(fs::path(name).concat(".osr"), osr) &&
         write_file(fs::path(name).concat(".orz"), orz);
}

std::vector<ReplayEntry> scan_replays(const fs::path& directory) {
  std::vector<ReplayEntry> entries;
  std::error_code          error;
  for (const auto& entry : fs::directory_iterator(directory, error)) {
    if (!entry.is_regular_file() ||
        lowercase(entry.path().extension().string()) != ".orz")
      continue;
    if (auto replay = read_replay(entry.path(), false))
      entries.push_back({entry.path(), std::move(*replay)});
  }
  return entries;
}

}  // namespace circles
