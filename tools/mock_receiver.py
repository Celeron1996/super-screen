#!/usr/bin/env python3
"""SSSC v1 mock receiver for Windows-end self tests.

See docs/PLAN.md section 5 and docs/plan-windows.md section 7.1 for the contract.
Only the Python standard library is required; --lz4-check additionally needs
the `lz4` package (python3-lz4 on Ubuntu, `pip install lz4` on Windows).
"""

import argparse
import os
import select
import socket
import struct
import sys
import time

SSSC_MAGIC = 0x43535353
SSSC_VERSION = 1
SSSC_PORT = 5901
SSSC_TILE_SIZE = 64
SSSC_FPS_CAP_MIN = 5
SSSC_MAX_MSG_ACCEPT = 256 * 1024

T_HELLO = 0x0001
T_HELLO_ACK = 0x0002
T_TILES = 0x0010
T_FULL_BEGIN = 0x0011
T_FULL_END = 0x0012
T_SET_QUALITY = 0x0030
T_PING = 0x0040
T_PONG = 0x0041

HDR = struct.Struct("<IHHI")
U16 = struct.Struct("<H")
U32 = struct.Struct("<I")
HELLO_P = struct.Struct("<HHBBI")
ACK_P = struct.Struct("<HHBBHH")
TILES_P = struct.Struct("<IHH")
FULL_P = struct.Struct("<II")
SETQ_P = struct.Struct("<BBH")
PING_P = struct.Struct("<I")
TILE_P = struct.Struct("<HHHHBBI")

NAMES = {
    T_HELLO: "HELLO",
    T_HELLO_ACK: "HELLO_ACK",
    T_TILES: "TILES",
    T_FULL_BEGIN: "FULL_BEGIN",
    T_FULL_END: "FULL_END",
    T_SET_QUALITY: "SET_QUALITY",
    T_PING: "PING",
    T_PONG: "PONG",
}


def log(level, msg, logf=None):
    line = "mock %s %s" % (level, msg)
    print(line, flush=True)
    if logf is not None:
        logf.write(line + "\n")
        logf.flush()


def log_stats(msg, logf=None):
    line = "STATS " + msg
    print(line, flush=True)
    if logf is not None:
        logf.write(line + "\n")
        logf.flush()


def now_ms():
    return int(time.time() * 1000)


