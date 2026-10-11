# Xtank wire protocol v1

**Frozen by P2.2, 2026-10-11. Status: awaiting owner protocol approval.**
Authority: `docs/PHASE2-PLAN.md` §3 (proposed protocol v1 contract) and
`docs/PHASE2-DECISIONS.md` D2-C (explicit binary protocol v1). This document
refines that contract into an exact, independently reviewable schema. It does
not redesign it; any change to a value or field below is a protocol change and
requires a new owner decision (D2-C).

Companion artifacts:

| Artifact | Role |
| --- | --- |
| `Protocol/limits.h` | frozen numeric bounds |
| `Protocol/wire.h` | frozen IDs, channels, ops, error codes |
| `Protocol/content-manifest.{h,c}` | content IDs and pinned SHA-256 identity |
| `Tests/phase2/fixtures/protocol/*` | golden wire bytes + independent verifier |

Nothing here is code: the codec lands in P2.3, the transport/admission in P2.7,
the view exporter in P2.5/P2.6 and the session in P2.8. This document is the
contract those tasks must implement.

The wire types are deliberately independent of the engine, SDL and ENet. No
native struct is copied, no enum/`long`/pointer/padding/host-endian bitset is
transmitted, and unknown gameplay messages are rejected rather than ignored.

---

## 1. Conventions

- **Byte order.** Every integer and float is encoded in network byte order
  (big-endian). Signed integers use two's complement. There is no padding: all
  fields are packed and offsets in this document are exact.
- **Fixed-width primitives.** `u8`, `u16`, `u32`, `u64` are unsigned; `i8`,
  `i16`, `i32`, `i64` are signed. They are encoded explicitly; the C names in
  `Protocol/wire.h` are labels only.
- **`f64`.** IEEE-754 binary64, serialized as its 8-byte big-endian bit pattern.
  `f64` carries world positions, body/turret headings and intent numeric
  operands, so input is never re-quantized in parity work. NaN and ±Inf are
  rejected on decode (`XTK_ERR_NONFINITE`). Snapshot `f64` values never feed
  back into physics.
- **Strings.** Length-prefixed: a `u8` byte count `n` (no terminator) followed
  by `n` ASCII bytes. `n` must not exceed `XTK_MAX_NAME_BYTES` (32) for names or
  `XTK_MAX_ERROR_TEXT_BYTES` (128) for error text. Empty is allowed where noted.
- **Counts.** Every count is bounded by a `Protocol/limits.h` constant. A
  decoder rejects an over-bound count before allocating (`XTK_ERR_MALFORMED` /
  `XTK_ERR_BAD_LENGTH`); it never allocates on an attacker-chosen length.
- **Bit 7 of `flags`** in any message is reserved and must be 0.

---

## 2. Envelope (`XTK_ENVELOPE_BYTES` = 32)

Every message begins with this fixed envelope. Offsets are from the start of
the message.

| Off | Size | Type | Field | Notes |
| --: | --: | --- | --- | --- |
| 0 | 4 | u32 | `magic` | `XTK_WIRE_MAGIC` = `0x58544B31` (`"XTK1"`) |
| 4 | 1 | u8 | `version_major` | `1` |
| 5 | 1 | u8 | `version_minor` | `0` |
| 6 | 1 | u8 | `message_type` | `enum XtkMessageType`; unknown ⇒ reject |
| 7 | 1 | u8 | `channel` | 0/1/2; must match the message type (§4) |
| 8 | 4 | u32 | `payload_length` | 0 … `XTK_MAX_PAYLOAD_BYTES` |
| 12 | 4 | u32 | `session_id` | 0 until WELCOME assigns one |
| 16 | 4 | u32 | `epoch` | 0 outside a running match |
| 20 | 4 | u32 | `message_sequence` | per-(session,channel) monotonic, §12.2 |
| 24 | 4 | i32 | `tick` | engine frame; 0 outside a match; never negative |
| 28 | 4 | u32 | `reserved` | must be 0 |
| 32 | … | — | `payload` | `payload_length` bytes, type-dependent |

Envelope validation, in order, before any payload is read:

1. `magic != XTK_WIRE_MAGIC` → drop; if a session is established send
   `PROTOCOL_ERROR` with `XTK_ERR_BAD_MAGIC`, else drop silently.
2. `version_major != 1 || version_minor != 0` → **exact version rejection**
   (`XTK_ERR_BAD_VERSION` / `XTK_ERR_VERSION_MISMATCH`, §13).
