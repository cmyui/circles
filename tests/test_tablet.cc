// The tablet read directly: its reports decoded, and the pen's place across
// the area, turned as OpenTabletDriver turns it.
#include <circles/tablet.h>
#include <circles/types.h>
#include <tests/support/test.h>

#include <array>

using circles::f32;
using circles::PenReport;
using circles::TabletArea;
using circles::u8;
using circles::Vec2;

namespace {

// A tablet of the Wacom Intuos format, 0.01 mm a unit and 2047 the most
// pressure.
constexpr circles::TabletModel kIntuos = {
    .name = "test",
    .driver_name = "test",
    .report_size = 10,
    .format = circles::ReportFormat::WacomIntuos,
    .width = 152,
    .height = 95,
    .max_x = 15200,
    .max_y = 9500,
    .max_pressure = 2047};

// One of the Wacom Intuos Pro format, 0.005 mm a unit.
constexpr circles::TabletModel kIntuosPro = {
    .name = "test",
    .driver_name = "test",
    .report_size = 10,
    .format = circles::ReportFormat::WacomIntuosPro,
    .width = 157.48f,
    .height = 98.425f,
    .max_x = 31496,
    .max_y = 19685,
    .max_pressure = 2047};

Vec2 across(const TabletArea& area, f32 x, f32 y) {
  return circles::map_pen(circles::tablet_mapping(area), x, y);
}

}  // namespace

static void test_decodes_pen_reports() {
  // In range, x 0x1234 and y 0x0567 hundredths of a mm, all the pressure.
  const std::array<u8, 10> in_range = {0x02, 0xe0, 0x34, 0x12, 0x67,
                                       0x05, 0xff, 0x07, 0x00, 0x00};
  PenReport                pen;
  CHECK(circles::decode_pen_report(kIntuos, in_range, 1.5, pen));
  CHECK(pen.in_range);
  CHECK_NEAR(pen.x, 46.6f, 1e-3f);
  CHECK_NEAR(pen.y, 13.83f, 1e-3f);
  CHECK_NEAR(pen.pressure, 100.0f, 1e-4f);
  CHECK_EQ(pen.arrived, 1.5);

  const std::array<u8, 10> out_of_range = {0x02, 0x80};
  CHECK(circles::decode_pen_report(kIntuos, out_of_range, 2, pen));
  CHECK(!pen.in_range);

  // Other reports, and the pen's that aren't about its position.
  const std::array<u8, 10> other_id = {0x03, 0xe0};
  const std::array<u8, 10> no_position = {0x02, 0x20};
  CHECK(!circles::decode_pen_report(kIntuos, other_id, 0, pen));
  CHECK(!circles::decode_pen_report(kIntuos, no_position, 0, pen));
}

static void test_decodes_intuos_pro_pen_reports() {
  // In range: x 0x1234 and y 0x0567 with byte 9's bits below, 9321 and
  // 2766 units; pressure 0x80 over 0b01 over 0, 1026.
  const std::array<u8, 10> in_range = {0x02, 0xe0, 0x12, 0x34, 0x05,
                                       0x67, 0x80, 0x40, 0x00, 0x02};
  PenReport                pen;
  CHECK(circles::decode_pen_report(kIntuosPro, in_range, 1.5, pen));
  CHECK(pen.in_range);
  CHECK_NEAR(pen.x, 46.605f, 1e-3f);
  CHECK_NEAR(pen.y, 13.83f, 1e-3f);
  CHECK_NEAR(pen.pressure, 1026.0f / 2047 * 100, 1e-3f);
  CHECK_EQ(pen.arrived, 1.5);

  // The far corner, all the pressure, byte 1's lowest bit included; as
  // report 16 too.
  std::array<u8, 10> corner = {0x02, 0xe1, 0x3d, 0x84, 0x26,
                               0x72, 0xff, 0xc0, 0x00, 0x01};
  for (u8 id : {0x02, 0x10}) {
    corner[0] = id;
    CHECK(circles::decode_pen_report(kIntuosPro, corner, 0, pen));
    CHECK_NEAR(pen.x, 157.48f, 1e-3f);
    CHECK_NEAR(pen.y, 98.425f, 1e-3f);
    CHECK_NEAR(pen.pressure, 100.0f, 1e-4f);
  }

  const std::array<u8, 10> out_of_range = {0x02, 0x80};
  CHECK(circles::decode_pen_report(kIntuosPro, out_of_range, 2, pen));
  CHECK(!pen.in_range);

  // Other reports, and the pen's that aren't about its position: its
  // rotation, a tool coming near, and report 16's 0x20.
  for (const std::array<u8, 10>& other :
       {std::array<u8, 10>{0x03, 0xe0}, std::array<u8, 10>{0x02, 0x2a},
        std::array<u8, 10>{0x02, 0xc2}, std::array<u8, 10>{0x10, 0x20}})
    CHECK(!circles::decode_pen_report(kIntuosPro, other, 0, pen));
}

