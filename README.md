# circles

An osu!-style rhythm game in C++20 and Metal that plays real osu!standard
beatmaps, parsed with
[fosu](https://github.com/cmyui/fast-osu-beatmap-parser).

Gameplay follows osu!stable's own code. fosu's stable mode works out
stable's slider paths, timing and stacking; input and note lock, hit
windows, slider and spinner scoring, combo, health, and score are ported
here. Plays score as they would there, and stable replays play back with
the same judgements.

## Build and run

macOS 15 or later: rendering uses Metal and CAMetalLayer. Needs CMake, GLFW and
xz, for replays' LZMA compression (`brew install cmake glfw xz`).

```sh
make run     # build into build/ and start the game
make test    # rule, archive, skin and replay tests
make test-metal # graphics checks with Metal API and shader validation
make format  # clang-format the sources
```

The build embeds the Metal shader source in the executable; the renderer
compiles it once during initialization. The game reads `beatmaps/`, `skins/` and
`replays/` from the working directory, so run it from this folder.

## Beatmaps

Put `.osz` files or extracted beatmap folders in `beatmaps/`. At startup each
`.osz` is extracted into a folder named after it (the archive is kept), and
every osu!standard difficulty is listed; other modes are skipped.

## Skins

Put osu! skins (`.osk` files or skin folders) in `skins/`. A newly added
skin becomes active; F9 cycles through the skins and the built-in look. Skins
cover gameplay: hit objects, cursor, judgements, score digits, health bar,
grades and hitsounds. Elements a skin leaves out keep the built-in look.

## Replays

Every finished play is saved to `replays/` as an osu!-compatible `.osr`, so
osu! can open it too. The results screen's Watch button (or W) plays it back;
song select lists the selected difficulty's replays, best first, and clicking
one plays it back. Replays from osu! dropped into `replays/` are listed once
their beatmap is in `beatmaps/`: a replay names its beatmap by the MD5 hash of
the `.osu` file, so it needs that exact version. Space skips a replay's intro
and Esc pauses it.

## Mods

F1, or the Mods button in song select, opens mod select. It has osu!stable's
osu!standard mods except Target Practice, with stable's keys, exclusions and
score multipliers:

| Key | Mod | Effect |
|---|---|---|
| Q | Easy | Halves circle size, approach rate, accuracy and HP settings; revives you twice |
| W | No Fail | You can't fail |
| E | Half Time | 0.75x speed, same pitch |
| A | Hard Rock | Raises the settings (circle size 1.3x, others 1.4x) and flips the playfield vertically |
| S | Sudden Death, then Perfect | Fail at the first miss or slider break; Perfect restarts on anything but a 300 |
| D | Double Time, then Nightcore | 1.5x speed; Nightcore raises the pitch and adds a drum track |
| F | Hidden | Circles fade out before you hit them; no approach circles after the first |
| G | Flashlight | Only the area around the cursor is visible, shrinking at 100 and 200 combo |
| Z | Relax | Circles hit themselves when the cursor is on them; you aim |
| X | Autopilot | The cursor moves itself; you tap |
| C | Spun Out | Spinners spin themselves |
| V | Auto, then Cinema | Watch a perfect play; Cinema shows only the background |
| B | Score V2 | osu!'s newer scoring, out of 1,000,000 |

Pressing a key again steps a two-mod button to its second mod, then off; 1
resets. The song select preview plays at the chosen speed, and the stats show
the settings as the mods make them feel. Replays save their mods and play back
with them. Hidden and Flashlight make S and SS grades silver.

## Controls

| Key | Action |
|---|---|
| Z, X, left and right mouse buttons | Hit (a tablet pen tap is a left click) |
| Esc | Pause, or go back |
| Space | Skip the intro |
| <code>`</code> | Retry immediately |
| `-` and `=` (Shift for 25 ms steps) | Global audio offset by 5 ms, on the main menu or in gameplay; raise it when sound reaches you late, as with Bluetooth or delayed speakers. Saved in `settings.txt` |
| Alt (Option) + scroll, or scroll on the main menu | Volume, saved in `settings.txt` |
| F9 | Next skin (outside gameplay) |
| F10 | Mouse buttons on or off for hitting, saved in `settings.txt` |
| F11 | Fullscreen |
| F1 | Mod select (song select) |

In song select, type to search, use the arrow keys to choose, Enter to play
and F2 for a random song.

## Rendering and profiling

Game updates, input, GPU encoding and presentation submission run on the main
thread. A worker obtains one drawable ahead of rendering, because the window
server can block drawable acquisition even with VSync off. Until a drawable is
available, game updates continue without building an image. Each rendered image
uses the latest game state and is submitted directly to the display layer.
There is no offscreen-frame mailbox or extra fullscreen presentation copy.

Upload buffers use three frame slots and are recycled only after GPU completion.
Command buffers retain the textures and buffers they reference. Snaking sliders
use individual scratch targets so batching preserves painter order.

The corner FPS counter counts submitted render frames, not updates or confirmed
display refreshes. F8 records update-loop phases, passive GPU command-buffer
timestamps and a `rendered` flag per update. `swap` is the CPU time encoding and
submitting the final pass; it does not include the worker's drawable wait or
measure when pixels reach the display. With the frame cap off, an update that
could not render waits for an input/drawable event for up to 1 ms on its next
poll, avoiding an idle spin. Input and drawable availability wake that wait.

```sh
python3 tools/perf_summary.py perf/<stamp>-frames.csv
```

## Replay benchmarks

Build Release and run a replay with automatic intro skipping:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8
./build/circles --benchmark replays/example.osr --uncapped --skip-intro --benchmark-seconds 22
```

`--benchmark-seconds` limits time from process startup and saves the profile
before exiting. `--skip-intro` uses a fixed landing time at the normal skip
boundary. Controls remain available; Space skips manually, the arrow keys
seek, and Esc pauses. Benchmarks continue when their window loses focus.
Avoid interacting during a measured comparison.

Run the CPU checks with `make test`. The separate graphics checks require a
logged-in macOS graphics session:

```sh
cmake --build build --target check-metal
```

Compare submitted render rate, update rate, render intervals and GPU time
separately. Keep the replay, skin, window size and presentation settings
identical, discard startup/warm-up, and exclude runs whose controls were used.
The benchmark deadline includes startup; an external process timeout can also
bound startup, shutdown and driver stalls.

## Limitations

- Without a skin, hitsounds are synthesized. Beatmaps' own hitsound samples,
  storyboards and videos are ignored.
- Skins don't restyle the menus, song select, pause screen or input overlay.
- osu!stable scores a replay's spinners partly on its own display frames, so
  watching a stable replay can differ from stable by a spinner bonus.
- There are no per-beatmap offsets; `-` and `=` set one global offset.