3. `payload_length > XTK_MAX_PAYLOAD_BYTES` or message shorter than
   `32 + payload_length` → `XTK_ERR_BAD_LENGTH`; the message has no effect.
4. `channel` is not the one assigned to `message_type` → `XTK_ERR_BAD_CHANNEL`.
5. `reserved != 0` → `XTK_ERR_MALFORMED`.
6. `tick < 0` while an epoch is running → `XTK_ERR_MALFORMED`.

---

## 3. Channels

| Ch | ENet mode | Carries |
| --: | --- | --- |
| 0 | reliable ordered | HELLO/WELCOME/REJECT/PING/PONG, CONTENT_CATALOG, CONFIG_*, READY, START, EPOCH_BEGIN, RESULTS, RESET, QUIT, EVENT, CMD_ACK, PROTOCOL_ERROR, RELIABLE_CMD |
| 1 | unreliable sequenced | VIEW_SNAPSHOT only |
| 2 | unreliable sequenced | INPUT_BATCH only |

A type on the wrong channel is discarded and reported once (§13). Channel 1 is
self-contained: each VIEW_SNAPSHOT is independently decodable, so loss needs no
delta-baseline repair. The first snapshot of an epoch carries `XTK_FLAG_KEYFRAME`
and is the reliable full-view barrier (also announced by `EPOCH_BEGIN`).

---

## 4. Message payloads

All offsets are payload-relative (after the 32-byte envelope). "Len" is the
fixed payload length; variable fields show the extension.

### 4.1 PING (0x04) / PONG (0x05) — len 4, ch0

| Off | Size | Type | Field |
| --: | --: | --- | --- |
| 0 | 4 | u32 | `nonce` (PONG echoes the PING value) |

### 4.2 HELLO (0x01) — ch0, client → server

| Off | Size | Type | Field |
| --: | --: | --- | --- |
| 0 | 4 | u32 | `client_nonce` |
| 4 | 1 | u8 | `proto_major` (= 1) |
| 5 | 1 | u8 | `proto_minor` (= 0) |
| 6 | 2 | u16 | `client_build` |
| 8 | 1 | u8 | `name_len` (0 … 32) |
| 9 | n | bytes | `name` |
| 9+n | 32 | u8[32] | `content_set_digest` (§10) |

Len = `41 + name_len`. `content_set_digest` is compared to
`xtk_content_set_digest`; inequality is rejected before any catalog is sent.

### 4.3 WELCOME (0x02) — ch0, server → client, len 8

| Off | Size | Type | Field |
| --: | --: | --- | --- |
| 0 | 4 | u32 | `server_nonce` |
| 4 | 1 | u8 | `role` (0 observer, 1 player) |
| 5 | 1 | u8 | `tick_hz` (= 15) |
| 6 | 1 | u8 | `max_players` (= 4) |
| 7 | 1 | u8 | `reserved` = 0 |

`session_id` is assigned in the envelope of WELCOME.

### 4.4 REJECT (0x03) — ch0, server → client

| Off | Size | Type | Field |
| --: | --: | --- | --- |
| 0 | 2 | u16 | `error_code` (`enum XtkErrorCode`) |
| 2 | 2 | u16 | `detail_code` (context-specific; 0 if none) |
| 4 | 1 | u8 | `text_len` (0 … 128) |
| 5 | n | bytes | `text` (ASCII, human-readable) |

Len = `5 + text_len`. Sent with `session_id` 0 when the session is not yet
established, otherwise the assigned session.

### 4.5 CONTENT_CATALOG (0x10) — ch0, server → client

| Off | Size | Type | Field |
| --: | --: | --- | --- |
| 0 | 32 | u8[32] | `content_set_digest` |
| 32 | 2 | u16 | `entry_count` (0 … 8) |
| 34 | … | repeat | `entry_count` × CONTENT_ENTRY |

CONTENT_ENTRY:

| Off | Size | Type | Field |
| --: | --: | --- | --- |
| 0 | 2 | u16 | `id` (stable; §10) |
| 2 | 1 | u8 | `kind` (`enum XtkContentKind`) |
| 3 | 1 | u8 | `name_len` (0 … 32) |
| 4 | n | bytes | `name` |
| 4+n | 32 | u8[32] | `source_sha256` |

Max len = `34 + 8 × 68` = 578.

