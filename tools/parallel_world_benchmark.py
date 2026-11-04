"""Compare identical staged C++ workloads with inline and Taskflow executors."""
import argparse
import csv
import io
import json
import os
import platform
from pathlib import Path
import statistics
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, default=Path("build/windows-x64/Release/helix_parallel_world_benchmark.exe"))
    parser.add_argument("--out", type=Path, default=Path("out/parallel-world"))
    parser.add_argument("--entities", type=int, default=4096)
    parser.add_argument("--ticks", type=int, default=100)
    parser.add_argument("--warmup", type=int, default=10)
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--workers", type=int, nargs="+", default=[1, 2, 4, 8])
    parser.add_argument("--batch-size", type=int, default=128)
    parser.add_argument("--cell-size", type=float, default=4)
    parser.add_argument("--visibility-radius", type=float, default=10)
    parser.add_argument("--area-radius", type=float, default=.15)
    parser.add_argument("--max-hits", type=int, default=131072)
    parser.add_argument("--queries", type=int, default=16)
    parser.add_argument("--timeout", type=float, default=180)
    args = parser.parse_args()
    if min(args.entities, args.ticks, args.runs, args.batch_size, args.max_hits, *args.workers) <= 0 or args.warmup < 0 or args.queries < 0:
        parser.error("Counts must be positive; warmup/queries may be zero")
    args.out.mkdir(parents=True, exist_ok=True)
    config = args.out / "workload.toml"
    config.write_text(f"""[mmo]
max_entities={args.entities}
max_pending_commands={args.entities * 2}
max_commands_per_tick={args.entities * 2}
batch_size={args.batch_size}
cell_size={args.cell_size}
visibility_radius={args.visibility_radius}
max_hits_per_tick={args.max_hits}
""", encoding="utf-8")
    # Never inherit user HELIX_* overrides into a supposedly identical comparison.
    env = {k: v for k, v in os.environ.items() if not k.upper().startswith("HELIX_")}
    rows, expected = [], {}
    workers = [0] + list(dict.fromkeys(args.workers))
    for run in range(args.runs):
        order = workers[run % len(workers):] + workers[:run % len(workers)]
        for scenario in ("distributed", "dense", "boss"):
            for worker in order:
                command = [str(args.exe.resolve()), str(config.resolve()), str(args.entities), str(args.ticks),
                           str(args.warmup), str(worker), scenario, str(args.area_radius), str(args.queries)]
                process = subprocess.run(command, capture_output=True, text=True, check=True, timeout=args.timeout, env=env,
                                         creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
                data = list(csv.DictReader(io.StringIO(process.stdout)))
                if len(data) != 1:
                    raise RuntimeError(f"Unexpected benchmark output: {process.stdout}")
                row = data[0]
                invariant = tuple(row[k] for k in ("hash", "applied", "rejected", "visible"))
                if expected.setdefault(scenario, invariant) != invariant:
                    raise RuntimeError(f"Worker/run changed simulation results for {scenario}: {row}")
                row["run"] = run + 1
                rows.append(row)
                # Preserve completed runs even if a subsequent run fails.
                (args.out / f"{scenario}-{worker}-{run + 1}.csv").write_text(process.stdout, encoding="utf-8")
                print(f"{scenario} workers={worker} run={run + 1}: p99={row['p99_us']} us", flush=True)
    with (args.out / "raw.csv").open("w", newline="", encoding="utf-8") as out:
        writer = csv.DictWriter(out, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    metadata = {"platform": platform.platform(), "cpu": platform.processor(), "logical_cpus": os.cpu_count(),
                "args": {k: str(v) if isinstance(v, Path) else v for k, v in vars(args).items()},
                "environment": "Release, sequential subprocesses, HELIX_* removed", "rows": rows}
    (args.out / "results.json").write_text(json.dumps(metadata, indent=2), encoding="utf-8")
    report = ["# C++ staged world comparison", "", "Inline and Taskflow execute identical staged rules. Legacy WorldInstance has different command semantics.",
              "Tick includes planning, copies, index, graph, hit evaluation/application and publication. Input enqueue, networking, external I/O and queries are outside tick timing.",
              "AOI queries are sequential on the caller and reported separately; this is not parallel packet generation or an MMO capacity claim.", "",
              "| Scenario | Workers (0=inline) | Median run p50 us | Median run p99 us | Run p99 min–max us |",
              "|---|---:|---:|---:|---:|"]
    for scenario in ("distributed", "dense", "boss"):
        for worker in workers:
            values = [r for r in rows if r["scenario"] == scenario and int(r["workers"]) == worker]
            p50 = [float(r["p50_us"]) for r in values]
            p99 = [float(r["p99_us"]) for r in values]
            report.append(f"| {scenario} | {worker} | {statistics.median(p50):.3f} | {statistics.median(p99):.3f} | {min(p99):.3f}–{max(p99):.3f} |")
    report += ["", "All final state hashes, accepted/rejected counts and visible counts agree across workers and runs.",
               "A repeated run's percentile medians are not pooled percentiles. Inspect raw CSV for variation and rejected work."]
    (args.out / "report.md").write_text("\n".join(report) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
