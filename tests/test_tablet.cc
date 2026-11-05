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

Vec2 across(const TabletArea& area, f32 x, f32 y) {
  return circles::map_pen(circles::tablet_mapping(area), x, y);
}

}  // namespace

static void test_decodes_pen_reports() {
  // In range, x 0x1234 and y 0x0567 hundredths of a mm, all the pressure.
  const std::array<u8, 10> in_range = {0x02, 0xe0, 0x34, 0x12, 0x67,
                                       0x05, 0xff, 0x07, 0x00, 0x00};
  PenReport                pen;
  CHECK(circles::decode_pen_report(in_range, 1.5, pen));
  CHECK(pen.in_range);
  CHECK_NEAR(pen.x, 46.6f, 1e-3f);
  CHECK_NEAR(pen.y, 13.83f, 1e-3f);
  CHECK_NEAR(pen.pressure, 100.0f, 1e-4f);
  CHECK_EQ(pen.arrived, 1.5);

  const std::array<u8, 10> out_of_range = {0x02, 0x80};
  CHECK(circles::decode_pen_report(out_of_range, 2, pen));
  CHECK(!pen.in_range);

  // Other reports, and the pen's that aren't about its position.
  const std::array<u8, 10> other_id = {0x03, 0xe0};
  const std::array<u8, 10> no_position = {0x02, 0x20};
  CHECK(!circles::decode_pen_report(other_id, 0, pen));
  CHECK(!circles::decode_pen_report(no_position, 0, pen));
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

int main() {
  test_decodes_pen_reports();
  test_spans_the_area();
  test_rotation_turns_the_area_clockwise();
  return test_result();
}
