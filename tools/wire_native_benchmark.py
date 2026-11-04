"""Run actual C++ codecs sequentially; separate timed and operator-new allocation executables."""
import argparse
import csv
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import platform
import subprocess

ROOT = Path(__file__).resolve().parent.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native-dir', default='build/windows-x64/Release')
    parser.add_argument('--iterations', type=int, default=10000)
    parser.add_argument('--samples', type=int, default=9)
    parser.add_argument('--runs', type=int, default=3)
    parser.add_argument('--allocation-iterations', type=int, default=1000)
    parser.add_argument('--out', default='out/wire-native.json')
    args = parser.parse_args()
    if args.runs < 2 or args.iterations < 1 or args.allocation_iterations < 1 or args.samples < 3 or args.samples % 2 == 0:
        raise ValueError('runs >=2, positive operations, odd samples >=3 required')
    suffix = '.exe' if os.name == 'nt' else ''
    output = ROOT / args.out
    directory = output.parent / 'wire-native-runs'
    directory.mkdir(parents=True, exist_ok=True)
    records = []
    for index in range(args.runs):
        files = {}
        logs = {}
        for mode, executable, operations in [('time', 'helix_wire_benchmark', args.iterations), ('allocation', 'helix_wire_allocation_probe', args.allocation_iterations)]:
            file = directory / f'run-{index}-{mode}.csv'
            command = [str(ROOT / args.native_dir / (executable + suffix)), str(operations), str(args.samples), str(file), str(index)]
            result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=600,
                                    creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
            if result.returncode:
                raise RuntimeError(result.stdout + result.stderr)
            print(result.stdout.strip(), flush=True)
            files[mode] = list(csv.DictReader(file.open(encoding='utf-8')))
            logs[mode] = result.stdout.strip()
            if len(files[mode]) != 36:
                raise ValueError('Native run did not contain all 18 cases x 2 operations')
        records.append({'Index': index, 'Utc': datetime.now(timezone.utc).isoformat(), 'TimeRows': files['time'], 'AllocationRows': files['allocation'], 'Logs': logs})
    output.write_text(json.dumps({'Contract': 'production codec + 4-byte TCP frame + complete DTO + semantic validation; socket I/O excluded',
                                  'Mode': 'MSVC Release uninstrumented time; separate operator-new probe',
                                  'Platform': platform.platform(), 'Architecture': platform.machine(), 'Processor': platform.processor(),
                                  'Utc': datetime.now(timezone.utc).isoformat(), 'WarmupOperations': 2000, 'Iterations': args.iterations,
                                  'Samples': args.samples, 'RunCount': args.runs, 'AllocationIterations': args.allocation_iterations,
                                  'AllocationMetric': 'requested operator-new/new[] (including aligned/nothrow) bytes and calls; direct malloc/system allocations excluded',
                                  'Runs': records}, indent=2), encoding='utf-8')
    print(f'Raw native wire result: {output}')


if __name__ == '__main__':
    main()
