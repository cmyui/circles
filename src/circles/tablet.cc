#include <circles/tablet.h>

#include <cmath>
#include <numbers>

namespace circles {

namespace {

// The CTL-472's digitizer and pen, from OpenTabletDriver's configuration.
constexpr f32 kWidthMm = 152, kHeightMm = 95;
constexpr f32 kMaxX = 15200, kMaxY = 9500;
constexpr f32 kMaxPressure = 2047;

u16 little_endian(std::span<const u8> bytes, size_t at) {
  return static_cast<u16>(bytes[at] | bytes[at + 1] << 8);
}

// Matrix3x2's product and constructors, in single precision as in .NET.
Matrix3x2 operator*(const Matrix3x2& a, const Matrix3x2& b) {
  return {a.m11 * b.m11 + a.m12 * b.m21,
          a.m11 * b.m12 + a.m12 * b.m22,
          a.m21 * b.m11 + a.m22 * b.m21,
          a.m21 * b.m12 + a.m22 * b.m22,
          a.m31 * b.m11 + a.m32 * b.m21 + b.m31,
          a.m31 * b.m12 + a.m32 * b.m22 + b.m32};
}

Matrix3x2 scale(f32 x, f32 y) {
  return {x, 0, 0, y, 0, 0};
}

Matrix3x2 translation(f32 x, f32 y) {
  return {1, 0, 0, 1, x, y};
}

// Exact for whole quarter turns, as .NET's CreateRotation is.
Matrix3x2 rotation(f32 radians) {
  constexpr f32 kPi = std::numbers::pi_v<f32>;
  constexpr f32 kEpsilon = 0.001f * kPi / 180;
  radians = std::remainder(radians, 2 * kPi);
  f32 c, s;
  if (radians > -kEpsilon && radians < kEpsilon)
    c = 1, s = 0;
  else if (radians > kPi / 2 - kEpsilon && radians < kPi / 2 + kEpsilon)
    c = 0, s = 1;
  else if (radians < -kPi + kEpsilon || radians > kPi - kEpsilon)
    c = -1, s = 0;
  else if (radians > -kPi / 2 - kEpsilon && radians < -kPi / 2 + kEpsilon)
    c = 0, s = -1;
  else
    c = std::cos(radians), s = std::sin(radians);
  return {c, s, -s, c, 0, 0};
}

}  // namespace

bool decode_pen_report(std::span<const u8> report,
                       f64                 arrived,
                       PenReport&          pen) {
  if (report.size() < 8 || report[0] != 0x02)
    return false;
  pen.arrived = arrived;
  if (report[1] == 0x80) {
    pen.in_range = false;
    return true;
  }
  if (!(report[1] & 0x40))
    return false;
  pen.in_range = true;
  pen.x = little_endian(report, 2);
  pen.y = little_endian(report, 4);
  pen.pressure = little_endian(report, 6) / kMaxPressure * 100;
  return true;
}

TabletMapping tablet_mapping(const TabletSettings& s) {
  // OpenTabletDriver's AbsoluteOutputMode: digitizer units to mm, centred on
  // the area, rotated, scaled to points, then placed on the display.
  TabletMapping mapping;
  mapping.transform =
      scale(kWidthMm / kMaxX, kHeightMm / kMaxY) *
      translation(-s.area_x, -s.area_y) *
      rotation(static_cast<f32>(-s.rotation * std::numbers::pi / 180)) *
      scale(s.display_width / s.area_width, s.display_height / s.area_height) *
      translation(s.display_x, s.display_y);
  mapping.min = {s.display_x - s.display_width / 2,
                 s.display_y - s.display_height / 2};
  mapping.max = {s.display_x + s.display_width / 2,
                 s.display_y + s.display_height / 2};
  mapping.clipping = s.clipping;
  mapping.limiting = s.limiting;
  return mapping;
}

bool map_pen(const TabletMapping& m, f32 x, f32 y, Vec2& point) {
  const Matrix3x2& t = m.transform;
  point = {x * t.m11 + y * t.m21 + t.m31, x * t.m12 + y * t.m22 + t.m32};
  const Vec2 clipped = {std::clamp(point.x, m.min.x, m.max.x),
                        std::clamp(point.y, m.min.y, m.max.y)};
  if (m.limiting && !(clipped == point))
    return false;
  if (m.clipping)
    point = clipped;
  return true;
}

}  // namespace circles
