#pragma once

// Changes audio's speed without changing its pitch, by WSOLA (waveform-
// similarity overlap-add), the method osu!stable's audio library uses: the
// output is overlapping windows of the input, each taken from about where the
// tempo puts it, then nudged to line up with the previous window's waveform.

#include <circles/types.h>

#include <cstddef>
#include <functional>
#include <vector>

namespace circles {

class TimeStretcher {
 public:
  // Reads up to `frames` interleaved frames into `out` and returns how many
  // it read, setting `ended` once the input has run out. Reading none without
  // ending means the input isn't available yet.
  using Read = std::function<u64(f32* out, u64 frames, bool& ended)>;

  TimeStretcher(u32 channels, u32 sample_rate);

  // Input frames per output frame: 1.5 plays half again as fast.
  void set_tempo(f64 tempo) { tempo_ = tempo; }
  f64 tempo() const { return tempo_; }
  // Forgets buffered audio; the next input read is frame `frame` of the
  // source.
  void reset(u64 frame);
  // Writes up to `frames` interleaved frames, reading input as needed.
  // Returns fewer than `frames` only at the end of the input or while it's
  // unavailable.
  u64 process(f32* out, u64 frames, const Read& read);
  // The input frame the next output frame corresponds to.
  f64 input_position() const { return start_ + delivered_ * tempo_; }
  // Whether the input has ended and all of its output has been written.
  bool finished() const {
    return finished_ && ready_read_ * channels_ >= ready_.size();
  }

 private:
  bool produce_hop(const Read& read);
  // The input frame at absolute position `frame` summed over channels; zero
  // outside the buffer.
  f32 mono(i64 frame) const;
  i64 best_offset(i64 ideal) const;

  u32              channels_;
  i64              hop_;     // output frames per window step
  i64              window_;  // two hops, so windows overlap by half
  i64              search_;  // frames either side of the ideal start to try
  std::vector<f32> shape_;   // Hann window; overlapping halves sum to 1
  f64              tempo_ = 1;

  std::vector<f32> input_;  // buffered input from frame input_start_
  i64              input_start_ = 0;
  bool             input_ended_ = false;
  std::vector<f32> overlap_;         // a window's worth of output being summed
  std::vector<f32> ready_;           // finished output not yet returned
  size_t           ready_read_ = 0;  // frames of ready_ already returned
  u64              start_ = 0;       // the input frame output began from
  u64              hops_ = 0;
  u64              delivered_ = 0;  // output frames returned since start_
  i64              previous_ = -1;  // where the last window was taken from
  bool             finished_ = false;
};

}  // namespace circles
