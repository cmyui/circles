#include <circles/audio.h>

#include <CoreAudio/CoreAudio.h>
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
  auto*     audio = static_cast<Audio*>(device->pUserData);
  const u32 cycle = audio->cycle_count.load(std::memory_order_relaxed);
  audio->cycle_starts[cycle % Audio::kCycles].store(
      clock_gettime_nsec_np(CLOCK_UPTIME_RAW) * 1e-9,
      std::memory_order_relaxed);
  audio->cycle_count.store(cycle + 1, std::memory_order_release);
  ma_engine_read_pcm_frames(&audio->engine, output, frame_count, nullptr);
  const auto* samples = static_cast<const f32*>(output);
  u32         write = audio->scope_write.load(std::memory_order_relaxed);
  for (ma_uint32 i = 0; i < frame_count; ++i, ++write)
    audio->scope[write % Audio::kScopeSize] =
        0.5f * (samples[2 * i] + samples[2 * i + 1]);
  audio->scope_write.store(write, std::memory_order_release);
}

// The output moved to another device, whose latency differs.
void notification_callback(const ma_device_notification* notification) {
  if (notification->type == ma_device_notification_type_rerouted)
    static_cast<Audio*>(notification->pDevice->pUserData)
        ->latency_stale.store(true, std::memory_order_relaxed);
}

// A Core Audio property of `object`, or 0 if it has none.
UInt32 core_audio_property(AudioObjectID               object,
                           AudioObjectPropertySelector selector,
                           AudioObjectPropertyScope    scope) {
  const AudioObjectPropertyAddress address{selector, scope,
                                           kAudioObjectPropertyElementMain};
  UInt32                           value = 0, size = sizeof(value);
  if (AudioObjectGetPropertyData(object, &address, 0, nullptr, &size, &value) !=
      noErr)
    return 0;
  return value;
}

// The default output's delay from being handed audio to playing it: the
// device's latency and safety offset, its IO buffer, and its first output
// stream's latency.
f64 query_output_latency() {
  constexpr AudioObjectPropertyScope kGlobal = kAudioObjectPropertyScopeGlobal;
  constexpr AudioObjectPropertyScope kOutput = kAudioObjectPropertyScopeOutput;
  const AudioDeviceID                device =
      core_audio_property(kAudioObjectSystemObject,
                          kAudioHardwarePropertyDefaultOutputDevice, kGlobal);
  const AudioObjectPropertyAddress address{
      kAudioDevicePropertyNominalSampleRate, kGlobal,
      kAudioObjectPropertyElementMain};
  Float64 rate = 0;
  UInt32  size = sizeof(rate);
  if (!device ||
      AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, &rate) !=
          noErr ||
      rate <= 0)
    return 0;
  const AudioStreamID stream =
      core_audio_property(device, kAudioDevicePropertyStreams, kOutput);
  const UInt32 frames =
      core_audio_property(device, kAudioDevicePropertyLatency, kOutput) +
      core_audio_property(device, kAudioDevicePropertySafetyOffset, kOutput) +
      core_audio_property(device, kAudioDevicePropertyBufferFrameSize,
                          kGlobal) +
      core_audio_property(stream, kAudioStreamPropertyLatency, kGlobal);
  return frames * 1000.0 / rate;
}

// The built-in sounds. Hitsounds come in three sample sets, as in osu!
// skins: normal (crisp), soft (warmer and duller) and drum (a kit). They're
// percussion made of filtered noise, sines and square waves; the hits are
// noise only, flat and drum-like, so hearing thousands in a map doesn't
// wear.
constexpr i32 kHitsoundKinds = static_cast<i32>(HitsoundKind::Count);
constexpr i32 kHitsoundEffects = 3 * kHitsoundKinds;

using Samples = std::vector<f32>;

f32 time_of(size_t i) { return static_cast<f32>(i) / kSampleRate; }

// Each sample at time t, from f(t).
template <typename F>
Samples render(f32 seconds, F&& f) {
  Samples out(static_cast<size_t>(seconds * kSampleRate));
  for (size_t i = 0; i < out.size(); ++i)
    out[i] = f(time_of(i));
  return out;
}

// Each sample of `x` at time t becomes f(t, it).
template <typename F>
Samples shaped(Samples x, F&& f) {
  for (size_t i = 0; i < x.size(); ++i)
    x[i] = f(time_of(i), x[i]);
  return x;
}

Samples noise(f32 seconds, u32 seed) {
  return render(seconds, [&](f32) {
    seed = seed * 1664525u + 1013904223u;
    return static_cast<f32>(seed >> 8) / 8388608.0f - 1;
  });
}

enum class Filter : u8 { Lowpass, Highpass, Bandpass };

