# Xtank authorized view v1

**Frozen by P2.5, 2026-10-11.** Authority: `docs/PHASE2-PLAN.md` §3 ("Snapshot,
event and visibility contract"), `docs/PROTOCOL-v1.md` §4.19/§5 (the frozen
`VIEW_SNAPSHOT` schema and its field authorization), and `docs/PHASE2-DECISIONS.md`
D2-F. This document is the independently reviewable derivation of **which
authoritative fields a player is authorized to see, and from which simulation
record they are read**, for the protocol-v1 `VIEW_SNAPSHOT`.

It refines the plan's contract into exact, testable rules; it does not widen the
approved field set. An exporter that would exceed a `Protocol/limits.h` bound
stops (`SERVER_VIEW_ERR_OVERFLOW`) and requests a revised bound or chunking
design; it never truncates silently. Widening the field set is a schema change
and requires a new owner decision (D2-C).

The implementation is `Src/server_view.c` (`Src/Include/server_view.h`); the
acceptance fixture is `Tests/phase2/{view_fixture.c,view.sh}`.

---

## 1. Export timing and the read-only contract

- A snapshot is produced by `server_view_export()` **after `server_step()`
  completes**, so it observes the simulation-owned end-of-tick bookkeeping
  (`disc_clear_throwers()`, camouflage shadow advancement, the map-window
  knowledge commit) and any early game-over exit already applied by the tick.
- The exporter is **strictly read-only**. It dereferences existing `Vehicle`,
  `Loc`, `Mapper`, `Radar`, `Bullet`, `Exp` and `Combatant` values and copies
  them into the wire DTO. It **never**:
  - calls a draw primitive or a special draw callback (no `draw_*`, no
    `do_special`);
  - runs a special *model* action (`SP_update`/`SP_activate`/`SP_deactivate`) or
    any control/game-rule function;
  - draws on, or advances, the RNG;
  - allocates or mutates simulation state.
- The only non-simulation value read is the read-only S1.11 instrumentation
  counter `special_stats.mapper_updates` (used as `map_revision`, §6.4); it is
  incremented by the mapper's own model pass during `server_step()`, never by
  the exporter.
- Exporting a tick zero, one or many times leaves simulation state
  byte-identical. `Tests/phase2/view_fixture.c` proves this by hashing the
  relevant simulation globals before and after 0/1/5 exports.

## 2. Stable presentation keys and lifetime counters

Every entity is keyed by `(kind, id, generation)` and vehicles additionally by
their slot:

- **Vehicle** slot/id = `Vehicle.number` (equal to `Combatant.number`). Own
  player in `SelfState.slot` and `VIEW_SNAPSHOT.own_slot`.
- **Bullet / explosion** id = the address-free array index within
  `Bset.array` / `Eset.array` (the `list[]` entries point into those arrays), so
  no pointer is ever placed in a DTO.
- **Landmark** id = the grid cell key (`(grid_x * GRID_HEIGHT + grid_y) & 0xFF`),
  provisional until P2.6.
- **`generation`** is a provisional, read-only proxy in P2.5:
  `Combatant.deaths + 1` for a vehicle/own player (`deaths` is incremented once
  by `kill_vehicle()`), and the owner's value for a bullet; `0` when there is no
  owner. P2.6 replaces it with a dedicated export-side entity-lifetime counter
  for slot reuse; the *shape* of the key does not change.

A caller may pass `generation != 0` to `server_view_export()`; the export is
rejected (`SERVER_VIEW_ERR_GENERATION`) unless it matches the current value, so
a stale controller cannot query another life's authorized view.

## 3. The reference animation viewport (camera)

The legacy `animate()` terminal pass places the animation window at the
controlled vehicle and the *same* window rule is the authorization rule here,
derived from `update_screen_locs()` (`Src/update.c:934`) and the geometry in
`Src/Include/screen.h`:

- Logical animation window: `ANIM_WIN_WIDTH = ANIM_WIN_HEIGHT = 768`, over
  `NUM_BOXES = 4` boxes per side (`Src/Include/tanklimits.h`), so one box is
  `BOX_WIDTH = BOX_HEIGHT = ANIM_WIN_WIDTH / NUM_BOXES = 192` world units.
