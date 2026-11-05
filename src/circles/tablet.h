#pragma once

// A pen tablet read by circles itself rather than through its driver's
// cursor. So far one tablet: the One by Wacom S (CTL-472), whose reports are
// decoded as OpenTabletDriver decodes them.

#include <circles/math.h>
#include <circles/types.h>

#include <span>
#include <vector>

namespace circles {

// The CTL-472's active surface, in mm.
inline constexpr f32 kTabletWidth = 152, kTabletHeight = 95;

// One report from the pen.
struct PenReport {
  f64  arrived = 0;  // uptime seconds, as macOS stamped its arrival
  bool in_range = false;
  f32  x = 0, y = 0;  // mm from the surface's top-left
  f32  pressure = 0;  // percent of the most the pen reads
};
// Decodes a report as OpenTabletDriver's Intuos parser does; false if it
// says nothing about the pen.
bool decode_pen_report(std::span<const u8> report, f64 arrived, PenReport& pen);

// The part of the tablet that maps onto the screen: its centre and size in
// mm, and how far it's turned clockwise, in degrees.
struct TabletArea {
  f32 x = kTabletWidth / 2, y = kTabletHeight / 2;
  f32 width = kTabletWidth, height = kTabletHeight;
  f32 rotation = 0;
};

// From mm on the tablet to a position across the area, 0 to 1 each way:
// an affine transform, row vectors as in System.Numerics' Matrix3x2.
struct TabletMapping {
  f32 m11 = 1, m12 = 0, m21 = 0, m22 = 1, m31 = 0, m32 = 0;
};
TabletMapping tablet_mapping(const TabletArea& area);
// Where the pen is across the area, clamped to it.
Vec2 map_pen(const TabletMapping& mapping, f32 x, f32 y);

// Platform glue (tablet_macos.mm).

// OpenTabletDriver's area and tip threshold (percent) for the CTL-472, to
// import; false if it has none. Its display area is taken to be the whole
// screen.
bool import_tablet_area(TabletArea& area, f32& tip_threshold);
// While on, keeps the tablet sending pen reports, switching it as
// OpenTabletDriver does; turned off, and at quit, it gets back the mode it
// had. Reports arrive through the pointer listener (listen_to_pointers).
void read_tablet(bool on);
// Pen reports since the last call, while reading the tablet.
void take_pen_reports(std::vector<PenReport>& reports);
// For the pointer listener: whether a device is the tablet's pen, which it
// then reads; and each of its reports.
bool tablet_device(void* device);
void tablet_report(std::span<const u8> report, f64 arrived);

}  // namespace circles
