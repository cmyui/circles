#pragma once

// Which sounds a hit plays and from which sample set. Every hit plays the
// normal sound; the whistle, finish and clap additions follow bits 2, 4 and 8
// of the object's hitsound field. Sample sets come from the object when it
// overrides them, else from the timing point in effect.

#include <circles/strings.h>
#include <circles/types.h>

#include <cstddef>
#include <string_view>
#include <vector>

namespace circles {

enum class SampleSet : u8 { Normal, Soft, Drum };

struct Hitsound {
  u32       additions;  // the object's hitsound bits
  SampleSet normal_set;
  SampleSet addition_set;
};

// The sample set a file value selects (1 normal, 2 soft, 3 drum), or
// `fallback` for 0 and anything else.
inline SampleSet sample_set_or(i32 value, SampleSet fallback) {
  switch (value) {
    case 1:
      return SampleSet::Normal;
    case 2:
      return SampleSet::Soft;
    case 3:
      return SampleSet::Drum;
    default:
      return fallback;
  }
}

// Resolves "normal:addition" sample set fields: the normal set falls back to
// the timing point's, and the addition set to the normal set.
inline Hitsound resolve_hitsound(u32              additions,
                                 std::string_view sets,
                                 SampleSet        timing_set) {
  const auto      fields = split(sets, ':');
  const SampleSet normal =
      sample_set_or(fields.empty() ? 0 : field_value(fields[0]), timing_set);
  const SampleSet addition =
      sample_set_or(fields.size() < 2 ? 0 : field_value(fields[1]), normal);
  return {additions, normal, addition};
}

// A hit object's sample volume override, in percent, from its
// "normal:addition:index:volume:filename" field; 0 when it doesn't override.
inline i32 hit_sample_volume(std::string_view hit_sample) {
  const auto fields = split(hit_sample, ':');
  return fields.size() < 4 ? 0 : field_value(fields[3]);
}

// The hitsound at each of a slider's edges: its head, each repeat, then its
// end. Edges come as '|'-separated hitsound bits and "normal:addition" sample
// sets; edges missing from either use the object's own.
inline std::vector<Hitsound> edge_hitsounds(std::string_view sounds,
                                            std::string_view sets,
                                            i32              edges,
                                            const Hitsound&  object,
                                            SampleSet        timing_set) {
  std::vector<Hitsound> result(edges, object);
  const auto            sound_fields = split(sounds, '|');
  const auto            set_fields = split(sets, '|');
  for (i32 i = 0; i < edges; ++i) {
    if (static_cast<size_t>(i) < set_fields.size())
      result[i] = resolve_hitsound(object.additions, set_fields[i], timing_set);
    if (static_cast<size_t>(i) < sound_fields.size() &&
        !sound_fields[i].empty())
      result[i].additions = static_cast<u32>(field_value(sound_fields[i]));
  }
  return result;
}

}  // namespace circles
