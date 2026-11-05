#include <circles/tablet.h>

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
      pen.x = little_endian(report, 2) * model.width / model.max_x;
      pen.y = little_endian(report, 4) * model.height / model.max_y;
      pen.pressure = little_endian(report, 6) / model.max_pressure * 100;
      return true;
  }
  return false;
}

TabletMapping tablet_mapping(const TabletArea& a) {
  // The pen's offset from the area's centre, turned back by the area's
  // rotation, as a fraction of its size, from its top-left.
  const f64     radians = a.rotation * std::numbers::pi / 180;
  const f32     c = static_cast<f32>(std::cos(radians));
  const f32     s = static_cast<f32>(std::sin(radians));
  TabletMapping m;
  m.m11 = c / a.width, m.m21 = s / a.width;
  m.m31 = -(a.x * c + a.y * s) / a.width + 0.5f;
  m.m12 = -s / a.height, m.m22 = c / a.height;
  m.m32 = (a.x * s - a.y * c) / a.height + 0.5f;
  return m;
}

Vec2 map_pen(const TabletMapping& m, f32 x, f32 y) {
  return {clamp01(x * m.m11 + y * m.m21 + m.m31),
          clamp01(x * m.m12 + y * m.m22 + m.m32)};
}

}  // namespace circles
