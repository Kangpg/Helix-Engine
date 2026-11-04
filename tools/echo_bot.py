#!/usr/bin/env python3
# =============================================================================
# Helix 검증용 더미 봇 (echo bot)
#
# 빌드 중인 Helix 서버를 띄워둔 상태에서 두드려 검증하는 도구.
# 빌드 불필요(순수 Python, 표준 라이브러리만). 단계별 검증에 재사용한다.
#
# 프레이밍(Phase 2~): 기본은 길이 프리픽스 프레이밍(4바이트 big-endian 헤더).
#   서버 FrameCodec 기본값과 일치. --header-size 2 / --little 로 조정.
# Phase 1(raw 에코) 서버를 칠 때는 --raw 로 프레이밍 없이 바이트 대조.
#
# 모드:
#   echo  : N개 동시 연결, 각 연결이 M개 메시지 송신→에코 대조. 지연/처리량 측정.
#   churn : connect→1메시지→disconnect 반복. 세션 수명/refcount(close 레이스) 스트레스.
#   load  : 지속 부하. --duration 초 동안 send→recv 루프(오류 시 재접속하며 부하 유지).
#           --rate 로 총 msg/s 제한(0=무제한). --stats-interval 마다 "@STAT {json}"
#           실시간 통계를 출력한다(대시보드 라이브 차트용).
#
# 멀티프로세스: --workers N 이면 자기 자신을 N개 자식 프로세스로 재실행해
#   clients/rate 를 분배(파이썬 GIL 우회 → 부하 생성기 자체가 병목이 안 되게).
#   부모는 자식들의 @STAT/@SUM 을 집계해 합산 통계를 출력한다.
#
# 기계 판독용 출력(대시보드/스크립트 연동):
#   @STAT {json} : (load) 초당 실시간 통계 — t, msgs, mps, mbps, p50/p95/p99, active, 에러
#   @SUM  {json} : (모든 모드) 최종 요약 — 처리량/지연 백분위/에러/PASS 여부
#
# 사용 예:
#   python echo_bot.py --port 5000 --clients 200 --messages 50 --size 256
#   python echo_bot.py churn --port 5000 --clients 500 --rounds 20
#   python echo_bot.py load --host 10.0.0.5 --port 5000 --clients 1000 \
#       --duration 60 --workers 4                    # 원격 서버 지속 부하
#   python echo_bot.py load --clients 500 --rate 20000 --duration 30   # rate 제한
#
# 종료코드: 통과=0, 실패(불일치/타임아웃/연결오류)=1  → 스크립트/CI 연동.
# =============================================================================

import argparse
import asyncio
import json
import os
import random
import subprocess
import sys
import threading
import time

LATENCY_RESERVOIR = 100_000   # 누적 지연 샘플 상한(장시간 load 시 메모리 보호)


class Stats:
    def __init__(self):
        self.clients_ok = 0
        self.clients_failed = 0
        self.roundtrips = 0
        self.mismatches = 0
        self.timeouts = 0
        self.conn_errors = 0
        self.bytes_sent = 0
        self.active = 0            # (load) 현재 연결된 클라이언트 수
        self.latencies = []        # ms — reservoir 샘플(누적 백분위용)
        self.lat_count = 0
        self.interval_lat = []     # (load) 현재 통계 구간의 지연 샘플

    def record_latency(self, ms):
        """reservoir sampling — 표본 수 상한을 유지하며 균등 샘플."""
        self.lat_count += 1
        if len(self.latencies) < LATENCY_RESERVOIR:
            self.latencies.append(ms)
        else:
            j = random.randrange(self.lat_count)
            if j < LATENCY_RESERVOIR:
                self.latencies[j] = ms


async def read_exact(reader, n, timeout):
    """정확히 n바이트를 읽어 반환. 타임아웃/EOF 시 예외."""
    buf = bytearray()
    while len(buf) < n:
        chunk = await asyncio.wait_for(reader.read(n - len(buf)), timeout)
        if not chunk:
            raise ConnectionError("peer closed mid-message")
        buf.extend(chunk)
    return bytes(buf)


def frame(payload, args):
    """송신 바이트 구성: framed면 길이 헤더 + payload, raw면 payload 그대로."""
    if args.raw:
        return payload
    order = "little" if args.little else "big"
    return len(payload).to_bytes(args.header_size, order) + payload


async def recv_echo(reader, sent_len, args, timeout):
    """에코 응답에서 페이로드를 읽어 반환."""
    if args.raw:
        return await read_exact(reader, sent_len, timeout)
    order = "little" if args.little else "big"
    hdr = await read_exact(reader, args.header_size, timeout)
    plen = int.from_bytes(hdr, order)
    return await read_exact(reader, plen, timeout)