### 4.6 CONFIG_SELECT (0x11) — ch0, client → server, len 8

| Off | Size | Type | Field |
| --: | --: | --- | --- |
| 0 | 2 | u16 | `maze_id` (`XTK_CONTENT_ID_COMBAT1` / `_ARENA`) |
| 2 | 2 | u16 | `vehicle_id` (`XTK_CONTENT_ID_VANGUARD`) |
| 4 | 4 | u32 | `reserved` = 0 |

IDs must be in the server allowlist; an unknown ID is
`CONFIG_STATUS.status != 0` with `XTK_ERR_BAD_CONTENT_ID` and no state effect.

### 4.7 CONFIG_STATUS (0x12) — ch0, server → client, len 8

| Off | Size | Type | Field |
| --: | --: | --- | --- |
| 0 | 2 | u16 | `maze_id` (echo) |
| 2 | 2 | u16 | `vehicle_id` (echo) |
| 4 | 2 | u16 | `status` (0 accepted) |
| 6 | 2 | u16 | `error_code` (`XTK_ERR_*`) |

### 4.8 READY (0x13) — ch0, client → server, len 0
### 4.9 START (0x14) — ch0, server → client, len 8

| Off | Size | Type | Field |
| --: | --: | --- | --- |
| 0 | 4 | u32 | `start_tick` (first engine tick of the epoch) |
| 4 | 2 | u16 | `maze_id` |
| 6 | 1 | u8 | `vehicle_id` |
| 7 | 1 | u8 | `reserved` = 0 |

### 4.10 EPOCH_BEGIN (0x15) — ch0, server → client, len 8

| Off | Size | Type | Field |
| --: | --: | --- | --- |
| 0 | 4 | u32 | `epoch` (must equal the envelope `epoch`) |
| 4 | 4 | u32 | `first_tick` |

Reliable epoch barrier. The client clears snapshot/event history on receipt and
expects the next channel-1 snapshot to be a keyframe.

### 4.11 RESULTS (0x16) — ch0, server → client

| Off | Size | Type | Field |
| --: | --: | --- | --- |
| 0 | 4 | u32 | `end_tick` |
| 4 | 1 | u8 | `result_kind` (0 win, 1 abort) |
| 5 | 1 | u8 | `winner_slot` (0xFF = none) |
| 6 | 1 | u8 | `vehicle_count` (0 … 4) |
| 7 | 1 | u8 | `reserved` = 0 |
| 8 | … | repeat | `vehicle_count` × RESULT_ENTRY |

RESULT_ENTRY:

| Off | Size | Type | Field |
| --: | --: | --- | --- |
| 0 | 1 | u8 | `slot` |
| 1 | 1 | u8 | `generation` |
| 2 | 2 | u16 | `reserved` = 0 |
| 4 | 4 | u32 | `score` |
| 8 | 1 | u8 | `name_len` |
| 9 | n | bytes | `name` |

Captured while scores still exist, delivered reliably, retained until RESET.
Max len = `8 + 4 × 41` = 172.

### 4.12 RESET (0x17) — ch0, client → server, len 8

| Off | Size | Type | Field |
| --: | --: | --- | --- |
| 0 | 4 | u32 | `prior_epoch` |
| 4 | 4 | u32 | `reserved` = 0 |

Only the administrator (first peer, D2-F) may send it; otherwise
`XTK_ERR_NOT_ADMIN` and no effect. The server answers with a fresh START and
EPOCH_BEGIN for `prior_epoch + 1`.

### 4.13 QUIT (0x18) — ch0, client → server, len 0

### 4.14 EVENT (0x20) — ch0, server → client

| Off | Size | Type | Field |
| --: | --: | --- | --- |
| 0 | 4 | u32 | `event_sequence` (per epoch, from 0) |
| 4 | 1 | u8 | `event_type` (`enum XtkEventType`) |
| 5 | 1 | u8 | `entity_kind` (`enum XtkEntityKind`) |
| 6 | 2 | u16 | `entity_id` |
| 8 | 4 | u32 | `entity_generation` |
| 12 | 4 | u32 | `aux0` (type-specific) |
| 16 | 4 | u32 | `aux1` (type-specific) |
| 20 | 1 | u8 | `text_len` (0 … 32, usually 0) |
| 21 | n | bytes | `text` |

