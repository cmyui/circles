#!/usr/bin/env python3
"""Summarizes a circles profile: where frame time goes, and input timing.

Usage: tools/perf_summary.py perf/<stamp>-frames.csv [--all-screens]

Profiles come from F8 in game (press again to save) or
`circles --benchmark replay.osr`. By default only gameplay frames count.
"""

import argparse
import bisect
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
    work = [f["update_ms"] + f["draw_ms"] + f["swap_ms"] for f in chosen]
    gpu = [f["gpu_ms"] for f in chosen if f["gpu_ms"] >= 0]

    rendered = [f for f in chosen if f.get("rendered", 1)]
    print(f"{args.frames_csv.name}: {len(chosen)} updates over {span:.1f} s, "
          f"{len(chosen) / max(span, 1e-9):.0f} updates/s, "
          f"{len(rendered) / max(span, 1e-9):.0f} submitted render frames/s")
    print("update-loop time (poll may include waiting for an event/drawable)")
    print(row("total", totals))
    for phase in PHASES:
        values = [f[f"{phase}_ms"] for f in chosen]
        share = sum(values) / max(sum(totals), 1e-9)
        print(row(phase, values) + f"  ({share:5.1%})")
    print(row("update/draw/submit", work) + "  (wall time, excludes event wait)")
    print(row("gpu", gpu))
    print(row("render interval", [
        b["start_ms"] - a["start_ms"] for a, b in zip(rendered, rendered[1:])
    ]))
    # Only sampled frames are GPU-timed.
    timed = [f for f in chosen if f["gpu_ms"] >= 0]
    gpu_longer = sum(1 for f in timed if f["gpu_ms"] > f["update_ms"] + f["draw_ms"] + f["swap_ms"])
    print(f"  GPU command-buffer duration exceeded update/draw/submit wall time on "
          f"{gpu_longer / max(len(timed), 1):.1%} of {len(timed)} sampled frames")
    if rendered:
        print(f"  draw calls {statistics.fmean(f['draw_calls'] for f in rendered):.1f}, "
              f"vertices {statistics.fmean(f['vertices'] for f in rendered):.0f} per rendered frame")
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
    # A press is processed in its update. Submission is the next rendered
    # frame; this does not measure physical display or input-to-photon latency.
    submissions = [
        f["start_ms"] + sum(f[f"{p}_ms"] for p in PHASES[:4])
        for f in frames if f.get("rendered", 1)
    ]
    to_update, to_swap = [], []
    for i in inputs:
        if i["kind"] not in ("key_down", "button_down"):
            continue
        frame = by_index.get(int(i["frame"]))
        if not frame:
            continue
        event = float(i["event_ms"])
        updated = frame["start_ms"] + frame["poll_ms"] + frame["update_ms"]
        to_update.append(updated - event)
        next_submission = bisect.bisect_left(submissions, updated)
        if next_submission < len(submissions):
            to_swap.append(submissions[next_submission] - event)
    if to_update:
        print("presses")
        print(row("event → judged", to_update))
        print(row("event → submitted", to_swap))


if __name__ == "__main__":
    main()