class Session:
    def __init__(self, conn, addr, args, logf):
        self.conn = conn
        self.addr = addr
        self.args = args
        self.logf = logf
        self.rx = bytearray()
        self.start = time.time()
        self.stats_at = self.start
        self.handshake_at = None
        self.quality_sent = False
        self.handshaken = False
        self.width = 0
        self.height = 0
        self.fmt = 0
        self.bpp = 4
        self.hello_fps = SSSC_FPS_CAP_MIN

        self.frames = 0
        self.tiles = 0
        self.rx_bytes = 0
        self.lz4_raw = 0
        self.lz4_comp = 0
        self.errors = 0
        self.pongs = 0
        self.full_refreshes = 0

        self.lz4_block = None

    def bpp_for(self, fmt):
        return 2 if fmt == 1 else 4

    def send_msg(self, mtype, payload):
        msg = HDR.pack(SSSC_MAGIC, SSSC_VERSION, mtype, len(payload)) + payload
        self.conn.sendall(msg)

    def run(self):
        log("INFO", "connected from %s:%d" % self.addr, self.logf)
        try:
            while True:
                r, _, _ = select.select([self.conn], [], [], 0.2)
                if r:
                    data = self.conn.recv(65536)
                    if not data:
                        log("INFO", "peer closed", self.logf)
                        break
                    self.rx_bytes += len(data)
                    self.rx.extend(data)
                    if not self.parse():
                        break
                now = time.time()
                self.maybe_set_quality(now)
                if now - self.stats_at >= 5.0:
                    self.stats_at = now
                    self.print_stats()
        finally:
            log("INFO", "session closed (%d errors)" % self.errors, self.logf)

    def maybe_set_quality(self, now):
        if self.quality_sent or not self.handshaken or self.args.set_quality is None:
            return
        if now - self.handshake_at < 5.0:
            return
        self.quality_sent = True
        fmt, fps = self.args.set_quality
        if fps is None:
            fps = self.hello_fps
        payload = SETQ_P.pack(fmt, fps, 0)
        self.send_msg(T_SET_QUALITY, payload)
        self.fmt = fmt
        self.bpp = self.bpp_for(fmt)
        log("INFO", "sent SET_QUALITY format=%d fps=%d" % (fmt, fps), self.logf)

    def parse(self):
        while len(self.rx) >= HDR.size:
            magic, version, mtype, length = HDR.unpack_from(self.rx, 0)
            if magic != SSSC_MAGIC or version != SSSC_VERSION:
                log("ERR", "bad header magic=0x%08x version=%d" % (magic, version), self.logf)
                self.errors += 1
                return False
            if length > SSSC_MAX_MSG_ACCEPT:
                log("ERR", "payload too large: %d" % length, self.logf)
                self.errors += 1
                return False
            if len(self.rx) < HDR.size + length:
                return True
            payload = bytes(self.rx[HDR.size:HDR.size + length])
            del self.rx[:HDR.size + length]
            if not self.dispatch(mtype, length, payload):
                return False
        return True

    def dispatch(self, mtype, length, payload):
        if mtype == T_HELLO:
            return self.on_hello(length, payload)
        if mtype == T_TILES:
            self.frames += 1
            return self.on_tiles(length, payload)
        if mtype == T_FULL_BEGIN:
            if length != FULL_P.size:
                self.bad_msg("FULL_BEGIN", length)
                return True
            seq, _res = FULL_P.unpack(payload)
            log("INFO", "FULL_BEGIN seq=%d" % seq, self.logf)
            return True
        if mtype == T_FULL_END:
            if length != FULL_P.size:
                self.bad_msg("FULL_END", length)
                return True
            seq, _res = FULL_P.unpack(payload)
            self.full_refreshes += 1
            log("INFO", "FULL_END seq=%d" % seq, self.logf)
            return True
        if mtype == T_PING:
            if length != PING_P.size:
                self.bad_msg("PING", length)
                return True
            ts = PING_P.unpack(payload)[0]
            self.send_msg(T_PONG, PING_P.pack(ts))
            self.pongs += 1
            return True
        if mtype == T_SET_QUALITY:
            log("WARN", "unexpected SET_QUALITY from sender, ignored", self.logf)
            return True
        log("WARN", "unknown message type=0x%04x len=%d, ignored" % (mtype, length), self.logf)
        return True

    def bad_msg(self, name, length):
        self.errors += 1
        log("ERR", "%s bad payload length=%d" % (name, length), self.logf)

    def on_hello(self, length, payload):
        if length != HELLO_P.size:
            self.errors += 1
            log("ERR", "HELLO bad payload length=%d" % length, self.logf)
            return False
        width, height, fmt, fps, _flags = HELLO_P.unpack(payload)
        if width == 0 or height == 0:
            self.errors += 1
            log("ERR", "HELLO invalid size %dx%d" % (width, height), self.logf)
            return False
        if self.args.fmt is not None:
            ack_fmt = self.args.fmt
        else:
            ack_fmt = fmt if fmt in (0, 1) else 0
        self.width = width
        self.height = height
        self.fmt = ack_fmt
        self.bpp = self.bpp_for(ack_fmt)
        self.hello_fps = fps if fps >= SSSC_FPS_CAP_MIN else SSSC_FPS_CAP_MIN
        ack = ACK_P.pack(width, height, ack_fmt, 0, SSSC_TILE_SIZE, 0)
        self.send_msg(T_HELLO_ACK, ack)
        self.handshaken = True
        self.handshake_at = time.time()
        log("INFO", "HELLO %dx%d fmt=%d fps=%d -> ACK fmt=%d"
            % (width, height, fmt, fps, ack_fmt), self.logf)
        return True

    def on_tiles(self, length, payload):
        if not self.handshaken:
            self.errors += 1
            log("ERR", "TILES before HELLO", self.logf)
            return True
        if length < TILES_P.size:
            self.bad_msg("TILES", length)
            return True
        seq, count, _res = TILES_P.unpack_from(payload, 0)
        log("INFO", "TILES seq=%d count=%d bytes=%d" % (seq, count, length), self.logf)
        pos = TILES_P.size
        for _ in range(count):
            if pos + TILE_P.size > length:
                self.errors += 1
                log("ERR", "tile header truncated", self.logf)
                return True
            x, y, w, h, codec, _resv, data_len = TILE_P.unpack_from(payload, pos)
            pos += TILE_P.size
            if w == 0 or h == 0 or w > SSSC_TILE_SIZE or h > SSSC_TILE_SIZE:
                self.errors += 1
                log("ERR", "tile bad size x=%d y=%d w=%d h=%d" % (x, y, w, h), self.logf)
                return True
            if codec not in (0, 1):
                self.errors += 1
                log("ERR", "tile bad codec=%d" % codec, self.logf)
                return True
            if pos + data_len > length:
                self.errors += 1
                log("ERR", "tile data truncated x=%d y=%d len=%d" % (x, y, data_len), self.logf)
                return True
            data = payload[pos:pos + data_len]
            pos += data_len
            expected = w * h * self.bpp
            if codec == 0:
                if data_len != expected:
                    self.errors += 1
                    log("ERR", "raw tile len %d != %d x=%d y=%d" % (data_len, expected, x, y),
                        self.logf)
                else:
                    self.lz4_raw += 0
            else:
                self.lz4_raw += expected
                self.lz4_comp += data_len
                if self.args.lz4_check:
                    self.check_lz4(x, y, w, h, expected, data)
            if self.args.save:
                self.save_tile(x, y, w, h, data)
            self.tiles += 1
        if pos != length:
            self.errors += 1
            log("ERR", "trailing bytes after tiles: %d" % (length - pos), self.logf)
        return True

    def check_lz4(self, x, y, w, h, expected, data):
        if self.lz4_block is None:
            return
        try:
            raw = self.lz4_block.decompress(data, uncompressed_size=expected)
        except Exception as exc:  # noqa: BLE001 - report any decoder failure
            self.errors += 1
            log("ERR", "lz4 decode failed x=%d y=%d: %s" % (x, y, exc), self.logf)
            return
        if len(raw) != expected:
            self.errors += 1
            log("ERR", "lz4 length %d != %d x=%d y=%d" % (len(raw), expected, x, y), self.logf)

    def save_tile(self, x, y, w, h, data):
        name = "tile_%d_%d_%d_%d.bin" % (x, y, w, h)
        try:
            with open(os.path.join(self.args.save, name), "wb") as fp:
                fp.write(data)
        except OSError as exc:
            self.errors += 1
            log("ERR", "save tile failed: %s" % exc, self.logf)

    def print_stats(self):
        elapsed = max(0.001, time.time() - self.start)
        ratio = (self.lz4_raw / self.lz4_comp) if self.lz4_comp else 0.0
        log_stats(
            "frames=%d tiles_s=%.1f rx_kbps=%.1f ratio=%.2f errors=%d pongs=%d full_refreshes=%d"
            % (self.frames, self.tiles / elapsed, self.rx_bytes * 8.0 / 1000.0 / elapsed,
               ratio, self.errors, self.pongs, self.full_refreshes),
            self.logf)