Dedup key = `(epoch, event_sequence)`; duplicates are ignored. `tick` is the
envelope `tick`. `aux0`/`aux1` per type: DEATH (`aux0` killer slot),
RESPAWN (`aux0` spawn x, `aux1` spawn y), SCORE (`aux0` delta, `aux1` total),
EXPLOSION (`aux0` kind), HIT (`aux0` side mask, `aux1` damage), MATCH_END
(`aux0` winner slot). Entities carry the stable `(kind, id, generation)` key.
Max len = 53.

### 4.15 CMD_ACK (0x21) — ch0, server → client

| Off | Size | Type | Field |
| --: | --: | --- | --- |
| 0 | 2 | u16 | `count` (0 … 32) |
| 2 | 2 | u16 | `reserved` = 0 |
| 4 | … | repeat | `count` × ACK_ENTRY |

ACK_ENTRY:

| Off | Size | Type | Field |
| --: | --: | --- | --- |
| 0 | 4 | u32 | `command_id` |
| 4 | 4 | u32 | `assigned_tick` |
| 8 | 1 | u8 | `status` (`enum XtkAckStatus`) |
| 9 | 1 | u8 | `reason` (`XTK_ERR_*`) |
| 10 | 2 | u16 | `reserved` = 0 |

Distinguishes accepted / applied / rejected and reports the assigned tick.
Max len = `4 + 32 × 12` = 388.

### 4.16 PROTOCOL_ERROR (0x22) — ch0, server → client, len 12

| Off | Size | Type | Field |
| --: | --: | --- | --- |
| 0 | 2 | u16 | `error_code` |
| 2 | 2 | u16 | `detail` |
| 4 | 4 | u32 | `offending_sequence` (0 if not applicable) |
| 8 | 2 | u16 | `offending_type` |
| 10 | 2 | u16 | `reserved` = 0 |

### 4.17 RELIABLE_CMD (0x23) — ch0, client → server

Reliable companion for one-shot fire and weapon toggles, under the same
`command_id` space as INPUT_BATCH (§9). Duplicate suppression is shared.

| Off | Size | Type | Field |
| --: | --: | --- | --- |
| 0 | 4 | u32 | `last_observed_server_tick` |
| 4 | 2 | u16 | `command_count` (0 … 32) |
| 6 | 2 | u16 | `reserved` = 0 |
| 8 | … | repeat | `command_count` × COMMAND |

Max len = `8 + 32 × 28` = 904.

### 4.18 INPUT_BATCH (0x60) — ch2, client → server

| Off | Size | Type | Field |
| --: | --: | --- | --- |
| 0 | 4 | u32 | `packet_sequence` (per session, monotonic; §12.3) |
| 4 | 4 | u32 | `last_observed_server_tick` (freshness hint) |
| 8 | 2 | u16 | `command_count` (0 … 32) |
| 10 | 2 | u16 | `reserved` = 0 |
| 12 | … | repeat | `command_count` × COMMAND |

Each batch repeats the previous `XTK_INPUT_REDUNDANCY_TICKS` (2) tick batches,
so a batch may carry up to three ticks of commands within the 32-command bound.
Redundancy is freshness, not a delivery guarantee.

COMMAND (28 bytes):

| Off | Size | Type | Field |
| --: | --: | --- | --- |
| 0 | 4 | u32 | `command_id` (client-owned, unique in the session) |
| 4 | 4 | u32 | `tick_hint` (client tick at production) |
| 8 | 1 | u8 | `op` (`enum XtkCommandOp`) |
| 9 | 1 | u8 | `flags` (`XTK_FLAG_EDGE` / `_HELD` / `_RELIABLE`) |
| 10 | 2 | u16 | `reserved` = 0 |
| 12 | 8 | f64 | `a` |
| 20 | 8 | f64 | `b` |

Max len = `12 + 32 × 28` = 908.

### 4.19 VIEW_SNAPSHOT (0x40) — ch1, server → client

Header:

| Off | Size | Type | Field |
| --: | --: | --- | --- |
| 0 | 4 | u32 | `snapshot_sequence` (per epoch, from 0) |
| 4 | 4 | u32 | `server_tick` (must equal envelope `tick`) |
| 8 | 2 | u16 | `flags` (`XTK_FLAG_KEYFRAME` bit 0) |
| 10 | 1 | u8 | `own_slot` |
| 11 | 1 | u8 | `own_generation` |
| 12 | 2 | u16 | `vehicle_count` (0 … 16) |
| 14 | 2 | u16 | `bullet_count` (0 … 511) |
| 16 | 2 | u16 | `explosion_count` (0 … 255) |
| 18 | 2 | u16 | `map_cell_count` (0 … 900) |
| 20 | 2 | u16 | `landmark_count` (0 … 50) |
| 22 | 1 | u8 | `radar_blip_count` (0 … 22) |
| 23 | 1 | u8 | `weapon_count` (0 … 6) |
| 24 | 4 | u32 | `map_revision` (bumped when mapper knowledge changes) |

