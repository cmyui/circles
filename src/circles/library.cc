#include <circles/library.h>
#include <circles/strings.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fosu/beatmap.h>
#include <fosu/parser.h>
#include <string_view>
#include <system_error>

namespace circles {

namespace {

namespace fs = std::filesystem;

BeatmapInfo summarize(const fosu::Beatmap& map,
                      const fs::path&      path,
                      const fs::path&      folder) {
  BeatmapInfo info{
      .osu_path = path.string(),
      .title = std::string(map.title),
      .artist = std::string(map.artist),
      .creator = std::string(map.creator),
      .version = std::string(map.version),
      .audio_path = (folder / std::string(map.audio_filename)).string(),
      .background_path = map.background.empty()
                             ? std::string()
                             : (folder / std::string(map.background)).string(),
      .search_text = lowercase(
          std::string(map.title) + ' ' + std::string(map.artist) + ' ' +
          std::string(map.creator) + ' ' + std::string(map.version) + ' ' +
          std::string(map.source) + ' ' + std::string(map.tags)),
      .preview_time = static_cast<f64>(map.preview_time),
      .hp = map.hp,
      .cs = map.cs,
      .od = map.od,
      .ar = map.ar,
  };
  for (const auto& object : map.hit_objects) {
    info.circles += object.is_circle();
    info.sliders += object.is_slider();
    info.spinners += object.is_spinner();
    info.end_time = std::max({info.end_time, object.time, object.end_time});
  }
  info.first_object_time = map.hit_objects.front().time;

  std::vector<fosu::TimingPoint> points(map.timing_points.begin(),
                                        map.timing_points.end());
  std::stable_sort(points.begin(), points.end(),
                   [](const auto& a, const auto& b) {
                     return a.time < b.time;
                   });
  f64 beat_length = 500, beat_origin = 0;
  for (const auto& point : points) {
    if (point.uninherited && point.beat_length > 0) {
      beat_length = point.beat_length;
      beat_origin = point.time;
      if (point.time <= info.end_time) {
        const f64 bpm = 60000 / beat_length;
        info.bpm_min = info.bpm_min ? std::min(info.bpm_min, bpm) : bpm;
        info.bpm_max = std::max(info.bpm_max, bpm);
      }
    }
    info.beats.push_back(
        {point.time, beat_origin, beat_length, (point.effects & 1) != 0});
  }
  return info;
}

// Easier difficulties have lower approach rates and accuracy demands, and
// fewer objects.
bool easier(const BeatmapInfo& a, const BeatmapInfo& b) {
  const f64 a_rating = a.ar + a.od, b_rating = b.ar + b.od;
  if (a_rating != b_rating)
    return a_rating < b_rating;
  return a.circles + a.sliders + a.spinners <
         b.circles + b.sliders + b.spinners;
}

}  // namespace

Library scan_library(const fs::path& directory) {
  Library               library;
  fosu::Parser          parser;
  std::error_code       error;
  std::vector<fs::path> folders;
  for (const auto& entry : fs::directory_iterator(directory, error))
    if (entry.is_directory())
      folders.push_back(entry.path());
  std::sort(folders.begin(), folders.end());

  for (const auto& folder : folders) {
    std::vector<fs::path> files;
    for (const auto& entry : fs::directory_iterator(folder, error))
      if (entry.is_regular_file() &&
          lowercase(entry.path().extension().string()) == ".osu")
        files.push_back(entry.path());
    std::vector<BeatmapInfo> difficulties;
    for (const auto& file : files) {
      const fosu::Beatmap* map =
          parser.parse_file(file.c_str(), {.calculate_slider_end_times = true});
      if (map && map->mode == 0 && !map->hit_objects.empty())
        difficulties.push_back(summarize(*map, file, folder));
    }
    if (difficulties.empty())
      continue;
    std::sort(difficulties.begin(), difficulties.end(), easier);
    BeatmapSet set{.title = difficulties.front().title,
                   .artist = difficulties.front().artist,
                   .creator = difficulties.front().creator};
    for (auto& info : difficulties) {
      set.maps.push_back(static_cast<i32>(library.maps.size()));
      library.maps.push_back(std::move(info));
    }
    library.sets.push_back(std::move(set));
  }

  std::sort(library.sets.begin(), library.sets.end(),
            [](const BeatmapSet& a, const BeatmapSet& b) {
              const auto a_artist = lowercase(a.artist);
              const auto b_artist = lowercase(b.artist);
              if (a_artist != b_artist)
                return a_artist < b_artist;
              return lowercase(a.title) < lowercase(b.title);
            });
  library.set_of_map.resize(library.maps.size());
  for (size_t s = 0; s < library.sets.size(); ++s)
    for (i32 map : library.sets[s].maps)
      library.set_of_map[map] = static_cast<i32>(s);
  return library;
}

Beat beat_at(const std::vector<BeatSection>& beats, f64 time) {
  BeatSection section{0, 0, 500, false};
  if (!beats.empty()) {
    // The last section starting at or before `time`, else the first.
    const auto next = std::upper_bound(beats.begin(), beats.end(), time,
                                       [](f64 t, const BeatSection& s) {
                                         return t < s.time;
                                       });
    section = next == beats.begin() ? beats.front() : *(next - 1);
  }
  const f64 beats_elapsed = (time - section.beat_origin) / section.beat_length;
  const f64 number = std::floor(beats_elapsed);
  return {section.beat_length, static_cast<i64>(number), beats_elapsed - number,
          section.kiai};
}

}  // namespace circles
