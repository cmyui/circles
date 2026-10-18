#pragma once

// osu!stable's gameplay rules, one game-loop frame at a time: input becomes
// presses, presses hit objects past note lock, sliders and spinners score,
// and combo, health and failing follow, in the order stable runs them.

#include <circles/app.h>

#include <fosu/beatmap.h>

namespace circles {

enum class StepOutcome : u8 { Continue, Fail, Recover, Restart };

// Prepares judging for the objects gameplay_start built from `map`.
void start_judging(App& app, const fosu::Beatmap& map);

// One frame at whole-ms `time`, with the cursor at `cursor` (osu! pixels)
// and `keys` held (replay frame bits). `scorable` marks stable's 60 Hz
// frames, and every frame of a replay; spinners only read input on them.
StepOutcome play_step(App& app, i32 time, Vec2 cursor, u32 keys, bool scorable);

// Plays a hit's normal sound and additions, panned by where it is.
f32 hitsound_pan(Vec2 position);
void play_hitsound(App&            app,
                   const Hitsound& hitsound,
                   f32             volume,
                   Vec2            position);

}  // namespace circles
