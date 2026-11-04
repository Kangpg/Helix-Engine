"""Generate Python test messages. Install tools/requirements-protobuf.txt into build/protobuf-python."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parent.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--protoc", help="Matching protoc, or HELIX_PROTOC environment variable")
    parser.add_argument("--output", default="build/protobuf-python")
    args = parser.parse_args()
    compiler = args.protoc or os.environ.get("HELIX_PROTOC")
    if not compiler:
        name = "protoc.exe" if os.name == "nt" else "protoc"
        candidates = [ROOT / "build/windows-x64/_deps/helix_protoc-src/bin" / name,
                      ROOT / "build/_deps/helix_protoc-src/bin" / name]
        compiler = next((str(path) for path in candidates if path.is_file()), None) or shutil.which("protoc")
    if not compiler:
        parser.error("Build C++ protocol first, or pass --protoc /path/to/protoc")
    version = subprocess.check_output([compiler, "--version"], text=True).strip()
    if version != "libprotoc 36.2":
        parser.error(f"Expected protoc 36.2, got {version}")
    output = ROOT / args.output
    output.mkdir(parents=True, exist_ok=True)
    subprocess.run([compiler, "--proto_path=" + str(ROOT / "protos"),
                    "--python_out=" + str(output), str(ROOT / "protos/helix_mmo.proto")], check=True)
    print(f"Generated Python Protobuf messages in {output}")


if __name__ == "__main__":
    main()
