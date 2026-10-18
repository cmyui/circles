#pragma once

#include <circles/types.h>

#include <filesystem>
#include <string>
#include <vector>

namespace circles {

// The beat at one timing point onward, for animating to the music.
struct BeatSection {
  f64  time;
  f64  beat_origin;  // time of the uninherited point the beats count from
  f64  beat_length;  // ms per beat
  bool kiai;
};

// What song select shows about one difficulty. The map itself is parsed again
// for gameplay.
struct BeatmapInfo {
  std::string              osu_path;
  std::string              md5;  // of the .osu file; replays name maps by it
  std::string              title;
  std::string              artist;
  std::string              creator;
  std::string              version;  // the difficulty's name
  std::string              audio_path;
  std::string              background_path;  // empty if none
  // Lowercase metadata and tags, for search.
  std::string              search_text;
  f64                      preview_time;  // ms; negative if unset
  f64                      hp;
  f64                      cs;
  f64                      od;
  f64                      ar;
  i32                      circles = 0;
  i32                      sliders = 0;
  i32                      spinners = 0;
  f64                      first_object_time = 0;
  f64                      end_time = 0;  // when the last object ends
  f64                      bpm_min = 0;
  f64                      bpm_max = 0;
  std::vector<BeatSection> beats;
};

// Difficulties sharing a folder, easiest first.
struct BeatmapSet {
  std::string      title;
  std::string      artist;
  std::string      creator;
  std::vector<i32> maps;  // indices into Library::maps
};

struct Library {
  std::vector<BeatmapInfo> maps;
  std::vector<BeatmapSet>  sets;  // sorted by artist, then title
  std::vector<i32>         set_of_map;
};

// Reads every osu!standard .osu file in the subfolders of `directory`.
Library scan_library(const std::filesystem::path& directory);

struct Beat {
  f64  length;  // ms
  i64  number;  // beats since the timing point the beats count from
  f64  phase;   // 0 to 1 through the current beat
  bool kiai;
};

// The beat at `time`, or 120 BPM when the map has no timing.
Beat beat_at(const std::vector<BeatSection>& beats, f64 time);

}  // namespace circles
