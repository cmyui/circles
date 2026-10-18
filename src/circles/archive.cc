#include <circles/archive.h>
#include <circles/strings.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stb_image.h>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace circles {

namespace {

namespace fs = std::filesystem;

constexpr u32    kEndOfDirectorySignature = 0x06054b50;
constexpr u32    kDirectoryEntrySignature = 0x02014b50;
constexpr u32    kLocalHeaderSignature = 0x04034b50;
constexpr size_t kEndOfDirectorySize = 22;
constexpr size_t kDirectoryEntrySize = 46;
constexpr size_t kLocalHeaderSize = 30;
constexpr u16    kStored = 0;
constexpr u16    kDeflated = 8;

u16 read_u16(const u8* p) {
  return static_cast<u16>(p[0] | p[1] << 8);
}

u32 read_u32(const u8* p) {
  return p[0] | p[1] << 8 | p[2] << 16 | static_cast<u32>(p[3]) << 24;
}

// A relative path with no component that climbs out of the destination.
bool is_contained(std::string_view name) {
  if (name.empty() || name.front() == '/' || name.find(':') != name.npos)
    return false;
  size_t start = 0;
  while (start <= name.size()) {
    const size_t end = std::min(name.find('/', start), name.size());
    if (name.substr(start, end - start) == "..")
      return false;
    start = end + 1;
  }
  return true;
}

bool write_file(const fs::path& path, std::span<const u8> bytes) {
  std::error_code error;
  fs::create_directories(path.parent_path(), error);
  std::ofstream file(path, std::ios::binary);
  file.write(reinterpret_cast<const char*>(bytes.data()),
             static_cast<std::streamsize>(bytes.size()));
  return static_cast<bool>(file);
}

}  // namespace

bool extract_zip(std::span<const u8> archive, const fs::path& destination) {
  if (archive.size() < kEndOfDirectorySize)
    return false;
  // The end-of-central-directory record ends the file, after a comment of at
  // most 64 KiB.
  const size_t lowest = archive.size() > kEndOfDirectorySize + 0xFFFF
                            ? archive.size() - kEndOfDirectorySize - 0xFFFF
                            : 0;
  size_t       end = archive.size() - kEndOfDirectorySize + 1;
  do {
    --end;
  } while (end > lowest && read_u32(&archive[end]) != kEndOfDirectorySignature);
  if (read_u32(&archive[end]) != kEndOfDirectorySignature)
    return false;

  const u16 entries = read_u16(&archive[end + 10]);
  size_t    p = read_u32(&archive[end + 16]);
  for (u16 i = 0; i < entries; ++i) {
    if (p + kDirectoryEntrySize > archive.size() ||
        read_u32(&archive[p]) != kDirectoryEntrySignature)
      return false;
    const u16 method = read_u16(&archive[p + 10]);
    const u32 compressed_size = read_u32(&archive[p + 20]);
    const u32 size = read_u32(&archive[p + 24]);
    const u16 name_length = read_u16(&archive[p + 28]);
    const u16 extra_length = read_u16(&archive[p + 30]);
    const u16 comment_length = read_u16(&archive[p + 32]);
    const u32 local_offset = read_u32(&archive[p + 42]);
    if (p + kDirectoryEntrySize + name_length > archive.size())
      return false;
    std::string name(reinterpret_cast<const char*>(&archive[p + 46]),
                     name_length);
    p += kDirectoryEntrySize + name_length + extra_length + comment_length;

    std::replace(name.begin(), name.end(), '\\', '/');
    if (name.empty() || name.back() == '/' || !is_contained(name))
      continue;
    if (static_cast<size_t>(local_offset) + kLocalHeaderSize > archive.size() ||
        read_u32(&archive[local_offset]) != kLocalHeaderSignature)
      return false;
    const size_t data = local_offset + kLocalHeaderSize +
                        read_u16(&archive[local_offset + 26]) +
                        read_u16(&archive[local_offset + 28]);
    if (data + compressed_size > archive.size())
      return false;

    std::vector<u8> bytes(size);
    if (method == kStored) {
      if (compressed_size != size)
        return false;
      std::copy_n(&archive[data], size, bytes.begin());
    } else if (method == kDeflated) {
      if (size &&
          stbi_zlib_decode_noheader_buffer(
              reinterpret_cast<char*>(bytes.data()), static_cast<int>(size),
              reinterpret_cast<const char*>(&archive[data]),
              static_cast<int>(compressed_size)) != static_cast<int>(size))
        return false;
    } else {
      continue;  // osu! never writes other methods
    }
    if (!write_file(destination / fs::path(name), bytes))
      return false;
  }
  return true;
}

std::vector<fs::path> import_archives(const fs::path&  directory,
                                      std::string_view extension) {
  std::error_code       error;
  std::vector<fs::path> imported;
  for (const auto& entry : fs::directory_iterator(directory, error)) {
    const fs::path& archive = entry.path();
    if (!entry.is_regular_file() ||
        lowercase(archive.extension().string()) != extension)
      continue;
    const fs::path target = directory / archive.stem();
    if (fs::exists(target))
      continue;
    std::ifstream   file(archive, std::ios::binary);
    std::vector<u8> bytes((std::istreambuf_iterator<char>(file)),
                          std::istreambuf_iterator<char>());
    // Extract beside the target so a failure never leaves a half-imported
    // folder that looks complete.
    fs::path        staging = target;
    staging += ".importing";
    fs::remove_all(staging, error);
    if (extract_zip(bytes, staging)) {
      fs::rename(staging, target, error);
      if (!error)
        imported.push_back(target);
    } else {
      fprintf(stderr, "import: %s is not a valid archive\n",
              archive.string().c_str());
      fs::remove_all(staging, error);
    }
  }
  return imported;
}

}  // namespace circles