async def echo_client(idx, args, stats):
    try:
        reader, writer = await asyncio.wait_for(
            asyncio.open_connection(args.host, args.port), args.timeout
        )
    except (asyncio.TimeoutError, OSError):
        stats.conn_errors += 1
        stats.clients_failed += 1
        return

    ok = True
    try:
        for _ in range(args.messages):
            size = args.size if args.size > 0 else random.randint(1, 1024)
            payload = os.urandom(size)
            t0 = time.perf_counter()
            writer.write(frame(payload, args))
            await writer.drain()
            try:
                data = await recv_echo(reader, size, args, args.timeout)
            except asyncio.TimeoutError:
                stats.timeouts += 1
                ok = False
                break
            except (ConnectionError, OSError):
                stats.conn_errors += 1
                ok = False
                break
            t1 = time.perf_counter()

            stats.roundtrips += 1
            stats.bytes_sent += size
            stats.record_latency((t1 - t0) * 1000.0)
            if data != payload:
                stats.mismatches += 1
                ok = False
                break
    finally:
        try:
            writer.close()
            await asyncio.wait_for(writer.wait_closed(), args.timeout)
        except (asyncio.TimeoutError, OSError):
            pass

    stats.clients_ok += 1 if ok else 0
    stats.clients_failed += 0 if ok else 1


async def churn_client(idx, args, stats):
    """connect→1메시지 에코→즉시 close 를 rounds회 반복. 세션 수명 스트레스."""
    for _ in range(args.rounds):
        try:
            reader, writer = await asyncio.wait_for(
                asyncio.open_connection(args.host, args.port), args.timeout
            )
        except (asyncio.TimeoutError, OSError):
            stats.conn_errors += 1
            stats.clients_failed += 1
            return
        try:
            size = args.size if args.size > 0 else 16
            payload = os.urandom(size)
            writer.write(frame(payload, args))
            await writer.drain()
            data = await recv_echo(reader, size, args, args.timeout)
            stats.roundtrips += 1
            stats.bytes_sent += size
            if data != payload:
                stats.mismatches += 1
        except (asyncio.TimeoutError, ConnectionError, OSError):
            stats.conn_errors += 1
        finally:
            try:
                writer.close()
            except OSError:
                pass
    stats.clients_ok += 1


async def load_client(idx, args, stats, deadline, send_interval):
    """deadline까지 send→recv 루프. 오류 시 백오프 후 재접속하며 부하를 유지한다."""
    reader = writer = None
    backoff = 0.05
    # rate 제한 시 클라이언트별 송신 시점을 흩뿌려 버스트를 방지
    next_send = time.perf_counter() + (random.random() * send_interval if send_interval > 0 else 0.0)

    async def drop():
        nonlocal reader, writer
        if writer is not None:
            stats.active -= 1
            try:
                writer.close()
            except OSError:
                pass
        reader = writer = None

    try:
        while time.perf_counter() < deadline:
            if writer is None:
                try:
                    reader, writer = await asyncio.wait_for(
                        asyncio.open_connection(args.host, args.port), args.timeout
                    )
                    stats.active += 1
                    backoff = 0.05
                except (asyncio.TimeoutError, OSError):
                    stats.conn_errors += 1
                    await asyncio.sleep(backoff)
                    backoff = min(backoff * 2, 1.0)
                    continue

            if send_interval > 0:
                now = time.perf_counter()
                if now < next_send:
                    await asyncio.sleep(next_send - now)
                next_send += send_interval

            size = args.size if args.size > 0 else random.randint(1, 1024)
            payload = os.urandom(size)
            t0 = time.perf_counter()
            try:
                writer.write(frame(payload, args))
                await writer.drain()
                data = await recv_echo(reader, size, args, args.timeout)
            except asyncio.TimeoutError:
                stats.timeouts += 1
                await drop()
                continue
            except (ConnectionError, OSError):
                stats.conn_errors += 1
                await drop()
                continue
            t1 = time.perf_counter()

            stats.roundtrips += 1
            stats.bytes_sent += size
            ms = (t1 - t0) * 1000.0
            stats.record_latency(ms)
            stats.interval_lat.append(ms)
            if data != payload:
                stats.mismatches += 1
    finally:
        await drop()
        stats.clients_ok += 1


