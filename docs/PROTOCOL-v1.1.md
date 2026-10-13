# Xtank wire protocol v1.1

**Frozen by P3.3, 2026-10-13 (owner-approved D3-A..D3-F).**
Authority: `docs/PHASE3-PLAN.md` §3 (P3.3) and §5 (approved extension list);
`docs/PHASE3-DECISIONS.md` D3-A..D3-F; `docs/PROTOCOL-v1.md` (the v1.0 base,
which this document supersedes byte-for-byte where it is silent and replaces
where it is not). Changing any value or field below is a protocol change and
requires a new owner decision.

This document freezes **only** the approved multiplayer/session/lifetime
extensions. It does not add a gameplay op, change a gameplay rule, widen
VIEW-v1 knowledge, add compression/delta snapshots, or reinterpret a reserved
field. `Protocol/wire.h`, `Protocol/limits.h`, `Protocol/encode.c` and
`Protocol/decode.c` implement exactly this document; the codec emits and
accepts exactly version **1.1** with no fallback (D3-A).

Companion artifacts:

| Artifact | Role |
| --- | --- |
| `Protocol/limits.h` | frozen numeric bounds |
| `Protocol/wire.h` | frozen IDs, channels, ops, error codes, shapes |
| `Protocol/content-manifest.{h,c}` | content IDs and pinned SHA-256 identity (unchanged) |
| `Tests/phase3/fixtures/protocol/*` | v1.1 golden wire bytes + independent verifier |
| `Tests/phase2/fixtures/protocol/*` | retained v1.0 golden bytes + verifier (unchanged) |

---

## 1. Version and compatibility (D3-A)

- The version is **exactly 1.1**: `version_major == 1` and
  `version_minor == 1`. The codec emits 1.1 on every message.
- A `1.0` envelope is rejected with `XTK_ERR_VERSION_MISMATCH`; any other minor
  with `XTK_ERR_VERSION_MISMATCH`; a non-1 major with `XTK_ERR_BAD_VERSION`.
  There is **no** negotiated dual-version mode and **no** fallback.
- Clients and server upgrade together. An older peer fails with an actionable
  mismatch before a tank is created.
- The v1.0 golden fixtures are retained unchanged and their independent decoder
  still passes; they are provenance, not a compatibility claim. The v1.1 codec
  does not decode them.

## 2. Conventions

- **Byte order.** Every integer and float is network byte order (big-endian).
  Signed integers are two's complement. No padding; every offset is exact.
- **Fixed-width primitives.** `u8`,`u16`,`u32`,`u64` unsigned; `i8`,`i16`,
  `i32`,`i64` signed. The C names are labels only.
- **`f64`.** IEEE-754 binary64, its 8-byte big-endian bit pattern. NaN and
  ±Inf are rejected on decode (`XTK_ERR_NONFINITE`). Positions and operand
  values stay binary64; input is never re-quantized.
- **Strings.** A `u8` count `n` (no terminator) followed by `n` ASCII bytes
  (`0x01..0x7F`; no embedded NUL). `n ≤ XTK_MAX_NAME_BYTES (32)` for names, or
  `≤ XTK_MAX_ERROR_TEXT_BYTES (128)` for error text.
- **Counts.** Bounded by `Protocol/limits.h`. An over-bound count is rejected
  before use (`XTK_ERR_BAD_LENGTH` / `XTK_ERR_MALFORMED`); the decoder never
  allocates on an attacker-chosen length.
- **Reserved fields.** Every field named `reserved*` must be `0`. A non-zero
  reserved field is `XTK_ERR_MALFORMED` on decode. No v1.0 reserved field is
  reused for new data; new data is appended as new bytes.

## 3. Envelope (`XTK_ENVELOPE_BYTES` = 32)

Unchanged from v1.0 except the exact version.

