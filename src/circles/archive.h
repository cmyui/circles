#pragma once

// .osz beatmap and .osk skin archives are zip files. This extracts stored and
// deflated entries, which is all osu! produces.

#include <circles/types.h>

#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

namespace circles {

// Writes the archive's files under `destination`. Entries whose names would
// land outside it (absolute paths, "..") are skipped. False if the archive is
// malformed; files extracted before the problem remain.
bool extract_zip(std::span<const u8>          archive,
                 const std::filesystem::path& destination);

// Extracts each archive with `extension` (".osz" beatmaps, ".osk" skins) in
// `directory` into a folder named after it, skipping archives whose folder
// already exists. Returns the folders created.
std::vector<std::filesystem::path> import_archives(
    const std::filesystem::path& directory,
    std::string_view             extension);

}  // namespace circles
