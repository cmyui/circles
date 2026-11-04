#pragma once

// A pen tablet read by circles itself rather than through its driver's
// cursor. So far one tablet: the One by Wacom S (CTL-472). Its reports are
// decoded as OpenTabletDriver decodes them and mapped to the screen with the
// area set in OpenTabletDriver, by its absolute mode's own arithmetic, so
// the cursor lands where OpenTabletDriver's would.

#include <circles/math.h>
#include <circles/types.h>

#include <span>
#include <vector>

namespace circles {

// One report from the pen.
struct PenReport {
  f64  arrived = 0;  // uptime seconds, as macOS stamped its arrival
  bool in_range = false;
  f32  x = 0, y = 0;  // digitizer units, 0.01 mm
  f32  pressure = 0;  // percent of the most the pen reads
};
// Decodes a report as OpenTabletDriver's Intuos parser does; false if it
// says nothing about the pen.
bool decode_pen_report(std::span<const u8> report, f64 arrived, PenReport& pen);

// OpenTabletDriver's absolute mode settings for the tablet.
struct TabletSettings {
  // The tablet area: its centre and size in mm, and rotation in degrees.
  f32  area_x = 0, area_y = 0, area_width = 1, area_height = 1, rotation = 0;
  // The display area it maps to: centre and size on OpenTabletDriver's
  // virtual screen, in points.
  f32  display_x = 0, display_y = 0, display_width = 1, display_height = 1;
  bool clipping = true;    // keeps the cursor in the display area
  bool limiting = false;   // ignores the pen outside the tablet area
  f32  tip_threshold = 1;  // pressure, in percent, past which the tip clicks
};

// System.Numerics' Matrix3x2, which OpenTabletDriver maps with: points are
// row vectors.
struct Matrix3x2 {
  f32 m11 = 1, m12 = 0, m21 = 0, m22 = 1, m31 = 0, m32 = 0;
};

struct TabletMapping {
  Matrix3x2 transform;
  Vec2      min{}, max{};  // the display area's corners
  bool      clipping = true, limiting = false;
};
TabletMapping tablet_mapping(const TabletSettings& settings);
// Where the pen points on the virtual screen, in points; false when it's
// outside the tablet area and limiting is on.
bool map_pen(const TabletMapping& mapping, f32 x, f32 y, Vec2& point);

// Platform glue (tablet_macos.mm).

// Reads OpenTabletDriver's absolute mode settings for the CTL-472; false if
// it has none.
bool load_tablet_settings(TabletSettings& settings);
// A virtual screen point minus this is in macOS's display coordinates.
Vec2 virtual_screen_offset();
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
