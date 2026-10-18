# circles

An osu!-style rhythm game in C++20 and OpenGL that plays real osu!standard
beatmaps, parsed with
[fosu](https://github.com/cmyui/fast-osu-beatmap-parser).

## Build and run

macOS only: rendering uses Apple's OpenGL 4.1 headers. Needs CMake and GLFW
(`brew install cmake glfw`).

```sh
make run     # build into build/ and start the game
make test    # rule and archive tests
make format  # clang-format the sources
```

The game reads `shaders/` and `beatmaps/` from the working directory, so run
it from this folder.

## Beatmaps

Put `.osz` files or extracted beatmap folders in `beatmaps/`. At startup each
`.osz` is extracted into a folder named after it (the archive is kept), and
every osu!standard difficulty is listed; other modes are skipped.

## Skins

Put osu! skins (`.osk` files or skin folders) in `skins/`. A newly added
skin becomes active; F9 cycles through the skins and the built-in look. Skins
cover gameplay: hit objects, cursor, judgements, score digits, health bar,
grades and hitsounds. Elements a skin leaves out keep the built-in look.

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

In song select, type to search, use the arrow keys to choose, Enter to play
and F2 for a random song.

## Limitations

- Without a skin, hitsounds are synthesized. Beatmaps' own hitsound samples,
  storyboards and videos are ignored.
- Skins don't restyle the menus, song select, pause screen or input overlay.
- No mods.
- Scoring follows osu!stable's score formula; health is a simplified version
  of osu!'s.
