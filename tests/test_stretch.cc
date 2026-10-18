// Speed changes that keep pitch.
#include <circles/stretch.h>
#include <tests/support/test.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

constexpr circles::u32 kRate = 48000;
constexpr double       kFrequency = 440;

// Two seconds of a stereo sine, stretched to `tempo`.
std::vector<float> stretch_sine(double tempo, double* input_position) {
  std::vector<float> input(2 * kRate * 2);
  for (size_t i = 0; i < input.size() / 2; ++i)
    input[2 * i] = input[2 * i + 1] =
        static_cast<float>(std::sin(6.283185307 * kFrequency * i / kRate));
  circles::TimeStretcher stretcher(2, kRate);
  stretcher.set_tempo(tempo);
  size_t read_at = 0;
  auto   read = [&](float* out, circles::u64 frames, bool& ended) {
    const size_t count = std::min<size_t>(frames, input.size() / 2 - read_at);
    std::copy_n(input.data() + read_at * 2, count * 2, out);
    read_at += count;
    ended = read_at == input.size() / 2;
    return static_cast<circles::u64>(count);
  };
  std::vector<float> output;
  float              block[2 * 512];
  while (!stretcher.finished()) {
    const auto count = stretcher.process(block, 512, read);
    output.insert(output.end(), block, block + count * 2);
  }
  *input_position = stretcher.input_position();
  return output;
}

// Cycles per second in the middle of the output, by upward zero crossings.
double frequency(const std::vector<float>& stereo) {
  const size_t frames = stereo.size() / 2;
  const size_t begin = frames / 4, end = frames * 3 / 4;
  int          crossings = 0;
  for (size_t i = begin + 1; i < end; ++i)
    crossings += stereo[2 * (i - 1)] < 0 && stereo[2 * i] >= 0;
  return crossings * static_cast<double>(kRate) / (end - begin);
}

}  // namespace

static void test_speeding_up_shortens_without_raising_pitch() {
  double     position = 0;
  const auto output = stretch_sine(1.5, &position);
  CHECK_NEAR(output.size() / 2.0 / kRate, 2 / 1.5, 0.05);
  CHECK_NEAR(frequency(output), kFrequency, 5);
  // The position maps output back to the input it came from.
  CHECK_NEAR(position / kRate, 2, 0.05);
}

static void test_slowing_down_lengthens_without_lowering_pitch() {
  double     position = 0;
  const auto output = stretch_sine(0.75, &position);
  CHECK_NEAR(output.size() / 2.0 / kRate, 2 / 0.75, 0.05);
  CHECK_NEAR(frequency(output), kFrequency, 5);
}

int main() {
  test_speeding_up_shortens_without_raising_pitch();
  test_slowing_down_lengthens_without_lowering_pitch();
  return test_result();
}