Then, in order: SelfState (always one), `vehicle_count` × VehicleView,
`bullet_count` × BulletView, `explosion_count` × ExplosionView,
`map_cell_count` × MapCell, `landmark_count` × LandmarkView,
`radar_blip_count` × RadarBlip.

SelfState (48 bytes, `SELF` — only the controlled player):

| Off | Size | Type | Field |
| --: | --: | --- | --- |
| 0 | 1 | u8 | `slot` |
| 1 | 1 | u8 | `generation` |
| 2 | 2 | u16 | `lifecycle_flags` |
| 4 | 8 | u16[4] | `armor` (front, back, left, right) |
| 12 | 2 | u16 | `fuel` |
| 14 | 2 | u16 | `heat` |
| 16 | 4 | u32 | `score` |
| 20 | 12 | u16[6] | `ammo` |
| 32 | 1 | u8 | `weapon_enabled_bitmap` |
| 33 | 1 | u8 | `weapon_count` |
| 34 | 12 | u16[6] | `reload` |
| 46 | 2 | u8[2] | `reserved` = 0 |

VehicleView (44 bytes, `VISIBLE` — any vehicle in the reference viewport):

| Off | Size | Type | Field |
| --: | --: | --- | --- |
| 0 | 1 | u8 | `slot` |
| 1 | 1 | u8 | `generation` |
| 2 | 1 | u8 | `kind` (body type id) |
| 3 | 1 | u8 | `flags` (lifecycle/visibility) |
| 4 | 8 | f64 | `x` |
| 12 | 8 | f64 | `y` |
| 20 | 8 | f64 | `body_angle` |
| 28 | 8 | f64 | `turret_angle` |
| 36 | 2 | u16 | `body_picture` (0 … 15) |
| 38 | 2 | u16 | `turret_picture` |
| 40 | 4 | u8[4] | `reserved` = 0 |

BulletView (32 bytes, `VISIBLE`):

| Off | Size | Type | Field |
| --: | --: | --- | --- |
| 0 | 2 | u16 | `id` (slot-lifetime id) |
| 2 | 2 | u16 | `flags` |
| 4 | 8 | f64 | `x` |
| 12 | 8 | f64 | `y` |
| 20 | 8 | f64 | `heading` |
| 28 | 1 | u8 | `owner_slot` |
| 29 | 1 | u8 | `owner_generation` |
| 30 | 1 | u8 | `kind` |
| 31 | 1 | u8 | `reserved` = 0 |

ExplosionView (24 bytes, `VISIBLE`):

| Off | Size | Type | Field |
| --: | --: | --- | --- |
| 0 | 2 | u16 | `id` |
| 2 | 1 | u8 | `kind` |
| 3 | 1 | u8 | `flags` |
| 4 | 8 | f64 | `x` |
| 12 | 8 | f64 | `y` |
| 20 | 4 | u8[4] | `reserved` = 0 |

MapCell (4 bytes, `MAP` — mapper knowledge only, never `real_map` in full):

| Off | Size | Type | Field |
| --: | --: | --- | --- |
| 0 | 1 | u8 | `grid_x` (0 … 29) |
| 1 | 1 | u8 | `grid_y` (0 … 29) |
| 2 | 1 | u8 | `flags` (known/stale/destructible wall bits) |
| 3 | 1 | u8 | `type` |

LandmarkView (24 bytes, `VISIBLE`):

| Off | Size | Type | Field |
| --: | --: | --- | --- |
| 0 | 1 | u8 | `kind` |
| 1 | 1 | u8 | `id` |
| 2 | 2 | u16 | `reserved` = 0 |
| 4 | 8 | f64 | `x` |
| 12 | 8 | f64 | `y` |
| 20 | 4 | u8[4] | `reserved` = 0 |

RadarBlip (20 bytes, `RADAR` — anonymous unless classification permits):