| Off | Size | Type | Field | Notes |
| --: | --: | --- | --- | --- |
| 0 | 4 | u32 | `magic` | `XTK_WIRE_MAGIC` = `0x58544B31` (`"XTK1"`) |
| 4 | 1 | u8 | `version_major` | `1` |
| 5 | 1 | u8 | `version_minor` | `1` |
| 6 | 1 | u8 | `message_type` | `enum XtkMessageType`; unknown ⇒ reject |
| 7 | 1 | u8 | `channel` | 0/1/2; must match the type (§4) |
| 8 | 4 | u32 | `payload_length` | 0 … `XTK_MAX_PAYLOAD_BYTES` |
| 12 | 4 | u32 | `session_id` | 0 until WELCOME assigns one |
| 16 | 4 | u32 | `epoch` | 0 outside a running match |
| 20 | 4 | u32 | `message_sequence` | per-(session,channel) monotonic |
| 24 | 4 | i32 | `tick` | engine frame; 0 outside a match; never negative |
| 28 | 4 | u32 | `reserved` | must be 0 |
| 32 | … | — | `payload` | `payload_length` bytes |

Envelope validation order: magic → exactly 1.1 → payload_length bound and
consistency with actual length → channel matches type → `reserved == 0` →
`tick >= 0`.

## 4. Channels

| Ch | ENet mode | Carries |
| --: | --- | --- |
| 0 | reliable ordered | HELLO/WELCOME/REJECT/PING/PONG, CONTENT_CATALOG, CONFIG_*, READY, START, START_REQUEST, EPOCH_BEGIN, LOBBY_STATE, RESULTS, RESET, QUIT, EVENT, CMD_ACK, PROTOCOL_ERROR, RELIABLE_CMD, VIEW_KEYFRAME |
| 1 | unreliable sequenced | VIEW_SNAPSHOT only |
| 2 | unreliable sequenced | INPUT_BATCH only |

A type on the wrong channel is discarded and reported once
(`XTK_ERR_BAD_CHANNEL`). Channel 1 remains self-contained. The reliable
initial-view barrier is `VIEW_KEYFRAME` on channel 0 (D3-C); the first channel-1
snapshot of an epoch still carries `XTK_FLAG_KEYFRAME`.

## 5. Message types

| Type | Value | Ch | Dir |
| --- | --: | --: | --- |
| HELLO | 0x01 | 0 | C→S |
| WELCOME | 0x02 | 0 | S→C |
| REJECT | 0x03 | 0 | S→C |
| PING / PONG | 0x04 / 0x05 | 0 | both |
| CONTENT_CATALOG | 0x10 | 0 | S→C |
| CONFIG_SELECT | 0x11 | 0 | C→S |
| CONFIG_STATUS | 0x12 | 0 | S→C |
| READY | 0x13 | 0 | C→S |
| START | 0x14 | 0 | S→C |
| EPOCH_BEGIN | 0x15 | 0 | S→C |
| RESULTS | 0x16 | 0 | S→C |
| RESET | 0x17 | 0 | C→S |
| QUIT | 0x18 | 0 | C→S |
| LOBBY_STATE | 0x19 | 0 | S→C |
| START_REQUEST | 0x1A | 0 | C→S |
| EVENT | 0x20 | 0 | S→C |
| CMD_ACK | 0x21 | 0 | S→C |
| PROTOCOL_ERROR | 0x22 | 0 | S→C |
| RELIABLE_CMD | 0x23 | 0 | C→S |
| VIEW_KEYFRAME | 0x30 | 0 | S→C |
| VIEW_SNAPSHOT | 0x40 | 1 | S→C |
| INPUT_BATCH | 0x60 | 2 | C→S |

## 6. Message payloads

All offsets are payload-relative (after the 32-byte envelope). "Len" is the
fixed payload length; variable fields show the extension.

### 6.1 PING (0x04) / PONG (0x05) — len 4, ch0 (unchanged)

| Off | Size | Type | Field |
| --: | --: | --- | --- |
| 0 | 4 | u32 | `nonce` |

### 6.2 HELLO (0x01) — ch0, client → server (proto 1.1)

| Off | Size | Type | Field |
| --: | --: | --- | --- |
| 0 | 4 | u32 | `client_nonce` |
| 4 | 1 | u8 | `proto_major` (= 1) |
| 5 | 1 | u8 | `proto_minor` (= 1) |
| 6 | 2 | u16 | `client_build` |
| 8 | 1 | u8 | `name_len` (0 … 32) |
| 9 | n | bytes | `name` |
| 9+n | 32 | u8[32] | `content_set_digest` |

