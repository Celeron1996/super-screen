#!/usr/bin/env python3
"""mock_sender.py — SSSC v1 发送端模拟器（Ubuntu 运行，用于板端验收）

CLI/行为契约见 docs/plan-imx6ull.md §8.1 与 docs/PLAN.md §5/§8.2。
"""

import argparse
import select
import socket
import struct
import sys
import time

MAGIC = 0x43535353
VERSION = 1

TYPE_HELLO = 0x0001
TYPE_HELLO_ACK = 0x0002
TYPE_TILES = 0x0010
TYPE_FULL_BEGIN = 0x0011
TYPE_FULL_END = 0x0012
TYPE_SET_QUALITY = 0x0030
TYPE_PING = 0x0040
TYPE_PONG = 0x0041

FMT_BGRA32 = 0
FMT_RGB565 = 1

WIDTH = 1024
HEIGHT = 600
TILE = 64
DEFAULT_PORT = 5901
FPS_DEFAULT = 20
FPS_MIN = 5
FPS_MAX = 30
MAX_PAYLOAD = 64 * 1024
MAX_TILES_PAYLOAD = MAX_PAYLOAD - 8
MAX_ACCEPT = 256 * 1024


def log(level, message):
    print("mock %s %s" % (level, message), flush=True)


def tile_rects():
    rects = []
    for y in range(0, HEIGHT, TILE):
        for x in range(0, WIDTH, TILE):
            rects.append((x, y, min(TILE, WIDTH - x), min(TILE, HEIGHT - y)))
    return rects


TILE_RECTS = tile_rects()


class PatternBase:
    def __init__(self, fmt):
        self.fmt = fmt
        self.bpp = 2 if fmt == FMT_RGB565 else 4
        self.stride = WIDTH * self.bpp
        self.frame = bytearray(self.stride * HEIGHT)
        self.black = bytes(self.stride * HEIGHT)

    def pixel(self, r, g, b):
        if self.bpp == 2:
            value = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
            return struct.pack("<H", value)
        return bytes((b, g, r, 255))

    def run(self, y, x, width, r, g, b):
        px = self.pixel(r, g, b)
        off = y * self.stride + x * self.bpp
        self.frame[off:off + width * self.bpp] = px * width

    def rect(self, x, y, width, height, r, g, b):
        for row in range(height):
            self.run(y + row, x, width, r, g, b)

    def clear(self):
        self.frame[:] = self.black

    def render(self):
        return self.frame