| Off | Size | Type | Field |
| --: | --: | --- | --- |
| 0 | 8 | f64 | `x` |
| 8 | 8 | f64 | `y` |
| 16 | 1 | u8 | `age` (ticks) |
| 17 | 1 | u8 | `classification` (anonymous / known) |
| 18 | 2 | u16 | `reserved` = 0 |

`PUBLIC` scoreboard truth is carried in RESULTS; no per-vehicle private
armor/ammo/targeting/bot state appears for a peer.

---

## 5. Field authorization

The exporter may only populate:

| Class | Fields | Bound |
| --- | --- | --- |
| `SELF` | SelfState: generation, armor, fuel, heat, score, ammo, weapon enabled/reload | one record |
| `VISIBLE` | VehicleView (any vehicle in the reference animation viewport, not private state), BulletView, ExplosionView, LandmarkView | viewport-bounded |
| `MAP` | MapCell from mapper knowledge (explored, including legitimate stale remembered walls) | ≤ 900 |
| `RADAR` | RadarBlip position/age/classification; no precise live identity on anonymous blips | ≤ 22 |
| `PUBLIC` | RESULTS score/winner/name | ≤ 4 |

The exact reference viewport/occlusion semantics and death-camera behavior are
derived from legacy evidence in P2.5 (`docs/VIEW-v1.md`) and approved by the
owner. P2.5 may narrow these bounds and field sets; it may not widen them
without a schema decision. An exporter that would exceed a bound stops and
requests a revised bound or chunking design (PHASE2-PLAN §6); it never
truncates silently.

---

## 6. Command op ranges and authorization

`a`/`b` are `f64`; they are rejected unless finite.

| Op | Value | `a` | `b` | Flags | Release |
| --- | --: | --- | --- | --- | --- |
| NONE | 0x00 | ignored | ignored | — | yes |
| DRIVE_REL | 0x01 | integral, −9 … 9 | 0 | held | yes |
| DRIVE_ABS | 0x02 | integral, 0 … 9 | 0 | held | yes |
| HEADING | 0x03 | radians, finite | 0 | one-shot | yes |
| FIRE | 0x04 | 0 | 0 | edge | yes |
| WEAPON | 0x05 | 0 … 5 | 0 off / 1 on / 2 fire | edge | yes |
| AIM_WORLD | 0x06 | world x, finite | world y, finite | one-shot | yes |
| AIM_TARGET | 0x07 | — | — | held | **test-only** |
| FIRE_HOLD | 0x08 | 0 / 1 | 0 | held | yes |
| CHASE | 0x09 | — | — | held | **test-only** |
| STANDOFF | 0x0A | — | — | held | **test-only** |
| HEARTBEAT | 0x0F | 0 | 0 | — | yes |

An op outside `XTK_OP_RELEASE_AUTHORIZED` on the network path is acknowledged
`XTK_ACK_REJECTED` / `XTK_ERR_UNAUTHORIZED_OP` and changes no state. The
test-only ops remain available only through the controlled tick-indexed replay
harness (P2.15), never a network client.

---

## 7. Content identity

`Protocol/content-manifest.{h,c}` pins:

| Content | ID | Kind | SHA-256 source |
| --- | --- | --- | --- |
| `combat1` | 0x0101 | maze | `Mazes/combat1.m` bytes |
| `Arena` | 0x0102 | maze | `Mazes/Arena.m` bytes |
| `Vanguard` | 0x0201 | vehicle | `Vehicles/Vanguard.V` bytes |

plus the canonical release preset (`release-preset.txt`) and the frozen geometry
table (`geometry.golden.sha256`). The `content_set_digest` is:

```
SHA-256(
  "XTK-CONTENT-SET-v1\n" ||
  "maze combat1 <hex>\n" ||
  "maze Arena <hex>\n" ||
  "vehicle Vanguard <hex>\n" ||
  "config release <hex>\n" ||
  "geometry table <hex>\n" )
```

A digest mismatch in HELLO or CONTENT_CATALOG is `XTK_ERR_CONTENT_MISMATCH`,
fails before start, and triggers no download. IDs are stable and never list
indices; the server owns maze/vehicle selection and validates IDs against the
allowlist.

---

## 8. Scheduling and acknowledgments

- Admission happens at one bounded pre-step point. Newly accepted commands are
  assigned to the **next unconsumed engine tick**, keeping admitted order.
  `tick_hint` is a freshness hint only: a client may not backdate or reserve an
  arbitrary future tick.
