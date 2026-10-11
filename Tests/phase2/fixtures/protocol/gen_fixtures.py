#!/usr/bin/env python3
"""Generate the frozen protocol-v1 golden fixtures (P2.2).

This script is a convenience for producing the committed *.hex files and for
reviewing them.  It is NOT the arbiter of correctness: verify.py is an
independent decoder and the frozen .hex files are authoritative.  Re-run only
when intentionally reviewing a schema change.

Layout authority: docs/PROTOCOL-v1.md.
"""

import os
import struct

HERE = os.path.dirname(os.path.abspath(__file__))

CONTENT_SET_DIGEST = bytes.fromhex(
    "32a94c128ee21438bbe389974dd05654e66bf3ed7aa1f1ef2520fc8dc1e1aec0")
COMBAT1 = bytes.fromhex(
    "d384f05cca5ac29e19b35f7605e9b1c31c2db6038a82020d250bbae1bce499e2")
ARENA = bytes.fromhex(
    "288d69fa0e62853b4ae7bfc3f74c38c74e08e6ad9e06e645ae214d9c3d37e35e")
VANGUARD = bytes.fromhex(
    "04dc0e299b9dad4a13ca0dcea53001c94cc3209036b6d4404bd3f6a53049cf11")

MAGIC = 0x58544B31


def u8(v):  return struct.pack(">B", v & 0xFF)
def u16(v): return struct.pack(">H", v & 0xFFFF)
def u32(v): return struct.pack(">I", v & 0xFFFFFFFF)
def i32(v): return struct.pack(">i", v)
def f64(v): return struct.pack(">d", v)


def env(mtype, channel, session, epoch, seq, tick, payload):
    assert len(payload) <= 65536 - 32, len(payload)
    return (u32(MAGIC) + u8(1) + u8(0) + u8(mtype) + u8(channel) +
            u32(len(payload)) + u32(session) + u32(epoch) + u32(seq) +
            i32(tick) + u32(0) + payload)


def command(cid, hint, op, flags, a, b):
    return (u32(cid) + u32(hint) + u8(op) + u8(flags) + u16(0) +
            f64(a) + f64(b))


def write(name, data):
    path = os.path.join(HERE, name)
    lines, cur = [], []
    for i, byte in enumerate(data):
        cur.append("%02x" % byte)
        if len(cur) == 16:
            lines.append(" ".join(cur)); cur = []
    if cur:
        lines.append(" ".join(cur))
    with open(path, "w") as fh:
        fh.write("# protocol v1 fixture %s (%d bytes)\n" % (name, len(data)))
        fh.write("\n".join(lines) + "\n")
    print("%-22s %4d bytes" % (name, len(data)))


def catalog_entry(cid, kind, name, digest):
    nb = name.encode()
    return u16(cid) + u8(kind) + u8(len(nb)) + nb + digest


def main():
    write("hello.hex", env(0x01, 0, 0, 0, 1, 0,
        u32(0x0A0B0C0D) + u8(1) + u8(0) + u16(0x0042) +
        u8(5) + b"pilot" + CONTENT_SET_DIGEST))

    write("welcome.hex", env(0x02, 0, 0x11223344, 0, 1, 0,
        u32(0xDEADBEEF) + u8(1) + u8(15) + u8(4) + u8(0)))

    write("reject_version.hex", env(0x03, 0, 0, 0, 0, 0,
        u16(0x0002) + u16(0x0000) + u8(16) + b"protocol version"))

    write("reject_content.hex", env(0x03, 0, 0, 0, 0, 0,
        u16(0x0011) + u16(0x0000) + u8(16) + b"content mismatch"))

    cat = u16(3) + (
        catalog_entry(0x0101, 0x01, "combat1", COMBAT1) +
        catalog_entry(0x0102, 0x01, "Arena", ARENA) +
        catalog_entry(0x0201, 0x02, "Vanguard", VANGUARD))
    write("catalog.hex", env(0x10, 0, 0x11223344, 0, 2, 0,
        CONTENT_SET_DIGEST + cat))

    write("config_select.hex", env(0x11, 0, 0x11223344, 0, 3, 0,
        u16(0x0101) + u16(0x0201) + u32(0)))

    write("start.hex", env(0x14, 0, 0x11223344, 1, 4, 1,
        u32(1) + u16(0x0101) + u8(0x01) + u8(0)))

    write("epoch_begin.hex", env(0x15, 0, 0x11223344, 1, 5, 1,
        u32(1) + u32(1)))

    write("results.hex", env(0x16, 0, 0x11223344, 1, 6, 231,
        u32(231) + u8(0) + u8(0) + u8(2) + u8(0) +
        u8(0) + u8(1) + u16(0) + u32(1000) + u8(5) + b"pilot" +
        u8(1) + u8(2) + u16(0) + u32(100) + u8(5) + b"drone"))

    write("event.hex", env(0x20, 0, 0x11223344, 1, 7, 170,
        u32(5) + u8(0x06) + u8(0x01) + u16(0) + u32(1) +
        u32(0x9) + u32(6) + u8(0)))

    write("cmd_ack.hex", env(0x21, 0, 0x11223344, 1, 8, 171,
        u16(2) + u16(0) +
        (u32(100) + u32(171) + u8(0) + u8(0) + u16(0)) +
        (u32(101) + u32(171) + u8(2) + u8(0x1E) + u16(0))))

    write("reliable_cmd.hex", env(0x23, 0, 0x11223344, 1, 9, 171,
        u32(170) + u16(1) + u16(0) + command(101, 171, 0x04, 0x01, 0.0, 0.0)))

    write("protocol_error.hex", env(0x22, 0, 0x11223344, 1, 10, 171,
        u16(0x0004) + u16(0) + u32(7) + u16(0x40) + u16(0)))

    write("input_batch.hex", env(0x60, 2, 0x11223344, 1, 11, 0,
        u32(10) + u32(170) + u16(2) + u16(0) +
        command(100, 171, 0x01, 0x02, 3.5, 0.0) +
        command(101, 171, 0x04, 0x01, 0.0, 0.0)))

    self_state = (u8(0) + u8(1) + u16(0x0001) +
                  u16(100) + u16(90) + u16(80) + u16(70) +
                  u16(500) + u16(20) + u32(1000) +
                  u16(10) + u16(20) + u16(0) + u16(0) + u16(0) + u16(0) +
                  u8(0x03) + u8(2) +
                  u16(0) + u16(5) + u16(0) + u16(0) + u16(0) + u16(0) +
                  u8(0) + u8(0))
    vehicle = (u8(0) + u8(1) + u8(6) + u8(0x01) +
               f64(100.5) + f64(200.25) + f64(1.5707963267948966) +
               f64(3.141592653589793) + u16(0) + u16(0) + u8(0)*4)
    bullet = (u16(7) + u16(0) + f64(101.5) + f64(200.25) +
              f64(1.5707963267948966) + u8(0) + u8(1) + u8(0) + u8(0))
    cell = u8(15) + u8(10) + u8(0x03) + u8(0)
    snap = (u32(42) + u32(170) + u16(0x0001) + u8(0) + u8(1) +
            u16(1) + u16(1) + u16(0) + u16(1) + u16(0) + u8(0) + u8(2) +
            u32(3) + self_state + vehicle + bullet + cell)
    write("view_snapshot.hex", env(0x40, 1, 0x11223344, 1, 12, 170, snap))

    # Raw cross-endian values (not enveloped): u16, u32, i32, f64.
    write("endian.hex", u16(0x0102) + u32(0x01020304) + i32(-2) + f64(1.0))


if __name__ == "__main__":
    main()
