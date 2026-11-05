#pragma once

// Pen tablets read by circles itself rather than through their driver's
// cursor. Tablets of a family share a report format, the layout of their
// reports and how to turn them on; each model is a row of kTablets, saying
// how to recognise it, its format and its surface.

#include <circles/math.h>
#include <circles/types.h>

#include <span>
#include <vector>

namespace circles {

enum class ReportFormat : u8 {
  // As OpenTabletDriver's Intuos parser reads it: report 2, the pen in range
  // when bit 6 of byte 1 is set and out of it when byte 1 is 0x80; position
  // at bytes 2 and 4 and pressure at 6, little-endian. Setting feature
  // report 2 to 2 turns it on.
  WacomIntuos,
};

// A feature report that turns a format's pen reports on: its ID and value.
struct FormatSetup {
  u8 report_id, value;
};
FormatSetup format_setup(ReportFormat format);

// A tablet circles can read, as the USB interface its pen reports on shows
// itself.
struct TabletModel {
  const char*  name;         // as shown
  const char*  driver_name;  // OpenTabletDriver's, for importing its area
  u16          vendor, product;
  u32          report_size;  // bytes in the interface's reports
  ReportFormat format;
  f32          width, height;  // the active surface, mm
  f32          max_x, max_y;   // the position's range across it
  f32          max_pressure;
};

// The first is assumed while none is connected.
inline constexpr TabletModel kTablets[] = {
    {.name = "One by Wacom S (CTL-472)",
     .driver_name = "Wacom CTL-472",
     .vendor = 0x056A,
     .product = 0x037A,
     .report_size = 10,
     .format = ReportFormat::WacomIntuos,
     .width = 152,
     .height = 95,
     .max_x = 15200,
     .max_y = 9500,
     .max_pressure = 2047},
};
// The model a device is, by its USB IDs and report size; null for none.
const TabletModel* find_tablet(u16 vendor, u16 product, u32 report_size);

// One report from the pen.
struct PenReport {
  f64  arrived = 0;  // uptime seconds, as macOS stamped its arrival
  bool in_range = false;
  f32  x = 0, y = 0;  // mm from the surface's top-left
  f32  pressure = 0;  // percent of the most the pen reads
};
// Decodes one of `model`'s reports; false if it says nothing about the pen.
bool decode_pen_report(const TabletModel&  model,
                       std::span<const u8> report,
                       f64                 arrived,
                       PenReport&          pen);

// The part of the tablet that maps onto the screen: its centre and size in
// mm, and how far it's turned clockwise, in degrees.
struct TabletArea {
  f32 x = kTablets[0].width / 2, y = kTablets[0].height / 2;
  f32 width = kTablets[0].width, height = kTablets[0].height;
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

// The tablet connected, as the pointer listener found it; null for none.
const TabletModel* connected_tablet();
// OpenTabletDriver's area and tip threshold (percent) for the connected
// tablet, or while none is, any it knows; false if it has none. Its display
// area is taken to be the whole screen.
bool import_tablet_area(TabletArea& area, f32& tip_threshold);
// While on, keeps the connected tablet sending pen reports, turning them on
// as its format says; turned off, and at quit, it gets back the mode it
// had. Reports arrive through the pointer listener (listen_to_pointers).
void read_tablet(bool on);
// Pen reports since the last call, while reading the tablet.
void take_pen_reports(std::vector<PenReport>& reports);
// For the pointer listener: whether a device just connected is a tablet's
// pen interface, which is then read; its reports; and devices leaving.
bool tablet_device(void* device);
void tablet_report(std::span<const u8> report, f64 arrived);
void tablet_removed(void* device);

}  // namespace circles
