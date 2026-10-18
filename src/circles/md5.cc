#include <circles/md5.h>

#include <cmath>
#include <cstdio>
#include <vector>

namespace circles {

namespace {

// Per-round left rotations, from RFC 1321.
constexpr u32 kShifts[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
};

u32 rotate_left(u32 x, u32 bits) {
  return x << bits | x >> (32 - bits);
}

}  // namespace

std::string md5_hex(std::span<const u8> data) {
  // The round constants are the integer parts of |sin(i + 1)| * 2^32.
  static const auto kSines = [] {
    std::vector<u32> sines(64);
    for (u32 i = 0; i < 64; ++i)
      sines[i] = static_cast<u32>(
          std::floor(std::abs(std::sin(i + 1.0)) * 4294967296.0));
    return sines;
  }();

  // Pad to a multiple of 64 bytes: a 1 bit, zeros, then the bit length.
  std::vector<u8> message(data.begin(), data.end());
  message.push_back(0x80);
  while (message.size() % 64 != 56)
    message.push_back(0);
  const u64 bits = static_cast<u64>(data.size()) * 8;
  for (i32 i = 0; i < 8; ++i)
    message.push_back(static_cast<u8>(bits >> (8 * i)));

  u32 state[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
  for (size_t chunk = 0; chunk < message.size(); chunk += 64) {
    u32 words[16];
    for (i32 i = 0; i < 16; ++i) {
      const u8* p = &message[chunk + 4 * i];
      words[i] = p[0] | p[1] << 8 | p[2] << 16 | static_cast<u32>(p[3]) << 24;
    }
    u32 a = state[0], b = state[1], c = state[2], d = state[3];
    for (u32 i = 0; i < 64; ++i) {
      u32 f, g;
      if (i < 16) {
        f = (b & c) | (~b & d);
        g = i;
      } else if (i < 32) {
        f = (d & b) | (~d & c);
        g = (5 * i + 1) % 16;
      } else if (i < 48) {
        f = b ^ c ^ d;
        g = (3 * i + 5) % 16;
      } else {
        f = c ^ (b | ~d);
        g = (7 * i) % 16;
      }
      f += a + kSines[i] + words[g];
      a = d;
      d = c;
      c = b;
      b += rotate_left(f, kShifts[i]);
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
  }

  std::string hex;
  for (u32 word : state) {
    for (i32 i = 0; i < 4; ++i) {
      char byte[3];
      snprintf(byte, sizeof(byte), "%02x", (word >> (8 * i)) & 0xFF);
      hex += byte;
    }
  }
  return hex;
}

}  // namespace circles
