# Vendored libraries

| Library | Version | Files |
|---|---|---|
| [fosu](https://github.com/cmyui/fast-osu-beatmap-parser) | `24ab81f` | `fosu/` (headers from `src/fosu`) |
| [miniaudio](https://github.com/mackron/miniaudio) | 0.11.25 | `miniaudio.h` |
| [osu_replay_zip](https://github.com/cmyui/osu_replay_zip) | `d69c786` (the fork's `experimental-unified-contexts` branch), changed as below | `osu_replay_zip/` (its headers, and `Lib/`, the LZMA SDK's decoder) |
| [stb](https://github.com/nothings/stb) | `2c980bb` | `stb_image.h`, `stb_truetype.h`, `stb_vorbis.c` |

The single-header implementations are compiled in `src/circles/third_party.c`.

osu_replay_zip's headers are compiled in `src/circles/orz.cc`. They differ
from upstream in these ways:

- **Frames straight in and out.** `osr_frames_to_orz` (`orz_write.h`) makes
  an `.orz` from an `.osr`'s fields and the replay's frames, and
  `replay_frames_to_orz` from frames alone, without the replay text and its
  LZMA being made and read back; `osr_fields` (`orz_read.h`) gives the fields
  of a decoded file; `dotnet::g7_float` (`orz_dotnet.h`) is the float a
  7-digit text reads back as. The `.orz` is the one the whole `.osr`, or the
  replay data, makes.
- **Replay data with no size in its LZMA header** is read
  (`decompress_osr_string`, `orz_shared.h`), ending on its end marker.
  Replays circles saved before it used stable's LZMA encoder are like this.
- **A device the caller gives, and lazer's positions.** A hinted setup
  can be forced: the replay is coded with its device without a search. A
  setup can be a lattice, or the two float steps lazer makes a position
  by (`chain_params`, `device_space.h`), which circles gives for a pen it
  reads; a replay in lazer's text is tried as whole pixels made that way
  (`orz_lazer.h`, a file added here). These are the library's pull requests
  39 and 41.
- **macOS 13.** libc++'s floating-point `std::from_chars` needs macOS 26 and
  `std::to_chars` 13.3; the six calls to them (in `orz_decimal.h`,
  `orz_dotnet.h`, `orz_life.h` and `orz_text.h`) go to stand-ins in
  `orz_chars.h`, a file added here.

For input upstream reads, the `.orz` files written are the same, but for
replays in lazer's text that fit its whole pixels, which are smaller.

The `.orz` format isn't final: a later osu_replay_zip may not read files this
one wrote. Before updating it, export the replays worth keeping as `.osr`
with the version that wrote them.

GLFW 3.4 isn't kept here: CMake downloads its release archive when it first
configures, pinned by URL and SHA-256 in `CMakeLists.txt`.
