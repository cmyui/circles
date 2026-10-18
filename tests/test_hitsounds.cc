// Which sample set each hitsound plays from.
#include <circles/hitsounds.h>
#include <tests/support/test.h>

namespace {

bool same(const circles::Hitsound& h,
          circles::u32             additions,
          circles::SampleSet       normal,
          circles::SampleSet       addition) {
  return h.additions == additions && h.normal_set == normal &&
         h.addition_set == addition;
}

}  // namespace

static void test_objects_override_the_timing_point_sample_set() {
  using circles::SampleSet;
  CHECK(same(circles::resolve_hitsound(2, "2:3:0:0:", SampleSet::Normal), 2,
             SampleSet::Soft, SampleSet::Drum));
  CHECK(same(circles::resolve_hitsound(0, "0:0:0:0:", SampleSet::Drum), 0,
             SampleSet::Drum, SampleSet::Drum));
  // Additions without their own set use the normal sound's.
  CHECK(same(circles::resolve_hitsound(8, "1:0", SampleSet::Soft), 8,
             SampleSet::Normal, SampleSet::Normal));
  CHECK(same(circles::resolve_hitsound(0, "", SampleSet::Soft), 0,
             SampleSet::Soft, SampleSet::Soft));
  CHECK(same(circles::resolve_hitsound(0, "x:9", SampleSet::Soft), 0,
             SampleSet::Soft, SampleSet::Soft));
}

static void test_sample_volume_override() {
  CHECK_EQ(circles::hit_sample_volume("0:0:0:70:"), 70);
  CHECK_EQ(circles::hit_sample_volume("1:2"), 0);
  CHECK_EQ(circles::hit_sample_volume(""), 0);
}

static void test_slider_edges_each_choose_their_sounds() {
  using circles::SampleSet;
  const circles::Hitsound object{2, SampleSet::Soft, SampleSet::Soft};
  const auto              edges =
      circles::edge_hitsounds("2|0|8", "3:0|0:0", 3, object, SampleSet::Normal);
  CHECK_EQ(edges.size(), 3u);
  CHECK(same(edges[0], 2, SampleSet::Drum, SampleSet::Drum));
  // "0:0" inherits from the timing point, not the object.
  CHECK(same(edges[1], 0, SampleSet::Normal, SampleSet::Normal));
  CHECK(same(edges[2], 8, SampleSet::Soft, SampleSet::Soft));
  for (const auto& edge :
       circles::edge_hitsounds("", "", 2, object, SampleSet::Normal))
    CHECK(same(edge, 2, SampleSet::Soft, SampleSet::Soft));
}

int main() {
  test_objects_override_the_timing_point_sample_set();
  test_sample_volume_override();
  test_slider_edges_each_choose_their_sounds();
  return test_result();
}