async def load_reporter(args, stats, t_start):
    """--stats-interval 마다 구간 통계를 '@STAT {json}' 한 줄로 출력."""
    last_rt = last_bytes = 0
    while True:
        await asyncio.sleep(args.stats_interval)
        now = time.perf_counter()
        d_rt = stats.roundtrips - last_rt
        d_bytes = stats.bytes_sent - last_bytes
        last_rt, last_bytes = stats.roundtrips, stats.bytes_sent
        lat = sorted(stats.interval_lat)
        stats.interval_lat = []
        obj = {
            "t": round(now - t_start, 1),
            "msgs": d_rt,
            "mps": round(d_rt / args.stats_interval),
            "mbps": round(d_bytes / (1024 * 1024) / args.stats_interval, 3),
            "p50": round(percentile(lat, 50), 2),
            "p95": round(percentile(lat, 95), 2),
            "p99": round(percentile(lat, 99), 2),
            "active": stats.active,
            "to": stats.timeouts, "ce": stats.conn_errors, "mm": stats.mismatches,
        }
        print("@STAT " + json.dumps(obj), flush=True)


def percentile(sorted_vals, p):
    if not sorted_vals:
        return 0.0
    k = (len(sorted_vals) - 1) * (p / 100.0)
    f = int(k)
    c = min(f + 1, len(sorted_vals) - 1)
    if f == c:
        return sorted_vals[f]
    return sorted_vals[f] + (sorted_vals[c] - sorted_vals[f]) * (k - f)


async def run(args):
    stats = Stats()

    t0 = time.perf_counter()
    if args.mode == "load":
        deadline = t0 + args.duration
        # 총 rate 를 클라이언트 수로 나눠 클라이언트별 송신 간격 산출
        send_interval = (args.clients / args.rate) if args.rate > 0 else 0.0
        tasks = [asyncio.create_task(load_client(i, args, stats, deadline, send_interval))
                 for i in range(args.clients)]
        rep = asyncio.create_task(load_reporter(args, stats, t0))
        await asyncio.gather(*tasks)
        rep.cancel()
        try:
            await rep
        except asyncio.CancelledError:
            pass
    else:
        client_fn = churn_client if args.mode == "churn" else echo_client
        tasks = [asyncio.create_task(client_fn(i, args, stats)) for i in range(args.clients)]
        await asyncio.gather(*tasks)
    elapsed = time.perf_counter() - t0

    lat = sorted(stats.latencies)
    mb = stats.bytes_sent / (1024 * 1024)
    framing = "raw" if args.raw else f"framed(h={args.header_size},{'le' if args.little else 'be'})"
    ok = (stats.mismatches == 0 and stats.timeouts == 0
          and stats.conn_errors == 0 and stats.clients_failed == 0)

    print("=" * 56)
    print(f"Helix echo bot — mode={args.mode} {framing}  target={args.host}:{args.port}")
    print("-" * 56)
    print(f"clients     : {args.clients}  (ok={stats.clients_ok}, failed={stats.clients_failed})")
    print(f"roundtrips  : {stats.roundtrips}")
    print(f"elapsed     : {elapsed:.2f}s")
    if elapsed > 0:
        print(f"throughput  : {stats.roundtrips / elapsed:,.0f} msg/s, {mb / elapsed:.2f} MB/s")
    if lat:
        print(f"latency ms  : p50={percentile(lat,50):.2f} p95={percentile(lat,95):.2f} "
              f"p99={percentile(lat,99):.2f} max={lat[-1]:.2f}")
    print("-" * 56)
    print(f"FAILURES    : mismatches={stats.mismatches}, timeouts={stats.timeouts}, "
          f"conn_errors={stats.conn_errors}")
    print("RESULT      : " + ("PASS" if ok else "FAIL"))
    print("=" * 56)

    # 기계 판독용 최종 요약(대시보드 런 비교/집계용)
    sum_obj = {
        "mode": args.mode, "target": f"{args.host}:{args.port}",
        "clients": args.clients, "ok": stats.clients_ok, "failed": stats.clients_failed,
        "roundtrips": stats.roundtrips, "bytes": stats.bytes_sent,
        "elapsed": round(elapsed, 2),
        "mps": round(stats.roundtrips / elapsed) if elapsed > 0 else 0,
        "mbps": round(mb / elapsed, 3) if elapsed > 0 else 0,
        "p50": round(percentile(lat, 50), 2), "p95": round(percentile(lat, 95), 2),
        "p99": round(percentile(lat, 99), 2), "max": round(lat[-1], 2) if lat else 0,
        "mm": stats.mismatches, "to": stats.timeouts, "ce": stats.conn_errors,
        "pass": ok,
    }
    print("@SUM " + json.dumps(sum_obj), flush=True)
    return 0 if ok else 1


