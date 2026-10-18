#!/usr/bin/env python3
"""Summarizes a circles profile: where frame time goes, and input timing.

Usage: tools/perf_summary.py perf/<stamp>-frames.csv [--all-screens]

Profiles come from F8 in game (press again to save) or
`circles --benchmark replay.osr`. By default only gameplay frames count.
"""

import argparse
import csv
import statistics
from collections import defaultdict
from pathlib import Path

PHASES = ["poll", "update", "draw", "swap", "sleep"]
SCREENS = ["main menu", "song select", "gameplay", "results"]


def pct(values, fraction):
    if not values:
        return float("nan")
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int(fraction * (len(ordered) - 1)))]


def row(label, values, unit="ms"):
    if not values:
        return f"  {label:<18} -"
    return (
        f"  {label:<18} mean {statistics.fmean(values):7.3f}  "
        f"p50 {pct(values, 0.5):7.3f}  p99 {pct(values, 0.99):7.3f}  "
        f"max {max(values):8.3f} {unit}"
    )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("frames_csv", type=Path)
    parser.add_argument("--all-screens", action="store_true")
    args = parser.parse_args()

    with args.frames_csv.open() as f:
        frames = [{k: float(v) for k, v in r.items()} for r in csv.DictReader(f)]
    by_index = {int(f["frame"]): f for f in frames}
    gameplay = SCREENS.index("gameplay")
    chosen = [f for f in frames if args.all_screens or f["screen"] == gameplay]
    if not chosen:
        print("no frames on that screen")
        return
    span = (chosen[-1]["start_ms"] - chosen[0]["start_ms"]) / 1000
    totals = [sum(f[f"{p}_ms"] for p in PHASES) for f in chosen]
    busy = [f["poll_ms"] + f["update_ms"] + f["draw_ms"] for f in chosen]
    gpu = [f["gpu_ms"] for f in chosen if f["gpu_ms"] >= 0]

    print(f"{args.frames_csv.name}: {len(chosen)} frames over {span:.1f} s, "
          f"{len(chosen) / max(span, 1e-9):.0f} fps")
    print("frame time")
    print(row("total", totals))
    for phase in PHASES:
        values = [f[f"{phase}_ms"] for f in chosen]
        share = sum(values) / max(sum(totals), 1e-9)
        print(row(phase, values) + f"  ({share:5.1%})")
    print(row("cpu busy", busy) + "  (poll + update + draw)")
    print(row("gpu", gpu))
    # Only sampled frames are GPU-timed.
    timed = [f for f in chosen if f["gpu_ms"] >= 0]
    gpu_bound = sum(1 for f in timed if f["gpu_ms"] > f["poll_ms"] + f["update_ms"] + f["draw_ms"])
    print(f"  gpu took longer than the cpu's work on "
          f"{gpu_bound / max(len(timed), 1):.1%} of {len(timed)} timed frames")
    print(f"  draw calls {statistics.fmean(f['draw_calls'] for f in chosen):.1f}, "
          f"vertices {statistics.fmean(f['vertices'] for f in chosen):.0f} per frame")
    slow = sorted(range(len(chosen)), key=lambda i: totals[i], reverse=True)[:5]
    print("slowest frames: " + ", ".join(
        f"#{int(chosen[i]['frame'])} {totals[i]:.2f} ms" for i in slow))

    input_csv = args.frames_csv.with_name(
        args.frames_csv.name.replace("-frames.csv", "-input.csv"))
    if not input_csv.exists():
        return
    with input_csv.open() as f:
        inputs = list(csv.DictReader(f))
    frame_ids = {int(f["frame"]) for f in chosen}
    inputs = [i for i in inputs if int(i["frame"]) in frame_ids]
    if not inputs:
        return
    print("input")
    devices = defaultdict(list)
    for i in inputs:
        devices[(i["device"], i["kind"] == "move")].append(i)
    for (device, moves), events in sorted(devices.items()):
        times = [float(e["event_ms"]) for e in events]
        gaps = [b - a for a, b in zip(times, times[1:]) if b > a]
        delays = [float(e["received_ms"]) - float(e["event_ms"]) for e in events]
        kind = "moves" if moves else "presses"
        print(f"  {device} {kind}: {len(events)} events"
              + (f", report interval p50 {pct(gaps, 0.5):.2f} ms "
                 f"(~{1000 / pct(gaps, 0.5):.0f} Hz)" if moves and gaps else ""))
        print("  " + row("event → seen", delays))
    # A press counts at the update after the poll that saw it, and shows once
    # that frame's swap returns.
    to_update, to_swap = [], []
    for i in inputs:
        if i["kind"] not in ("key_down", "button_down"):
            continue
        frame = by_index.get(int(i["frame"]))
        if not frame:
            continue
        event = float(i["event_ms"])
        updated = frame["start_ms"] + frame["poll_ms"] + frame["update_ms"]
        swapped = updated + frame["draw_ms"] + frame["swap_ms"]
        to_update.append(updated - event)
        to_swap.append(swapped - event)
    if to_update:
        print("presses")
        print(row("event → judged", to_update))
        print(row("event → swapped", to_swap))


if __name__ == "__main__":
    main()