def parse_set_quality(text):
    parts = text.split(":")
    fmt_name = parts[0].strip().lower()
    if fmt_name in ("rgb565", "rgb"):
        fmt = 1
    elif fmt_name in ("bgra32", "bgra"):
        fmt = 0
    else:
        raise argparse.ArgumentTypeError("format must be bgra32 or rgb565")
    fps = None
    if len(parts) > 1:
        try:
            fps = int(parts[1])
        except ValueError:
            raise argparse.ArgumentTypeError("fps must be an integer") from None
        if fps < 0 or fps > 255:
            raise argparse.ArgumentTypeError("fps out of range 0..255")
    return fmt, fps


def main():
    parser = argparse.ArgumentParser(description="SSSC v1 mock receiver")
    parser.add_argument("--port", type=int, default=SSSC_PORT)
    parser.add_argument("--fmt", choices=["bgra32", "rgb565"], default=None,
                        help="force HELLO_ACK format (default: echo HELLO.format)")
    parser.add_argument("--set-quality", type=parse_set_quality, default=None,
                        metavar="FMT[:FPS]", help="send one SET_QUALITY 5s after handshake")
    parser.add_argument("--save", metavar="DIR", default=None, help="dump received tiles")
    parser.add_argument("--lz4-check", action="store_true", help="decompress LZ4 tiles and verify")
    parser.add_argument("--log", metavar="FILE", default=None, help="tee logs to FILE")
    args = parser.parse_args()

    args.fmt = None if args.fmt is None else (1 if args.fmt == "rgb565" else 0)

    lz4_block = None
    if args.lz4_check:
        try:
            import lz4.block  # type: ignore
            lz4_block = lz4.block
        except ImportError:
            print("mock ERR --lz4-check requires the lz4 package "
                  "(Ubuntu: apt install python3-lz4; Windows: pip install lz4)",
                  flush=True)
            return 2

    if args.save:
        try:
            os.makedirs(args.save, exist_ok=True)
        except OSError as exc:
            print("mock ERR cannot create save dir: %s" % exc, flush=True)
            return 2

    logf = None
    if args.log:
        try:
            logf = open(args.log, "a", encoding="utf-8")
        except OSError as exc:
            print("mock ERR cannot open log file: %s" % exc, flush=True)
            return 2

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        srv.bind(("0.0.0.0", args.port))
        srv.listen(1)
    except OSError as exc:
        print("mock ERR listen failed: %s" % exc, flush=True)
        srv.close()
        if logf:
            logf.close()
        return 2

    log("INFO", "listening on 0.0.0.0:%d" % args.port, logf)
    summary = {"sessions": 0, "tiles": 0, "errors": 0}
    try:
        while True:
            conn, addr = srv.accept()
            conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            session = Session(conn, addr, args, logf)
            session.lz4_block = lz4_block
            summary["sessions"] += 1
            try:
                session.run()
            finally:
                summary["tiles"] += session.tiles
                summary["errors"] += session.errors
                conn.close()
    except KeyboardInterrupt:
        log("INFO", "interrupted", logf)
    finally:
        srv.close()
        log("INFO", "summary sessions=%d tiles=%d errors=%d"
            % (summary["sessions"], summary["tiles"], summary["errors"]), logf)
        if logf:
            logf.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