# --- 멀티프로세스 워커 오케스트레이션 ----------------------------------------

def _split(total, n):
    """total 을 n개로 최대한 균등 분배."""
    base, rem = divmod(total, n)
    return [base + (1 if i < rem else 0) for i in range(n)]


def _child_cmd(args, clients, rate):
    cmd = [sys.executable, "-u", os.path.abspath(__file__), args.mode,
           "--host", args.host, "--port", str(args.port),
           "--clients", str(clients), "--messages", str(args.messages),
           "--rounds", str(args.rounds), "--size", str(args.size),
           "--timeout", str(args.timeout), "--header-size", str(args.header_size),
           "--duration", str(args.duration), "--rate", str(rate),
           "--stats-interval", str(args.stats_interval),
           "--workers", "1", "--_child"]
    if args.raw:
        cmd.append("--raw")
    if args.little:
        cmd.append("--little")
    return cmd


def _wavg(items, key, wkey):
    """items 의 key 를 wkey 가중 평균(근사 백분위 집계)."""
    tot = sum(i.get(wkey, 0) for i in items)
    if tot <= 0:
        return 0.0
    return sum(i.get(key, 0.0) * i.get(wkey, 0) for i in items) / tot


def run_workers(args):
    """--workers N: 자식 봇 N개를 띄우고 @STAT/@SUM 을 집계해 합산 출력."""
    n = args.workers
    clients_per = _split(args.clients, n)
    rate_per = ([args.rate / n] * n) if args.rate > 0 else [0] * n

    lock = threading.Lock()
    stat_buf, sums = [], []

    def reader(i, proc):
        for raw in iter(proc.stdout.readline, b""):
            line = raw.decode("utf-8", errors="replace").rstrip("\n")
            if line.startswith("@STAT "):
                try:
                    with lock:
                        stat_buf.append(json.loads(line[6:]))
                except ValueError:
                    pass
            elif line.startswith("@SUM "):
                try:
                    with lock:
                        sums.append(json.loads(line[5:]))
                except ValueError:
                    pass
            else:
                print(f"[w{i}] {line}", flush=True)
        proc.stdout.close()

    env = dict(os.environ, PYTHONUTF8="1", PYTHONIOENCODING="utf-8")
    procs, threads = [], []
    print(f"[echo_bot] workers={n}  clients={'+'.join(map(str, clients_per))}"
          + (f"  rate={args.rate:g} msg/s(총)" if args.rate > 0 else ""), flush=True)
    for i in range(n):
        p = subprocess.Popen(_child_cmd(args, clients_per[i], rate_per[i]),
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             bufsize=0, env=env)
        procs.append(p)
        t = threading.Thread(target=reader, args=(i, p), daemon=True)
        t.start()
        threads.append(t)

    t_start = time.perf_counter()
    try:
        # 집계 루프: interval 마다 도착한 워커 @STAT 을 합산해 하나의 @STAT 으로
        while any(p.poll() is None for p in procs):
            time.sleep(args.stats_interval)
            with lock:
                batch, stat_buf[:] = list(stat_buf), []
            if not batch:
                continue
            agg = {
                "t": round(time.perf_counter() - t_start, 1),
                "msgs": sum(b.get("msgs", 0) for b in batch),
                "mps": sum(b.get("mps", 0) for b in batch),
                "mbps": round(sum(b.get("mbps", 0) for b in batch), 3),
                "p50": round(_wavg(batch, "p50", "msgs"), 2),
                "p95": round(_wavg(batch, "p95", "msgs"), 2),
                "p99": round(_wavg(batch, "p99", "msgs"), 2),
                "active": sum(b.get("active", 0) for b in batch),
                "to": sum(b.get("to", 0) for b in batch),
                "ce": sum(b.get("ce", 0) for b in batch),
                "mm": sum(b.get("mm", 0) for b in batch),
                "workers": len(batch),
            }
            print("@STAT " + json.dumps(agg), flush=True)
    except KeyboardInterrupt:
        for p in procs:
            if p.poll() is None:
                p.terminate()
    for t in threads:
        t.join(timeout=5)
    rcs = [p.wait() for p in procs]

    # 최종 합산 요약
    with lock:
        final = list(sums)
    if final:
        rt = sum(s.get("roundtrips", 0) for s in final)
        by = sum(s.get("bytes", 0) for s in final)
        elapsed = max(s.get("elapsed", 0) for s in final)
        ok = all(s.get("pass") for s in final) and all(rc == 0 for rc in rcs)
        agg_sum = {
            "mode": args.mode, "target": f"{args.host}:{args.port}",
            "workers": n,
            "clients": sum(s.get("clients", 0) for s in final),
            "ok": sum(s.get("ok", 0) for s in final),
            "failed": sum(s.get("failed", 0) for s in final),
            "roundtrips": rt, "bytes": by, "elapsed": elapsed,
            "mps": round(rt / elapsed) if elapsed > 0 else 0,
            "mbps": round(by / (1024 * 1024) / elapsed, 3) if elapsed > 0 else 0,
            "p50": round(_wavg(final, "p50", "roundtrips"), 2),
            "p95": round(_wavg(final, "p95", "roundtrips"), 2),
            "p99": round(_wavg(final, "p99", "roundtrips"), 2),
            "max": round(max(s.get("max", 0) for s in final), 2),
            "mm": sum(s.get("mm", 0) for s in final),
            "to": sum(s.get("to", 0) for s in final),
            "ce": sum(s.get("ce", 0) for s in final),
            "pass": ok,
        }
        print("=" * 56)
        print(f"Helix echo bot — mode={args.mode} workers={n}  target={args.host}:{args.port} (합산)")
        print("-" * 56)
        print(f"clients     : {agg_sum['clients']}  (ok={agg_sum['ok']}, failed={agg_sum['failed']})")
        print(f"roundtrips  : {agg_sum['roundtrips']}")
        print(f"elapsed     : {elapsed:.2f}s")
        if elapsed > 0:
            print(f"throughput  : {agg_sum['mps']:,} msg/s, {agg_sum['mbps']:.2f} MB/s")
        print(f"latency ms  : p50={agg_sum['p50']:.2f} p95={agg_sum['p95']:.2f} "
              f"p99={agg_sum['p99']:.2f} max={agg_sum['max']:.2f}")
        print("-" * 56)
        print(f"FAILURES    : mismatches={agg_sum['mm']}, timeouts={agg_sum['to']}, "
              f"conn_errors={agg_sum['ce']}")
        print("RESULT      : " + ("PASS" if ok else "FAIL"))
        print("=" * 56)
        print("@SUM " + json.dumps(agg_sum), flush=True)
        return 0 if ok else 1
    return max(rcs) if rcs else 1


