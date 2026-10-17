#!/usr/bin/env python3
"""SSSC v1 板端协议负向测试（主机运行，无需改动板端）。

覆盖 docs/PLAN.md §5.7 的边界规则：畸形头、非法 HELLO、未知消息、
tile 越界/长度不符、保留字段、心跳超时等。每个用例独立建连。
"""

import argparse
import socket
import struct
import sys
import time

try:
    from lz4 import block as lz4
except ImportError:
    lz4 = None

DEFAULT_HOST = "192.168.1.14"
DEFAULT_PORT = 5901

MAGIC = 0x43535353
VERSION = 1

HELLO = 0x0001
HELLO_ACK = 0x0002
TILES = 0x0010
FULL_BEGIN = 0x0011
FULL_END = 0x0012
SET_QUALITY = 0x0030
PING = 0x0040
PONG = 0x0041

HOST = DEFAULT_HOST
PORT = DEFAULT_PORT
results = []


class SkipTest(Exception):
    pass


def recv_exact(sock, size):
    data = bytearray()
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise EOFError("peer closed")
        data.extend(chunk)
    return bytes(data)


def connect(retries=20):
    last = None
    for _ in range(retries):
        try:
            return socket.create_connection((HOST, PORT), timeout=3)
        except OSError as exc:
            last = exc
            time.sleep(0.2)
    raise last


def send_msg(sock, mtype, payload=b""):
    sock.sendall(struct.pack("<IHHI", MAGIC, VERSION, mtype, len(payload)) +
                 payload)


def read_msg(sock, timeout=2.0):
    sock.settimeout(timeout)
    hdr = recv_exact(sock, 12)
    magic, version, mtype, length = struct.unpack("<IHHI", hdr)
    if magic != MAGIC or version != VERSION:
        raise ValueError("bad reply header")
    payload = recv_exact(sock, length)
    return mtype, payload


def hello(sock, width=1024, height=600, fmt=0, fps=20, flags=0):
    send_msg(sock, HELLO,
             struct.pack("<HHBBI", width, height, fmt, fps, flags))
    mtype, payload = read_msg(sock)
    if mtype != HELLO_ACK:
        raise ValueError("expected HELLO_ACK, got 0x%04x" % mtype)
    return struct.unpack("<HHBBHH", payload[:10])


def expect_closed(sock, timeout=2.5):
    try:
        sock.settimeout(timeout)
        data = sock.recv(1024)
        return len(data) == 0
    except socket.timeout:
        return False
    except OSError:
        return True


def is_alive(sock):
    stamp = struct.pack("<I", 0x12345678)
    send_msg(sock, PING, stamp)
    try:
        mtype, payload = read_msg(sock, 1.5)
    except (OSError, EOFError, ValueError):
        return False
    return mtype == PONG and payload[:4] == stamp


def tile_hdr(x, y, w, h, codec, data_len, reserved=0):
    return struct.pack("<HHHHBBI", x, y, w, h, codec, reserved, data_len)


def tiles_payload(seq, entries, reserved=0):
    body = struct.pack("<IHH", seq, len(entries), reserved)
    for entry in entries:
        body += entry
    return body


def run(name, fn):
    try:
        fn()
    except SkipTest as exc:
        results.append((name, "SKIP", str(exc)))
        print("SKIP %-32s %s" % (name, exc), flush=True)
    except Exception as exc:
        results.append((name, "FAIL", "%s: %s" % (type(exc).__name__, exc)))
        print("FAIL %-32s %s" % (name, exc), flush=True)
    else:
        results.append((name, "PASS", ""))
        print("PASS %-32s" % name, flush=True)


def test_heartbeat_timeout():
    sock = connect()
    hello(sock)
    start = time.monotonic()
    closed = expect_closed(sock, 20)
    elapsed = time.monotonic() - start
    sock.close()
    assert closed, "board did not close after heartbeat timeout"
    assert 14 <= elapsed <= 18, "timeout %.2fs not near 15s" % elapsed


def test_invalid_mode():
    sock = connect()
    _, _, _, status, _, _ = hello(sock, width=1920, height=1080)
    assert status == 1, "status=%d" % status
    assert expect_closed(sock), "board did not close after status=1"
    sock.close()


def test_invalid_format_fallback():
    sock = connect()
    width, height, fmt, status, tile, _ = hello(sock, fmt=2)
    assert (width, height) == (1024, 600)
    assert status == 0 and fmt == 0 and tile == 64
    assert is_alive(sock), "connection lost after invalid format"
    sock.close()


def test_bad_magic():
    sock = connect()
    sock.sendall(struct.pack("<IHHI", 0xDEADBEEF, VERSION, HELLO, 0))
    assert expect_closed(sock), "board did not close on bad magic"
    sock.close()


def test_bad_version():
    sock = connect()
    hello(sock)
    sock.sendall(struct.pack("<IHHI", MAGIC, 2, PING, 4) + b"\0\0\0\0")
    assert expect_closed(sock), "board did not close on bad version"
    sock.close()


def test_oversize_length():
    sock = connect()
    hello(sock)
    sock.sendall(struct.pack("<IHHI", MAGIC, VERSION, TILES, 0x100001))
    assert expect_closed(sock), "board did not close on oversize length"
    sock.close()


def test_unknown_type():
    sock = connect()
    hello(sock)
    send_msg(sock, 0x9999)
    assert is_alive(sock), "board dropped connection on unknown type"
    sock.close()


def test_tiles_before_hello():
    sock = connect()
    send_msg(sock, TILES, tiles_payload(1, []))
    assert expect_closed(sock), "board accepted TILES before HELLO"
    sock.close()