Len = `41 + name_len`. The digest policy is unchanged (§9).

### 6.3 WELCOME (0x02) — ch0, server → client, len 9

| Off | Size | Type | Field |
| --: | --: | --- | --- |
| 0 | 4 | u32 | `server_nonce` |
| 4 | 1 | u8 | `role` (0 observer, 1 player) |
| 5 | 1 | u8 | `tick_hz` (= 15) |
| 6 | 1 | u8 | `max_players` (= 4 vehicles) |
| 7 | 1 | u8 | `human_capacity` (≤ 2 admitted humans) |
| 8 | 1 | u8 | reserved = 0 |

`max_players` is the vehicle preset bound; `human_capacity` is the admitted
human count. Player role never implies administrator (D3-B).

### 6.4 REJECT (0x03) — ch0, server → client

| Off | Size | Type | Field |
| --: | --: | --- | --- |
| 0 | 2 | u16 | `error_code` |
| 2 | 2 | u16 | `detail_code` |
| 4 | 1 | u8 | `text_len` (0 … 128) |
| 5 | n | bytes | `text` |

Len = `5 + text_len`. New v1.1 codes used here include
`XTK_ERR_VERSION_MISMATCH`, `XTK_ERR_DUPLICATE_NAME` and
`XTK_ERR_CONTENT_MISMATCH`.

### 6.5 CONTENT_CATALOG (0x10) — ch0, server → client (unchanged)

Header `content_set_digest[32]`, `entry_count u16` (0 … 8), then
`entry_count × CONTENT_ENTRY` where CONTENT_ENTRY = `id u16`, `kind u8`,
`name_len u8`, `name`, `source_sha256[32]`. Max len 578.

### 6.6 CONFIG_SELECT (0x11) — ch0, client → server, len 12

| Off | Size | Type | Field |
| --: | --: | --- | --- |
| 0 | 2 | u16 | `maze_id` |
| 2 | 2 | u16 | `vehicle_id` |
| 4 | 1 | u8 | `drone_count` (0 … 2) |
| 5 | 1 | u8 | reserved = 0 |
| 6 | 2 | u16 | reserved = 0 |
| 8 | 4 | u32 | reserved = 0 |

### 6.7 CONFIG_STATUS (0x12) — ch0, server → client, len 16

| Off | Size | Type | Field |
| --: | --: | --- | --- |
| 0 | 2 | u16 | `maze_id` (echo) |
| 2 | 2 | u16 | `vehicle_id` (echo) |
| 4 | 1 | u8 | `drone_count` |
| 5 | 1 | u8 | reserved = 0 |
| 6 | 2 | u16 | `status` (0 accepted) |
| 8 | 2 | u16 | `error_code` |
| 10 | 2 | u16 | reserved = 0 |
| 12 | 4 | u32 | `config_revision` |

A config change increments `config_revision` and invalidates all readiness
(D3-B). A bad content ID yields `status != 0` with `XTK_ERR_BAD_CONTENT_ID`.

### 6.8 READY (0x13) — ch0, client → server, len 8

| Off | Size | Type | Field |
| --: | --: | --- | --- |
| 0 | 1 | u8 | `ready` (0 unready, 1 ready) |
| 1 | 1 | u8 | reserved = 0 |
| 2 | 2 | u16 | reserved = 0 |
| 4 | 4 | u32 | `config_revision` |

The explicit toggle replaces v1.0's zero-length READY (D3-B). Readiness is
against the named `config_revision`; stale revisions are `XTK_ERR_STALE_CONFIG`.

### 6.9 START (0x14) — ch0, server → client, len 16

| Off | Size | Type | Field |
| --: | --: | --- | --- |
| 0 | 4 | u32 | `start_tick` |
| 4 | 2 | u16 | `maze_id` |
| 6 | 2 | u16 | `vehicle_id` (full-width content id) |
| 8 | 1 | u8 | `drone_count` |
| 9 | 1 | u8 | reserved = 0 |
| 10 | 2 | u16 | reserved = 0 |
| 12 | 4 | u32 | `config_revision` |

