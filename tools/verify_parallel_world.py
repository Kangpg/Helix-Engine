"""Run the native C++ legacy and staged world servers with the Protobuf bot."""
import argparse
import os
from pathlib import Path
import socket
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parent.parent


def verify(exe, execution, backend, workers):
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]
    env = {key: value for key, value in os.environ.items() if not key.upper().startswith("HELIX_")}
    env.update(HELIX_NETWORK__IP="127.0.0.1", HELIX_NETWORK__PORT=str(port), HELIX_NETWORK__BACKEND=backend,
               HELIX_NETWORK__TRANSPORT="tcp", HELIX_NETWORK__IO_THREADS="2", HELIX_CLUSTER__ENABLED="false",
               HELIX_PROTOCOL__SERIALIZER="protobuf", HELIX_MMO__EXECUTION=execution,
               HELIX_MMO__WORKER_THREADS=str(workers), HELIX_MMO__BATCH_SIZE="1", PYTHONUTF8="1")
    directory = ROOT / "build/verification"
    directory.mkdir(parents=True, exist_ok=True)
    flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
    path = directory / f"native-world-{execution}-{backend}.log"
    with path.open("w", encoding="utf-8") as log:
        server = subprocess.Popen([str(exe.resolve()), "config/helix.toml"], cwd=ROOT, env=env, stdout=log, stderr=log, creationflags=flags)
        try:
            deadline = time.monotonic() + 15
            while True:
                if server.poll() is not None:
                    raise RuntimeError(f"Server exited: {path.read_text(encoding='utf-8')}")
                try:
                    with socket.create_connection(("127.0.0.1", port), .2):
                        break
                except OSError:
                    if time.monotonic() >= deadline:
                        raise TimeoutError(f"Server startup timed out; see {path}")
                    time.sleep(.05)
            result = subprocess.run([sys.executable, "tools/world_bot.py", "--port", str(port)], cwd=ROOT, env=env,
                                    capture_output=True, text=True, encoding="utf-8", timeout=45, creationflags=flags)
            if result.returncode:
                raise RuntimeError(result.stdout + result.stderr)
            if server.poll() is not None:
                raise RuntimeError("Server exited during scenario")
            text = path.read_text(encoding="utf-8")
            if f"world_execution={execution}" not in text:
                raise AssertionError("Requested world mode was not activated")
            if backend != "auto" and f"backend={'iocp+rio' if backend == 'iocp_rio' else backend} " not in text:
                raise AssertionError("Requested backend was not activated")
            print(f"PASS native {execution}, {backend}: {result.stdout.strip()}", flush=True)
        finally:
            if server.poll() is None:
                server.terminate()
                try:
                    server.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    server.kill()
                    server.wait(timeout=5)
            # terminate on Windows is cleanup, not evidence of graceful shutdown.


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, default=ROOT / "build/windows-x64/Release/helix_world_server.exe")
    parser.add_argument("--backend", choices=("auto", "iocp", "iocp_rio", "epoll"), default="auto")
    parser.add_argument("--workers", type=int, default=4)
    args = parser.parse_args()
    if args.workers <= 0:
        parser.error("workers must be positive")
    subprocess.run([sys.executable, "tools/generate_protocol.py"], cwd=ROOT, check=True,
                   creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
    for execution in ("sequential", "parallel"):
        verify(args.exe, execution, args.backend, args.workers)


if __name__ == "__main__":
    main()
