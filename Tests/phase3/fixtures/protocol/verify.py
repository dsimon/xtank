#!/usr/bin/env python3
"""Independent verifier for the frozen protocol-v1.1 golden fixtures (P3.3).

This decoder is written directly from docs/PROTOCOL-v1.1.md offsets -- it does
not import or reuse gen_fixtures.py, and it does not call the C codec.  It
reads each committed *.hex and asserts fields against the hand-authored
expected-fields.txt, so a shared encoder/decoder bug cannot make the fixtures
agree with themselves.

Usage: python3 verify.py            (exit 0 = all fixtures verified)
"""

import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
MAGIC = 0x58544B31
MAJOR = 1
MINOR = 1


def load_hex(name):
    out = bytearray()
    with open(os.path.join(HERE, name)) as fh:
        for line in fh:
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            for tok in line.split():
                out.append(int(tok, 16))
    return bytes(out)


class R:
    def __init__(self, buf, off=0):
        self.b = buf
        self.o = off

    def u8(self):
        v = self.b[self.o]; self.o += 1; return v

    def u16(self):
        v = struct.unpack_from(">H", self.b, self.o)[0]; self.o += 2; return v

    def u32(self):
        v = struct.unpack_from(">I", self.b, self.o)[0]; self.o += 4; return v

    def i32(self):
        v = struct.unpack_from(">i", self.b, self.o)[0]; self.o += 4; return v

    def f64(self):
        v = struct.unpack_from(">d", self.b, self.o)[0]; self.o += 8; return v

    def raw(self, n):
        v = self.b[self.o:self.o + n]; self.o += n; return v

    def s(self):
        n = self.u8()
        return self.raw(n).decode("ascii")


def envelope(buf):
    r = R(buf)
    return {
        "magic": r.u32(), "version_major": r.u8(), "version_minor": r.u8(),
        "type": r.u8(), "channel": r.u8(), "payload_length": r.u32(),
        "session_id": r.u32(), "epoch": r.u32(), "message_sequence": r.u32(),
        "tick": r.i32(), "reserved": r.u32(),
    }, buf[32:]


def dec_hello(env, p):
    r = R(p)
    return {"client_nonce": r.u32(), "proto_major": r.u8(),
            "proto_minor": r.u8(), "client_build": r.u16(), "name": r.s(),
            "content_set_digest": r.raw(32)}


def dec_welcome(env, p):
    r = R(p)
    return {"server_nonce": r.u32(), "role": r.u8(), "tick_hz": r.u8(),
            "max_players": r.u8(), "human_capacity": r.u8(), "reserved": r.u8()}


def dec_reject(env, p):
    r = R(p)
    return {"error_code": r.u16(), "detail_code": r.u16(), "text": r.s()}


def dec_catalog(env, p):
    r = R(p)
    d = {"content_set_digest": r.raw(32)}
    n = r.u16()
    d["entry_count"] = n
    d["entry"] = []
    for _ in range(n):
        e = {"id": r.u16(), "kind": r.u8(), "name": r.s(),
             "source_sha256": r.raw(32)}
        d["entry"].append(e)
    return d


def dec_config_select(env, p):
    r = R(p)
    return {"maze_id": r.u16(), "vehicle_id": r.u16(), "drone_count": r.u8(),
            "reserved": r.u8(), "reserved2": r.u16(), "reserved3": r.u32()}


def dec_config_status(env, p):
    r = R(p)
    return {"maze_id": r.u16(), "vehicle_id": r.u16(), "drone_count": r.u8(),
            "reserved": r.u8(), "status": r.u16(), "error_code": r.u16(),
            "reserved2": r.u16(), "config_revision": r.u32()}


def dec_ready(env, p):
    r = R(p)
    return {"ready": r.u8(), "reserved": r.u8(), "reserved2": r.u16(),
            "config_revision": r.u32()}


def dec_start(env, p):
    r = R(p)
    return {"start_tick": r.u32(), "maze_id": r.u16(), "vehicle_id": r.u16(),
            "drone_count": r.u8(), "reserved": r.u8(), "reserved2": r.u16(),
            "config_revision": r.u32()}


def dec_start_request(env, p):
    r = R(p)
    return {"expected_config_revision": r.u32(), "reserved": r.u32()}


def dec_epoch_begin(env, p):
    r = R(p)
    return {"epoch": r.u32(), "first_tick": r.u32()}


def dec_lobby_state(env, p):
    r = R(p)
    d = {"config_revision": r.u32(), "session_state": r.u8(),
         "drone_count": r.u8(), "maze_id": r.u16(), "vehicle_id": r.u16(),
         "self_index": r.u8(), "admin_index": r.u8(), "human_count": r.u8(),
         "reserved": r.u8(), "reserved2": r.u16(), "entry": []}
    for _ in range(d["human_count"]):
        d["entry"].append({"connection_id": r.u32(), "controller_id": r.u32(),
                           "slot": r.u8(), "ready": r.u8(), "flags": r.u8(),
                           "departure_reason": r.u8(), "name": r.s()})
    d["consumed"] = r.o
    return d


