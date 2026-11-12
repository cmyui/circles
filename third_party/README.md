# Vendored libraries

| Library | Version | Files |
|---|---|---|
| [fosu](https://github.com/cmyui/fast-osu-beatmap-parser) | `24ab81f` | `fosu/` (headers from `src/fosu`) |
| [miniaudio](https://github.com/mackron/miniaudio) | 0.11.25 | `miniaudio.h` |
| [osu_replay_zip](https://github.com/rumoi/osu_replay_zip) | `c9d2474` (its `experimental` branch), changed as below | `osu_replay_zip/` (its headers, and `Lib/`, the LZMA SDK's decoder) |
| [stb](https://github.com/nothings/stb) | `2c980bb` | `stb_image.h`, `stb_truetype.h`, `stb_vorbis.c` |

The single-header implementations are compiled in `src/circles/third_party.c`.

osu_replay_zip's headers are compiled in `src/circles/orz.cc`. They differ
from upstream in one way, so they build for macOS 13: libc++'s floating-point
`std::from_chars` needs macOS 26 and `std::to_chars` 13.3, and the six calls
to them (in `orz_decimal.h`, `orz_dotnet.h`, `orz_life.h` and `orz_text.h`)
go to stand-ins in `orz_chars.h`, a file added here. The `.orz` files written
are the same.

The `.orz` format isn't final: a later osu_replay_zip may not read files this
one wrote. Before updating it, turn the `.orz` replays worth keeping back into
`.osr` with the version that wrote them.

GLFW 3.4 and xz 5.8.4 (for liblzma's headers, to decompress replays; the
library is macOS's) aren't kept here: CMake downloads their release archives
when it first configures, pinned by URL and SHA-256 in `CMakeLists.txt`.
