#!/usr/bin/env python3
"""Compare OpenGL batching and upload reuse with unchanged presentation.

The baseline executable must use main's renderer with the same benchmark
automatic intro skip and --benchmark-seconds support. Each process is limited to
30 seconds. Summaries use seconds 2..22 of the profile, so startup, warmup and
small differences in shutdown time do not change the measured replay section.
"""

import argparse
import csv
import hashlib
import json
from pathlib import Path
import random
import shutil
import statistics
import subprocess
import time


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def summarize(path):
    with path.open() as file:
        frames = [
            {key: float(value) for key, value in row.items()}
            for row in csv.DictReader(file)
        ]
    if frames[-1]['start_ms'] < 22000:
        raise RuntimeError('Recording did not cover the complete measurement window')
    chosen = [f for f in frames if 2000 <= f['start_ms'] < 22000 and f['screen'] == 2]
    if len(chosen) < 2:
        raise RuntimeError('No gameplay in measurement window')
    phases = ['poll_ms', 'update_ms', 'draw_ms', 'swap_ms', 'sleep_ms']
    totals = sorted(sum(f[p] for p in phases) for f in chosen)
    gpu = sorted(f['gpu_ms'] for f in chosen if f['gpu_ms'] >= 0)
    span = chosen[-1]['start_ms'] - chosen[0]['start_ms']
    inputs = path.with_name(path.name.replace('-frames', '-input'))
    interactions = 0
    if inputs.exists():
        with inputs.open() as file:
            interactions = sum(
                row['kind'] in ('key_down', 'button_down')
                and 0 <= float(row['event_ms']) < 22000
                for row in csv.DictReader(file)
            )
    return {
        'interactions': interactions,
        'frames': len(chosen),
        'fps': (len(chosen) - 1) * 1000 / span,
        'mean_ms': statistics.fmean(totals),
        'p99_ms': totals[int((len(totals) - 1) * .99)],
        'max_ms': max(totals),
        'phase_mean_ms': {p: statistics.fmean(f[p] for f in chosen) for p in phases},
        'gpu_mean_ms': statistics.fmean(gpu) if gpu else None,
        'gpu_p99_ms': gpu[int((len(gpu) - 1) * .99)] if gpu else None,
        'draw_calls': statistics.fmean(f['draw_calls'] for f in chosen),
        'vertices': statistics.fmean(f['vertices'] for f in chosen),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path, required=True)
    parser.add_argument('--experiment', type=Path, required=True)
    parser.add_argument('--replay', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--rounds', type=int, default=3)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    baseline, experiment, replay = [p.resolve() for p in (args.baseline, args.experiment, args.replay)]
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    work = output / 'run'
    work.mkdir()
    for name in ['beatmaps', 'skins', 'replays', 'shaders']:
        (work / name).symlink_to(root / name, target_is_directory=True)
    shutil.copyfile(root / 'settings.txt', work / 'settings.txt')
    variants = {
        'main': (baseline, []),
        'corrected': (experiment, []),
        'batching': (experiment, ['--batch-sliders']),
        'uploads': (experiment, ['--reuse-uploads']),
        'both': (experiment, ['--batch-sliders', '--reuse-uploads']),
    }
    result = {
        'source_commit': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip(),
        'measurement_window_ms': [2000, 22000],
        'process_timeout_seconds': 30,
        'automatic_intro_skip': True,
        'baseline_sha256': digest(baseline),
        'experiment_sha256': digest(experiment),
        'replay': str(replay), 'replay_sha256': digest(replay),
        'settings': (work / 'settings.txt').read_text(),
        'runs': [],
    }
    (output / 'source.diff').write_bytes(subprocess.check_output(['git', 'diff', 'HEAD', '--', 'CMakeLists.txt', 'src', 'tests', 'tools'], cwd=root))
    rng = random.Random(41)
    order = list(variants)
    for round_index in range(args.rounds):
        if round_index == 1:
            order.reverse()
        elif round_index > 1:
            rng.shuffle(order)
        for variant in order:
            executable, flags = variants[variant]
            command = [str(executable), '--benchmark', str(replay), '--size', '1280x720',
                       '--uncapped', '--skip-intro', '--benchmark-seconds', '28', *flags]
            print(f'round {round_index + 1}: {variant}', flush=True)
            started = time.monotonic()
            process = subprocess.Popen(command, cwd=work, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            try:
                log, _ = process.communicate(timeout=30)
            except subprocess.TimeoutExpired:
                process.kill()
                log, _ = process.communicate()
                raise RuntimeError(f'{variant} exceeded the 30-second process limit: {log}')
            except BaseException:
                process.kill()
                process.communicate()
                raise
            elapsed = time.monotonic() - started
            name = f'{round_index + 1}-{variant}'
            (output / f'{name}.log').write_text(log)
            if process.returncode:
                raise RuntimeError(f'{variant} failed: {log}')
            profile = next((line.removeprefix('profile: ') for line in log.splitlines() if line.startswith('profile: ')), None)
            if not profile:
                raise RuntimeError(f'{variant} produced no profile: {log}')
            source = work / profile
            target = output / f'{name}-frames.csv'
            shutil.move(source, target)
            input_source = source.with_name(source.name.replace('-frames', '-input'))
            if input_source.exists():
                shutil.move(input_source, output / f'{name}-input.csv')
            summary = summarize(target)
            result['runs'].append({'round': round_index + 1, 'variant': variant,
                                   'wall_seconds': elapsed, 'command': command, **summary})
            (output / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
            print(f"  {summary['fps']:.0f} FPS, p99 {summary['p99_ms']:.3f} ms, {elapsed:.1f}s process", flush=True)
            if summary['interactions']:
                print('  Excluded from comparison: keyboard or mouse interaction before the window ended.', flush=True)
    for variant in variants:
        runs = [r for r in result['runs'] if r['variant'] == variant and not r['interactions']]
        if not runs:
            print(f'{variant}: no uninterrupted samples')
            continue
        print(f"{variant}: median {statistics.median(r['fps'] for r in runs):.0f} FPS "
              f"[{min(r['fps'] for r in runs):.0f}..{max(r['fps'] for r in runs):.0f}]")


if __name__ == '__main__':
    main()
