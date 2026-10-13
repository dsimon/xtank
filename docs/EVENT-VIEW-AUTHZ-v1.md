# Xtank event/view authorization addendum v1 (P3.8)

**Authority.** `docs/PHASE3-DECISIONS.md` **D3-C** (identity/barrier fixes and
event recipient table, approved 2026-10-12) and `docs/PHASE3-PLAN.md` §3 P3.8.
This document refines D3-C into the exact, testable per-recipient rules the
P3.8 exporter implements. It is the third leg of the frozen knowledge
contract alongside `docs/VIEW-v1.md` (field/visibility) and
`docs/PROTOCOL-v1.1.md` §6.16/§6.13 (wire schemas). It **does not widen** any
field set: a widening is a schema change and needs a new owner decision.

Implementation: `Src/server_events.c` (`Src/Include/server_events.h`) and the
per-recipient delivery in `Src/server_session.c`; `Src/server_view.c`
(`Src/Include/server_view.h`) for the view side. Acceptance:
`Tests/phase3/{events.sh,events_fixture.c}` and
`Tests/phase3/{view.sh,view_fixture.c}`.

The exporter and the delivery path are **strictly read-only** with respect to
the simulation. They dereference existing records, copy them into wire DTOs and
write only to their own export-side state. They never call a draw primitive, a
special model action, the RNG or a control/game-rule function, and they never
allocate or mutate simulation state.

---

## 1. Stable entity identity and generations

Every entity is keyed by `(epoch, kind, id, generation)`. The epoch is part of
the key so a stale identity can never alias a new one.

- **`generation` is a u32 entity-life counter owned by the exporter**
  (`server_events.c`), not a simulation field. It advances when a life
  begins/reuses a slot:
  - **Vehicle:** advances on every `activate_vehicle()` spawn. It is monotonic
    across epochs (never reset), so the same slot after a respawn or a rejoin
    reports a strictly greater generation.
  - **Bullet / explosion:** advance when a new life appears in an array slot
    (a presence change or a signature change at that index), so same-slot reuse
    after a disappearance is a new generation.
  - It is carried full-width (u32) in `SELF`, `VISIBLE`, and `RESULTS`; no
    truncation to u8 is permitted. A generation that would truncate or alias is
    a P3.8 stop condition.
- Binding generation (`ServerHumanBinding`) and entity-life generation are
  **independent identities**. View export validates the entity-life generation;
  command admission validates the binding generation. Neither substitutes for
  the other.
- `server_view_export(slot, generation, ...)` rejects a non-zero `generation`
  that does not match the current entity-life generation
  (`SERVER_VIEW_ERR_GENERATION`): a stale controller cannot query a later life's
  view, and there is no caller-supplied camera.

## 2. The reference viewport and the camera

The camera is **always the controlled recipient's own vehicle**. There is no
API to request an arbitrary centre or a full map.

- Half-open world square, half-extent `ANIM_WIN_WIDTH / 2 = 384` world units,
  centred on the controlled vehicle's `Loc` (docs/VIEW-v1.md §3):
  `own.x-HALF <= x < own.x+HALF`, same for `y`.
- When the controlled vehicle is dead its `Loc` is frozen; that frozen point is
  the recipient's camera for view export **and** for event authorization (the
  death camera, VIEW-v1.md §5.4).
- A recipient with no bound/controlled vehicle receives no snapshot and no
  viewport-authorized events (only public `MATCH_END`).

## 3. View knowledge (unchanged from VIEW-v1.md; P3.8 specifics)

- `MapCell` is the controlled player's own `Mapper` record only; `real_map` is
  read only for the viewport-limited `LandmarkView`. No full-map export.
- `RadarBlip.classification` is always `SERVER_RADAR_ANONYMOUS`; the
  `Blip.flags` vehicle-naming bitmask is discarded. Anonymous radar identity is
  never exported.
- `map_revision` is **recipient-local export metadata** (D3-C): it advances only
  when that controlled slot's remembered mapper knowledge changes (an
  export-side fingerprint). It never discloses the global mapper-update counter.
- `own_generation`, `VehicleView.generation`, `BulletView.generation`,
  `ExplosionView.generation`, `BulletView.owner_generation` are u32.
  `LandmarkView` carries the full `grid_x`/`grid_y` key and u16 `id`.

