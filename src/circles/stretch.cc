#include <circles/stretch.h>

#include <algorithm>
#include <cmath>

namespace circles {

namespace {

constexpr f64 kHopSeconds = 0.016;
// How far a window may move to line up with the last; past a period of the
// lowest pitches that matter.
constexpr f64 kSearchSeconds = 0.008;
// The search tries every few offsets, then each one around the best.
constexpr i64 kCoarseStep = 4;

}  // namespace

TimeStretcher::TimeStretcher(u32 channels, u32 sample_rate)
    : channels_(channels),
      hop_(std::lround(sample_rate * kHopSeconds)),
      window_(2 * hop_),
      search_(std::lround(sample_rate * kSearchSeconds)),
      shape_(window_) {
  for (i64 i = 0; i < window_; ++i)
    shape_[i] = 0.5f - 0.5f * std::cos(6.2831853f * i / window_);
  reset(0);
}

void TimeStretcher::reset(u64 frame) {
  input_.clear();
  input_start_ = static_cast<i64>(frame);
  input_ended_ = false;
  overlap_.assign(window_ * channels_, 0);
  ready_.clear();
  ready_read_ = 0;
  start_ = frame;
  hops_ = 0;
  delivered_ = 0;
  previous_ = -1;
  finished_ = false;
}

f32 TimeStretcher::mono(i64 frame) const {
  const i64 index = frame - input_start_;
  if (index < 0 || index >= static_cast<i64>(input_.size() / channels_))
    return 0;
  f32 sum = 0;
  for (u32 c = 0; c < channels_; ++c)
    sum += input_[index * channels_ + c];
  return sum;
}

// The start near `ideal` whose first half best matches what follows the
// previous window, so the two blend without cancelling out.
i64 TimeStretcher::best_offset(i64 ideal) const {
  std::vector<f32> target(hop_);
  for (i64 i = 0; i < hop_; ++i)
    target[i] = mono(previous_ + hop_ + i);
  std::vector<f32> candidates(2 * search_ + hop_);
  for (i64 i = 0; i < static_cast<i64>(candidates.size()); ++i)
    candidates[i] = mono(ideal - search_ + i);

  auto similarity = [&](i64 offset) {
    const f32* a = candidates.data() + offset + search_;
    f32        dot = 0, energy = 1e-9f;
    for (i64 i = 0; i < hop_; ++i) {
      dot += a[i] * target[i];
      energy += a[i] * a[i];
    }
    return dot / std::sqrt(energy);
  };
  i64 best = 0;
  f32 best_score = similarity(0);
  for (i64 offset = -search_; offset <= search_; offset += kCoarseStep) {
    const f32 score = similarity(offset);
    if (score > best_score) {
      best_score = score;
      best = offset;
    }
  }
  const i64 coarse = best;
  for (i64 offset = std::max(-search_, coarse - kCoarseStep + 1);
       offset <= std::min(search_, coarse + kCoarseStep - 1); ++offset) {
    const f32 score = similarity(offset);
    if (score > best_score) {
      best_score = score;
      best = offset;
    }
  }
  return ideal + best;
}

bool TimeStretcher::produce_hop(const Read& read) {
  if (finished_)
    return false;
  const i64 ideal =
      static_cast<i64>(start_) + std::llround(hops_ * hop_ * tempo_);
  const i64 needed = ideal + search_ + window_;
  while (!input_ended_ &&
         input_start_ + static_cast<i64>(input_.size() / channels_) < needed) {
    const i64 want =
        needed - input_start_ - static_cast<i64>(input_.size() / channels_);
    const size_t old = input_.size();
    input_.resize(old + want * channels_);
    bool      ended = false;
    const u64 got = read(input_.data() + old, want, ended);
    input_.resize(old + got * channels_);
    input_ended_ = ended;
    if (got == 0 && !ended)
      return false;  // the input isn't ready yet
  }

  const i64 input_end =
      input_start_ + static_cast<i64>(input_.size() / channels_);
  if (input_ended_ && ideal >= input_end) {
    // The last window's second half is all that's left.
    ready_.insert(ready_.end(), overlap_.begin(),
                  overlap_.begin() + hop_ * channels_);
    finished_ = true;
    return true;
  }

  const i64 source = previous_ < 0 ? ideal : best_offset(ideal);
  for (i64 i = 0; i < window_; ++i) {
    const i64 index = source + i - input_start_;
    if (index < 0 || index >= input_end - input_start_)
      continue;
    for (u32 c = 0; c < channels_; ++c)
      overlap_[i * channels_ + c] += input_[index * channels_ + c] * shape_[i];
  }
  // The first hop now has both windows over it, so it's finished.
  ready_.insert(ready_.end(), overlap_.begin(),
                overlap_.begin() + hop_ * channels_);
  std::copy(overlap_.begin() + hop_ * channels_, overlap_.end(),
            overlap_.begin());
  std::fill(overlap_.begin() + hop_ * channels_, overlap_.end(), 0.0f);
  previous_ = source;
  ++hops_;

  // Drop input no later window or comparison can reach.
  const i64 next_ideal =
      static_cast<i64>(start_) + std::llround(hops_ * hop_ * tempo_);
  const i64 drop =
      std::min(std::min(next_ideal - search_, previous_ + hop_) - input_start_,
               input_end - input_start_);
  if (drop > 0) {
    input_.erase(input_.begin(), input_.begin() + drop * channels_);
    input_start_ += drop;
  }
  return true;
}

u64 TimeStretcher::process(f32* out, u64 frames, const Read& read) {
  u64 written = 0;
  while (written < frames) {
    const u64 available = ready_.size() / channels_ - ready_read_;
    if (available == 0) {
      ready_.clear();
      ready_read_ = 0;
      if (!produce_hop(read))
        break;
      continue;
    }
    const u64 count = std::min(available, frames - written);
    std::copy_n(ready_.data() + ready_read_ * channels_, count * channels_,
                out + written * channels_);
    ready_read_ += count;
    written += count;
  }
  delivered_ += written;
  return written;
}

}  // namespace circles
