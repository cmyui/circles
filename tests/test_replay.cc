// .osr encoding and decoding.
#include <circles/replay.h>
#include <tests/support/test.h>

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
  test_listing_skips_frames();
  test_malformed_replays_are_rejected();
  return test_result();
}
