#pragma once

#include <circles/hitsounds.h>
#include <circles/stretch.h>
#include <circles/types.h>

#include <atomic>
#include <functional>
#include <memory>
#include <miniaudio.h>
#include <string>
#include <vector>

namespace circles {

// Every sound effect. Each sample set has its own hitsounds, ordered as
// HitsoundKind; skins can replace any of them.
enum class Sound : u8 {
  NormalHitNormal,
  NormalHitWhistle,
  NormalHitFinish,
  NormalHitClap,
  NormalSliderTick,
  SoftHitNormal,
  SoftHitWhistle,
  SoftHitFinish,
  SoftHitClap,
  SoftSliderTick,
  DrumHitNormal,
  DrumHitWhistle,
  DrumHitFinish,
  DrumHitClap,
  DrumSliderTick,
  ComboBreak,
  MenuHover,
  MenuClick,
  SpinnerBonus,
  // Nightcore's drum track.
  NightcoreKick,
  NightcoreClap,
  NightcoreHat,
  NightcoreFinish,
  Count,
};

enum class HitsoundKind : u8 {
  Normal,
  Whistle,
  Finish,
  Clap,
  SliderTick,
  Count
};

inline Sound hitsound_effect(SampleSet set, HitsoundKind kind) {
  return static_cast<Sound>(static_cast<i32>(set) *
                                static_cast<i32>(HitsoundKind::Count) +
                            static_cast<i32>(kind));
}

// The file name, without extension, a skin uses for a sound.
std::string sound_file_stem(Sound sound);

// One playing copy of an effect; several per effect let them overlap.
struct Voice {
  ma_audio_buffer buffer;
  ma_sound        sound;
};

struct SoundBank {
  static constexpr i32 kVoices = 12;
  std::vector<f32>     pcm;  // mono samples shared by every voice
  Voice                voices[kVoices];
  i32                  next = 0;
  bool                 loaded = false;
};

// The music as its sound plays it: the decoded stream, through a time
// stretcher while its tempo isn't 1. The audio thread reads it.
struct MusicSource {
  ma_data_source_base             base;  // first, as miniaudio requires
  ma_resource_manager_data_source stream;
  std::unique_ptr<TimeStretcher>  stretcher;
  std::atomic<f64>                requested_tempo{1};
  f64                             tempo = 1;
  std::atomic<u64>                cursor{0};  // the stream frame playing
};

// The output device, music stream and effects. Neither copyable nor movable:
// miniaudio keeps pointers into it.
struct Audio {
  // False when no output device opened; every call is then a no-op.
  bool                         ready = false;
  ma_engine                    engine;
  ma_device                    device;
  ma_sound                     music;
  std::unique_ptr<MusicSource> music_source;
  bool                         music_loaded = false;
  std::string                  music_path;
  f64                          music_rate = 1;
  bool                         music_keep_pitch = true;
  f32                          music_volume = 0.7f;
  f32                          effects_volume = 0.6f;
  f32       master_volume = 1;  // scales everything; the player's setting
  SoundBank sounds[static_cast<i32>(Sound::Count)];

  // A mono copy of the latest output for the visualizer, written by the audio
  // thread.
  static constexpr u32 kScopeSize = 2048;
  f32                  scope[kScopeSize] = {};
  std::atomic<u32>     scope_write{0};
};

bool audio_init(Audio& audio);
void audio_shutdown(Audio& audio);

// Replaces every effect: decoded from the file `find_file` returns for its
// stem, or synthesized when that's an empty path or won't decode.
void load_sounds(
    Audio&                                                     audio,
    const std::function<std::string(const std::string& stem)>& find_file);

// Streams a file as the music track, replacing the current one; does nothing
// if `path` is already loaded. False if the file can't be opened.
bool music_load(Audio& audio, const std::string& path);
void music_play(Audio& audio);
void music_pause(Audio& audio);
bool music_playing(const Audio& audio);
bool music_at_end(const Audio& audio);
void music_seek(Audio& audio, f64 ms);
f64 music_position(const Audio& audio);  // ms
// Plays the music `rate` times as fast, from now on and for later tracks;
// without `keep_pitch` the pitch rises and falls with it.
void music_set_rate(Audio& audio, f64 rate, bool keep_pitch);
// Speeds up or slows the music on top of its rate, changing its pitch too.
void music_set_speed(Audio& audio, f32 speed);
void music_fade_in(Audio& audio, f32 ms);

// 0 to 1; values outside are clamped.
void set_master_volume(Audio& audio, f32 volume);

// `pan` is -1 (left) to 1 (right).
void play_sound(Audio& audio, Sound sound, f32 volume, f32 pan = 0);

// Loudness of the latest output in log-spaced bands from bass to treble, each
// roughly 0 to 1.
void audio_spectrum(Audio& audio, f32* bands, i32 band_count);

}  // namespace circles