def dec_results(env, p):
    r = R(p)
    d = {"end_tick": r.u32(), "result_kind": r.u8(), "winner_slot": r.u8(),
         "vehicle_count": r.u8(), "reserved": r.u8(), "entry": []}
    for _ in range(d["vehicle_count"]):
        d["entry"].append({"slot": r.u8(), "flags": r.u8(),
                           "reserved": r.u16(), "generation": r.u32(),
                           "score": r.u32(), "name": r.s()})
    return d


def dec_reset(env, p):
    r = R(p)
    return {"prior_epoch": r.u32(), "reserved": r.u32()}


def dec_quit(env, p):
    return {}


def dec_event(env, p):
    r = R(p)
    return {"event_sequence": r.u32(), "capture_tick": r.u32(),
            "event_type": r.u8(), "entity_kind": r.u8(), "entity_id": r.u16(),
            "entity_generation": r.u32(), "aux0": r.u32(), "aux1": r.u32(),
            "text": r.s()}


def dec_cmd_ack(env, p):
    r = R(p)
    d = {"count": r.u16(), "reserved": r.u16(), "entry": []}
    for _ in range(d["count"]):
        d["entry"].append({"command_id": r.u32(), "assigned_tick": r.u32(),
                           "status": r.u8(), "reason": r.u8(),
                           "reserved": r.u16()})
    return d


def _commands(r, n):
    out = []
    for _ in range(n):
        out.append({"command_id": r.u32(), "tick_hint": r.u32(),
                    "life_generation": r.u32(), "op": r.u8(), "flags": r.u8(),
                    "reserved": r.u16(), "a": r.f64(), "b": r.f64()})
    return out


def dec_reliable_cmd(env, p):
    r = R(p)
    d = {"last_observed_server_tick": r.u32(), "command_count": r.u16(),
         "reserved": r.u16()}
    d["command"] = _commands(r, d["command_count"])
    return d


def dec_protocol_error(env, p):
    r = R(p)
    return {"error_code": r.u16(), "detail": r.u16(),
            "offending_sequence": r.u32(), "offending_type": r.u16(),
            "reserved": r.u16()}


def dec_input_batch(env, p):
    r = R(p)
    d = {"packet_sequence": r.u32(), "last_observed_server_tick": r.u32(),
         "command_count": r.u16(), "reserved": r.u16()}
    d["command"] = _commands(r, d["command_count"])
    return d


def dec_view_snapshot(env, p):
    r = R(p)
    d = {"snapshot_sequence": r.u32(), "server_tick": r.u32(),
         "flags": r.u16(), "own_slot": r.u8(), "reserved": r.u8(),
         "own_generation": r.u32(), "vehicle_count": r.u16(),
         "bullet_count": r.u16(), "explosion_count": r.u16(),
         "map_cell_count": r.u16(), "landmark_count": r.u16(),
         "radar_blip_count": r.u8(), "weapon_count": r.u8(),
         "map_revision": r.u32()}
    d["self"] = {
        "slot": r.u8(), "reserved": r.u8(), "lifecycle_flags": r.u16(),
        "armor": [r.u16() for _ in range(4)], "fuel": r.u16(),
        "heat": r.u16(), "score": r.u32(),
        "ammo": [r.u16() for _ in range(6)],
        "weapon_enabled_bitmap": r.u8(), "weapon_count": r.u8(),
        "reload": [r.u16() for _ in range(6)], "generation": r.u32(),
        "reserved2": r.raw(2)}
    d["vehicle"] = []
    for _ in range(d["vehicle_count"]):
        d["vehicle"].append({"slot": r.u8(), "reserved": r.u8(),
                             "kind": r.u8(), "flags": r.u8(), "x": r.f64(),
                             "y": r.f64(), "body_angle": r.f64(),
                             "turret_angle": r.f64(), "body_picture": r.u16(),
                             "turret_picture": r.u16(), "generation": r.u32(),
                             "reserved2": r.raw(4)})
    d["bullet"] = []
    for _ in range(d["bullet_count"]):
        d["bullet"].append({"id": r.u16(), "flags": r.u16(), "x": r.f64(),
                            "y": r.f64(), "heading": r.f64(),
                            "owner_slot": r.u8(), "kind": r.u8(),
                            "reserved": r.u16(), "generation": r.u32(),
                            "owner_generation": r.u32()})
    d["explosion"] = []
    for _ in range(d["explosion_count"]):
        d["explosion"].append({"id": r.u16(), "kind": r.u8(), "flags": r.u8(),
                               "x": r.f64(), "y": r.f64(),
                               "generation": r.u32(), "reserved": r.raw(4)})
    d["cell"] = []
    for _ in range(d["map_cell_count"]):
        d["cell"].append({"grid_x": r.u8(), "grid_y": r.u8(),
                          "flags": r.u8(), "type": r.u8()})
    d["landmark"] = []
    for _ in range(d["landmark_count"]):
        d["landmark"].append({"kind": r.u8(), "grid_x": r.u8(),
                              "grid_y": r.u8(), "reserved": r.u8(),
                              "x": r.f64(), "y": r.f64(), "id": r.u16(),
                              "reserved2": r.u16(), "reserved3": r.raw(4)})
    d["radar"] = []
    for _ in range(d["radar_blip_count"]):
        d["radar"].append({"x": r.f64(), "y": r.f64(), "age": r.u8(),
                           "classification": r.u8(), "reserved": r.u16()})
    d["consumed"] = r.o
    return d