`vehicle_id` is `u16` so Vanguard `0x0201` is not truncated. START stays
server-authored; the client's explicit request is `START_REQUEST`.

### 6.10 START_REQUEST (0x1A) — ch0, client → server, len 8

| Off | Size | Type | Field |
| --: | --: | --- | --- |
| 0 | 4 | u32 | `expected_config_revision` |
| 4 | 4 | u32 | reserved = 0 |

Admin-only. Rejected with `XTK_ERR_NOT_ADMIN` if the sender is not the
administrator, `XTK_ERR_NOT_READY` if an admitted human is not ready against
`expected_config_revision`, and `XTK_ERR_STALE_CONFIG` if the revision moved.

### 6.11 EPOCH_BEGIN (0x15) — ch0, server → client, len 8 (unchanged)

| Off | Size | Type | Field |
| --: | --: | --- | --- |
| 0 | 4 | u32 | `epoch` (= envelope epoch) |
| 4 | 4 | u32 | `first_tick` |

### 6.12 LOBBY_STATE (0x19) — ch0, server → client

| Off | Size | Type | Field |
| --: | --: | --- | --- |
| 0 | 4 | u32 | `config_revision` |
| 4 | 1 | u8 | `session_state` (0 lobby, 1 running, 2 results) |
| 5 | 1 | u8 | `drone_count` |
| 6 | 2 | u16 | `maze_id` |
| 8 | 2 | u16 | `vehicle_id` |
| 10 | 1 | u8 | `self_index` (recipient roster index; 0xFF none) |
| 11 | 1 | u8 | `admin_index` (0xFF none) |
| 12 | 1 | u8 | `human_count` (0 … 2) |
| 13 | 1 | u8 | reserved = 0 |
| 14 | 2 | u16 | reserved = 0 |
| 16 | … | repeat | `human_count` × ROSTER_ENTRY |

ROSTER_ENTRY:

| Off | Size | Type | Field |
| --: | --: | --- | --- |
| 0 | 4 | u32 | `connection_id` (stable connection identity) |
| 4 | 4 | u32 | `controller_id` (stable controller identity) |
| 8 | 1 | u8 | `slot` (bound vehicle slot; 0xFF unbound) |
| 9 | 1 | u8 | `ready` |
| 10 | 1 | u8 | `flags` (bit 0 `XTK_FLAG_DEPARTED`) |
| 11 | 1 | u8 | `departure_reason` (`XTK_ERR_*`; 0 none) |
| 12 | 1 | u8 | `name_len` |
| 13 | n | bytes | `name` |

Max len = `16 + 2 × 45 = 106`. Reliable. Valid in lobby, running and results
for status changes. It carries **no** hidden vehicle/HUD/score data (D3-C).
Solo auto-start is suppressed once a second human is present; the explicit
`START_REQUEST` is the release start path (D3-B).

### 6.13 RESULTS (0x16) — ch0, server → client

| Off | Size | Type | Field |
| --: | --: | --- | --- |
| 0 | 4 | u32 | `end_tick` |
| 4 | 1 | u8 | `result_kind` (0 win, 1 abort) |
| 5 | 1 | u8 | `winner_slot` (0xFF none) |
| 6 | 1 | u8 | `vehicle_count` (0 … 4) |
| 7 | 1 | u8 | reserved = 0 |
| 8 | … | repeat | `vehicle_count` × RESULT_ENTRY |

RESULT_ENTRY:

| Off | Size | Type | Field |
| --: | --: | --- | --- |
| 0 | 1 | u8 | `slot` |
| 1 | 1 | u8 | `flags` (bit 0 `XTK_FLAG_DEPARTED`) |
| 2 | 2 | u16 | reserved = 0 |
| 4 | 4 | u32 | `generation` |
| 8 | 4 | u32 | `score` |
| 12 | 1 | u8 | `name_len` |
| 13 | n | bytes | `name` |