// The RBJ cookbook's biquads; the bandpass peaks at 0 dB.
Samples filtered(Samples x, Filter kind, f32 freq, f32 q = 0.707f) {
  const f32 w = kTau * freq / kSampleRate;
  const f32 alpha = std::sin(w) / (2 * q), cos = std::cos(w);
  f32       b[3];
  if (kind == Filter::Lowpass)
    b[0] = b[2] = (1 - cos) / 2, b[1] = 1 - cos;
  else if (kind == Filter::Highpass)
    b[0] = b[2] = (1 + cos) / 2, b[1] = -(1 + cos);
  else
    b[0] = alpha, b[1] = 0, b[2] = -alpha;
  const f32 a0 = 1 + alpha, a1 = -2 * cos / a0, a2 = (1 - alpha) / a0;
  f32       x1 = 0, x2 = 0, y1 = 0, y2 = 0;
  for (f32& v : x) {
    const f32 y = (b[0] * v + b[1] * x1 + b[2] * x2) / a0 - a1 * y1 - a2 * y2;
    x2 = x1, x1 = v, y2 = y1, y1 = y;
    v = y;
  }
  return x;
}

// Falls by e every `tau` seconds from `start`; silent before it.
f32 decay(f32 t, f32 tau, f32 start = 0) {
  return t < start ? 0 : std::exp(-(t - start) / tau);
}

f32 attack(f32 t, f32 seconds) { return std::min(1.0f, t / seconds); }

// A sine whose pitch falls from `start` to `end` Hz, by e every `tau`.
Samples sweep(f32 seconds, f32 start, f32 end, f32 tau) {
  f32 phase = 0;
  return render(seconds, [&](f32 t) {
    phase += kTau * (end + (start - end) * std::exp(-t / tau)) / kSampleRate;
    return std::sin(phase);
  });
}

// Six square waves at inharmonic ratios: a cymbal's shimmer.
Samples metal(f32 seconds, f32 scale) {
  return render(seconds, [&](f32 t) {
    f32 sum = 0;
    for (f32 f : {205.3f, 304.4f, 369.6f, 522.7f, 540.0f, 800.0f})
      sum += std::sin(kTau * f * scale * t) >= 0 ? 1 : -1;
    return sum / 6;
  });
}

// A flat, drum-like hit: noise through a band twice, so no pitch stands
// out, dying by e every `tau`, over a little low noise for a thump.
Samples drum_hit(f32 seconds, f32 centre, f32 q, f32 tau, f32 thump_tau,
                 u32 seed) {
  const Samples thump =
      filtered(noise(seconds, seed + 1), Filter::Lowpass, 300);
  size_t i = 0;
  return shaped(filtered(filtered(noise(seconds, seed), Filter::Bandpass,
                                  centre, q),
                         Filter::Bandpass, centre, q),
                [&](f32 t, f32 band) {
                  return (band * decay(t, tau) +
                          0.4f * thump[i++] * decay(t, thump_tau)) *
                         attack(t, 0.0005f);
                });
}

Samples crash(f32 seconds, f32 tau, f32 cutoff, f32 attack_s, u32 seed) {
  const Samples hiss = filtered(
      filtered(noise(seconds, seed), Filter::Highpass, 4000), Filter::Lowpass,
      10000);
  const Samples hit =
      filtered(noise(seconds, seed + 1), Filter::Bandpass, 3000);
  size_t i = 0;
  return shaped(filtered(metal(seconds, 1), Filter::Highpass, cutoff),
                [&](f32 t, f32 shimmer) {
                  const f32 out = shimmer * decay(t, tau) +
                                  0.5f * hiss[i] * decay(t, tau * 0.7f) +
                                  0.6f * hit[i] * decay(t, 0.01f);
                  ++i;
                  return out * attack(t, attack_s);
                });
}

// Three quick bursts of band noise, then a tail.
Samples clap(f32 centre, f32 burst_tau, f32 tail_tau, u32 seed) {
  return shaped(
      filtered(noise(0.33f, seed), Filter::Bandpass, centre, 1.2f),
      [&](f32 t, f32 band) {
        f32 envelope = 0.7f * decay(t, tail_tau, 0.027f);
        for (f32 start : {0.0f, 0.009f, 0.018f})
          envelope = std::max(envelope, decay(t, burst_tau, start));
        return band * envelope;
      });
}