- The server acknowledges each command with `(command_id, assigned_tick,
  status, reason)`. `status` is accepted, applied or rejected.
- Duplicates are suppressed across packet and channel boundaries by
  `(session_id, command_id)`; a ch2 copy may apply first, then the reliable ch0
  copy is acknowledged as `XTK_ERR_DUPLICATE` and never applies twice. A
  delayed reliable edge older than `XTK_CMD_EXPIRY_TICKS` past its assigned
  tick, or from a prior epoch/generation, is rejected rather than fired after
  respawn/reset.
- Held fire/drive/aim state is refreshed every tick; edge actions keep a unique
  `command_id`.
- A packet sequence is never mapped directly to the engine's
  `Intent.sequence`. The peer never supplies an authoritative player/vehicle
  slot; the session maps connection → controller → current vehicle slot, and
  commands name the controlled generation and epoch.

---

## 9. Session state machine

```
Disconnected → Handshake → Lobby → Running → Results → Lobby
                     ↘ (timeout/QUIT) Disconnected
```

| State | Allowed client messages | Server messages |
| --- | --- | --- |
| Handshake | HELLO | WELCOME / REJECT |
| Lobby | CONTENT_CATALOG ack, CONFIG_SELECT, READY, QUIT | CONTENT_CATALOG, CONFIG_STATUS, START, EPOCH_BEGIN, REJECT, PROTOCOL_ERROR |
| Running | INPUT_BATCH, RELIABLE_CMD, RESET (admin), QUIT | VIEW_SNAPSHOT, EVENT, CMD_ACK, RESULTS, PROTOCOL_ERROR |
| Results | RESET (admin), QUIT | RESULTS (retained until RESET) |

A message invalid for the state is `XTK_ERR_BAD_STATE` with no effect. A second
peer is rejected (`XTK_ERR_SESSION_FULL`) in the Phase 2 slice. Two seconds
without valid traffic triggers neutral drive/fire, session abort, cleanup and a
return to lobby (D2-F); the abort is an abort result, not a death, so it never
awards score or leaves a respawn pending.

---

## 10. Sequences, epochs and wrap/reset policy

Engine `tick` stays a signed `int`; an epoch ends before any overflow rather
than changing simulation arithmetic.

| Field | Owner | Scope | Wrap / reset |
| --- | --- | --- | --- |
| `session_id` | server | process | monotonic u32; never reuse while live; on wrap require no live collision |
| `epoch` | server | session | +1 per START/RESET; not reused while pending events/snapshots reference it; wrap forces session reset |
| `message_sequence` | sender | (session, channel) | +1 per sent message; reliable gaps are an error; modular compare; not used for command dedup |
| `packet_sequence` | client | session, ch2 | monotonic; newer-wins window; duplicated/older ignored |
| `command_id` | client | session | monotonic; dedup window `XTK_CMD_DEDUP_WINDOW` (256); stale after `XTK_CMD_EXPIRY_TICKS` |
| `event_sequence` | server | epoch | from 0, +1 per event; dedup key `(epoch, event_sequence)`; epoch ends before wrap |
| `snapshot_sequence` | server | epoch | from 0, +1 per snapshot; client drops non-newer; epoch ends before wrap |
| `tick` | server | process | signed i32; epoch closed before `INT32_MAX` |

On epoch change (START/RESET) the client clears the snapshot ring
(`XTK_SNAPSHOT_RING` = 8) and pending events (`XTK_MAX_PENDING_EVENTS`),
discards older-epoch messages, and waits for the keyframe barrier. The server
discards commands and events from a prior epoch.

---

## 11. Limits and worst-case sizes

Bounds are in `Protocol/limits.h`. Worst-case message sizes with the maximum
counts:

| Message | Calculation | Payload | + envelope |
| --- | --- | ---: | ---: |
| HELLO | 41 + 32 (name max) | 73 | 105 |
| CONTENT_CATALOG | 34 + 8×68 | 578 | 610 |
| RESULTS | 8 + 4×41 | 172 | 204 |
| EVENT | 20 + 32 | 52 | 84 |
| CMD_ACK | 4 + 32×12 | 388 | 420 |
| RELIABLE_CMD | 8 + 32×28 | 904 | 936 |
| INPUT_BATCH | 12 + 32×28 | 908 | 940 |
| VIEW_SNAPSHOT | see below | 28 492 | 28 524 |