Max len = `8 + 4 × 45 = 188`. Generation is full-width `u32`; a departed
combatant keeps its earned score/name with `XTK_FLAG_DEPARTED` set (D3-D).
Captured before score reset, delivered reliably, retained until RESET.

### 6.14 RESET (0x17) — ch0, client → server, len 8 (unchanged layout)

| Off | Size | Type | Field |
| --: | --: | --- | --- |
| 0 | 4 | u32 | `prior_epoch` |
| 4 | 4 | u32 | reserved = 0 |

Admin-only. RESET returns through a shared lobby barrier (a LOBBY_STATE in the
lobby state) before a later ready/start transaction increments the epoch; it
preserves the prior maze/drone selection (D3-B). A reset from a live match is a
labeled abort.

### 6.15 QUIT (0x18) — ch0, client → server, len 0 (unchanged)

### 6.16 EVENT (0x20) — ch0, server → client

| Off | Size | Type | Field |
| --: | --: | --- | --- |
| 0 | 4 | u32 | `event_sequence` (per recipient, per epoch, from 0) |
| 4 | 4 | u32 | `capture_tick` (engine tick the event occurred) |
| 8 | 1 | u8 | `event_type` |
| 9 | 1 | u8 | `entity_kind` |
| 10 | 2 | u16 | `entity_id` |
| 12 | 4 | u32 | `entity_generation` |
| 16 | 4 | u32 | `aux0` |
| 20 | 4 | u32 | `aux1` |
| 24 | 1 | u8 | `text_len` (0 … 32) |
| 25 | n | bytes | `text` |

Max len 57. `capture_tick` is the occurrence tick and is preserved through
delayed delivery; the envelope `tick` is the delivery frame. `event_sequence`
is recipient-local: a filtered private gap is not interpreted as loss (D3-C).
Removal is conveyed by session status (`LOBBY_STATE`) plus snapshot
disappearance, not by a distinct REMOVE event (D3-C); DEATH is a gameplay kill.

### 6.17 CMD_ACK (0x21) — ch0, server → client (unchanged)

Header `count u16` (0 … 32), reserved u16, then `count × ACK_ENTRY` where
ACK_ENTRY = `command_id u32`, `assigned_tick u32`, `status u8`, `reason u8`
(now able to report `XTK_ERR_STALE_LIFE`), reserved u16. Max len 388.

### 6.18 PROTOCOL_ERROR (0x22) — ch0, server → client, len 12 (unchanged)

| Off | Size | Type | Field |
| --: | --: | --- | --- |
| 0 | 2 | u16 | `error_code` |
| 2 | 2 | u16 | `detail` |
| 4 | 4 | u32 | `offending_sequence` |
| 8 | 2 | u16 | `offending_type` |
| 10 | 2 | u16 | reserved = 0 |

### 6.19 RELIABLE_CMD (0x23) — ch0, client → server

| Off | Size | Type | Field |
| --: | --: | --- | --- |
| 0 | 4 | u32 | `last_observed_server_tick` |
| 4 | 2 | u16 | `command_count` (0 … 32) |
| 6 | 2 | u16 | reserved = 0 |
| 8 | … | repeat | `command_count` × COMMAND |

COMMAND (32 bytes):

| Off | Size | Type | Field |
| --: | --: | --- | --- |
| 0 | 4 | u32 | `command_id` |
| 4 | 4 | u32 | `tick_hint` |
| 8 | 4 | u32 | `life_generation` (controlled entity-life generation) |
| 12 | 1 | u8 | `op` |
| 13 | 1 | u8 | `flags` (`XTK_FLAG_EDGE`/`_HELD`/`_RELIABLE`) |
| 14 | 2 | u16 | reserved = 0 |
| 16 | 8 | f64 | `a` |
| 24 | 8 | f64 | `b` |

Max len = `8 + 32 × 32 = 1032`.

### 6.20 INPUT_BATCH (0x60) — ch2, client → server

| Off | Size | Type | Field |
| --: | --: | --- | --- |
| 0 | 4 | u32 | `packet_sequence` |
| 4 | 4 | u32 | `last_observed_server_tick` |
| 8 | 2 | u16 | `command_count` (0 … 32) |
| 10 | 2 | u16 | reserved = 0 |
| 12 | … | repeat | `command_count` × COMMAND (as §6.19) |

