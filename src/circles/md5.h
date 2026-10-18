#pragma once

#include <circles/types.h>

#include <span>
#include <string>

namespace circles {

// MD5 as 32 lowercase hex digits. osu! identifies beatmaps by the MD5 of
// their .osu file, which is how replays find their map.
std::string md5_hex(std::span<const u8> data);

}  // namespace circles