def parse_args():
    p = argparse.ArgumentParser(description="Helix 검증용 에코 더미 봇")
    p.add_argument("mode", nargs="?", default="echo", choices=["echo", "churn", "load"],
                   help="echo(기본) | churn(연결 수명 스트레스) | load(지속 부하)")
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=5000)
    p.add_argument("--clients", type=int, default=100, help="동시 연결 수(워커 합산)")
    p.add_argument("--messages", type=int, default=50, help="(echo) 연결당 메시지 수")
    p.add_argument("--rounds", type=int, default=20, help="(churn) 연결당 connect/disconnect 반복")
    p.add_argument("--size", type=int, default=256, help="페이로드 바이트(0=랜덤 1~1024)")
    p.add_argument("--timeout", type=float, default=5.0, help="I/O 타임아웃(초)")
    # load 모드
    p.add_argument("--duration", type=float, default=30.0, help="(load) 지속 시간(초)")
    p.add_argument("--rate", type=float, default=0.0, help="(load) 총 목표 msg/s(0=무제한)")
    p.add_argument("--stats-interval", type=float, default=1.0, help="(load) 실시간 통계 주기(초)")
    p.add_argument("--workers", type=int, default=1,
                   help="봇 프로세스 수(GIL 우회, clients/rate 를 분배)")
    p.add_argument("--_child", dest="child", action="store_true", help=argparse.SUPPRESS)
    # 프레이밍 옵션 (서버 FrameCodec과 일치시킬 것)
    p.add_argument("--raw", action="store_true", help="프레이밍 없이 raw 바이트(Phase 1 서버)")
    p.add_argument("--header-size", type=int, default=4, choices=[2, 4], help="길이 헤더 바이트")
    p.add_argument("--little", action="store_true", help="리틀엔디안 헤더(기본=big-endian)")
    return p.parse_args()


def main():
    args = parse_args()
    try:
        if args.workers > 1 and not args.child:
            rc = run_workers(args)
        else:
            rc = asyncio.run(run(args))
    except KeyboardInterrupt:
        print("\ninterrupted")
        rc = 130
    sys.exit(rc)


if __name__ == "__main__":
    main()
