#pragma once

#include <circles/types.h>

#include <algorithm>
#include <cmath>

namespace circles {

struct Vec2 {
  f32 x, y;
  Vec2 operator+(Vec2 b) const { return {x + b.x, y + b.y}; }
  Vec2 operator-(Vec2 b) const { return {x - b.x, y - b.y}; }
  Vec2 operator*(f32 scale) const { return {x * scale, y * scale}; }
  f32 length() const { return std::sqrt(x * x + y * y); }
  bool operator==(const Vec2&) const = default;
};

inline Vec2 lerp(Vec2 a, Vec2 b, f32 t) {
  return a + (b - a) * t;
}

inline f32 clamp01(f32 t) {
  return std::clamp(t, 0.0f, 1.0f);
}

// Decelerates from 0 to 1 as t goes from 0 to 1.
inline f32 ease_out(f32 t) {
  t = 1 - clamp01(t);
  return 1 - t * t * t;
}

// Exponential smoothing toward target: the gap shrinks by a factor of e every
// 1/rate seconds, whatever the frame rate.
inline f32 approach(f32 value, f32 target, f32 rate, f32 dt) {
  return target + (value - target) * std::exp(-rate * dt);
}

}  // namespace circles