static void test_spans_the_area() {
  // 80 x 45 mm around (100, 50).
  const TabletArea area{100, 50, 80, 45, 0};
  const Vec2       centre = across(area, 100, 50);
  CHECK_NEAR(centre.x, 0.5f, 1e-5f);
  CHECK_NEAR(centre.y, 0.5f, 1e-5f);
  const Vec2 corner = across(area, 60, 27.5f);
  CHECK_NEAR(corner.x, 0.0f, 1e-5f);
  CHECK_NEAR(corner.y, 0.0f, 1e-5f);
  // Off the area, held at its edge.
  const Vec2 off = across(area, 150, 0);
  CHECK_EQ(off.x, 1.0f);
  CHECK_EQ(off.y, 0.0f);
}

static void test_rotation_turns_the_area_clockwise() {
  // A half turn: the tablet's top-left of the area is the screen's
  // bottom-right.
  const TabletArea half{100, 50, 80, 45, 180};
  const Vec2       corner = across(half, 60, 27.5f);
  CHECK_NEAR(corner.x, 1.0f, 1e-5f);
  CHECK_NEAR(corner.y, 1.0f, 1e-5f);
  // A quarter turn clockwise puts the area's left side at the tablet's top:
  // a point above the centre is left of it on screen.
  const TabletArea quarter{100, 50, 80, 45, 90};
  const Vec2       above = across(quarter, 100, 50 - 20);
  CHECK_NEAR(above.x, 0.5f - 20.0f / 80, 1e-5f);
  CHECK_NEAR(above.y, 0.5f, 1e-5f);
}

static void test_fits_the_area_on_the_surface() {
  // Already on it: unchanged.
  const TabletArea whole{76, 47.5f, 152, 95, 0};
  CHECK(circles::fit_area(whole, kIntuos) == whole);
  // Hanging off the left: moved in.
  const TabletArea left = circles::fit_area({10, 47.5f, 80, 45, 0}, kIntuos);
  CHECK_NEAR(left.x, 40.0f, 1e-4f);
  CHECK_EQ(left.width, 80.0f);
  // Turned a quarter, it stands 80 mm tall: moved down from the top.
  const TabletArea turned =
      circles::fit_area({76, 10, 80, 45, 90}, kIntuos);
  CHECK_NEAR(turned.y, 40.0f, 1e-3f);
  // Too big: shrunk to fit, keeping its shape, then moved within it.
  const TabletArea big = circles::fit_area({0, 0, 304, 95, 0}, kIntuos);
  CHECK_NEAR(big.width, 152.0f, 1e-3f);
  CHECK_NEAR(big.height, 47.5f, 1e-3f);
  CHECK_NEAR(big.x, 76.0f, 1e-3f);
  CHECK_NEAR(big.y, 23.75f, 1e-3f);
}

static void test_finds_tablets_by_their_ids_and_report_size() {
  for (const circles::TabletModel& model : circles::kTablets) {
    CHECK(circles::find_tablet(model.vendor, model.product,
                               model.report_size) == &model);
    CHECK(!circles::find_tablet(model.vendor, model.product + 1,
                                model.report_size));
    CHECK(!circles::find_tablet(model.vendor, model.product,
                                model.report_size + 1));
  }
}

int main() {
  test_finds_tablets_by_their_ids_and_report_size();
  test_decodes_pen_reports();
  test_decodes_intuos_pro_pen_reports();
  test_spans_the_area();
  test_fits_the_area_on_the_surface();
  test_rotation_turns_the_area_clockwise();
  return test_result();
}