Max len = `12 + 32 × 32 = 1036`. `life_generation` is the controlled
entity-life generation at production; a command for a stale life is rejected
`XTK_ERR_STALE_LIFE` even inside the 15-tick expiry window. The peer never
supplies an authoritative vehicle slot.

### 6.21 VIEW_SNAPSHOT (0x40) / VIEW_KEYFRAME (0x30)

`VIEW_KEYFRAME` is the reliable channel-0 initial-view barrier; it carries the
**identical payload layout** as `VIEW_SNAPSHOT` and must have
`XTK_FLAG_KEYFRAME` set. Subsequent per-tick views are `VIEW_SNAPSHOT` on
channel 1. A lost first channel-1 view cannot strand the client because the
barrier is reliable and per-recipient ordered.

Header (32 bytes):

| Off | Size | Type | Field |
| --: | --: | --- | --- |
| 0 | 4 | u32 | `snapshot_sequence` (per epoch, from 0) |
| 4 | 4 | u32 | `server_tick` (= envelope tick) |
| 8 | 2 | u16 | `flags` (`XTK_FLAG_KEYFRAME` bit 0) |
| 10 | 1 | u8 | `own_slot` |
| 11 | 1 | u8 | reserved = 0 |
| 12 | 4 | u32 | `own_generation` |
| 16 | 2 | u16 | `vehicle_count` (0 … 16) |
| 18 | 2 | u16 | `bullet_count` (0 … 511) |
| 20 | 2 | u16 | `explosion_count` (0 … 255) |
| 22 | 2 | u16 | `map_cell_count` (0 … 900) |
| 24 | 2 | u16 | `landmark_count` (0 … 50) |
| 26 | 1 | u8 | `radar_blip_count` (0 … 22) |
| 27 | 1 | u8 | `weapon_count` (0 … 6) |
| 28 | 4 | u32 | `map_revision` (recipient-local export metadata) |

`map_revision` stays recipient-local export metadata; it never discloses the
global mapper-update counter (D3-C).

Then, in order: SelfState (one), `vehicle_count × VehicleView`,
`bullet_count × BulletView`, `explosion_count × ExplosionView`,
`map_cell_count × MapCell`, `landmark_count × LandmarkView`,
`radar_blip_count × RadarBlip`.

SelfState (52 bytes, `SELF`):

| Off | Size | Type | Field |
| --: | --: | --- | --- |
| 0 | 1 | u8 | `slot` |
| 1 | 1 | u8 | reserved = 0 |
| 2 | 2 | u16 | `lifecycle_flags` |
| 4 | 8 | u16[4] | `armor` (front, back, left, right) |
| 12 | 2 | u16 | `fuel` |
| 14 | 2 | u16 | `heat` |
| 16 | 4 | u32 | `score` |
| 20 | 12 | u16[6] | `ammo` |
| 32 | 1 | u8 | `weapon_enabled_bitmap` |
| 33 | 1 | u8 | `weapon_count` |
| 34 | 12 | u16[6] | `reload` |
| 46 | 4 | u32 | `generation` |
| 50 | 2 | u8[2] | reserved = 0 |

VehicleView (48 bytes, `VISIBLE`):

| Off | Size | Type | Field |
| --: | --: | --- | --- |
| 0 | 1 | u8 | `slot` |
| 1 | 1 | u8 | reserved = 0 |
| 2 | 1 | u8 | `kind` |
| 3 | 1 | u8 | `flags` |
| 4 | 8 | f64 | `x` |
| 12 | 8 | f64 | `y` |
| 20 | 8 | f64 | `body_angle` |
| 28 | 8 | f64 | `turret_angle` |
| 36 | 2 | u16 | `body_picture` |
| 38 | 2 | u16 | `turret_picture` |
| 40 | 4 | u32 | `generation` |
| 44 | 4 | u8[4] | reserved = 0 |

BulletView (40 bytes, `VISIBLE`):