class BarsPattern(PatternBase):
    BANDS = (
        (255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 255),
        (255, 255, 0), (0, 255, 255), (255, 0, 255), (0, 0, 0),
    )
    GRAY = (0, 36, 73, 109, 146, 182, 219, 255)

    def __init__(self, fmt):
        super().__init__(fmt)
        for y in range(0, 32):
            self.run(y, 0, WIDTH, 255, 255, 255)
        for y in range(32, 512):
            for i, (r, g, b) in enumerate(self.BANDS):
                self.run(y, i * 128, 128, r, g, b)
        for y in range(512, 576):
            for x in range(0, WIDTH, 16):
                white = (((x // 16) + (y // 16)) & 1) != 0
                v = 255 if white else 0
                self.run(y, x, 16, v, v, v)
        for y in range(576, HEIGHT):
            for x in range(0, WIDTH, 32):
                v = self.GRAY[(x // 32) % 8]
                self.run(y, x, 32, v, v, v)


class MovePattern(PatternBase):
    def __init__(self, fmt):
        super().__init__(fmt)
        self.x = (WIDTH - TILE) // 2
        self.y = (HEIGHT - TILE) // 2
        self.vx = 8
        self.vy = 8
        self.old = None
        self.rect(self.x, self.y, TILE, TILE, 255, 255, 255)
        self.old = (self.x, self.y)

    def render(self):
        if self.old is not None:
            ox, oy = self.old
            self.rect(ox, oy, TILE, TILE, 0, 0, 0)

        self.x += self.vx
        self.y += self.vy
        if self.x < 0:
            self.x = 0
            self.vx = 8
        elif self.x > WIDTH - TILE:
            self.x = WIDTH - TILE
            self.vx = -8
        if self.y < 0:
            self.y = 0
            self.vy = 8
        elif self.y > HEIGHT - TILE:
            self.y = HEIGHT - TILE
            self.vy = -8

        self.rect(self.x, self.y, TILE, TILE, 255, 255, 255)
        self.old = (self.x, self.y)
        return self.frame


class ScrollPattern(PatternBase):
    BAND_HEIGHT = 100

    def __init__(self, fmt):
        super().__init__(fmt)
        self.y = HEIGHT + 8
        self.rows = []
        for i in range(self.BAND_HEIGHT):
            v = (255 * i) // (self.BAND_HEIGHT - 1)
            self.rows.append(v)

    def render(self):
        self.clear()
        for i, v in enumerate(self.rows):
            row = self.y + i
            if 0 <= row < HEIGHT:
                self.run(row, 0, WIDTH, v, v, v)
        self.y -= 8
        if self.y + self.BAND_HEIGHT <= 0:
            self.y = HEIGHT
        return self.frame


def make_pattern(name, fmt):
    if name == "bars":
        return BarsPattern(fmt)
    if name == "move":
        return MovePattern(fmt)
    return ScrollPattern(fmt)


class TileEncoder:
    def __init__(self, mode, lz4block):
        self.mode = mode
        self.lz4block = lz4block

    def encode(self, raw):
        if self.mode == "raw" or self.lz4block is None:
            return 0, bytes(raw)
        compressed = self.lz4block.compress(bytes(raw), store_size=False)
        if self.mode == "auto" and len(compressed) >= len(raw):
            return 0, bytes(raw)
        return 1, bytes(compressed)


def extract_tile(frame, rect, bpp):
    x, y, w, h = rect
    stride = WIDTH * bpp
    rowbytes = w * bpp
    data = bytearray(w * h * bpp)
    for row in range(h):
        src = (y + row) * stride + x * bpp
        dst = row * rowbytes
        data[dst:dst + rowbytes] = frame[src:src + rowbytes]
    return data


def diff_tiles(frame, clean, bpp):
    stride = WIDTH * bpp
    changed = []
    for rect in TILE_RECTS:
        x, y, w, h = rect
        rowbytes = w * bpp
        differ = False
        for row in range(h):
            off = (y + row) * stride + x * bpp
            if frame[off:off + rowbytes] != clean[off:off + rowbytes]:
                differ = True
                break
        if differ:
            changed.append(rect)
    return changed


class Stats:
    def __init__(self):
        self.frames = 0
        self.tiles = 0
        self.wire = 0
        self.raw = 0
        self.data = 0
        self.last_time = time.monotonic()
        self.last_frames = 0
        self.last_tiles = 0
        self.last_wire = 0
        self.last_raw = 0
        self.last_data = 0
        self.pongs = 0

    def wire_msg(self, payload_len):
        self.wire += 12 + payload_len

    def snapshot(self):
        self.last_time = time.monotonic()
        self.last_frames = self.frames
        self.last_tiles = self.tiles
        self.last_wire = self.wire
        self.last_raw = self.raw
        self.last_data = self.data


def send_message(sock, mtype, payload, stats=None):
    header = struct.pack("<IHHI", MAGIC, VERSION, mtype, len(payload))
    sock.sendall(header + payload)
    if stats is not None:
        stats.wire_msg(len(payload))


def chunk_tiles(tiles):
    chunks = []
    payload = bytearray()
    count = 0
    for rect, codec, data in tiles:
        x, y, w, h = rect
        entry = struct.pack("<HHHHBBI", x, y, w, h, codec, 0, len(data)) + data
        if count > 0 and len(payload) + len(entry) > MAX_TILES_PAYLOAD:
            chunks.append((count, bytes(payload)))
            payload = bytearray()
            count = 0
        payload.extend(entry)
        count += 1
    if count > 0:
        chunks.append((count, bytes(payload)))
    return chunks


def send_tiles(sock, seq, tiles, stats):
    for count, payload in chunk_tiles(tiles):
        body = struct.pack("<IHH", seq, count, 0) + payload
        send_message(sock, TYPE_TILES, body, stats)


def send_full(sock, seq, tiles, stats):
    send_message(sock, TYPE_FULL_BEGIN, struct.pack("<II", seq, 0), stats)
    send_tiles(sock, seq, tiles, stats)
    send_message(sock, TYPE_FULL_END, struct.pack("<II", seq, 0), stats)


def recv_exact(sock, size, timeout):
    data = bytearray()
    deadline = time.monotonic() + timeout
    while len(data) < size:
        remain = deadline - time.monotonic()
        if remain <= 0:
            raise OSError("handshake timeout")
        sock.settimeout(remain)
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise OSError("connection closed during handshake")
        data.extend(chunk)
    return bytes(data)


def recv_message(sock, timeout):
    header = recv_exact(sock, 12, timeout)
    magic, version, mtype, length = struct.unpack("<IHHI", header)
    if magic != MAGIC or version != VERSION:
        raise ValueError("bad SSSC header")
    if length > MAX_ACCEPT:
        raise ValueError("message too large: %u" % length)
    payload = recv_exact(sock, length, timeout)
    return mtype, payload


def connect_hello(host, port, fmt):
    sock = socket.create_connection((host, port), timeout=3)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    hello = struct.pack("<HHBBI", WIDTH, HEIGHT, fmt, FPS_DEFAULT, 0)
    send_message(sock, TYPE_HELLO, hello)
    mtype, payload = recv_message(sock, 3)
    if mtype != TYPE_HELLO_ACK:
        raise ValueError("expected HELLO_ACK, got 0x%04x" % mtype)
    if len(payload) < 10:
        raise ValueError("short HELLO_ACK")
    width, height, ack_fmt, status, tile_size, _ = struct.unpack(
        "<HHBBHH", payload[:10])
    if status != 0:
        raise ValueError("board rejected mode (status=%u)" % status)
    if tile_size != TILE:
        raise ValueError("unexpected tile_size=%u" % tile_size)
    if (width, height) != (WIDTH, HEIGHT):
        raise ValueError("unexpected mode %ux%u" % (width, height))
    if ack_fmt not in (FMT_BGRA32, FMT_RGB565):
        raise ValueError("invalid ACK format=%u" % ack_fmt)
    sock.settimeout(None)
    return sock, ack_fmt


class RxState:
    def __init__(self):
        self.buf = bytearray()


def drain_rx(sock, state):
    while True:
        readable, _, _ = select.select([sock], [], [], 0)
        if not readable:
            break
        chunk = sock.recv(65536)
        if not chunk:
            raise OSError("connection closed by board")
        state.buf.extend(chunk)
        if len(state.buf) > 2 * 1024 * 1024:
            raise ValueError("receive buffer overflow")

    messages = []
    while len(state.buf) >= 12:
        magic, version, mtype, length = struct.unpack_from("<IHHI", state.buf, 0)
        if magic != MAGIC or version != VERSION:
            raise ValueError("bad SSSC header")
        if length > MAX_ACCEPT:
            raise ValueError("message too large: %u" % length)
        if len(state.buf) < 12 + length:
            break
        payload = bytes(state.buf[12:12 + length])
        del state.buf[:12 + length]
        messages.append((mtype, payload))
    return messages


def print_stats(stats, pong_rtt):
    now = time.monotonic()
    elapsed = now - stats.last_time
    if elapsed < 5.0:
        return
    frames_delta = stats.frames - stats.last_frames
    tiles_delta = stats.tiles - stats.last_tiles
    wire_delta = stats.wire - stats.last_wire
    raw_delta = stats.raw - stats.last_raw
    data_delta = stats.data - stats.last_data
    ratio = (float(raw_delta) / data_delta) if data_delta > 0 else 0.0
    print("STATS frames=%d tiles_s=%.1f tx_kbps=%.1f ratio=%.2f pong_rtt=%d"
          % (stats.frames, tiles_delta / elapsed,
             wire_delta * 8.0 / 1000.0 / elapsed, ratio, pong_rtt),
          flush=True)
    stats.snapshot()


def summary(stats, start, longest_static):
    elapsed = max(time.monotonic() - start, 1e-6)
    mbps = stats.wire * 8.0 / elapsed / 1000000.0
    log("INFO", "summary frames=%d bytes=%d avg_mbps=%.2f "
        "longest_static_s=%.1f"
        % (stats.frames, stats.wire, mbps, longest_static))


def parse_args(argv):
    parser = argparse.ArgumentParser(
        description="SSSC v1 mock sender for IMX6ULL board")
    parser.add_argument("board_ip")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--pattern", choices=("bars", "move", "scroll"),
                        default="move")
    parser.add_argument("--fmt", choices=("bgra32", "rgb565"),
                        default="bgra32")
    parser.add_argument("--codec", choices=("auto", "raw", "lz4"),
                        default="auto")
    parser.add_argument("--fps", type=int, default=10)
    parser.add_argument("--seconds", type=float, default=0)
    parser.add_argument("--full-every", type=float, default=5)
    args = parser.parse_args(argv)

    if not 1 <= args.port <= 65535:
        parser.error("invalid port")
    if not 1 <= args.fps <= FPS_MAX:
        parser.error("fps must be in [1, %d]" % FPS_MAX)
    if args.seconds < 0:
        parser.error("seconds must be >= 0")
    if args.full_every < 0:
        parser.error("full-every must be >= 0")
    return args


def run(args):
    lz4block = None
    codec_mode = args.codec

    if args.codec in ("auto", "lz4"):
        try:
            import lz4.block as lz4block
        except ImportError:
            if args.codec == "lz4":
                log("ERR", "python3-lz4 is required for --codec lz4 "
                    "(apt install python3-lz4)")
                return 2
            log("WARN", "python3-lz4 not installed, fallback to raw")
            codec_mode = "raw"

    fmt = FMT_RGB565 if args.fmt == "rgb565" else FMT_BGRA32
    sock, fmt = connect_hello(args.board_ip, args.port, fmt)
    bpp = 2 if fmt == FMT_RGB565 else 4
    fmt_name = "rgb565" if fmt == FMT_RGB565 else "bgra32"
    log("INFO", "connected, board format=%s codec=%s" % (fmt_name, codec_mode))

    encoder = TileEncoder(codec_mode, lz4block)
    pattern = make_pattern(args.pattern, fmt)
    clean = bytearray(pattern.frame)
    rx = RxState()
    stats = Stats()

    fps = float(args.fps)
    need_full = True
    last_full = 0.0
    seq = 1
    last_ping = 0.0
    ping_sent_at = None
    pong_rtt = -1
    start = time.monotonic()
    next_tick = start
    static_start = None
    longest_static = 0.0

    def handle_messages(messages):
        nonlocal fps, need_full, pattern, clean, bpp, fmt, fmt_name
        nonlocal pong_rtt, ping_sent_at, lz4block, encoder

        for mtype, payload in messages:
            if mtype == TYPE_PONG:
                if len(payload) < 4:
                    log("WARN", "short PONG ignored")
                    continue
                stats.pongs += 1
                if ping_sent_at is not None:
                    pong_rtt = max(0, int((time.monotonic() -
                                           ping_sent_at) * 1000))
                    ping_sent_at = None
            elif mtype == TYPE_SET_QUALITY:
                if len(payload) < 4:
                    log("WARN", "short SET_QUALITY ignored")
                    continue
                new_fmt, new_fps, _ = struct.unpack("<BBH", payload[:4])
                if new_fmt not in (FMT_BGRA32, FMT_RGB565):
                    log("WARN", "invalid SET_QUALITY format=%u ignored"
                        % new_fmt)
                    continue
                if new_fps == 0:
                    new_fps = FPS_MIN
                if new_fps > int(fps):
                    new_fps = int(fps)
                if new_fps < FPS_MIN and int(fps) >= FPS_MIN:
                    new_fps = FPS_MIN
                if new_fmt == fmt and new_fps == int(fps):
                    continue
                if new_fmt != fmt:
                    fmt = new_fmt
                    bpp = 2 if fmt == FMT_RGB565 else 4
                    fmt_name = "rgb565" if fmt == FMT_RGB565 else "bgra32"
                    pattern = make_pattern(args.pattern, fmt)
                    clean = bytearray(pattern.frame)
                fps = float(new_fps)
                need_full = True
                log("INFO", "SET_QUALITY adopted format=%s fps_cap=%d"
                    % (fmt_name, new_fps))
            elif mtype == TYPE_HELLO_ACK:
                log("WARN", "unexpected HELLO_ACK ignored")
            else:
                log("WARN", "unknown message type=0x%04x ignored" % mtype)

    try:
        while True:
            messages = drain_rx(sock, rx)
            handle_messages(messages)

            now = time.monotonic()
            if args.seconds > 0 and now - start >= args.seconds:
                break

            if ping_sent_at is not None and now - ping_sent_at >= 5.0:
                log("WARN", "PONG timeout")
                pong_rtt = -1
                ping_sent_at = None

            if now - last_ping >= 5.0:
                stamp = int(now * 1000) & 0xFFFFFFFF
                send_message(sock, TYPE_PING, struct.pack("<I", stamp), stats)
                last_ping = now
                ping_sent_at = now

            frame = pattern.render()
            changed = diff_tiles(frame, clean, bpp)
            probe = bool(changed) and static_start is not None

            if changed:
                if static_start is not None:
                    longest_static = max(longest_static, now - static_start)
                    static_start = None
            elif static_start is None:
                static_start = now

            do_full = need_full or (args.full_every > 0 and bool(changed) and
                                    (now - last_full) >= args.full_every)
            rects = TILE_RECTS if do_full else changed

            if rects:
                if probe:
                    tx_ts = int(time.time() * 1000)
                    for x, y, _, _ in changed:
                        log("INFO", "lat_probe tx_ts=%d tile=%d,%d"
                            % (tx_ts, x, y))

                tiles = []
                raw_total = 0
                data_total = 0
                for rect in rects:
                    raw = extract_tile(frame, rect, bpp)
                    codec, data = encoder.encode(raw)
                    tiles.append((rect, codec, data))
                    raw_total += len(raw)
                    data_total += len(data)

                if do_full:
                    send_full(sock, seq, tiles, stats)
                    last_full = now
                    need_full = False
                else:
                    send_tiles(sock, seq, tiles, stats)

                seq += 1
                stats.frames += 1
                stats.tiles += len(tiles)
                stats.raw += raw_total
                stats.data += data_total
                clean[:] = frame

            print_stats(stats, pong_rtt)

            next_tick += 1.0 / max(1.0, fps)
            while True:
                wait = next_tick - time.monotonic()
                if wait <= 0:
                    break
                readable, _, _ = select.select([sock], [], [], wait)
                if readable:
                    handle_messages(drain_rx(sock, rx))
            if next_tick < time.monotonic() - 1.0:
                next_tick = time.monotonic()
    except KeyboardInterrupt:
        log("INFO", "interrupted by user")
    finally:
        sock.close()
        if static_start is not None:
            longest_static = max(longest_static,
                                 time.monotonic() - static_start)
        summary(stats, start, longest_static)

    return 0


def main(argv):
    args = parse_args(argv)
    try:
        return run(args)
    except (OSError, ValueError) as exc:
        log("ERR", str(exc))
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
