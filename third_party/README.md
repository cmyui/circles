# Vendored libraries

| Library | Version | Files |
|---|---|---|
| [fosu](https://github.com/cmyui/fast-osu-beatmap-parser) | `24ab81f` | `fosu/` (headers from `src/fosu`) |
| [GLFW](https://github.com/glfw/glfw) | 3.4 | `glfw/` (its `include`, `src`, `CMake`, `CMakeLists.txt` and licence; built with its own CMake) |
| [xz](https://github.com/tukaani-project/xz) | 5.8.4 | `xz/` (liblzma's headers and licence; the library is macOS's) |
| [miniaudio](https://github.com/mackron/miniaudio) | 0.11.25 | `miniaudio.h` |
| [stb](https://github.com/nothings/stb) | `2c980bb` | `stb_image.h`, `stb_truetype.h`, `stb_vorbis.c` |

The single-header implementations are compiled in `src/circles/third_party.c`.
