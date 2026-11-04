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

macOS only: rendering uses Metal. Needs CMake, GLFW and xz, for replays'
LZMA compression (`brew install cmake glfw xz`).

```sh
make run     # build into build/ and start the game
make test    # rule, archive, skin and replay tests
make format  # clang-format the sources
```

The game reads `shaders/`, `beatmaps/`, `skins/` and `replays/` from the
working directory, so run it from this folder. It builds as an app bundle,
`build/circles.app`, which marks it as a game: macOS turns on Game Mode while
it's full screen (F11), giving it priority on the CPU and GPU.

## Frames and latency

The game loop runs up to 1000 times a second (the cap can be lifted in
settings, Ctrl+O). Each pass reads input, advances the game and plays its
sounds; presses are judged at the time macOS stamped them. How frames reach
the display depends on the window:

- Windowed, every pass draws a frame and hands it to macOS's window
  compositor, which shows the newest at each display refresh.
- Full screen (F11), frames skip the compositor and go straight to the
  display, one per refresh: each is drawn as soon as the display frees a
  buffer, and shows at the next refresh.

The bottom-right corner shows, for example,
`120 fps   1000 updates/s   display 7.8 ms   audio 19.4 ms   input 0.4 ms`:

- **fps**: frames the display showed per second, as Metal reports them, in
  full screen. Windowed, macOS doesn't report what the compositor shows, so
  it reads **drawn/s**: frames drawn per second.
- **updates/s**: game loop passes per second, each reading input, advancing
  the game and starting sounds.
- **display**: from starting to draw a frame to the display showing it, the
  median of the last 30 frames, as Metal reports it. Full screen only; `-`
  windowed.
- **audio**: from a sound being started to it leaving the speakers: the wait
  for the audio's next cycle, half of one on average, and the output
  device's delay as Core Audio reports it. Not measured per sound.
- **input**: from a key or button press, as macOS stamped it, to the game
  loop reading it, the median of the last 30 presses. A press read over
  100 ms late, as while a map loads, is left out.

What a press takes to show adds these up: input, then the wait for the next
frame to start drawing, then display. Its hitsound takes input, then audio.

To measure further: F8 records a profile to `perf/` (F8 again saves it)
for `tools/perf_summary.py`. In full screen the profile also times each
frame from the start of its drawing to the display showing it.

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
| F11 | Full screen, as the window's green button enters it: the display keeps its resolution |
| F1 | Mod select (song select) |

In song select, type to search, use the arrow keys to choose, Enter to play
and F2 for a random song.

## Limitations

- Without a skin, hitsounds are synthesized. Beatmaps' own hitsound samples,
  storyboards and videos are ignored.
- Skins don't restyle the menus, song select, pause screen or input overlay.
- osu!stable scores a replay's spinners partly on its own display frames, so
  watching a stable replay can differ from stable by a spinner bonus.
- There are no per-beatmap offsets; `-` and `=` set one global offset.
