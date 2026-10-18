// .osz extraction, including archives that try to write outside their beatmap
// folder.
#include <circles/archive.h>
#include <circles/types.h>
#include <tests/support/test.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
using circles::u8;

void put_u16(std::vector<u8>& out, unsigned value) {
  out.push_back(static_cast<u8>(value));
  out.push_back(static_cast<u8>(value >> 8));
}

void put_u32(std::vector<u8>& out, unsigned value) {
  put_u16(out, value & 0xFFFF);
  put_u16(out, value >> 16);
}

// A zip of uncompressed files. Extraction doesn't verify CRCs, so they're 0.
std::vector<u8> stored_zip(
    const std::vector<std::pair<std::string, std::string>>& files) {
  std::vector<u8>       zip;
  std::vector<unsigned> offsets;
  for (const auto& [name, contents] : files) {
    offsets.push_back(static_cast<unsigned>(zip.size()));
    put_u32(zip, 0x04034b50);
    put_u16(zip, 20);  // version needed
    put_u16(zip, 0);   // flags
    put_u16(zip, 0);   // stored
    put_u32(zip, 0);   // time and date
    put_u32(zip, 0);   // crc
    put_u32(zip, static_cast<unsigned>(contents.size()));
    put_u32(zip, static_cast<unsigned>(contents.size()));
    put_u16(zip, static_cast<unsigned>(name.size()));
    put_u16(zip, 0);
    zip.insert(zip.end(), name.begin(), name.end());
    zip.insert(zip.end(), contents.begin(), contents.end());
  }
  const unsigned directory = static_cast<unsigned>(zip.size());
  for (size_t i = 0; i < files.size(); ++i) {
    const auto& [name, contents] = files[i];
    put_u32(zip, 0x02014b50);
    put_u16(zip, 20);  // version made by
    put_u16(zip, 20);  // version needed
    put_u16(zip, 0);
    put_u16(zip, 0);
    put_u32(zip, 0);
    put_u32(zip, 0);
    put_u32(zip, static_cast<unsigned>(contents.size()));
    put_u32(zip, static_cast<unsigned>(contents.size()));
    put_u16(zip, static_cast<unsigned>(name.size()));
    put_u16(zip, 0);  // extra
    put_u16(zip, 0);  // comment
    put_u16(zip, 0);  // disk
    put_u16(zip, 0);  // internal attributes
    put_u32(zip, 0);  // external attributes
    put_u32(zip, offsets[i]);
    zip.insert(zip.end(), name.begin(), name.end());
  }
  const unsigned directory_size = static_cast<unsigned>(zip.size()) - directory;
  put_u32(zip, 0x06054b50);
  put_u16(zip, 0);
  put_u16(zip, 0);
  put_u16(zip, static_cast<unsigned>(files.size()));
  put_u16(zip, static_cast<unsigned>(files.size()));
  put_u32(zip, directory_size);
  put_u32(zip, directory);
  put_u16(zip, 0);
  return zip;
}

std::string read(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file),
          std::istreambuf_iterator<char>()};
}

// An empty directory nested two levels inside the temp directory, so escapes
// one level up stay inside the test's own tree.
fs::path fresh_directory(const char* name) {
  const fs::path root = fs::temp_directory_path() / "circles_tests" / name;
  fs::remove_all(root);
  fs::create_directories(root / "beatmaps" / "set");
  return root / "beatmaps" / "set";
}

}  // namespace

static void test_stored_entries_extract_into_folders() {
  const fs::path destination = fresh_directory("stored");
  CHECK(circles::extract_zip(stored_zip({{"map.osu", "osu file format v14"},
                                         {"sb/bg.jpg", "jpeg"},
                                         {"Song/", ""}}),
                             destination));
  CHECK_EQ(read(destination / "map.osu"), "osu file format v14");
  CHECK_EQ(read(destination / "sb" / "bg.jpg"), "jpeg");
}

static void test_deflated_entries_extract() {
  // Written by Python's zipfile with ZIP_DEFLATED.
  const std::vector<u8> zip = {
      0x50, 0x4b, 0x03, 0x04, 0x14, 0x00, 0x00, 0x00, 0x08, 0x00, 0x6a, 0xa8,
      0x51, 0x5d, 0xe3, 0x0f, 0x8f, 0xc8, 0x19, 0x00, 0x00, 0x00, 0xa0, 0x00,
      0x00, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x53, 0x6f, 0x6e, 0x67, 0x2f, 0x6d,
      0x61, 0x70, 0x2e, 0x6f, 0x73, 0x75, 0xcb, 0x2f, 0x2e, 0x55, 0x48, 0xcb,
      0xcc, 0x49, 0x55, 0x48, 0xcb, 0x2f, 0xca, 0x4d, 0x2c, 0x51, 0x28, 0x33,
      0x34, 0xe1, 0xca, 0x1f, 0x44, 0x62, 0x00, 0x50, 0x4b, 0x01, 0x02, 0x14,
      0x03, 0x14, 0x00, 0x00, 0x00, 0x08, 0x00, 0x6a, 0xa8, 0x51, 0x5d, 0xe3,
      0x0f, 0x8f, 0xc8, 0x19, 0x00, 0x00, 0x00, 0xa0, 0x00, 0x00, 0x00, 0x0c,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80,
      0x01, 0x00, 0x00, 0x00, 0x00, 0x53, 0x6f, 0x6e, 0x67, 0x2f, 0x6d, 0x61,
      0x70, 0x2e, 0x6f, 0x73, 0x75, 0x50, 0x4b, 0x05, 0x06, 0x00, 0x00, 0x00,
      0x00, 0x01, 0x00, 0x01, 0x00, 0x3a, 0x00, 0x00, 0x00, 0x43, 0x00, 0x00,
      0x00, 0x00, 0x00,
  };
  const fs::path destination = fresh_directory("deflated");
  CHECK(circles::extract_zip(zip, destination));
  std::string expected;
  for (int i = 0; i < 8; ++i)
    expected += "osu file format v14\n";
  CHECK_EQ(read(destination / "Song" / "map.osu"), expected);
}

static void test_entries_outside_the_destination_are_skipped() {
  const fs::path destination = fresh_directory("escape");
  CHECK(circles::extract_zip(stored_zip({{"../escaped.txt", "x"},
                                         {"a/../../escaped_too.txt", "x"},
                                         {"..\\backslash.txt", "x"},
                                         {"/absolute.txt", "x"},
                                         {"kept.txt", "kept"}}),
                             destination));
  CHECK(!fs::exists(destination.parent_path() / "escaped.txt"));
  CHECK(!fs::exists(destination.parent_path() / "escaped_too.txt"));
  CHECK(!fs::exists(destination.parent_path() / "backslash.txt"));
  CHECK(!fs::exists("/absolute.txt"));
  CHECK_EQ(read(destination / "kept.txt"), "kept");
}

static void test_truncated_archives_are_rejected() {
  auto zip = stored_zip({{"map.osu", "osu file format v14"}});
  zip.resize(zip.size() - 10);
  CHECK(!circles::extract_zip(zip, fresh_directory("truncated")));
  CHECK(!circles::extract_zip({}, fresh_directory("empty")));
}

int main() {
  test_stored_entries_extract_into_folders();
  test_deflated_entries_extract();
  test_entries_outside_the_destination_are_skipped();
  test_truncated_archives_are_rejected();
  return test_result();
}
