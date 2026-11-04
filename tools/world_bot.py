"""Generated Protobuf client verifies both C++/C# MMO servers."""
import argparse
from pathlib import Path
import socket
import struct
import sys
import time

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "build/protobuf-python"))
try:
    import helix_mmo_pb2 as wire
except ImportError as error:
    raise SystemExit("Install tools/requirements-protobuf.txt into build/protobuf-python and run tools/generate_protocol.py") from error


def exact(sock, count):
    result = bytearray()
    while len(result) < count:
        chunk = sock.recv(count - len(result))
        if not chunk:
            raise RuntimeError("unexpected disconnect")
        result.extend(chunk)
    return bytes(result)


def receive(sock):
    size = struct.unpack("!I", exact(sock, 4))[0]
    if size > 65536:
        raise RuntimeError("unexpected frame size")
    packet = wire.ServerPacket.FromString(exact(sock, size))
    assert packet.protocol_version == 1, packet
    return packet


def raw_request(sock, data):
    sock.sendall(struct.pack("!I", len(data)) + data)
    return receive(sock)


def request(sock, **body):
    packet = wire.ClientPacket(protocol_version=1, request_id=123, **body)
    reply = raw_request(sock, packet.SerializeToString())
    assert reply.request_id == packet.request_id, reply
    return reply


def wait_for(sock, predicate, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = request(sock, look=wire.LookCommand())
        if value.HasField("state") and predicate(value.state):
            return value.state
        time.sleep(0.01)
    raise AssertionError("world state did not reach expected result")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=5000)
    parser.add_argument("--timeout", type=float, default=5)
    args = parser.parse_args()
    sockets = []
    try:
        groups = {1: [], 2: []}
        for _ in range(4):
            sock = socket.create_connection((args.host, args.port), args.timeout)
            sockets.append(sock)
            hello = receive(sock)
            assert hello.HasField("welcome"), hello
            groups[hello.welcome.world_id].append((sock, hello.welcome.entity_id))
        assert all(len(group) == 2 for group in groups.values()), groups
        for group in groups.values():
            for sock, _ in group:
                wait_for(sock, lambda state: True, args.timeout)
        field, arena = groups[1], groups[2]
        assert request(field[0][0], move=wire.MoveCommand(destination=wire.Position(x=3, y=4, z=5))).reply.status == wire.REPLY_STATUS_QUEUED
        wait_for(field[0][0], lambda state: (state.position.x, state.position.y, state.position.z) == (3, 4, 5), args.timeout)
        assert request(arena[0][0], attack=wire.AttackCommand(target_entity_id=arena[1][1])).reply.status == wire.REPLY_STATUS_QUEUED
        wait_for(arena[1][0], lambda state: state.health == 90, args.timeout)
        assert request(field[0][0], attack=wire.AttackCommand(target_entity_id=field[1][1])).reply.status == wire.REPLY_STATUS_QUEUED
        # A later applied command proves the preceding rejected attack was processed.
        assert request(field[1][0], move=wire.MoveCommand(destination=wire.Position(x=1, y=2, z=3))).reply.status == wire.REPLY_STATUS_QUEUED
        state = wait_for(field[1][0], lambda state: state.position.x == 1, args.timeout)
        assert state.health == 100, state
        for group in groups.values():
            for sock, sid in group:
                visible = request(sock, look=wire.LookCommand()).state.visible_entity_ids
                assert set(visible) == {peer for _, peer in group if peer != sid}, visible
        sock = field[0][0]
        malformed = raw_request(sock, b"\x52\x05\x08")
        assert malformed.reply.status == wire.REPLY_STATUS_INVALID and malformed.request_id == 0
        invalid = [wire.ClientPacket(protocol_version=2, request_id=45, look=wire.LookCommand()),
                   wire.ClientPacket(protocol_version=1, request_id=45),
                   wire.ClientPacket(protocol_version=1, request_id=45, attack=wire.AttackCommand()),
                   wire.ClientPacket(protocol_version=1, request_id=45, move=wire.MoveCommand()),
                   wire.ClientPacket(protocol_version=1, request_id=45, move=wire.MoveCommand(destination=wire.Position(x=float("nan"))))]
        for packet in invalid:
            reply = raw_request(sock, packet.SerializeToString())
            assert reply.reply.status == wire.REPLY_STATUS_INVALID and reply.request_id == 45, reply
        compatible = wire.ClientPacket(protocol_version=1, request_id=999, look=wire.LookCommand()).SerializeToString() + b"\xa0\x06\x01"
        reply = raw_request(sock, compatible)
        assert reply.HasField("state") and reply.request_id == 999, reply
        print("PASS: Protobuf C++/C# parity; movement, field rejection, arena damage, AOI; malformed/version/finite validation, unknown fields, request IDs")
    finally:
        for sock in sockets:
            sock.close()


if __name__ == "__main__":
    main()
