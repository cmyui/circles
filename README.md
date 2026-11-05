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

The game reads `shaders/`, `beatmaps/`, `skins/` and `replays/` from its
working directory: `make run` opens the app with `--working-dir` set to this
folder, and its output goes to the terminal. Quit with Cmd+Q: Ctrl+C stops
`make`, not the game. It builds as an app bundle, `build/circles.app`,
which marks it as a game: macOS turns on Game Mode while it's full screen
(F11), giving it priority on the CPU and GPU. Opened as an app, it's also
circles, not the terminal, that macOS asks for permissions.

macOS keeps a permission such as Input Monitoring only while the app's
signature matches. The build signs ad hoc, which changes with every build;
to keep permissions across builds, sign with a certificate:
`cmake -B build -DCIRCLES_SIGN_IDENTITY="Apple Development"`.

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
`120 fps   1000 updates/s   key to screen 11.8 ms   aim to screen 8.8 ms   key to sound 19.6 ms`:

- **fps**: frames the display showed per second, as Metal reports them, in
  full screen. Windowed, macOS doesn't report what the compositor shows, so
  it reads **drawn/s**: frames drawn per second.
- **updates/s**: game loop passes per second, each reading input, advancing
  the game and starting sounds.
- **key to screen**: from a key or button press, as macOS stamped it, to the
  first frame drawn after the game read it being on screen.
- **aim to screen**: from a cursor movement reaching macOS (its device's
  report, once the driver stage below is known; until then, the movement's
  own stamp) to the first frame drawn after it being on screen.
- **key to sound**: from a press to its hitsound leaving the speakers: the
  read, wait and output stages below, added up.

F6 switches between that line, a table of each path's stages, and nothing
(saved in `settings.txt`):

```
                driver  read  wait  output   total
key to screen        -  0.52   3.1     8.0    11.8 ms
aim to screen     0.08  0.12  0.48     8.0     8.8 ms
key to sound         -  0.52  0.42    18.7    19.6 ms

keyboard reports every 8.0 ms   aim every 0.99 ms
120 fps   1000 updates/s
```

- **driver**: from a pointing device's report reaching macOS to the cursor
  movement made from it. A mouse's movement is stamped with its report's
  arrival, so about 0; a tablet driver such as OpenTabletDriver adds its
  processing. It times the movement, not a smoothing filter's lag in
  position. Reading device reports needs the Input Monitoring permission,
  which the table asks for. Not measured for keys: `-`.
- **read**: from macOS's stamp to the game loop reading the input.
- **wait**: until the next frame starts drawing, or for sound, the audio's
  next cycle starts. A moving cursor's newest movement is fresh when a frame
  starts, so its wait is shorter than a press's.
- **output**: from a frame starting to draw to the display showing it, as
  Metal reports it; for sound, the output device's delay as Core Audio
  reports it, not measured.
- **total**: for the screen, measured from the input's stamp to the
  display, plus the driver for aim; for sound, the stages added up.
- **keyboard reports every**: the longest interval a connected keyboard
  tells macOS it reports at (not measured). A press waits up to that long
  before macOS sees it, which no total includes. **aim every**: the time
  between cursor movements.

Figures are medians of the last 30 samples, refreshed four times a second
and shown to hundredths under 1 ms; one with no new sample for 3 seconds
reads `-`. A median of a whole path needn't equal its stages' medians added
up. The screen's output and totals are full screen only (`-` windowed:
macOS doesn't tell an app when its window's frame reaches the display), and
an input read over 100 ms late, as while a map loads, is left out.

To measure further: F8 records a profile to `perf/` (F8 again saves it)
for `tools/perf_summary.py`. In full screen the profile also times each
frame from the start of its drawing to the display showing it.

## Tablet

F5, or the Tablet page of the settings (Ctrl+O, then Tab), switches how the
pen aims:

- **Through its driver**, the default: the cursor macOS gets from the
  tablet's driver, such as OpenTabletDriver.
- **Read by circles**: circles reads the tablet's own reports and places the
  cursor itself, skipping the driver and macOS's cursor events. In the stage
  table, aim to screen then has no driver stage, and its read runs from a
  report reaching macOS to the game using it.

Reading the tablet needs the Input Monitoring permission, which F5 asks for.
So far it reads one tablet, the One by Wacom S (CTL-472), decoding its
reports as OpenTabletDriver does. circles switches it to sending pen reports
itself, so no driver needs to run; switching back, or quitting, gives the
tablet its previous mode again. While the pen is in range, a driver's cursor
and clicks are ignored; a mouse or trackpad still works.

The Tablet page sets:

- **The area**: the part of the tablet that maps onto the screen, by its
  centre and size in mm and its clockwise rotation in degrees. The first
  time the pen is read, circles imports the area OpenTabletDriver has, if
  any; **Import area from OpenTabletDriver** does it again. Only the tablet
  area is imported: it maps onto the whole display, whatever display area
  OpenTabletDriver had.
- **Map to full screen**: on, the area maps onto the whole display, and
  windowed, the cursor stops at the window's edges; off, it maps onto just
  the window. In full screen they're the same.
- **Tip pressure**: how hard, in percent of what the pen reads, the tip must
  press to click the left button. The side buttons aren't read.

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
| F6 | Latency readout: a line, a table of stages, or hidden; saved in `settings.txt` |
| F5 | The pen: read by circles, or through its driver (see Tablet); saved in `settings.txt` |
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
