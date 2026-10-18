#include <circles/audio.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <optional>
#include <utility>

namespace circles {

namespace {

constexpr ma_uint32 kSampleRate = 48000;
constexpr f32       kTau = 6.2831853f;

// The engine runs without its own device so this callback can copy the mix
// for the visualizer.
void data_callback(ma_device* device,
                   void*      output,
                   const void* /*input*/,
                   ma_uint32 frame_count) {
  auto* audio = static_cast<Audio*>(device->pUserData);
  ma_engine_read_pcm_frames(&audio->engine, output, frame_count, nullptr);
  const auto* samples = static_cast<const f32*>(output);
  u32         write = audio->scope_write.load(std::memory_order_relaxed);
  for (ma_uint32 i = 0; i < frame_count; ++i, ++write)
    audio->scope[write % Audio::kScopeSize] =
        0.5f * (samples[2 * i] + samples[2 * i + 1]);
  audio->scope_write.store(write, std::memory_order_release);
}

// The built-in sounds. Every sample set shares one set of hitsounds.
enum class Synth : u8 {
  HitNormal,
  HitWhistle,
  HitFinish,
  HitClap,
  SliderTick,
  ComboBreak,
  MenuHover,
  MenuClick,
  SpinnerBonus,
  Kick,
  Clap,
  Hat,
  Crash,
};

constexpr i32 kHitsoundKinds = static_cast<i32>(HitsoundKind::Count);
constexpr i32 kHitsoundEffects = 3 * kHitsoundKinds;

Synth synth_for(Sound sound) {
  const i32 index = static_cast<i32>(sound);
  if (index < kHitsoundEffects)
    return static_cast<Synth>(index % kHitsoundKinds);
  return static_cast<Synth>(index - kHitsoundEffects + kHitsoundKinds);
}

std::vector<f32> synthesize(Synth sound) {
  const f32 rate = kSampleRate;
  u32       seed = 0x9E3779B9u + static_cast<u32>(sound) * 7919u;
  auto      noise = [&seed] {
    seed = seed * 1664525u + 1013904223u;
    return static_cast<f32>(seed >> 8) / 8388608.0f - 1;
  };
  auto render = [&](f32 seconds, auto&& sample) {
    std::vector<f32> out(static_cast<size_t>(seconds * rate));
    for (size_t i = 0; i < out.size(); ++i)
      out[i] = sample(i / rate);
    return out;
  };

  std::vector<f32> pcm;
  f32              phase = 0, lowpass = 0, previous = 0;
  switch (sound) {
    case Synth::HitNormal:
      // A soft "tock": a quickly falling pitch over a burst of bright noise.
      pcm = render(0.12f, [&](f32 t) {
        phase += kTau * (160 + 340 * std::exp(-t * 70)) / rate;
        const f32 n = noise();
        lowpass += 0.35f * (n - lowpass);
        return 0.7f * std::sin(phase) * std::exp(-t * 38) +
               0.45f * (n - lowpass) * std::exp(-t * 160);
      });
      break;
    case Synth::HitWhistle:
      pcm = render(0.3f, [&](f32 t) {
        const f32 envelope = std::min(1.0f, t * 200) * std::exp(-t * 10);
        const f32 vibrato = 1 + 0.004f * std::sin(kTau * 7 * t);
        return envelope * (std::sin(kTau * 1320 * vibrato * t) +
                           0.25f * std::sin(kTau * 2640 * vibrato * t));
      });
      break;
    case Synth::HitFinish:
      // Cymbal-like: high-passed noise with a long tail over a low thump.
      pcm = render(1.1f, [&](f32 t) {
        const f32 n = noise();
        const f32 high = n - previous;
        previous = n;
        return 0.5f * high * std::exp(-t * 4) +
               0.5f * std::sin(kTau * 95 * t) * std::exp(-t * 9);
      });
      break;
    case Synth::HitClap:
      // Three quick noise bursts, then a short tail.
      pcm = render(0.22f, [&](f32 t) {
        const f32 n = noise();
        lowpass += 0.25f * (n - lowpass);
        f32 envelope = 0.35f * std::exp(-t * 22);
        for (f32 start : {0.0f, 0.012f, 0.025f})
          if (t >= start)
            envelope = std::max(envelope, std::exp(-(t - start) * 220));
        return (n - lowpass) * envelope;
      });
      break;
    case Synth::SliderTick:
      pcm = render(0.05f, [&](f32 t) {
        return 0.6f * std::sin(kTau * 2600 * t) * std::exp(-t * 90);
      });
      break;
    case Synth::ComboBreak:
      pcm = render(0.35f, [&](f32 t) {
        phase += kTau * 320 * std::exp(-t * 5) / rate;
        return std::sin(phase) * std::exp(-t * 7) * std::min(1.0f, t * 300);
      });
      break;
    case Synth::MenuHover:
      pcm = render(0.035f, [&](f32 t) {
        return 0.5f * std::sin(kTau * 2200 * t) * std::exp(-t * 140);
      });
      break;
    case Synth::MenuClick:
      pcm = render(0.12f, [&](f32 t) {
        phase += kTau * (700 + 500 * std::exp(-t * 40)) / rate;
        return std::sin(phase) * std::exp(-t * 35);
      });
      break;
    case Synth::SpinnerBonus:
      pcm = render(0.7f, [&](f32 t) {
        const f32 envelope = std::min(1.0f, t * 300) * std::exp(-t * 5);
        return envelope *
               (std::sin(kTau * 1568 * t) + 0.5f * std::sin(kTau * 2352 * t) +
                0.25f * std::sin(kTau * 3136 * t));
      });
      break;
    case Synth::Kick:
      // A sine dropping from a punchy click to a low boom.
      pcm = render(0.35f, [&](f32 t) {
        phase += kTau * (48 + 140 * std::exp(-t * 30)) / rate;
        return std::sin(phase) * std::exp(-t * 9) * std::min(1.0f, t * 2000);
      });
      break;
    case Synth::Clap:
      pcm = render(0.25f, [&](f32 t) {
        const f32 n = noise();
        lowpass += 0.4f * (n - lowpass);
        f32 envelope = 0.4f * std::exp(-t * 18);
        for (f32 start : {0.0f, 0.009f, 0.018f})
          if (t >= start)
            envelope = std::max(envelope, std::exp(-(t - start) * 260));
        return (n - lowpass) * envelope;
      });
      break;
    case Synth::Hat:
      pcm = render(0.06f, [&](f32 t) {
        const f32 n = noise();
        const f32 high = n - previous;
        previous = n;
        return high * std::exp(-t * 80);
      });
      break;
    case Synth::Crash:
      pcm = render(1.4f, [&](f32 t) {
        const f32 n = noise();
        const f32 high = n - previous;
        previous = n;
        return high * std::exp(-t * 3) * std::min(1.0f, t * 1000);
      });
      break;
  }

  // Additions, ticks and the drum track sit under the main hit sound.
  const f32 loudness = sound == Synth::HitWhistle   ? 0.56f
                       : sound == Synth::HitClap    ? 0.64f
                       : sound == Synth::SliderTick ? 0.48f
                       : sound == Synth::Hat        ? 0.3f
                       : sound == Synth::Crash      ? 0.4f
                       : sound >= Synth::Kick       ? 0.6f
                                                    : 0.8f;
  f32       peak = 1e-6f;
  for (f32 s : pcm)
    peak = std::max(peak, std::abs(s));
  for (f32& s : pcm)
    s *= loudness / peak;
  return pcm;
}

// Mono samples at the engine's rate, or nothing if the file won't decode.
// Skins silence a sound with an empty file, so that decodes to silence.
std::optional<std::vector<f32>> decode_file(const std::string& path) {
  ma_decoder_config config =
      ma_decoder_config_init(ma_format_f32, 1, kSampleRate);
  ma_uint64 frames = 0;
  void*     data = nullptr;
  if (ma_decode_file(path.c_str(), &config, &frames, &data) != MA_SUCCESS)
    return std::nullopt;
  const auto*      samples = static_cast<const f32*>(data);
  std::vector<f32> pcm(samples, samples + frames);
  ma_free(data, nullptr);
  if (pcm.empty())
    pcm.push_back(0);
  return pcm;
}

void unload_bank(SoundBank& bank) {
  if (!bank.loaded)
    return;
  for (auto& voice : bank.voices) {
    ma_sound_uninit(&voice.sound);
    ma_audio_buffer_uninit(&voice.buffer);
  }
  bank.loaded = false;
}

void load_bank(Audio& audio, SoundBank& bank, std::vector<f32> pcm) {
  unload_bank(bank);
  bank.pcm = std::move(pcm);
  for (auto& voice : bank.voices) {
    ma_audio_buffer_config config = ma_audio_buffer_config_init(
        ma_format_f32, 1, bank.pcm.size(), bank.pcm.data(), nullptr);
    config.sampleRate = kSampleRate;
    ma_audio_buffer_init(&config, &voice.buffer);
    ma_sound_init_from_data_source(
        &audio.engine, &voice.buffer,
        MA_SOUND_FLAG_NO_SPATIALIZATION | MA_SOUND_FLAG_NO_PITCH, nullptr,
        &voice.sound);
  }
  bank.loaded = true;
}

// ---- Music ----

MusicSource& music_source(ma_data_source* source) {
  return *reinterpret_cast<MusicSource*>(source);
}

// Starts stretching afresh from `frame` at the requested tempo.
void restart_music(MusicSource& music, u64 frame) {
  music.tempo = music.requested_tempo.load(std::memory_order_relaxed);
  music.stretcher->reset(frame);
  music.stretcher->set_tempo(music.tempo);
  music.cursor.store(frame, std::memory_order_relaxed);
}

ma_result read_music(ma_data_source* source,
                     void*           out,
                     ma_uint64       frame_count,
                     ma_uint64*      frames_read) {
  auto& music = music_source(source);
  if (music.requested_tempo.load(std::memory_order_relaxed) != music.tempo) {
    const u64 frame = music.cursor.load(std::memory_order_relaxed);
    ma_data_source_seek_to_pcm_frame(&music.stream, frame);
    restart_music(music, frame);
  }
  if (music.tempo == 1) {
    const ma_result result = ma_data_source_read_pcm_frames(
        &music.stream, out, frame_count, frames_read);
    ma_uint64 cursor = 0;
    ma_data_source_get_cursor_in_pcm_frames(&music.stream, &cursor);
    music.cursor.store(cursor, std::memory_order_relaxed);
    return result;
  }

  ma_uint32 channels = 0;
  ma_data_source_get_data_format(&music.stream, nullptr, &channels, nullptr,
                                 nullptr, 0);
  // Reading without an output buffer skips ahead.
  std::vector<f32> discard;
  if (!out)
    discard.resize(frame_count * channels);
  const u64 count = music.stretcher->process(
      out ? static_cast<f32*>(out) : discard.data(), frame_count,
      [&](f32* input, u64 frames, bool& ended) {
        ma_uint64       got = 0;
        const ma_result result =
            ma_data_source_read_pcm_frames(&music.stream, input, frames, &got);
        ended = result == MA_AT_END;
        return static_cast<u64>(got);
      });
  *frames_read = count;
  music.cursor.store(std::llround(music.stretcher->input_position()),
                     std::memory_order_relaxed);
  if (count > 0)
    return MA_SUCCESS;
  return music.stretcher->finished() ? MA_AT_END : MA_BUSY;
}

ma_result seek_music(ma_data_source* source, ma_uint64 frame) {
  auto&           music = music_source(source);
  const ma_result result =
      ma_data_source_seek_to_pcm_frame(&music.stream, frame);
  restart_music(music, frame);
  return result;
}

ma_result music_format(ma_data_source* source,
                       ma_format*      format,
                       ma_uint32*      channels,
                       ma_uint32*      sample_rate,
                       ma_channel*     channel_map,
                       size_t          channel_map_capacity) {
  return ma_data_source_get_data_format(&music_source(source).stream, format,
                                        channels, sample_rate, channel_map,
                                        channel_map_capacity);
}

ma_result music_cursor(ma_data_source* source, ma_uint64* cursor) {
  *cursor = music_source(source).cursor.load(std::memory_order_relaxed);
  return MA_SUCCESS;
}

ma_result music_length(ma_data_source* source, ma_uint64* length) {
  return ma_data_source_get_length_in_pcm_frames(&music_source(source).stream,
                                                 length);
}

ma_data_source_vtable kMusicVtable = {
    read_music, seek_music, music_format, music_cursor, music_length,
    nullptr,    0};

void music_unload(Audio& audio) {
  if (audio.music_loaded) {
    ma_sound_uninit(&audio.music);
    ma_data_source_uninit(&audio.music_source->base);
    ma_resource_manager_data_source_uninit(&audio.music_source->stream);
    audio.music_source.reset();
  }
  audio.music_loaded = false;
  audio.music_path.clear();
}

// Applies the rate and an extra speed factor to the loaded track.
void apply_music_rate(Audio& audio, f32 speed) {
  audio.music_source->requested_tempo.store(
      audio.music_keep_pitch ? audio.music_rate : 1, std::memory_order_relaxed);
  ma_sound_set_pitch(
      &audio.music,
      static_cast<f32>(audio.music_keep_pitch ? 1 : audio.music_rate) * speed);
}

ma_uint32 music_sample_rate(const Audio& audio) {
  ma_uint32 rate = 0;
  ma_sound_get_data_format(&audio.music, nullptr, nullptr, &rate, nullptr, 0);
  return rate ? rate : kSampleRate;
}

// In-place iterative radix-2 FFT; `n` must be a power of two.
void fft(std::complex<f32>* data, i32 n) {
  for (i32 i = 1, j = 0; i < n; ++i) {
    i32 bit = n >> 1;
    for (; j & bit; bit >>= 1)
      j ^= bit;
    j ^= bit;
    if (i < j)
      std::swap(data[i], data[j]);
  }
  for (i32 length = 2; length <= n; length <<= 1) {
    const std::complex<f32> step = std::polar(1.0f, -kTau / length);
    for (i32 start = 0; start < n; start += length) {
      std::complex<f32> w = 1;
      for (i32 k = 0; k < length / 2; ++k, w *= step) {
        const std::complex<f32> even = data[start + k];
        const std::complex<f32> odd = data[start + k + length / 2] * w;
        data[start + k] = even + odd;
        data[start + k + length / 2] = even - odd;
      }
    }
  }
}

}  // namespace

bool audio_init(Audio& audio) {
  ma_engine_config engine_config = ma_engine_config_init();
  engine_config.noDevice = MA_TRUE;
  engine_config.channels = 2;
  engine_config.sampleRate = kSampleRate;
  if (ma_engine_init(&engine_config, &audio.engine) != MA_SUCCESS) {
    fprintf(stderr, "audio: failed to start the engine\n");
    return false;
  }

  ma_device_config device_config =
      ma_device_config_init(ma_device_type_playback);
  device_config.playback.format = ma_format_f32;
  device_config.playback.channels = 2;
  device_config.sampleRate = kSampleRate;
  device_config.dataCallback = data_callback;
  device_config.pUserData = &audio;
  // Small buffers keep the delay between a hit and its sound short.
  device_config.performanceProfile = ma_performance_profile_low_latency;
  device_config.periodSizeInMilliseconds = 5;
  if (ma_device_init(nullptr, &device_config, &audio.device) != MA_SUCCESS) {
    fprintf(stderr, "audio: no output device\n");
    ma_engine_uninit(&audio.engine);
    return false;
  }

  audio.ready = true;
  load_sounds(audio, [](const std::string&) {
    return std::string();
  });
  if (ma_device_start(&audio.device) != MA_SUCCESS)
    fprintf(stderr, "audio: failed to start the output device\n");
  return true;
}

void audio_shutdown(Audio& audio) {
  if (!audio.ready)
    return;
  ma_device_uninit(&audio.device);
  music_unload(audio);
  for (auto& bank : audio.sounds)
    unload_bank(bank);
  ma_engine_uninit(&audio.engine);
  audio.ready = false;
}

std::string sound_file_stem(Sound sound) {
  constexpr const char* kSets[] = {"normal", "soft", "drum"};
  constexpr const char* kKinds[] = {"hitnormal", "hitwhistle", "hitfinish",
                                    "hitclap", "slidertick"};
  constexpr const char* kOthers[] = {
      "combobreak",     "click-short",    "menuclick",     "spinnerbonus",
      "nightcore-kick", "nightcore-clap", "nightcore-hat", "nightcore-finish"};
  const i32 index = static_cast<i32>(sound);
  if (index < kHitsoundEffects)
    return std::string(kSets[index / kHitsoundKinds]) + '-' +
           kKinds[index % kHitsoundKinds];
  return kOthers[index - kHitsoundEffects];
}

void load_sounds(
    Audio&                                                     audio,
    const std::function<std::string(const std::string& stem)>& find_file) {
  if (!audio.ready)
    return;
  for (i32 i = 0; i < static_cast<i32>(Sound::Count); ++i) {
    const auto        sound = static_cast<Sound>(i);
    const std::string path = find_file(sound_file_stem(sound));
    auto              pcm = path.empty() ? std::nullopt : decode_file(path);
    if (!pcm && !path.empty())
      fprintf(stderr, "audio: can't decode %s\n", path.c_str());
    load_bank(audio, audio.sounds[i],
              pcm ? std::move(*pcm) : synthesize(synth_for(sound)));
  }
}

bool music_load(Audio& audio, const std::string& path) {
  if (!audio.ready)
    return false;
  if (audio.music_loaded && audio.music_path == path)
    return true;
  music_unload(audio);
  // Streaming decodes as it plays, so long songs start instantly.
  auto source = std::make_unique<MusicSource>();
  if (ma_resource_manager_data_source_init(
          ma_engine_get_resource_manager(&audio.engine), path.c_str(),
          MA_RESOURCE_MANAGER_DATA_SOURCE_FLAG_STREAM, nullptr,
          &source->stream) != MA_SUCCESS) {
    fprintf(stderr, "audio: can't open %s\n", path.c_str());
    return false;
  }
  ma_uint32 channels = 0, sample_rate = 0;
  ma_data_source_get_data_format(&source->stream, nullptr, &channels,
                                 &sample_rate, nullptr, 0);
  source->stretcher = std::make_unique<TimeStretcher>(channels, sample_rate);
  ma_data_source_config config = ma_data_source_config_init();
  config.vtable = &kMusicVtable;
  ma_data_source_init(&config, &source->base);
  if (ma_sound_init_from_data_source(&audio.engine, &source->base,
                                     MA_SOUND_FLAG_NO_SPATIALIZATION, nullptr,
                                     &audio.music) != MA_SUCCESS) {
    ma_data_source_uninit(&source->base);
    ma_resource_manager_data_source_uninit(&source->stream);
    fprintf(stderr, "audio: can't play %s\n", path.c_str());
    return false;
  }
  audio.music_source = std::move(source);
  audio.music_loaded = true;
  audio.music_path = path;
  ma_sound_set_volume(&audio.music, audio.music_volume);
  apply_music_rate(audio, 1);
  return true;
}

void music_play(Audio& audio) {
  if (audio.music_loaded)
    ma_sound_start(&audio.music);
}

void music_pause(Audio& audio) {
  if (audio.music_loaded)
    ma_sound_stop(&audio.music);
}

bool music_playing(const Audio& audio) {
  return audio.music_loaded && ma_sound_is_playing(&audio.music);
}

bool music_at_end(const Audio& audio) {
  return audio.music_loaded && ma_sound_at_end(&audio.music);
}

void music_seek(Audio& audio, f64 ms) {
  if (audio.music_loaded)
    ma_sound_seek_to_pcm_frame(
        &audio.music, static_cast<ma_uint64>(std::max(0.0, ms) *
                                             music_sample_rate(audio) / 1000));
}

f64 music_length(const Audio& audio) {
  if (!audio.music_loaded)
    return 0;
  ma_uint64 length = 0;
  ma_data_source_get_length_in_pcm_frames(&audio.music_source->stream, &length);
  return length * 1000.0 / music_sample_rate(audio);
}

f64 audio_file_length(const Audio& audio, const std::string& path) {
  if (audio.music_loaded && audio.music_path == path)
    return music_length(audio);
  ma_decoder decoder;
  if (ma_decoder_init_file(path.c_str(), nullptr, &decoder) != MA_SUCCESS)
    return 0;
  ma_uint64 length = 0;
  ma_decoder_get_length_in_pcm_frames(&decoder, &length);
  const f64 ms = length * 1000.0 / decoder.outputSampleRate;
  ma_decoder_uninit(&decoder);
  return ms;
}

f64 music_position(const Audio& audio) {
  if (!audio.music_loaded)
    return 0;
  ma_uint64 cursor = 0;
  ma_sound_get_cursor_in_pcm_frames(&audio.music, &cursor);
  return cursor * 1000.0 / music_sample_rate(audio);
}

void music_set_rate(Audio& audio, f64 rate, bool keep_pitch) {
  audio.music_rate = rate;
  audio.music_keep_pitch = keep_pitch;
  if (audio.music_loaded)
    apply_music_rate(audio, 1);
}

void music_set_speed(Audio& audio, f32 speed) {
  if (audio.music_loaded)
    apply_music_rate(audio, speed);
}

void set_master_volume(Audio& audio, f32 volume) {
  audio.master_volume = std::clamp(volume, 0.0f, 1.0f);
  if (audio.ready)
    ma_engine_set_volume(&audio.engine, audio.master_volume);
}

void music_fade_in(Audio& audio, f32 ms) {
  // The fade multiplies the volume set by music_load, so it goes from 0 to 1.
  if (audio.music_loaded)
    ma_sound_set_fade_in_milliseconds(&audio.music, 0, 1,
                                      static_cast<ma_uint64>(ms));
}

void play_sound(Audio& audio, Sound sound, f32 volume, f32 pan) {
  if (!audio.ready || audio.effects_muted)
    return;
  auto&  bank = audio.sounds[static_cast<i32>(sound)];
  Voice& voice = bank.voices[bank.next];
  bank.next = (bank.next + 1) % SoundBank::kVoices;
  ma_sound_seek_to_pcm_frame(&voice.sound, 0);
  ma_sound_set_volume(&voice.sound, volume * audio.effects_volume);
  ma_sound_set_pan(&voice.sound, pan);
  ma_sound_start(&voice.sound);
}

void audio_spectrum(Audio& audio, f32* bands, i32 band_count) {
  constexpr i32     kSize = 1024;
  std::complex<f32> data[kSize];
  const u32         end = audio.scope_write.load(std::memory_order_acquire);
  for (i32 i = 0; i < kSize; ++i) {
    const f32 hann = 0.5f - 0.5f * std::cos(kTau * i / (kSize - 1));
    data[i] = audio.scope[(end - kSize + i) % Audio::kScopeSize] * hann;
  }
  fft(data, kSize);

  // Bands span 40 Hz to 16 kHz, each a fixed ratio wider than the last.
  constexpr f32 kLow = 40, kHigh = 16000;
  for (i32 b = 0; b < band_count; ++b) {
    const f32 f0 =
        kLow * std::pow(kHigh / kLow, static_cast<f32>(b) / band_count);
    const f32 f1 =
        kLow * std::pow(kHigh / kLow, static_cast<f32>(b + 1) / band_count);
    const i32 k0 = std::max(1, static_cast<i32>(f0 * kSize / kSampleRate));
    const i32 k1 = std::clamp(static_cast<i32>(f1 * kSize / kSampleRate), k0,
                              kSize / 2 - 1);
    f32       peak = 0;
    for (i32 k = k0; k <= k1; ++k)
      peak = std::max(peak, std::abs(data[k]));
    // Loudness is perceived logarithmically: map about -40..+10 dB to 0..1.
    bands[b] =
        std::clamp((20 * std::log10(peak + 1e-6f) + 40) / 50, 0.0f, 1.0f);
  }
}

}  // namespace circles