## 4. Event recipient table

Each captured event is delivered only to recipients authorized by D3-C. The
`SELF`/`VISIBLE`/`PUBLIC` column classifies the authorization source.

| Event (`event_type`) | Recipients | Authorization |
| --- | --- | --- |
| `SCORE` | **own only** | `SELF`: the event's vehicle id equals the recipient's controlled slot. Remote score is **not** broadcast; it is public only through `RESULTS`. |
| `DEATH` | victim **and** killer | participants: the dying vehicle's slot or the killer's slot equals the recipient's controlled slot. A non-participant learns only what the ordinary view shows (the vehicle disappearing, a visible explosion). |
| `RESPAWN` | owning recipient always; others only when the spawn point is in the recipient's reference viewport | `SELF` + `VISIBLE`: a remote spawn coordinate is delivered only when that spawn point lies in the recipient's half-open viewport (the legacy reference would have drawn it). |
| `EXPLOSION` | only when the explosion point is in the recipient's reference viewport | `VISIBLE` transient. No out-of-view transient, and no remote private effect. |
| `MATCH_END` | every admitted recipient | `PUBLIC` winner/result marker. |
| `HIT` | (reserved; not emitted in v1.1) | — |

The event DTO carries no remote armor/ammo/fuel/heat/score/targeting/camo/bot
field. Authorization reads only the export-side occurrence location captured
read-only at the mutation site (vehicle death/spawn `Loc`, explosion `x`/`y`).

## 5. Bounded delivery, cursors, retirement and slow-peer failure

Events are captured into a bounded per-epoch ring
(`SERVER_EVENTS_RING = 512`) and each receives a monotonic per-epoch **capture
ordinal**. Delivery is per recipient and bounded:

- Each recipient keeps `ev_cursor`: the next capture ordinal it will consider.
  A healthy recipient reads every ordinal once per tick and never falls behind.
- `ev_cursor < first_retained_ordinal` means one or more events were **retired**
  before that recipient read them. Retirement is never silent: the recipient is
  **failed explicitly** through the P3.7 departure path
  (`XTK_ERR_QUEUE_SATURATED`), so an overflow is never mistaken for delivery.
- A transport send failure on an event does **not** advance the cursor and does
  not consume a sequence number; the peer is failed rather than marking the
  event delivered.
- No required lifecycle/result event is ever dropped for a healthy recipient:
  the ring is sized above the worst-case single-tick production, and
  `MATCH_END`/`RESULTS` are also carried by the reliable control path, so a lost
  transient never loses the match outcome.

## 6. Per-recipient sequencing and skipped private events

`EVENT.event_sequence` is **recipient-local**: it counts the events actually
delivered to that recipient in that epoch, from 0, contiguous.

- A filtered (unauthorized) private event is **skipped without consuming a
  recipient-local sequence number**. The recipient therefore sees `0,1,2,…` and
  never observes a gap caused by filtering. A gap is never interpreted as
  packet loss, and a sequence number is never reused.
- The capture ordinal (authorization order) is not the wire sequence; it is
  export-internal. The recipient's `event_sequence` is what the client dedups
  and orders on.
- `capture_tick` is the **occurrence tick** written when the event is captured
  and is preserved byte-for-byte through delayed delivery; the envelope `tick`
  is the delivery frame. A delayed event therefore reports the tick it happened,
  not the tick it was sent.

## 7. Results

`RESULTS` are captured **once per epoch, before the per-match score reset**, by
the reviewed read-only hook in `game_finalize_scores()` (before the
`v->owner->score = 0` loop). They:

- include every required participant (all retained combatants, including
  departed ones, with their earned score/name and `XTK_FLAG_DEPARTED` where the
  P3.7 departure path applied it);
- carry the full-width u32 entity-life generation;
- are immutable copies (no pointer) and survive the score reset and subsequent
  departures until RESET, when the next epoch starts fresh.

## 8. Stop conditions

- Any private event/map/contact leak, or a remote private field in a DTO.
- Export or delivery that mutates simulation state, draws, advances the RNG, or
  reads/writes a control function.
- Generation truncation or alias; a stale generation that is accepted.
- Silent overflow/retirement, or a send failure marked delivered.
- An arbitrary-camera/full-map/anonymous-identity API.
