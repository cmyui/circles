// Replays as .orz files, and their exchange with osu! as .osr.
#include <circles/orz.h>
#include <circles/replay.h>
#include <tests/support/test.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace fs = std::filesystem;

static circles::Replay sample_replay() {
  circles::Replay replay;
  replay.beatmap_md5 = "0123456789abcdef0123456789abcdef";
  replay.player = "player";
  replay.counts = {.hit300 = 400, .hit100 = 12, .hit50 = 3, .miss = 1};
  replay.score = 12345678;
  replay.max_combo = 789;
  replay.perfect = false;
  replay.timestamp = 1790000000;
  replay.frames = {
      {0, {256, -500}, 0},
      {-1, {256, -500}, 0},
      {10.4, {100.25f, 200.5f}, circles::kReplayKey1 | circles::kReplayMouse1},
      {26.6, {101, 201}, 0},
  };
  for (int i = 0; i < 300; ++i)
    replay.frames.push_back({40 + i * 16.0,
                             {100.25f + i / 3.0f, 316.684235f - i * 0.4f},
                             i % 7 == 0 ? circles::kReplayMouse2 : 0});
  return replay;
}

static std::vector<circles::u8> read_file(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file),
          std::istreambuf_iterator<char>()};
}

static void test_replays_round_trip() {
  circles::Replay replay = sample_replay();
  // Long enough that its length takes two bytes of ULEB128.
  replay.player = std::string(200, 'p');
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
  // The closing frame osu! expects isn't input.
  CHECK_EQ(decoded->frames.size(), replay.frames.size());
  if (decoded->frames.size() != replay.frames.size())
    return;
  // Times are stored in whole milliseconds.
  CHECK_EQ(decoded->frames[1].time, -1);
  CHECK_EQ(decoded->frames[2].time, 10);
  CHECK_EQ(decoded->frames[3].time, 27);
  CHECK_EQ(decoded->frames[2].keys,
           circles::kReplayKey1 | circles::kReplayMouse1);
  // Positions keep the 7 significant digits an .osr's text does: the float
  // nearest those digits.
  CHECK(decoded->frames[2].position == (circles::Vec2{100.25f, 200.5f}));
  CHECK(decoded->frames[4].position == (circles::Vec2{100.25f, 316.6842f}));
  for (size_t i = 0; i < replay.frames.size(); ++i)
    CHECK_NEAR(decoded->frames[i].position.x, replay.frames[i].position.x,
               0.0001f);
}

static void test_listing_skips_frames() {
  const auto decoded =
      circles::decode_replay(circles::encode_replay(sample_replay()), false);
  CHECK(decoded && decoded->frames.empty());
  if (decoded) {
    CHECK_EQ(decoded->player, "player");
    CHECK_EQ(decoded->score, 12345678);
  }
}

static void test_malformed_replays_are_rejected() {
  const auto orz = circles::encode_replay(sample_replay());
  auto       half = orz;
  half.resize(half.size() / 2);
  CHECK(!circles::decode_replay(half));
  CHECK(!circles::decode_replay({}));
  // Only osu!standard replays are playable.
  auto taiko = circles::osr_from_orz(orz);
  CHECK(!taiko.empty());
  if (taiko.empty())
    return;
  taiko[0] = 1;
  CHECK(!circles::decode_replay(circles::orz_from_osr(taiko)));
}

// An .osr exported and imported again is the .orz it came from, so the
// replay made from the game's frames is the one its .osr makes.
static void test_osr_exchange_is_lossless() {
  const auto orz = circles::encode_replay(sample_replay());
  CHECK(!orz.empty());
  const auto osr = circles::osr_from_orz(orz);
  CHECK(!osr.empty() && osr[0] == 0);
  CHECK(circles::orz_from_osr(osr) == orz);
  CHECK(circles::osr_from_orz(circles::orz_from_osr(osr)) == osr);
}

static void test_replay_files_are_imported_and_exported() {
  const fs::path  store = fs::temp_directory_path() / "circles_test_replays";
  const fs::path  exports = fs::temp_directory_path() / "circles_test_exports";
  std::error_code error;
  fs::remove_all(store, error);
  fs::remove_all(exports, error);

  CHECK(circles::write_replay(store / "play.orz", sample_replay()));
  const auto orz = read_file(store / "play.orz");
  CHECK(circles::export_replay(store / "play.orz", exports));
  CHECK(read_file(exports / "play.orz") == orz);
  const auto osr = read_file(exports / "play.osr");
  CHECK(osr == circles::osr_from_orz(orz));

  // An .osr from elsewhere comes in as its .orz, once.
  fs::rename(exports / "play.osr", exports / "other.osr");
  CHECK(circles::import_replay(exports / "other.osr", store) ==
        store / "other.orz");
  CHECK(read_file(store / "other.orz") == orz);
  CHECK(circles::import_replay(exports / "other.osr", store).empty());
  CHECK(fs::exists(exports / "other.osr"));

  // One lying in the store is imported and, held whole by its .orz, removed.
  fs::copy_file(exports / "other.osr", store / "third.osr");
  CHECK_EQ(circles::import_osr_replays(store), 1);
  CHECK(read_file(store / "third.orz") == orz);
  CHECK(!fs::exists(store / "third.osr"));
  CHECK_EQ(circles::scan_replays(store).size(), 3u);

  // What isn't a replay stays out.
  std::ofstream(store / "junk.osr") << "not a replay";
  CHECK(circles::import_replay(store / "junk.osr", exports).empty());
  CHECK_EQ(circles::import_osr_replays(store), 0);
  CHECK(fs::exists(store / "junk.osr"));

  fs::remove_all(store, error);
  fs::remove_all(exports, error);
}

int main() {
  test_replays_round_trip();
  test_listing_skips_frames();
  test_malformed_replays_are_rejected();
  test_osr_exchange_is_lossless();
  test_replay_files_are_imported_and_exported();
  return test_result();
}