VIEW_SNAPSHOT worst case:

| Section | Records × bytes | Bytes |
| --- | ---: | ---: |
| header | 1 × 28 | 28 |
| SelfState | 1 × 48 | 48 |
| VehicleView | 16 × 44 | 704 |
| BulletView | 511 × 32 | 16 352 |
| ExplosionView | 255 × 24 | 6 120 |
| MapCell | 900 × 4 | 3 600 |
| LandmarkView | 50 × 24 | 1 200 |
| RadarBlip | 22 × 20 | 440 |
| **payload** | | **28 492** |
| **message** | | **28 524** |

`28 524 ≤ 65 536` (`XTK_MAX_MESSAGE_BYTES`), so the maximum authorized view is
a single application message. Reliable output is bounded per peer
(`XTK_MAX_RELIABLE_QUEUE_BYTES` = 256 KiB); saturation aborts the solo session
with a reason rather than buffering without bound. At most the newest unsent
snapshot is retained. If ENet fragmentation proves unreliable in testing
(P2.7), a multi-datagram view must be applied atomically or a revised bound
approved before proceeding.

---

## 12. Rejection and error semantics

| Condition | Result | Effect |
| --- | --- | --- |
| bad magic | drop | none |
| version major/minor ≠ 1.0 | REJECT `XTK_ERR_VERSION_MISMATCH` (pre-session) / PROTOCOL_ERROR | none |
| payload length over bound / short | PROTOCOL_ERROR `XTK_ERR_BAD_LENGTH` | none |
| wrong channel | PROTOCOL_ERROR `XTK_ERR_BAD_CHANNEL` | none |
| unknown message type | PROTOCOL_ERROR `XTK_ERR_BAD_TYPE` | none |
| nonzero reserved | PROTOCOL_ERROR `XTK_ERR_MALFORMED` | none |
| content digest mismatch | REJECT `XTK_ERR_CONTENT_MISMATCH` | no start |
| NaN/±Inf operand | ack rejected `XTK_ERR_NONFINITE` | none |
| operand out of op range | ack rejected `XTK_ERR_RANGE` | none |
| unauthorized op | ack rejected `XTK_ERR_UNAUTHORIZED_OP` | none |
| stale epoch/generation | ack rejected `XTK_ERR_STALE_EPOCH` / `XTK_ERR_STALE_SEQUENCE` | none |
| duplicate command_id | ack `XTK_ERR_DUPLICATE` | applied at most once |
| foreign vehicle target | REJECT/ack `XTK_ERR_FOREIGN_OWNER` | none |
| queue saturation | disconnect/abort `XTK_ERR_QUEUE_SATURATED` | session abort |
| no traffic for 2 s | abort `XTK_ERR_TIMEOUT` | session abort |

---

## 13. Golden fixtures

`Tests/phase2/fixtures/protocol/` contains committed golden bytes for HELLO,
WELCOME, REJECT (version and content), CONTENT_CATALOG, CONFIG_SELECT, START,
EPOCH_BEGIN, RESULTS, EVENT, CMD_ACK, RELIABLE_CMD, PROTOCOL_ERROR, INPUT_BATCH,
VIEW_SNAPSHOT and a cross-endian value fixture. Each `*.hex` is lowercase hex
(whitespace and `#` comments ignored). `expected-fields.txt` lists the decoded
value of every field by explicit offset, and `verify.py` is an independent
decoder (not the generator) that asserts the bytes against it. `gen_fixtures.py`
regenerates the hex for review; it is never the arbiter. P2.3's codec must
reproduce these bytes exactly.

Cross-endian fixture: a `u16` 0x0102, a `u32` 0x01020304, an `i32` −2 and an
`f64` 1.0 encode as `01 02`, `01 02 03 04`, `FF FF FF FE` and
`3F F0 00 00 00 00 00 00`; the host's native byte order cannot change them.

---

## 14. Deferred / not in v1

- Compression, quantization or delta snapshots (later measurement work).
- Multi-client sessions, mid-match join/resume, admin transfer, observers, chat
  and spectators (Phase 3).
- Prediction/reconciliation/lag compensation (explicitly out of scope).
- Target-aim/chase/standoff on the network path (test-only until their
  authorization and UI are separately specified).
- A complete `init_settings()` table hash; v1 pins the approved release override
  set and special preset through `release-preset.txt`. Widening this changes the
  content identity and is a synchronized content-version event.