// One of the hitsounds, by sample set and kind.
Samples hitsound(Sound sound) {
  switch (sound) {
    case Sound::NormalHitNormal:
      return drum_hit(0.2f, 1150, 0.9f, 0.03f, 0.015f, 1);
    case Sound::NormalHitWhistle: {
      const Samples breath = filtered(noise(0.35f, 3), Filter::Bandpass, 2000, 4);
      size_t i = 0;
      return shaped(sweep(0.35f, 2050, 1950, 0.15f), [&](f32 t, f32 tone) {
        tone += 0.15f * std::sin(kTau * 3950 * t) +
                0.1f * breath[i++] * decay(t, 0.04f);
        return tone * attack(t, 0.003f) * decay(t, 0.07f);
      });
    }
    case Sound::NormalHitFinish:
    case Sound::DrumHitFinish:
      return crash(1.6f, 0.35f, 3500, 0.001f, 4);
    case Sound::NormalHitClap:
      return clap(1100, 0.004f, 0.045f, 5);
    case Sound::NormalSliderTick:
      return shaped(filtered(noise(0.06f, 6), Filter::Highpass, 3000),
                    [](f32 t, f32 click) {
                      return 0.3f * click * decay(t, 0.002f) +
                             std::sin(kTau * 3200 * t) * decay(t, 0.01f);
                    });
    case Sound::SoftHitNormal:
      return filtered(drum_hit(0.18f, 620, 0.8f, 0.025f, 0.015f, 7),
                      Filter::Lowpass, 1500);
    case Sound::SoftHitWhistle:
      return shaped(sweep(0.45f, 1550, 1480, 0.2f), [](f32 t, f32 tone) {
        return tone * attack(t, 0.008f) * decay(t, 0.09f);
      });
    case Sound::SoftHitFinish:
      return filtered(crash(2, 0.45f, 3000, 0.005f, 8), Filter::Lowpass, 6000);
    case Sound::SoftHitClap:
      return clap(850, 0.006f, 0.06f, 10);
    case Sound::SoftSliderTick:
      return render(0.05f, [](f32 t) {
        return std::sin(kTau * 2200 * t) * attack(t, 0.001f) * decay(t, 0.01f);
      });
    case Sound::DrumHitNormal: {
      const Samples click = filtered(noise(0.55f, 11), Filter::Bandpass, 3000);
      size_t i = 0;
      return shaped(sweep(0.55f, 160, 55, 0.025f), [&](f32 t, f32 kick) {
        return (kick * decay(t, 0.12f) + 0.3f * click[i++] * decay(t, 0.003f)) *
               attack(t, 0.0005f);
      });
    }
    case Sound::DrumHitWhistle: {
      const Samples skin = filtered(noise(0.55f, 12), Filter::Lowpass, 1500);
      size_t i = 0;
      return shaped(sweep(0.55f, 220, 160, 0.06f), [&](f32 t, f32 tom) {
        return (tom * decay(t, 0.12f) + 0.2f * skin[i++] * decay(t, 0.02f)) *
               attack(t, 0.0005f);
      });
    }
    case Sound::DrumHitClap:
      return shaped(filtered(noise(0.35f, 13), Filter::Bandpass, 4000, 0.7f),
                    [](f32 t, f32 wires) {
                      return (0.5f * std::sin(kTau * 190 * t) * decay(t, 0.04f) +
                              wires * decay(t, 0.07f)) *
                             attack(t, 0.0005f);
                    });
    case Sound::DrumSliderTick:
      return shaped(filtered(metal(0.06f, 4), Filter::Highpass, 8000),
                    [](f32 t, f32 hat) { return hat * decay(t, 0.012f); });
    default:
      return {};
  }
}

