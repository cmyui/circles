// MD5, which must match osu!'s beatmap hashes exactly.
#include <circles/md5.h>
#include <circles/types.h>
#include <tests/support/test.h>

#include <string>
#include <string_view>
#include <vector>

namespace {

std::string md5_of(std::string_view text) {
  return circles::md5_hex(
      {reinterpret_cast<const circles::u8*>(text.data()), text.size()});
}

}  // namespace

static void test_known_digests() {
  CHECK_EQ(md5_of(""), "d41d8cd98f00b204e9800998ecf8427e");
  CHECK_EQ(md5_of("The quick brown fox jumps over the lazy dog"),
           "9e107d9d372bb6826bd81d3542a419d6");
}

static void test_inputs_spanning_padding_and_several_blocks() {
  // 56 bytes leaves no room for the length in the first block.
  std::vector<circles::u8> bytes(56);
  for (size_t i = 0; i < bytes.size(); ++i)
    bytes[i] = static_cast<circles::u8>(i);
  CHECK_EQ(circles::md5_hex(bytes), "51fdd1acda72405dfdfa03fcb85896d7");
  CHECK_EQ(md5_of(std::string(1000, 'a')), "cabe45dcc9ae5b66ba86600cca6b8ba8");
}

int main() {
  test_known_digests();
  test_inputs_spanning_padding_and_several_blocks();
  return test_result();
}
