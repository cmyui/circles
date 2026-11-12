#include <circles/tablet.h>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace circles {

namespace {

u16 little_endian(std::span<const u8> bytes, size_t at) {
  return static_cast<u16>(bytes[at] | bytes[at + 1] << 8);
}

}  // namespace

FormatSetup format_setup(ReportFormat format) {
  switch (format) {
    case ReportFormat::WacomIntuos:
    case ReportFormat::WacomIntuosPro:
      return {2, 2};
  }
  return {};
}

const TabletModel* find_tablet(u16 vendor, u16 product, u32 report_size) {
  for (const TabletModel& model : kTablets)
    if (model.vendor == vendor && model.product == product &&
        model.report_size == report_size)
      return &model;
  return nullptr;
}

bool decode_pen_report(const TabletModel&  model,
                       std::span<const u8> report,
                       f64                 arrived,
                       PenReport&          pen) {
  switch (model.format) {
    case ReportFormat::WacomIntuos:
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
      pen.pressure = little_endian(report, 6) / model.max_pressure * 100;
      return true;
    case ReportFormat::WacomIntuosPro: {
      if (report.size() < 10 || (report[0] != 0x02 && report[0] != 0x10) ||
          (report[0] == 0x10 && report[1] == 0x20))
        return false;
      pen.arrived = arrived;
      if (report[1] == 0x80) {
        pen.in_range = false;
        return true;
      }
      const bool rotation = (report[1] & 0x02) && (report[1] & 0x08);
      if (rotation || !(report[1] & 0x20))
        return false;
      const i32 x = (report[2] << 8 | report[3]) << 1 | (report[9] >> 1 & 1);
      const i32 y = (report[4] << 8 | report[5]) << 1 | (report[9] & 1);
      const i32 pressure =
          report[6] << 3 | (report[7] & 0xC0) >> 5 | (report[1] & 1);
      pen.in_range = true;
      pen.x = static_cast<f32>(x);
      pen.y = static_cast<f32>(y);
      pen.pressure = pressure / model.max_pressure * 100;
      return true;
    }
  }
  return false;
}

namespace {

// System.Numerics' Matrix3x2 product and constructors.
TabletMapping operator*(const TabletMapping& a, const TabletMapping& b) {
  TabletMapping m;
  m.m11 = a.m11 * b.m11 + a.m12 * b.m21;
  m.m12 = a.m11 * b.m12 + a.m12 * b.m22;
  m.m21 = a.m21 * b.m11 + a.m22 * b.m21;
  m.m22 = a.m21 * b.m12 + a.m22 * b.m22;
  m.m31 = a.m31 * b.m11 + a.m32 * b.m21 + b.m31;
  m.m32 = a.m31 * b.m12 + a.m32 * b.m22 + b.m32;
  return m;
}

TabletMapping scaling(f32 x, f32 y) {
  return {.m11 = x, .m22 = y};
}

TabletMapping translation(f32 x, f32 y) {
  return {.m31 = x, .m32 = y};
}

// Quarter turns are exact, as Matrix3x2.CreateRotation makes them.
TabletMapping rotation(f32 radians) {
  const f32 kEpsilon = 0.001f * std::numbers::pi_v<f32> / 180;
  const f32 kPi = std::numbers::pi_v<f32>;
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
  return {.m11 = c, .m12 = s, .m21 = -s, .m22 = c};
}

}  // namespace

TabletMapping tablet_mapping(const TabletArea&  a,
                             const TabletModel& model,
                             Vec2               origin,
                             Vec2               size) {
  TabletMapping m =
      scaling(model.width / model.max_x, model.height / model.max_y) *
      translation(-a.x, -a.y) *
      rotation(static_cast<f32>(-a.rotation * std::numbers::pi / 180)) *
      scaling(size.x / a.width, size.y / a.height) *
      translation(origin.x + size.x / 2, origin.y + size.y / 2);
  m.low = origin;
  m.high = {origin.x + size.x, origin.y + size.y};
  return m;
}

TabletArea fit_area(TabletArea a, const TabletModel& model) {
  const f64 radians = a.rotation * std::numbers::pi / 180;
  const f32 c = static_cast<f32>(std::abs(std::cos(radians)));
  const f32 s = static_cast<f32>(std::abs(std::sin(radians)));
  // The turned area's bounds on the surface.
  f32       across = a.width * c + a.height * s;
  f32       down = a.width * s + a.height * c;
  const f32 shrink =
      std::min({1.0f, model.width / across, model.height / down});
  a.width *= shrink, a.height *= shrink, across *= shrink, down *= shrink;
  a.x = std::clamp(a.x, across / 2, model.width - across / 2);
  a.y = std::clamp(a.y, down / 2, model.height - down / 2);
  return a;
}

Vec2 map_pen(const TabletMapping& m, f32 x, f32 y) {
  return {std::clamp(x * m.m11 + y * m.m21 + m.m31, m.low.x, m.high.x),
          std::clamp(x * m.m12 + y * m.m22 + m.m32, m.low.y, m.high.y)};
}

}  // namespace circles