def test_short_hello():
    sock = connect()
    send_msg(sock, HELLO, b"\x00" * 5)
    assert expect_closed(sock), "board accepted short HELLO"
    sock.close()


def test_truncated_tile_header():
    sock = connect()
    hello(sock)
    send_msg(sock, TILES, struct.pack("<IHH", 1, 5, 0))
    assert expect_closed(sock), "board accepted truncated TILES"
    sock.close()


def test_tile_too_large():
    sock = connect()
    hello(sock)
    body = tiles_payload(1, [tile_hdr(0, 0, 65, 1, 0, 0)])
    send_msg(sock, TILES, body)
    assert expect_closed(sock), "board accepted w=65 tile"
    sock.close()


def test_raw_length_mismatch():
    sock = connect()
    hello(sock)
    body = tiles_payload(1, [tile_hdr(0, 0, 64, 64, 0, 100) + b"x" * 100])
    send_msg(sock, TILES, body)
    assert expect_closed(sock), "board accepted bad raw length"
    sock.close()


def test_lz4_length_mismatch():
    if lz4 is None:
        raise SkipTest("python3-lz4 not installed")
    sock = connect()
    hello(sock)
    comp = lz4.compress(b"x" * 100, store_size=False)
    body = tiles_payload(1, [tile_hdr(0, 0, 64, 64, 1, len(comp)) + comp])
    send_msg(sock, TILES, body)
    assert expect_closed(sock), "board accepted bad LZ4 length"
    sock.close()


def test_oob_tile_clipped():
    sock = connect()
    hello(sock)
    raw = (b"\x00\x00\xff\xff") * (64 * 64)
    body = tiles_payload(1, [tile_hdr(1000, 580, 64, 64, 0, len(raw)) + raw])
    send_msg(sock, TILES, body)
    assert is_alive(sock), "board dropped connection on OOB tile"
    sock.close()


def test_zero_size_tile():
    sock = connect()
    hello(sock)
    body = tiles_payload(1, [tile_hdr(0, 0, 0, 0, 0, 0)])
    send_msg(sock, TILES, body)
    assert is_alive(sock), "board dropped connection on zero-size tile"
    sock.close()


def test_reserved_and_seq_jump():
    sock = connect()
    hello(sock, flags=0xFFFFFFFF)
    raw = b"\x00\x00\xff\xff"
    entry = tile_hdr(0, 0, 1, 1, 0, len(raw), reserved=0xAA) + raw
    body = tiles_payload(0xFFFFFFFF, [entry], reserved=0xFFFF)
    send_msg(sock, TILES, body)
    assert is_alive(sock), "board rejected reserved fields"
    body = tiles_payload(0, [entry])
    send_msg(sock, TILES, body)
    assert is_alive(sock), "board rejected seq jump"
    sock.close()


def test_short_ping():
    sock = connect()
    hello(sock)
    send_msg(sock, PING, b"\x01\x02")
    assert expect_closed(sock), "board accepted short PING"
    sock.close()


def test_set_quality_from_win_ignored():
    sock = connect()
    hello(sock)
    send_msg(sock, SET_QUALITY, struct.pack("<BBH", 1, 5, 0))
    assert is_alive(sock), "board dropped connection on SET_QUALITY"
    sock.close()


def test_short_full_begin():
    sock = connect()
    hello(sock)
    send_msg(sock, FULL_BEGIN, b"\0\0\0\0")
    assert expect_closed(sock), "board accepted short FULL_BEGIN"
    sock.close()


def parse_args(argv):
    parser = argparse.ArgumentParser(
        description="SSSC v1 board-side protocol negative tests")
    parser.add_argument("--host", default=DEFAULT_HOST,
                        help="board IP (default %(default)s)")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT,
                        help="board port (default %(default)s)")
    parser.add_argument("--skip-heartbeat", action="store_true",
                        help="skip the 15s heartbeat timeout case")
    args = parser.parse_args(argv)
    if not 1 <= args.port <= 65535:
        parser.error("invalid port")
    return args


def main(argv):
    global HOST, PORT
    args = parse_args(argv)
    HOST = args.host
    PORT = args.port

    tests = [
        ("bad_magic", test_bad_magic),
        ("bad_version", test_bad_version),
        ("oversize_length", test_oversize_length),
        ("tiles_before_hello", test_tiles_before_hello),
        ("short_hello", test_short_hello),
        ("invalid_mode", test_invalid_mode),
        ("invalid_format_fallback", test_invalid_format_fallback),
        ("unknown_type", test_unknown_type),
        ("truncated_tile_header", test_truncated_tile_header),
        ("tile_too_large", test_tile_too_large),
        ("raw_length_mismatch", test_raw_length_mismatch),
        ("lz4_length_mismatch", test_lz4_length_mismatch),
        ("oob_tile_clipped", test_oob_tile_clipped),
        ("zero_size_tile", test_zero_size_tile),
        ("reserved_and_seq_jump", test_reserved_and_seq_jump),
        ("short_ping", test_short_ping),
        ("set_quality_from_win", test_set_quality_from_win_ignored),
        ("short_full_begin", test_short_full_begin),
    ]
    if not args.skip_heartbeat:
        tests.append(("heartbeat_timeout", test_heartbeat_timeout))

    print("target %s:%d" % (HOST, PORT), flush=True)
    for name, fn in tests:
        run(name, fn)
        time.sleep(0.2)

    passed = sum(1 for _, status, _ in results if status == "PASS")
    failed = sum(1 for _, status, _ in results if status == "FAIL")
    skipped = sum(1 for _, status, _ in results if status == "SKIP")
    print("RESULT pass=%d fail=%d skip=%d total=%d"
          % (passed, failed, skipped, len(results)))
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