| Off | Size | Type | Field |
| --: | --: | --- | --- |
| 0 | 2 | u16 | `id` |
| 2 | 2 | u16 | `flags` |
| 4 | 8 | f64 | `x` |
| 12 | 8 | f64 | `y` |
| 20 | 8 | f64 | `heading` |
| 28 | 1 | u8 | `owner_slot` |
| 29 | 1 | u8 | `kind` |
| 30 | 2 | u16 | reserved = 0 |
| 32 | 4 | u32 | `generation` (bullet life generation) |
| 36 | 4 | u32 | `owner_generation` |

ExplosionView (28 bytes, `VISIBLE`):

| Off | Size | Type | Field |
| --: | --: | --- | --- |
| 0 | 2 | u16 | `id` |
| 2 | 1 | u8 | `kind` |
| 3 | 1 | u8 | `flags` |
| 4 | 8 | f64 | `x` |
| 12 | 8 | f64 | `y` |
| 20 | 4 | u32 | `generation` |
| 24 | 4 | u8[4] | reserved = 0 |

MapCell (4 bytes, `MAP` — unchanged): `grid_x u8`, `grid_y u8`, `flags u8`,
`type u8`.

LandmarkView (28 bytes, `VISIBLE`):

| Off | Size | Type | Field |
| --: | --: | --- | --- |
| 0 | 1 | u8 | `kind` |
| 1 | 1 | u8 | `grid_x` |
| 2 | 1 | u8 | `grid_y` |
| 3 | 1 | u8 | reserved = 0 |
| 4 | 8 | f64 | `x` |
| 12 | 8 | f64 | `y` |
| 20 | 2 | u16 | `id` |
| 22 | 2 | u16 | reserved = 0 |
| 24 | 4 | u8[4] | reserved = 0 |

RadarBlip (20 bytes, `RADAR` — unchanged): `x f64`, `y f64`, `age u8`,
`classification u8`, reserved u16.

## 7. Field authorization (unchanged; D3-C does not widen knowledge)

The v1.0 authorization table stands. `SELF` remains one private record;
`VISIBLE`/`MAP`/`RADAR` remain viewport-/knowledge-bounded; `PUBLIC` results
remain ≤ 4. The new identity widths (u32 generations, u16 content/landmark ids)
add lifetime precision only; they do not add occluded/special knowledge.
`docs/VIEW-v1.md` knowledge rules remain authoritative.

Special v1.1 semantics: removal from the world is communicated by a
`LOBBY_STATE` status change and by the vehicle's disappearance from subsequent
snapshots; there is no REMOVE event. Remote SCORE is still `SELF`-only until
`RESULTS`; spawn/transient events obey the accepted recipient table.

## 8. Session state machine

```
Disconnected → Handshake → Lobby → Running → Results → Lobby
                     ↘ (timeout/QUIT) Disconnected
```

| State | Allowed client messages | Server messages |
| --- | --- | --- |
| Handshake | HELLO | WELCOME / REJECT |
| Lobby | CONTENT_CATALOG ack, CONFIG_SELECT, READY, START_REQUEST (admin), RESET, QUIT | CONTENT_CATALOG, CONFIG_STATUS, LOBBY_STATE, START, EPOCH_BEGIN, VIEW_KEYFRAME, REJECT, PROTOCOL_ERROR |
| Running | INPUT_BATCH, RELIABLE_CMD, RESET (admin), QUIT | VIEW_SNAPSHOT, VIEW_KEYFRAME, EVENT, CMD_ACK, LOBBY_STATE, RESULTS, PROTOCOL_ERROR |
| Results | RESET (admin), QUIT | RESULTS (retained until RESET), LOBBY_STATE |

- A message invalid for the state is `XTK_ERR_BAD_STATE` with no effect.
- The bounded handshake deadline is `XTK_HANDSHAKE_TIMEOUT_MS` (3000 ms); an
  abandoned handshake/lobby peer slot is reclaimed after it. Only valid fresh
  traffic counts as alive; malformed/stale/replayed traffic never does (D3-E).
- The two-second valid-input timeout and neutral drive/fire policy are unchanged
  (`XTK_NEUTRAL_TIMEOUT_MS`); a timeout is a departure, not a death.
