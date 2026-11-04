#!/usr/bin/env python3
# =============================================================================
# Helix scale-out(Phase 6) 크로스노드 라우팅 검증 봇
#
# helix_route_phase6 서버를 (보통) 두 노드로 띄우고 공용 Redis를 물린 뒤,
# 한 노드의 클라이언트가 다른 노드의 클라이언트로 메시지를 보내는 라우팅을 검증한다.
#
# 사전 준비:
#   1) Redis 기동 (기본 127.0.0.1:6379)
#   2) 노드0:  HELIX_CLUSTER__ENABLED=true HELIX_CLUSTER__NODE_ID=0 HELIX_NETWORK__PORT=5000 helix_route_phase6
#   3) 노드1:  HELIX_CLUSTER__ENABLED=true HELIX_CLUSTER__NODE_ID=1 HELIX_NETWORK__PORT=5001 helix_route_phase6
#   4) python tools/cluster_route_bot.py --ports 5000 5001
#
# 프로토콜(route_phase6 샘플과 일치, 4B big-endian 프레이밍):
#   - 접속 시 서버 → 클라: 8B big-endian 전역 SessionId
#   - 클라 → 서버: [8B big-endian 대상 SessionId][payload]  → 서버가 대상에 전달
#
# 종료코드: 통과=0, 실패=1
# =============================================================================

import argparse
import os
import socket
import sys


def recv_exact(sock, n, timeout):
    sock.settimeout(timeout)
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("peer closed mid-message")
        buf.extend(chunk)
    return bytes(buf)


def read_frame(sock, header_size, big, timeout):
    order = "big" if big else "little"
    hdr = recv_exact(sock, header_size, timeout)
    plen = int.from_bytes(hdr, order)
    return recv_exact(sock, plen, timeout)


def send_frame(sock, payload, header_size, big):
    order = "big" if big else "little"
    sock.sendall(len(payload).to_bytes(header_size, order) + payload)


def connect(host, port, timeout):
    s = socket.create_connection((host, port), timeout)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    return s


def read_id(sock, args):
    """접속 직후 서버가 보내는 8B 전역 SessionId 프레임을 읽어 정수로 반환."""
    payload = read_frame(sock, args.header_size, not args.little, args.timeout)
    if len(payload) != 8:
        raise ValueError(f"expected 8-byte id frame, got {len(payload)} bytes")
    return int.from_bytes(payload, "big" if not args.little else "little")


def route_msg(target_sid, payload):
    return target_sid.to_bytes(8, "big") + payload


class Result:
    def __init__(self):
        self.passed = 0
        self.failed = 0
        self.notes = []

    def check(self, name, ok, detail=""):
        if ok:
            self.passed += 1
            self.notes.append(f"  PASS  {name}")
        else:
            self.failed += 1
            self.notes.append(f"  FAIL  {name}  {detail}")


def one_direction(res, name, sender, sender_id, receiver, receiver_id, args):
    """sender → receiver_id 로 count회 라우팅 후 receiver 수신 일치 검증."""
    big = not args.little
    ok_all = True
    for _ in range(args.count):
        payload = os.urandom(args.size)
        send_frame(sender, route_msg(receiver_id, payload), args.header_size, big)
        try:
            got = read_frame(receiver, args.header_size, big, args.timeout)
        except (socket.timeout, ConnectionError, OSError) as e:
            res.check(name, False, f"recv error: {e}")
            return
        if got != payload:
            ok_all = False
            break
    res.check(name, ok_all, "payload mismatch" if not ok_all else "")


def main():
    ap = argparse.ArgumentParser(description="Helix scale-out 크로스노드 라우팅 검증 봇")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--ports", type=int, nargs="+", default=[5000, 5001],
                    help="노드 포트들. 1개면 같은 노드 라우팅만, 2개+면 노드 간 라우팅 검증")
    ap.add_argument("--count", type=int, default=20, help="방향당 라우팅 메시지 수")
    ap.add_argument("--size", type=int, default=128, help="페이로드 바이트")
    ap.add_argument("--timeout", type=float, default=5.0)
    ap.add_argument("--header-size", type=int, default=4, choices=[2, 4])
    ap.add_argument("--little", action="store_true", help="리틀엔디안 헤더(기본=big)")
    args = ap.parse_args()

    res = Result()
    socks = []
    try:
        port0 = args.ports[0]
        a = connect(args.host, port0, args.timeout)
        socks.append(a)
        sid_a = read_id(a, args)

        # --- 같은 노드 라우팅 (node0 내 A → A2) ---
        a2 = connect(args.host, port0, args.timeout)
        socks.append(a2)
        sid_a2 = read_id(a2, args)
        one_direction(res, f"same-node  A(sid={sid_a}) -> A2(sid={sid_a2})",
                      a, sid_a, a2, sid_a2, args)

        # --- 노드 간 라우팅 (포트 2개 이상일 때) ---
        if len(args.ports) >= 2:
            port1 = args.ports[1]
            b = connect(args.host, port1, args.timeout)
            socks.append(b)
            sid_b = read_id(b, args)

            node_a = sid_a >> 40
            node_b = sid_b >> 40
            res.check(f"distinct-node-ids (A.node={node_a}, B.node={node_b})",
                      node_a != node_b,
                      "두 서버의 node_id가 같음 — HELIX_CLUSTER__NODE_ID를 다르게 띄울 것")

            one_direction(res, f"cross-node A(node{node_a}) -> B(node{node_b})",
                          a, sid_a, b, sid_b, args)
            one_direction(res, f"cross-node B(node{node_b}) -> A(node{node_a})",
                          b, sid_b, a, sid_a, args)
        else:
            res.notes.append("  SKIP  cross-node (포트 1개만 지정됨)")
    except (socket.timeout, ConnectionError, OSError, ValueError) as e:
        res.check("setup", False, f"connection/setup error: {e}")
    finally:
        for s in socks:
            try:
                s.close()
            except OSError:
                pass

    print("=" * 60)
    print(f"Helix cluster route bot  ports={args.ports}  count={args.count}")
    print("-" * 60)
    print("\n".join(res.notes))
    print("-" * 60)
    ok = res.failed == 0 and res.passed > 0
    print(f"RESULT      : {'PASS' if ok else 'FAIL'}  (pass={res.passed}, fail={res.failed})")
    print("=" * 60)
    return 0 if ok else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\ninterrupted")
        sys.exit(130)
