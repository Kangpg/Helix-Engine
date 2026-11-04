"""Windows native Release TCP loopback IOCP/RIO comparison; actual backend names are required."""
import argparse
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parent.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", default="build/windows-x64/Release/helix_echo_phase1.exe")
    parser.add_argument("--clients", type=int, default=50)
    parser.add_argument("--messages", type=int, default=500)
    parser.add_argument("--samples", type=int, default=3)
    parser.add_argument("--sizes", default="256,8192")
    parser.add_argument("--io-threads", type=int, default=4)
    parser.add_argument("--out", default="out/backend-benchmark.json")
    args = parser.parse_args()
    if os.name != "nt":
        parser.error("RIO is Windows-only")
    if min(args.clients, args.messages, args.samples, args.io_threads) < 1:
        parser.error("counts must be positive")
    sizes = [int(value) for value in args.sizes.split(",")]
    if min(sizes) < 1 or max(sizes) > 1048576:
        parser.error("sizes must be 1..1048576")
    rows = []
    directory = ROOT / "build/backend-benchmark"
    directory.mkdir(parents=True, exist_ok=True)
    environment = os.environ.copy()
    environment["PYTHONUTF8"] = "1"
    for sample in range(args.samples):
        # Alternate order to reduce systematic warm/cold bias.
        for backend in (["iocp", "iocp_rio"] if sample % 2 == 0 else ["iocp_rio", "iocp"]):
            with socket.socket() as probe:
                probe.bind(("127.0.0.1", 0)); port = probe.getsockname()[1]
            server_env = environment.copy()
            server_env.update(HELIX_NETWORK__IP="127.0.0.1", HELIX_NETWORK__PORT=str(port),
                              HELIX_NETWORK__TRANSPORT="tcp", HELIX_NETWORK__BACKEND=backend,
                              HELIX_NETWORK__IO_THREADS=str(args.io_threads), HELIX_CLUSTER__ENABLED="false")
            path = directory / f"{backend}-{sample}.log"
            with path.open("w", encoding="utf-8") as log:
                process = subprocess.Popen([str(ROOT / args.server)], cwd=ROOT, env=server_env,
                                           stdout=log, stderr=log, creationflags=subprocess.CREATE_NO_WINDOW)
                try:
                    deadline = time.monotonic() + 10
                    expected = "iocp+rio" if backend == "iocp_rio" else "iocp"
                    while f"backend={expected} " not in path.read_text(encoding="utf-8"):
                        if process.poll() is not None or time.monotonic() > deadline:
                            raise RuntimeError(f"Backend did not start: {path}")
                        time.sleep(0.05)
                    def measure(size, clients, messages):
                        result = subprocess.run([sys.executable, "tools/echo_bot.py", "echo", "--port", str(port),
                                                 "--clients", str(clients), "--messages", str(messages), "--size", str(size)],
                                                cwd=ROOT, env=environment, capture_output=True, text=True, encoding="utf-8", timeout=120)
                        if result.returncode:
                            raise RuntimeError(result.stdout + result.stderr)
                        record = next(json.loads(line[5:]) for line in result.stdout.splitlines() if line.startswith("@SUM "))
                        if not record["pass"]:
                            raise AssertionError(record)
                        return record
                    measure(256, 10, 100)
                    for size in sizes:
                        record = measure(size, args.clients, args.messages)
                        record.update(backend=expected, sample=sample, size=size)
                        rows.append(record)
                        print(f"{expected} size={size} sample={sample}: {record['mps']} msg/s, p95={record['p95']}ms")
                finally:
                    if process.poll() is None:
                        process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill(); process.wait(timeout=5)
    output = ROOT / args.out
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps({"mode": "Windows TCP loopback, Python asyncio client, includes framing/network/scheduling; not graceful-stop proof",
                                  "server": args.server, "clients": args.clients, "messages": args.messages,
                                  "io_threads": args.io_threads, "sizes": sizes, "samples": args.samples,
                                  "utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), "rows": rows}, indent=2), encoding="utf-8")


if __name__ == "__main__":
    main()