- The camera follows the controlled vehicle: `left_x = own.x - NUM_BOXES *
  BOX_WIDTH / 2`, i.e. a **half-extent of `ANIM_WIN_WIDTH / 2 = 384` world
  units** on each axis, centred on the controlled vehicle's `Loc` (`x`, `y`).
- A point `(x, y)` is **in view** iff

  ```
  own.x - HALF <= x < own.x + HALF
  own.y - HALF <= y < own.y + HALF          (HALF = server_view_half_extent())
  ```

  This is the world-space equivalent of the legacy per-terminal screen clip
  (`screen_x = x - camera_x` kept in `[0, ANIM_WIN_WIDTH)`), and it is the
  legacy animation view rule. It is **not** a wall-occlusion or line-of-sight
  rule; walls do not hide vehicles. Ties at the far edge (`x - own.x == HALF`)
  are excluded because the legacy window is half-open. `neutral` observer or
  `full_*` views are **not** exported.
- The camera centre is the controlled vehicle's current `Loc`. When the
  controlled vehicle is dead its `Loc` is frozen, which is the **death camera**
  (§5.4).

Reference points tested against the window:

| Entity | Point |
| --- | --- |
| VehicleView | `Vehicle.loc->x`, `loc->y` (body centre) |
| BulletView | `Bullet.loc->x`, `loc->y` |
| ExplosionView | `Exp.x`, `y` |
| LandmarkView | box centre `grid_x*BOX_WIDTH + BOX_WIDTH/2`, same for y |

## 4. Field/visibility authorization table

`SELF` and `VISIBLE`/`MAP`/`RADAR` are the classes from `PROTOCOL-v1.md` §5. The
"source" column is the exact simulation record/field the exporter reads.

### 4.1 `SelfState` — class `SELF` (one record, the controlled player only)

| Wire field | Source | Rule |
| --- | --- | --- |
| `slot` | `Vehicle.number` | own vehicle |
| `generation` | `Combatant.deaths + 1` | §2 |
| `lifecycle_flags` | `Vehicle.status & VS_MASK` | `VS_functioning..VS_permanently_dead` (bits 0..6) only |
| `armor[4]` | `Vehicle.armor.side[FRONT,BACK,LEFT,RIGHT]` | clamp to `u16` |
| `fuel` | `Vehicle.fuel` | clamp to `u16` |
| `heat` | `Vehicle.heat` | clamp to `u16` |
| `score` | `Combatant.score` | own owner |
| `ammo[6]` | `Vehicle.weapon[i].ammo` | `i < num_weapons`, else 0 |
| `weapon_enabled_bitmap` | `Vehicle.weapon[i].status & WS_on` | bit `i` |
| `weapon_count` | `Vehicle.num_weapons` | clamp to 6 |
| `reload[6]` | `Vehicle.weapon[i].reload_counter` | `i < num_weapons`, else 0 |
| `reserved[2]` | — | always 0 |

### 4.2 `VehicleView` — class `VISIBLE` (any vehicle in the viewport)

| Wire field | Source | Rule |
| --- | --- | --- |
| `slot` / `generation` | `Vehicle.number` / owner `deaths+1` | §2 |
| `kind` | `Vehicle.vdesc->body` | body/picture-table id |
| `flags` | `Vehicle.status & VS_MASK` | as §4.1 |
| `x` / `y` | `Vehicle.loc->x` / `y` | §3 viewport |
| `body_angle` | `Vehicle.vector.heading` | radians |
| `turret_angle` | `Vehicle.turret[0].angle`, else `vector.heading` | primary turret |
| `body_picture` | `Vehicle.vector.rot` | rotation index 0..15 |
| `turret_picture` | `Vehicle.turret[0].rot`, else 0 | primary turret |
| `reserved[4]` | — | always 0 |

A remote vehicle is exported **only** with this visual state. It carries no
armor, ammo, reload, fuel, heat, score, targeting, bot program, IFF or camo
bookkeeping; those fields do not exist in the DTO.

