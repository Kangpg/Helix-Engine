#!/usr/bin/env python3
# =============================================================================
# Helix 벤치마크 자동화 — echo_bot.py 를 설정 매트릭스로 스윕하고 리포트 생성.
#
# 전제: Helix 에코 서버가 이미 떠 있어야 한다(이 스크립트는 부하만 건다).
#   .\build\windows-x64\Debug\helix_echo_phase1.exe     # 또는 release
#
# 사용:
#   python tools\benchmark.py --port 5000
#   python tools\benchmark.py --port 5000 --out bench --label tcp-debug
#   # 전송 비교: 서버를 tcp/kcp로 각각 띄우고 --label 만 바꿔 두 번 돌린 뒤 리포트 비교
#
# 산출물: <out>.csv + <out>.md  (기본 benchmark_report.csv/.md)
# 종료코드: 모든 케이스 PASS=0, 하나라도 FAIL=1
# =============================================================================

import argparse
import os
import re
import subprocess
import sys
from datetime import datetime, timezone

# (clients, messages, size) 매트릭스 — 필요시 --matrix 로 교체.
DEFAULT_MATRIX = [
    (50, 200, 64),
    (200, 200, 256),
    (500, 100, 256),
    (500, 200, 1024),
    (1000, 50, 256),
]

RE_THROUGHPUT = re.compile(r"throughput\s*:\s*([\d,]+)\s*msg/s,\s*([\d.]+)\s*MB/s")
RE_LATENCY = re.compile(r"latency ms\s*:\s*p50=([\d.]+)\s*p95=([\d.]+)\s*p99=([\d.]+)\s*max=([\d.]+)")
RE_RESULT = re.compile(r"RESULT\s*:\s*(PASS|FAIL)")


def run_case(args, clients, messages, size):
    bot = os.path.join(os.path.dirname(os.path.abspath(__file__)), "echo_bot.py")
    cmd = [
        sys.executable, bot,
        "--host", args.host, "--port", str(args.port),
        "--clients", str(clients), "--messages", str(messages), "--size", str(size),
        "--timeout", str(args.timeout),
    ]
    if args.header_size != 4:
        cmd += ["--header-size", str(args.header_size)]
    if args.little:
        cmd += ["--little"]

    out = subprocess.run(cmd, capture_output=True, text=True).stdout

    row = {
        "clients": clients, "messages": messages, "size": size,
        "msg_per_s": "", "mb_per_s": "",
        "p50": "", "p95": "", "p99": "", "max": "",
        "result": "FAIL",
    }
    m = RE_THROUGHPUT.search(out)
    if m:
        row["msg_per_s"] = m.group(1).replace(",", "")
        row["mb_per_s"] = m.group(2)
    m = RE_LATENCY.search(out)
    if m:
        row["p50"], row["p95"], row["p99"], row["max"] = m.groups()
    m = RE_RESULT.search(out)
    if m:
        row["result"] = m.group(1)
    return row


def write_reports(args, rows):
    cols = ["clients", "messages", "size", "msg_per_s", "mb_per_s", "p50", "p95", "p99", "max", "result"]

    csv_path = args.out + ".csv"
    with open(csv_path, "w", encoding="utf-8") as f:
        f.write(",".join(cols) + "\n")
        for r in rows:
            f.write(",".join(str(r[c]) for c in cols) + "\n")

    md_path = args.out + ".md"
    ts = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M UTC")
    with open(md_path, "w", encoding="utf-8") as f:
        f.write(f"# Helix 벤치마크 리포트\n\n")
        f.write(f"- 라벨: **{args.label}**  ·  대상: {args.host}:{args.port}  ·  {ts}\n")
        f.write(f"- 헤더: p50/p95/p99/max 단위 ms, 처리량 msg/s · MB/s\n\n")
        f.write("| clients | messages | size | msg/s | MB/s | p50 | p95 | p99 | max | result |\n")
        f.write("|---:|---:|---:|---:|---:|---:|---:|---:|---:|:--:|\n")
        for r in rows:
            f.write("| {clients} | {messages} | {size} | {msg_per_s} | {mb_per_s} | "
                    "{p50} | {p95} | {p99} | {max} | {result} |\n".format(**r))
    return csv_path, md_path


def main():
    p = argparse.ArgumentParser(description="Helix 벤치마크 자동화(echo_bot 스윕)")
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=5000)
    p.add_argument("--timeout", type=float, default=10.0)
    p.add_argument("--out", default="benchmark_report", help="리포트 파일 접두어(.csv/.md)")
    p.add_argument("--label", default="run", help="리포트 라벨(예: tcp-release)")
    p.add_argument("--header-size", type=int, default=4, choices=[2, 4])
    p.add_argument("--little", action="store_true")
    p.add_argument("--matrix", default="", help='커스텀 매트릭스 "c,m,s; c,m,s" 형식')
    args = p.parse_args()

    matrix = DEFAULT_MATRIX
    if args.matrix:
        matrix = []
        for grp in args.matrix.split(";"):
            grp = grp.strip()
            if not grp:
                continue
            c, m, s = (int(x) for x in grp.split(","))
            matrix.append((c, m, s))

    print(f"== Helix benchmark ({args.label}) → {args.host}:{args.port} ==")
    rows = []
    for (c, m, s) in matrix:
        print(f"  case clients={c} messages={m} size={s} ...", flush=True)
        row = run_case(args, c, m, s)
        rows.append(row)
        print(f"    {row['result']}  {row['msg_per_s']} msg/s  {row['mb_per_s']} MB/s  "
              f"p99={row['p99']}ms")

    csv_path, md_path = write_reports(args, rows)
    all_pass = all(r["result"] == "PASS" for r in rows)
    print(f"\n리포트: {csv_path} , {md_path}")
    print("RESULT: " + ("PASS" if all_pass else "FAIL"))
    return 0 if all_pass else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\ninterrupted")
        sys.exit(130)
