// .osr encoding and decoding.
#include <circles/orz.h>
#include <circles/replay.h>
#include <tests/support/test.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

static void test_replays_round_trip() {
  circles::Replay replay;
  replay.beatmap_md5 = "0123456789abcdef0123456789abcdef";
  // Long enough that its length takes two bytes of ULEB128.
  replay.player = std::string(200, 'p');
  replay.counts = {.hit300 = 400, .hit100 = 12, .hit50 = 3, .miss = 1};
  replay.score = 12345678;
  replay.max_combo = 789;
  replay.perfect = false;
  replay.timestamp = 1790000000;
  replay.frames = {
      {-1500, {256, -500}, 0},
      {10.4, {100.25f, 200.5f}, circles::kReplayKey1 | circles::kReplayMouse1},
      {26.6, {101, 201}, 0},
      {5000, {511.99f, 383.99f}, circles::kReplayMouse2},
  };

  const auto decoded = circles::decode_replay(circles::encode_replay(replay));
  CHECK(decoded);
  if (!decoded)
    return;
  CHECK_EQ(decoded->beatmap_md5, replay.beatmap_md5);
  CHECK_EQ(decoded->player, replay.player);
  CHECK_EQ(decoded->counts.hit300, 400);
  CHECK_EQ(decoded->counts.hit100, 12);
  CHECK_EQ(decoded->counts.hit50, 3);
  CHECK_EQ(decoded->counts.miss, 1);
  CHECK_EQ(decoded->score, 12345678);
  CHECK_EQ(decoded->max_combo, 789);
  CHECK_EQ(decoded->timestamp, 1790000000);
  // The seed frame osu! expects at the end isn't input.
  CHECK_EQ(decoded->frames.size(), replay.frames.size());
  // Times are stored in whole milliseconds.
  CHECK_EQ(decoded->frames[1].time, 10);
  CHECK_EQ(decoded->frames[2].time, 27);
  CHECK_EQ(decoded->frames[0].time, -1500);
  CHECK_NEAR(decoded->frames[1].position.x, 100.25f, 0.01f);
  CHECK_EQ(decoded->frames[1].keys,
           circles::kReplayKey1 | circles::kReplayMouse1);
  CHECK_EQ(decoded->frames[3].keys, circles::kReplayMouse2);
}

// osu! and tools that read its replays take the frames' text and its LZMA
// header as osu!stable writes them.
static void test_frames_are_written_as_stable_writes_them() {
  const auto text = [](circles::f32 x, circles::f32 y) {
    return circles::replay_frames_text({{16, {x, y}, 5}});
  };
  const std::string seed = "-12345|0|0|0,";
  // Up to 7 significant digits, with no trailing zeros.
  CHECK_EQ(text(256, -500), "16|256|-500|5," + seed);
  CHECK_EQ(text(100.25f, 316.684235f), "16|100.25|316.6842|5," + seed);
  // A half rounds away from zero, and negative zero has no sign.
  CHECK_EQ(text(0.48828125f, -0.0f), "16|0.4882813|0|5," + seed);
  CHECK_EQ(text(0.001f, 9999999), "16|0.001|9999999|5," + seed);
  CHECK_EQ(text(0.00001f, 12345678), "16|1E-05|1.234568E+07|5," + seed);

  // With no strings but the frames' hash, the compressed frames start 77
  // bytes in: the LZMA properties, the dictionary's size, the text's length.
  const auto data = circles::encode_replay({});
  CHECK(data.size() > 90);
  if (data.size() <= 90)
    return;
  CHECK_EQ(data[77], 0x5d);
  CHECK_EQ(data[78] | data[79] << 8 | data[80] << 16 | data[81] << 24, 1 << 21);
  CHECK_EQ(data[82], seed.size());
  for (int i = 83; i < 90; ++i)
    CHECK_EQ(data[i], 0);
}

// A replay saved as an .orz reads back as its .osr would.
static void test_orz_replays_round_trip() {
  circles::Replay replay;
  replay.beatmap_md5 = "0123456789abcdef0123456789abcdef";
  replay.player = "player";
  replay.counts = {.hit300 = 400, .hit100 = 12, .miss = 1};
  replay.score = 12345678;
  replay.max_combo = 789;
  replay.timestamp = 1790000000;
  replay.frames = {{0, {256, -500}, 0}, {-1, {256, -500}, 0}};
  for (int i = 0; i < 300; ++i)
    replay.frames.push_back({i * 16.0,
                             {100.25f + i / 3.0f, 200.5f - i * 0.4f},
                             i % 7 == 0 ? circles::kReplayMouse1 : 0});

  const auto osr = circles::encode_replay(replay);
  const auto orz = circles::orz_from_osr(osr);
  CHECK(circles::is_orz(orz) && !circles::is_orz(osr));
  CHECK(orz.size() < osr.size());
  const auto path =
      std::filesystem::temp_directory_path() / "circles_test_replay.orz";
  CHECK(circles::write_replay(path, replay));
  std::ifstream                  file(path, std::ios::binary);
  const std::vector<circles::u8> written((std::istreambuf_iterator<char>(file)),
                                         std::istreambuf_iterator<char>());
  CHECK(written == orz);
  // The frames an .osr gives: positions to the 7 digits its text keeps.
  const auto from_osr = circles::decode_replay(osr);
  const auto read = circles::read_replay(path);
  CHECK(from_osr && read && read->frames.size() == replay.frames.size());
  if (from_osr && read && read->frames.size() == replay.frames.size())
    for (size_t i = 0; i < replay.frames.size(); ++i) {
      CHECK_EQ(read->frames[i].time, from_osr->frames[i].time);
      CHECK(read->frames[i].position == from_osr->frames[i].position);
      CHECK_EQ(read->frames[i].keys, from_osr->frames[i].keys);
    }
  // Listed without its frames, it still has its result.
  const auto listed = circles::read_replay(path, false);
  CHECK(listed && listed->frames.empty());
  if (listed) {
    CHECK_EQ(listed->beatmap_md5, replay.beatmap_md5);
    CHECK_EQ(listed->score, replay.score);
    CHECK_EQ(listed->counts.hit100, 12);
    CHECK_EQ(listed->timestamp, replay.timestamp);
  }
  std::filesystem::remove(path);

  // Damaged, it's rejected rather than read as something else.
  auto damaged = orz;
  damaged.resize(damaged.size() / 2);
  CHECK(!circles::read_orz(damaged));
}

static void test_listing_skips_frames() {
  circles::Replay replay;
  replay.frames = {{0, {0, 0}, 0}};
  const auto decoded =
      circles::decode_replay(circles::encode_replay(replay), false);
  CHECK(decoded && decoded->frames.empty());
}

static void test_malformed_replays_are_rejected() {
  auto data = circles::encode_replay({});
  data.resize(data.size() / 2);
  CHECK(!circles::decode_replay(data));
  CHECK(!circles::decode_replay({}));
  // Only osu!standard replays are playable.
  auto taiko = circles::encode_replay({});
  taiko[0] = 1;
  CHECK(!circles::decode_replay(taiko));
}

int main() {
  test_replays_round_trip();
  test_frames_are_written_as_stable_writes_them();
  test_orz_replays_round_trip();
  test_listing_skips_frames();
  test_malformed_replays_are_rejected();
  return test_result();
}
