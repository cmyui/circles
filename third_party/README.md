# Vendored libraries

| Library | Version | Files |
|---|---|---|
| [fosu](https://github.com/cmyui/fast-osu-beatmap-parser) | `24ab81f` | `fosu/` (headers from `src/fosu`) |
| [miniaudio](https://github.com/mackron/miniaudio) | 0.11.25 | `miniaudio.h` |
| [osu_replay_zip](https://github.com/rumoi/osu_replay_zip) | `e1ebf5a` | `osu_replay_zip/` (`osu_lzma.h`, the LZMA encoder osu! uses, and the `orz_types.h` it includes) |
| [stb](https://github.com/nothings/stb) | `2c980bb` | `stb_image.h`, `stb_truetype.h`, `stb_vorbis.c` |

The single-header implementations are compiled in `src/circles/third_party.c`.

GLFW 3.4 and xz 5.8.4 (for liblzma's headers, to decompress replays; the
library is macOS's) aren't kept here: CMake downloads their release archives
when it first configures, pinned by URL and SHA-256 in `CMakeLists.txt`.
