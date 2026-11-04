// The tablet read directly: its reports decoded, and mapped to the screen as
// OpenTabletDriver's absolute mode maps them.
#include <circles/tablet.h>
#include <circles/types.h>
#include <tests/support/test.h>

#include <array>

using circles::f32;
using circles::PenReport;
using circles::TabletSettings;
using circles::u8;
using circles::Vec2;

namespace {

// A 152 x 95 mm area on the 152 x 95 mm digitizer, centred, onto a
// 1000 x 500 point display at the virtual screen's origin.
TabletSettings whole_tablet() {
  TabletSettings s;
  s.area_x = 76, s.area_y = 47.5f, s.area_width = 152, s.area_height = 95;
  s.display_x = 500, s.display_y = 250;
  s.display_width = 1000, s.display_height = 500;
  return s;
}

Vec2 map(const TabletSettings& settings, f32 x, f32 y) {
  Vec2 point{};
  CHECK(circles::map_pen(circles::tablet_mapping(settings), x, y, point));
  return point;
}

}  // namespace

static void test_decodes_pen_reports() {
  // In range, x 0x1234, y 0x0567, pressure 2047 (all of it).
  const std::array<u8, 10> in_range = {0x02, 0xe0, 0x34, 0x12, 0x67,
                                       0x05, 0xff, 0x07, 0x00, 0x00};
  PenReport                pen;
  CHECK(circles::decode_pen_report(in_range, 1.5, pen));
  CHECK(pen.in_range);
  CHECK_EQ(pen.x, 0x1234);
  CHECK_EQ(pen.y, 0x0567);
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

static void test_maps_the_area_onto_the_display() {
  const TabletSettings s = whole_tablet();
  const Vec2           centre = map(s, 7600, 4750);
  CHECK_NEAR(centre.x, 500.0f, 1e-3f);
  CHECK_NEAR(centre.y, 250.0f, 1e-3f);
  const Vec2 corner = map(s, 15200, 9500);
  CHECK_NEAR(corner.x, 1000.0f, 1e-3f);
  CHECK_NEAR(corner.y, 500.0f, 1e-3f);
}

static void test_rotation_turns_the_area_about_its_centre() {
  TabletSettings s = whole_tablet();
  s.rotation = 180;
  // A half turn sends the tablet's top-left corner to the display's
  // bottom-right, exactly: .NET makes quarter turns exact.
  const Vec2 corner = map(s, 0, 0);
  CHECK_EQ(corner.x, 1000.0f);
  CHECK_EQ(corner.y, 500.0f);
  // A quarter turn clockwise puts the area's left side at the tablet's top:
  // the top edge's middle maps left of the display's centre.
  s.rotation = 90;
  const Vec2 quarter = map(s, 7600, 0);
  CHECK_NEAR(quarter.x, 500.0f - 47.5f * 1000 / 152, 1e-2f);
  CHECK_NEAR(quarter.y, 250.0f, 1e-2f);
}

static void test_clipping_and_limiting() {
  TabletSettings s = whole_tablet();
  s.area_width = 76, s.area_height = 47.5f;  // half the tablet, centred
  const Vec2 clipped = map(s, 0, 0);
  CHECK_EQ(clipped.x, 0.0f);
  CHECK_EQ(clipped.y, 0.0f);

  s.clipping = false;
  CHECK(map(s, 0, 0).x < 0);

  s.limiting = true;
  Vec2 point{};
  CHECK(!circles::map_pen(circles::tablet_mapping(s), 0, 0, point));
  CHECK(circles::map_pen(circles::tablet_mapping(s), 7600, 4750, point));
}

int main() {
  test_decodes_pen_reports();
  test_maps_the_area_onto_the_display();
  test_rotation_turns_the_area_about_its_centre();
  test_clipping_and_limiting();
  return test_result();
}