### 4.3 `BulletView` — class `VISIBLE`

| Wire field | Source | Rule |
| --- | --- | --- |
| `id` | `Bullet` array index | §2 |
| `flags` | — | always 0 |
| `x` / `y` | `Bullet.loc->x` / `y` | §3 viewport |
| `heading` | `atan2(yspeed, xspeed)` | 0 when both are 0 |
| `owner_slot` | `Bullet.owner->number`, else `0xFF` | |
| `owner_generation` | owner `deaths+1`, else 0 | §2 |
| `kind` | `Bullet.type` | weapon/bullet type |
| `reserved` | — | always 0 |

### 4.4 `ExplosionView` — class `VISIBLE`

| Wire field | Source | Rule |
| --- | --- | --- |
| `id` | `Exp` array index | §2 |
| `kind` | index of `Exp.obj` in `exp_obj[]` | 0xFF if unresolved |
| `flags` | — | always 0 |
| `x` / `y` | `Exp.x` / `y` | §3 viewport |
| `reserved[4]` | — | always 0 |

### 4.5 `LandmarkView` — class `VISIBLE`

Source is **`real_map` only inside the viewport** (currently visible geometry),
never the full map. A cell is exported when
`real_map[x][y].type != NORMAL` and its box centre is in view.

| Wire field | Source |
| --- | --- |
| `kind` | `real_map[x][y].type` |
| `id` | grid cell key (§2) |
| `x` / `y` | `x*BOX_WIDTH + BOX_WIDTH/2`, same for y |
| `reserved` / `reserved2[4]` | always 0 |

`LandmarkView` is the *currently visible* landmark set. It is deliberately
distinct from `MapCell` (§4.6), which is *remembered* knowledge and may be stale.

### 4.6 `MapCell` — class `MAP` (mapper knowledge only)

Source is **the controlled player's `Mapper` record** (`Vehicle.special[MAPPER]`),
never `real_map`. The mapper records what the player has explored and keeps
legitimate stale remembered walls until it revisits a box; mapper knowledge is
therefore **not** synonymous with currently visible geometry.

| Wire field | Source | Rule |
| --- | --- | --- |
| `grid_x` / `grid_y` | cell index | 0..29 |
| `flags` | see below | |
| `type` | `Mapper.map[x][y].type` | landmark/scroll/goal type |

A `Mapper.map[x][y]` cell is "known" (and therefore exported) iff any of its
persisted fields is non-zero:
`flags != 0 || type != 0 || team != 0 || strength != 0`. Unexplored cells are
**omitted**, not exported as empty.

`flags` bits (`Src/Include/server_view.h`):

| Bit | Name | Source bit |
| --: | --- | --- |
| 0 | `SERVER_MAPCELL_KNOWN` | always set on an exported cell |
| 1 | `SERVER_MAPCELL_NORTH_WALL` | `NORTH_WALL` |
| 2 | `SERVER_MAPCELL_WEST_WALL` | `WEST_WALL` |
| 3 | `SERVER_MAPCELL_NORTH_DEST` | `NORTH_DEST` |
| 4 | `SERVER_MAPCELL_WEST_DEST` | `WEST_DEST` |
| 5 | `SERVER_MAPCELL_TEAM_KNOWN` | `team != NEUTRAL` |
| 6..7 | reserved | 0 |

### 4.7 `RadarBlip` — class `RADAR`

Source is the controlled player's ordinary `Radar` record
(`Vehicle.special[RADAR]`) while that special is `SP_on`; otherwise no blips.

| Wire field | Source | Rule |
| --- | --- | --- |
| `x` / `y` | `Blip.x` / `Blip.y` | map-window coordinates (`grid2map()`, px) |
| `age` | `SERVER_RADAR_BORN - Blip.life` | ticks since acquisition, clamped `>= 0` |
| `classification` | — | `SERVER_RADAR_ANONYMOUS` (0) always |
| `reserved` | — | always 0 |

