"""Build outputs must exist. Verify native/managed echo, churn and MMO samples on loopback."""
import argparse
import os
from pathlib import Path
import socket
import subprocess
import sys
import time


ROOT = Path(__file__).resolve().parent.parent
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8")


def run(command):
    environment = os.environ.copy()
    environment["PYTHONUTF8"] = "1"
    result = subprocess.run(command, cwd=ROOT, env=environment, capture_output=True, text=True, encoding="utf-8", timeout=60)
    if result.returncode:
        raise RuntimeError(" ".join(map(str, command)) + "\n" + result.stdout + result.stderr)
    print(result.stdout.strip())


def verify_server(label, command, mode, backend=None, serializer="protobuf", managed_config="Release"):
    with socket.socket() as reserved:
        reserved.bind(("127.0.0.1", 0))
        port = reserved.getsockname()[1]
    environment = os.environ.copy()
    environment.update(HELIX_NETWORK__IP="127.0.0.1", HELIX_NETWORK__PORT=str(port),
                       HELIX_NETWORK__TRANSPORT="tcp", HELIX_CLUSTER__ENABLED="false",
                       HELIX_PROTOCOL__SERIALIZER=serializer)
    if backend:
        environment["HELIX_NETWORK__BACKEND"] = backend
    output_dir = ROOT / "build" / "verification"
    output_dir.mkdir(parents=True, exist_ok=True)
    flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
    with (output_dir / (label + ".log")).open("w", encoding="utf-8") as log:
        process = subprocess.Popen(command, cwd=ROOT, env=environment, stdout=log, stderr=log, creationflags=flags)
        try:
            deadline = time.monotonic() + 10
            while True:
                if process.poll() is not None:
                    raise RuntimeError(f"{label} exited before accepting; see {log.name}")
                try:
                    with socket.create_connection(("127.0.0.1", port), 0.2):
                        break
                except OSError:
                    if time.monotonic() >= deadline:
                        raise TimeoutError(f"{label} did not start; see {log.name}")
                    time.sleep(0.05)
            if mode == "echo":
                run([sys.executable, "tools/echo_bot.py", "echo", "--port", str(port), "--clients", "20", "--messages", "50"])
                run([sys.executable, "tools/echo_bot.py", "churn", "--port", str(port), "--clients", "20", "--rounds", "10"])
            else:
                if serializer == "protobuf":
                    run([sys.executable, "tools/world_bot.py", "--port", str(port)])
                run(["dotnet", str(ROOT / "csharp/tests/Helix.Tests/bin" / managed_config / "net10.0/Helix.Tests.dll"),
                     "--world-client", "127.0.0.1", str(port), serializer])
            if process.poll() is not None:
                raise RuntimeError(f"{label} unexpectedly exited; see {log.name}")
            if backend in ("iocp", "iocp_rio", "epoll"):
                expected = "iocp+rio" if backend == "iocp_rio" else backend
                if f"backend={expected} " not in Path(log.name).read_text(encoding="utf-8"):
                    raise AssertionError(f"{label} did not report requested backend {expected}")
            print(f"PASS: {label}")
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native-dir", default="build/windows-x64/Debug")
    parser.add_argument("--managed-config", default="Release")
    parser.add_argument("--protoc", help="Matching protoc for the shared Python test client")
    parser.add_argument("--native-backend", choices=["auto", "iocp", "iocp_rio", "epoll"], default="auto")
    args = parser.parse_args()
    native = ROOT / args.native_dir
    suffix = ".exe" if os.name == "nt" else ""
    managed = ROOT / "csharp" / "samples"
    config = args.managed_config
    generate = [sys.executable, "tools/generate_protocol.py"]
    if args.protoc:
        generate.extend(["--protoc", args.protoc])
    run(generate)
    run(["dotnet", str(ROOT / "csharp/tests/Helix.Tests/bin" / config / "net10.0/Helix.Tests.dll")])
    codec_client = ["dotnet", str(ROOT / "csharp/tests/Helix.Tests/bin" / config / "net10.0/Helix.Tests.dll")]
    native_codec = str(native / ("helix_serializer_test" + suffix))
    for serializer in ("protobuf", "flatbuffers"):
        for origin in ("cpp", "csharp"):
            directory = str(ROOT / "build/verification/fixtures" / serializer / origin)
            writer, reader = ([native_codec], codec_client) if origin == "cpp" else (codec_client, [native_codec])
            run(writer + ["--write", directory, serializer])
            run(reader + ["--verify", directory, serializer])
    verify_server("cpp-echo", [str(native / ("helix_echo_phase1" + suffix))], "echo", args.native_backend)
    verify_server("csharp-echo", ["dotnet", str(managed / "Helix.Echo/bin" / config / "net10.0/Helix.Echo.dll")], "echo")
    verify_server("cpp-world", [str(native / ("helix_world_server" + suffix))], "world", args.native_backend, managed_config=config)
    verify_server("csharp-world", ["dotnet", str(managed / "Helix.WorldServer/bin" / config / "net10.0/Helix.WorldServer.dll")], "world", managed_config=config)
    verify_server("cpp-world-flatbuffers", [str(native / ("helix_world_server" + suffix))], "world", args.native_backend, "flatbuffers", config)
    for serializer in ("flatbuffers", "memorypack"):
        verify_server("csharp-world-" + serializer, ["dotnet", str(managed / "Helix.WorldServer/bin" / config / "net10.0/Helix.WorldServer.dll")], "world", serializer=serializer, managed_config=config)


if __name__ == "__main__":
    main()