DECODERS = {
    "hello": dec_hello, "welcome": dec_welcome, "reject_version": dec_reject,
    "reject_content": dec_reject, "reject_duplicate_name": dec_reject,
    "catalog": dec_catalog, "config_select": dec_config_select,
    "config_status": dec_config_status, "ready": dec_ready, "start": dec_start,
    "start_request": dec_start_request, "epoch_begin": dec_epoch_begin,
    "lobby_state": dec_lobby_state, "results": dec_results, "reset": dec_reset,
    "quit": dec_quit, "event": dec_event, "cmd_ack": dec_cmd_ack,
    "reliable_cmd": dec_reliable_cmd, "protocol_error": dec_protocol_error,
    "input_batch": dec_input_batch, "view_snapshot": dec_view_snapshot,
    "view_keyframe": dec_view_snapshot,
}


def parse_expected(path):
    out = []
    with open(path) as fh:
        for line in fh:
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            fixture, rest = line.split(None, 1)
            path_s, value_s = rest.split("=", 1)
            out.append((fixture, path_s.strip(), value_s.strip()))
    return out


def get_path(obj, path):
    cur = obj
    for part in path.split("."):
        if "[" in part:
            name, idx = part[:-1].split("[")
            cur = cur[name][int(idx)]
        else:
            cur = cur[part]
    return cur


def coerce(value_s):
    if value_s.startswith("hex:"):
        return bytes.fromhex(value_s[4:])
    if value_s.startswith('"'):
        return value_s[1:-1]
    try:
        return int(value_s, 0)
    except ValueError:
        return float(value_s)


def main():
    expected = parse_expected(os.path.join(HERE, "expected-fields.txt"))
    decoded = {}
    failures = 0
    by_fixture = {}
    for fixture, path_s, value_s in expected:
        by_fixture.setdefault(fixture, []).append((path_s, value_s))

    for fixture, checks in by_fixture.items():
        if fixture == "endian":
            continue
        raw = load_hex(fixture + ".hex")
        env, payload = envelope(raw)
        if env["magic"] != MAGIC:
            print("FAIL %s: bad magic 0x%08x" % (fixture, env["magic"]))
            failures += 1
            continue
        if env["version_major"] != MAJOR or env["version_minor"] != MINOR:
            print("FAIL %s: version %d.%d != 1.1" %
                  (fixture, env["version_major"], env["version_minor"]))
            failures += 1
        if env["payload_length"] != len(payload):
            print("FAIL %s: payload_length %d != %d" %
                  (fixture, env["payload_length"], len(payload)))
            failures += 1
        dec = DECODERS[fixture](env, payload)
        decoded[fixture] = (env, dec)
        for path_s, value_s in checks:
            if path_s.startswith("envelope."):
                continue
            want = coerce(value_s)
            got = get_path(dec, path_s)
            if isinstance(want, float):
                ok = abs(got - want) <= 1e-12
            else:
                ok = got == want
            if not ok:
                print("FAIL %s %s: got %r want %r" %
                      (fixture, path_s, got, want))
                failures += 1

    for fixture, (env, dec) in decoded.items():
        for path_s, value_s in by_fixture.get(fixture, []):
            if not path_s.startswith("envelope."):
                continue
            want = coerce(value_s)
            got = get_path(env, path_s[len("envelope."):])
            if got != want:
                print("FAIL %s %s: got %r want %r" %
                      (fixture, path_s, got, want))
                failures += 1

    end = load_hex("endian.hex")
    e = {"u16": struct.unpack_from(">H", end, 0)[0],
         "u32": struct.unpack_from(">I", end, 2)[0],
         "i32": struct.unpack_from(">i", end, 6)[0],
         "f64": struct.unpack_from(">d", end, 10)[0],
         "len": len(end)}
    for path_s, value_s in by_fixture.get("endian", []):
        want = coerce(value_s)
        got = e[path_s.split(".")[-1]]
        if isinstance(want, float):
            ok = abs(got - want) <= 1e-12
        else:
            ok = got == want
        if not ok:
            print("FAIL endian %s: got %r want %r" % (path_s, got, want))
            failures += 1

    if failures:
        print("PROTOCOL-FIXTURES FAIL (%d)" % failures)
        return 1
    print("PROTOCOL-FIXTURES PASS (%d fixtures, %d checks)" %
          (len(by_fixture), len(expected)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