- Duplicate lobby names are rejected with `XTK_ERR_DUPLICATE_NAME`; connection
  identity is independent of names (D3-E).

## 9. Content identity (unchanged)

The content set, IDs, `content_set_digest` construction and mismatch behavior
are unchanged from v1.0 (§7 there). Full-width content IDs now survive START
(`vehicle_id u16`). No asset, geometry, preset or manifest byte changed in P3.3.

## 10. Error codes

v1.0 codes are retained unchanged. v1.1 adds:

| Code | Value | Meaning |
| --- | --: | --- |
| `XTK_ERR_STALE_LIFE` | 0x001F | command's entity-life generation is stale |
| `XTK_ERR_DUPLICATE_NAME` | 0x0020 | lobby name already in use |
| `XTK_ERR_NOT_READY` | 0x0021 | start requested before every human is ready |
| `XTK_ERR_STALE_CONFIG` | 0x0022 | config revision in the message has moved on |

## 11. Limits and worst-case sizes

Bounds are in `Protocol/limits.h` (v1.1 adds `XTK_MAX_HUMANS=2`,
`XTK_MAX_DRONES=2`, `XTK_SOLO_DRONES=1`, `XTK_LOBBY_ROSTER_MAX=2`,
`XTK_HANDSHAKE_TIMEOUT_MS=3000`).

| Message | Calculation | Payload | + envelope |
| --- | --- | ---: | ---: |
| HELLO | 41 + 32 | 73 | 105 |
| WELCOME | 9 | 9 | 41 |
| CONFIG_SELECT | 12 | 12 | 44 |
| CONFIG_STATUS | 16 | 16 | 48 |
| READY | 8 | 8 | 40 |
| START | 16 | 16 | 48 |
| START_REQUEST | 8 | 8 | 40 |
| LOBBY_STATE | 16 + 2×45 | 106 | 138 |
| RESULTS | 8 + 4×45 | 188 | 220 |
| EVENT | 25 + 32 | 57 | 89 |
| CMD_ACK | 4 + 32×12 | 388 | 420 |
| RELIABLE_CMD | 8 + 32×32 | 1032 | 1064 |
| INPUT_BATCH | 12 + 32×32 | 1036 | 1068 |
| PROTOCOL_ERROR | 12 | 12 | 44 |
| VIEW_* | see below | 33872 | 33904 |

VIEW_* worst case:

| Section | Records × bytes | Bytes |
| --- | ---: | ---: |
| header | 1 × 32 | 32 |
| SelfState | 1 × 52 | 52 |
| VehicleView | 16 × 48 | 768 |
| BulletView | 511 × 40 | 20440 |
| ExplosionView | 255 × 28 | 7140 |
| MapCell | 900 × 4 | 3600 |
| LandmarkView | 50 × 28 | 1400 |
| RadarBlip | 22 × 20 | 440 |
| **payload** | | **33872** |
| **message** | | **33904** |

`33904 ≤ 65536` (`XTK_MAX_MESSAGE_BYTES`). Reliable output is bounded per peer
(`XTK_MAX_RELIABLE_QUEUE_BYTES` = 256 KiB). All encoding is explicit
fixed-width; no native struct, pointer, allocator or host byte order is used.

## 12. Golden fixtures

`Tests/phase3/fixtures/protocol/` contains the committed v1.1 bytes listed in
its `README.md`, an independent decoder (`verify.py`) and the hand-authored
`expected-fields.txt`. `Tests/phase2/fixtures/protocol/` retains the v1.0
fixtures unchanged; `Tests/phase3/fixtures/protocol/v1.0-fixtures.sha256`
records their exact bytes. `Tests/phase3/{protocol.sh,codec.sh}` are the
acceptance drivers (Linux + MinGW codec, ASan/UBSan fuzz, explicit 1.0
rejection).

## 13. Deferred / not in v1.1

Gameplay ops, prediction/reconciliation, compression/delta/quantization,
mid-match join/resume, observers/chat, a REMOVE event, Windows hosting and any
knowledge-widening remain outside this freeze. Widening the schema or changing
a bound requires a new owner decision.