`Blip.flags` (the `ANY_VEHICLE` bitmask naming exactly which vehicles are in the
box) is **deliberately discarded**: anonymous blips must not carry precise live
identity or location. The ordinary radar draws anonymous blips; vehicle numbers
are only drawn by the observer `full_radar()` view, which is never exported.
Expired contacts are already removed from `Radar.num_blips` by the radar model
pass, so they simply do not appear.

### 4.8 `PUBLIC` scoreboard

Not part of `VIEW_SNAPSHOT`. Per `PROTOCOL-v1.md` §5, public score/winner/name
travel in `RESULTS` (P2.6). `VIEW_SNAPSHOT` carries no per-vehicle private or
scoreboard fields.

## 5. Boundary and lifecycle rules

### 5.1 No full `real_map`

`real_map` is read **only** at the viewport-limited landmarks in §4.5. It is
never copied wholesale, never used for `MapCell`, and never used to reveal
unexplored knowledge or remote private state.

### 5.2 Remote private state

No remote `armor`, `ammo`, `reload`, `fuel`, `heat`, `score`, camo/stealth
state, IFF keys, program/bot state or targeting is exported; the DTO has no
fields for them. A client cannot reconstruct them.

### 5.3 Unexplored vs remembered map

An unexplored mapper cell is absent from `MapCell`. A remembered cell survives
until the mapper revises it (including remembered walls that no longer exist in
`real_map`), which is the legacy `special_mapper()` model behaviour.

### 5.4 Death camera

When the controlled vehicle is not alive (its `VS_is_alive` bit has cleared and
it is on the dead list awaiting respawn):

- `SelfState` is still exported with `lifecycle_flags` showing not-alive and the
  frozen last armor/ammo/score;
- the camera centre is the dead vehicle's **frozen last `Loc`**, so the world
  viewport and landmarks remain the last authorized view;
- the dead vehicle is **not** included in `VehicleView` (only live vehicles
  are), and bullets/explosions continue to be exported if in view;
- mapper and radar records are not updated for a dead vehicle, so those panels
  freeze, exactly as the legacy per-vehicle specials do.

On respawn the same `Vehicle` slot is re-activated and `generation` advances
(a death occurred), so the client snaps rather than interpolating through death
(interpolation is a P2.11 client concern).

### 5.5 Observer / specials

New Radar, Tac Link, Camo and Rdf are disabled in the release preset; the
exporter reads only ordinary `Radar` and `Mapper`. This does **not** change
their oracle behaviour (they remain compiled and exercised by the frozen
reference).

## 6. Header fields

| Field | Rule |
| --- | --- |
| `snapshot_sequence` | caller-supplied, per epoch |
| `server_tick` | `frame` (the tick just completed) |
| `flags` | caller-supplied (`XTK_FLAG_KEYFRAME` on the epoch barrier) |
| `own_slot` / `own_generation` | controlled vehicle §4.1 |
| `vehicle_count` / `bullet_count` / `explosion_count` | exact appended counts |
| `map_cell_count` / `landmark_count` / `radar_blip_count` | exact appended counts |
| `weapon_count` | own `num_weapons`, clamp 6 |
| `map_revision` | `special_stats.mapper_updates` (read-only mapper model counter) |

## 7. Bounds and worst-case size

Counts never exceed the frozen `Protocol/limits.h` bounds (16 vehicles, 511
bullets, 255 explosions, 900 map cells, 50 landmarks, 22 radar blips, 6
weapons). The calculated worst case is exactly the `PROTOCOL-v1.md` §11 figure —
**28 492 payload bytes / 28 524 message bytes** — which is a single application
message (`<= XTK_MAX_MESSAGE_BYTES = 65 536`). `Tests/phase2/view_fixture.c`
encodes a bound-maximized snapshot through the independent P2.3 codec and
asserts the length.

## 8. Deferred

- True entity lifetimes/slot-reuse generations and events (`DEATH`, `RESPAWN`,
  `SCORE`, `MATCH_END`, transient effects) are **P2.6**; `generation` here is the
  provisional `deaths+1` proxy.
- Client-side interpolation/camera and stale-history rules are P2.11/P2.13.
- Any widening of the field set (e.g. precise contact identity) requires a new
  D2-C schema decision and an owner review of this table.