Samples synthesize(Sound sound) {
  const f32 rate = kSampleRate;
  u32       seed = 0x9E3779B9u + static_cast<u32>(sound) * 7919u;
  auto      next_noise = [&seed] {
    seed = seed * 1664525u + 1013904223u;
    return static_cast<f32>(seed >> 8) / 8388608.0f - 1;
  };

  const i32 index = static_cast<i32>(sound);
  Samples   pcm;
  f32       phase = 0, lowpass = 0, previous = 0;
  switch (sound) {
    case Sound::ComboBreak:
      pcm = render(0.35f, [&](f32 t) {
        phase += kTau * 320 * std::exp(-t * 5) / rate;
        return std::sin(phase) * std::exp(-t * 7) * std::min(1.0f, t * 300);
      });
      break;
    case Sound::MenuHover:
      pcm = render(0.035f, [&](f32 t) {
        return 0.5f * std::sin(kTau * 2200 * t) * std::exp(-t * 140);
      });
      break;
    case Sound::MenuClick:
      pcm = render(0.12f, [&](f32 t) {
        phase += kTau * (700 + 500 * std::exp(-t * 40)) / rate;
        return std::sin(phase) * std::exp(-t * 35);
      });
      break;
    case Sound::SpinnerBonus:
      pcm = render(0.7f, [&](f32 t) {
        const f32 envelope = std::min(1.0f, t * 300) * std::exp(-t * 5);
        return envelope *
               (std::sin(kTau * 1568 * t) + 0.5f * std::sin(kTau * 2352 * t) +
                0.25f * std::sin(kTau * 3136 * t));
      });
      break;
    case Sound::NightcoreKick:
      // A sine dropping from a punchy click to a low boom.
      pcm = render(0.35f, [&](f32 t) {
        phase += kTau * (48 + 140 * std::exp(-t * 30)) / rate;
        return std::sin(phase) * std::exp(-t * 9) * std::min(1.0f, t * 2000);
      });
      break;
    case Sound::NightcoreClap:
      pcm = render(0.25f, [&](f32 t) {
        const f32 n = next_noise();
        lowpass += 0.4f * (n - lowpass);
        f32 envelope = 0.4f * std::exp(-t * 18);
        for (f32 start : {0.0f, 0.009f, 0.018f})
          if (t >= start)
            envelope = std::max(envelope, std::exp(-(t - start) * 260));
        return (n - lowpass) * envelope;
      });
      break;
    case Sound::NightcoreHat:
      pcm = render(0.06f, [&](f32 t) {
        const f32 n = next_noise();
        const f32 high = n - previous;
        previous = n;
        return high * std::exp(-t * 80);
      });
      break;
    case Sound::NightcoreFinish:
      pcm = render(1.4f, [&](f32 t) {
        const f32 n = next_noise();
        const f32 high = n - previous;
        previous = n;
        return high * std::exp(-t * 3) * std::min(1.0f, t * 1000);
      });
      break;
    default:
      pcm = hitsound(sound);
      break;
  }

  // Additions, ticks and the drum track sit under the main hit sound.
  const auto kind = index < kHitsoundEffects
                        ? static_cast<HitsoundKind>(index % kHitsoundKinds)
                        : HitsoundKind::Count;
  const f32  loudness = kind == HitsoundKind::Whistle      ? 0.56f
                        : kind == HitsoundKind::Clap       ? 0.64f
                        : kind == HitsoundKind::SliderTick ? 0.48f
                        : sound == Sound::NightcoreHat     ? 0.3f
                        : sound == Sound::NightcoreFinish  ? 0.4f
                        : sound >= Sound::NightcoreKick    ? 0.6f
                                                           : 0.8f;
  // The last 15 ms fade out, so no sound stops abruptly.
  const size_t fade = std::min(pcm.size(), static_cast<size_t>(0.015f * rate));
  for (size_t i = 0; i < fade; ++i)
    pcm[pcm.size() - fade + i] *= 1 - static_cast<f32>(i) / fade;
  f32 peak = 1e-6f;
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
  device_config.notificationCallback = notification_callback;
  device_config.pUserData = &audio;
  // Small buffers keep the delay between a hit and its sound short: 64
  // frames is 1.3 ms.
  device_config.performanceProfile = ma_performance_profile_low_latency;
  device_config.periodSizeInFrames = 64;
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

void sound_waits(Audio& audio, std::vector<f64>& waits) {
  const u32 count = audio.cycle_count.load(std::memory_order_acquire);
  std::erase_if(audio.sound_starts, [&](f64 start) {
    // The first cycle to begin after it, among those still recorded.
    f64 first = -1;
    for (u32 i = count > Audio::kCycles ? count - Audio::kCycles : 0; i < count;
         ++i) {
      const f64 begun = audio.cycle_starts[i % Audio::kCycles].load(
          std::memory_order_relaxed);
      if (begun >= start && (first < 0 || begun < first))
        first = begun;
    }
    if (first >= 0)
      waits.push_back((first - start) * 1000);
    // Too old to still be waiting: the cycle that mixed it was overwritten.
    return first >= 0 ||
           clock_gettime_nsec_np(CLOCK_UPTIME_RAW) * 1e-9 - start > 0.05;
  });
}

void update_output_latency(Audio& audio) {
  if (audio.ready && audio.latency_stale.exchange(false))
    audio.output_latency = query_output_latency();
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
              pcm ? std::move(*pcm) : synthesize(sound));
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
  audio.sound_starts.push_back(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) * 1e-9);
}

void audio_spectrum(Audio& audio, f32* bands, i32 band_count) {
  // 23 Hz per bin, fine enough that the lowest bands differ.
  constexpr i32     kSize = 2048;
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
    // A sine of amplitude A peaks at A * kSize / 4 through the Hann window.
    const f32 dbfs = 20 * std::log10(peak * 4 / kSize + 1e-9f);
    const f32 tilt = 3 * std::log2(std::sqrt(f0 * f1) / 1000);
    bands[b] = std::clamp((dbfs + tilt + 45) / 30, 0.0f, 1.0f);
  }
}

}  // namespace circles
