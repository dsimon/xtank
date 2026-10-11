# Phase 2 — Windows client and single-client vertical slice

**APPROVED — owner-approved 2026-10-11; execution authorized subject to the
gates below. Decisions D2-A–D2-F are ruled in `docs/PHASE2-DECISIONS.md`; pinned
dependency/toolchain versions are recorded in `P2.1-REPORT.md`.**

Prepared 2026-10-11. Planning allowance: **4–6 engineer-weeks**, subject to the
early protocol, human-binding and Windows-runtime gates below.

## 1. Goal, authority and inherited baseline

Deliver a native Windows 11 x64 client that connects directly to the existing
Linux authoritative server, selects approved supplied content in a basic lobby,
and completes and resets a Combat match against one `drone`, without X11.
The client owns presentation and controls; the server remains the sole owner of
movement, collisions, damage, RNG, bots, knowledge, respawn and scoring.

Authority, in order of relevance:

- `docs/PORT-SCOPE.md`, Phase 2: Windows client and single-client vertical slice.
- `docs/DECISIONS.md`: all eight unchanged owner approvals; the detailed approved
  release contract is in `docs/PHASE0-BRIEF.md` §3.
- `PHASE1-REPORT.md`, `docs/PHASE1-PLAN.md` and `AGENTS.md`: delivered core,
  invariants, frozen reference and task/evidence discipline.

**Resolve a labeling discrepancy:** `PHASE1-REPORT.md` and `AGENTS.md` describe
the frontend as Phase 3 in their hand-off summaries. This draft follows the
authoritative `PORT-SCOPE.md` Phase 2 heading. Phase 3 extends this slice to
two-PC multiplayer and resilience; it does not begin the Windows frontend.
The approved Linux-only MVP also overrides the scope survey's earlier suggestion
to include Windows hosting in Phase 3.

Phase 1 exits PASS according to its report. It supplies `Src/xtank-server`,
15 Hz stepping, the 128-entry intent queue, content loaders, geometry, cooperative
`drone`, knowledge/draw separation and `make phase1-check`. The actual oracle's
`--subject auto` selects the **null/headless candidate**, not the shipped server
CLI. Phase 2 must therefore test its actual network entry path as well.

Record the Phase 2 starting revision **and working-tree manifest** at execution;
do not assume all Phase 1 work is committed. Preserve the report's baseline-tree
provenance (`63016bad6781ea238c0e52ea2b6efda1b9a8db45`, reproducing the original
`14e6d42…` pin) and frozen goldens. The release identity remains
`Xtank Windows MVP 0.1.0`, with upstream provenance separately reported.

### Source-level gaps to close deliberately

1. `ServerPlayers` currently maps names to terminal slots, not authenticated
   connections. `init_combatants()` and `setup_combatant()` in `Src/setup.c`,
   and activation/death in `Src/vehicle.c`, still use `terminal[]` for humans.
   `activate_vehicle()` also distinguishes humans for teleport defaults through
   `Combatant.num_players`. A network human cannot simply be labeled a bot or
   given an allocated dummy terminal without proving legacy behavior.
2. `Intent.player` is effectively the legacy vehicle index. `intent_check()`
   checks existence; it does **not** prove that a peer owns that vehicle or
   comprehensively validate finite/ranged arguments. Ownership and wire validation
   must precede `intent_push()`.
3. `Intent.sequence` is an ordering key within a tick; pending duplicate checks
   are not a session-wide replay window. Introduce independent wire sequencing.
4. `GraphicsBackend` is a useful neutral primitive contract, but legacy
   `display.c` and specials render from authoritative globals. Installing SDL
   behind that contract alone does not create a snapshot-driven client.
5. `game_results()` finalizes and resets scores. Capture match results before
   destructive finalization/cleanup. No snapshot export or event stream exists.

## 2. Scope and exit gate

### Included

- One Linux server process, one world, **one connected human + one `drone`**.
  Ubuntu 24.04 x86-64 remains the supported simulation environment.
- Windows 11 x64 frontend; controlled direct IP/port connection to Linux over
  a local/private LAN. Linux client/loopback is a development harness. Windows
  loopback is possible with an independently running Linux VM/WSL guest and
  verified forwarding; WSL is not a required client runtime or hosting product.
- Combat/FFA, 15 Hz, respawn, individual winning score 1,000, bots may win.
  `server_config_release()`/`server_release_settings()` remain authoritative.
- Lobby selection of **`combat1` or `Arena`**; vehicle choice explicitly shows
  **Vanguard only**. No arbitrary settings or asset paths arrive from a client.
- Console, Mapper, HUD and ordinary Radar; New Radar, Tac Link, Camo and Rdf
  disabled only in the release preset. Preserve all oracle control scenarios.
- Keyboard drive/steer, world-coordinate mouse aim/fire, weapon enable/select;
  armor by side, ammo, fuel, heat, score, explored map, authorized radar contacts.
  Original 2D assets, rotations, offsets, appearance and world collision scale.
- Versioned intent/snapshot/event protocol, content identity, bounded transport,
  single-peer lobby/start/results/reset/quit and minimum ownership/view filtering.
  These prerequisites are needed now even though Phase 3 broadens session work.
- Installable client archive and reproducible Linux network-server package,
  dependency notices, launch instructions and owner acceptance evidence.

### Deferred to later phases

Two humans/0–2 drones and four-vehicle multiplayer acceptance; admin transfer;
mid-session departures with tank removal without kill credit; lobby rejoining;
30-minute impairment matrix and ten two-human matches belong to Phase 3.
Phase 2 must not claim those approved release criteria have passed.

Windows-hosted server/fibers and offline host-and-play remain deferred beyond
the Linux-only MVP. Also deferred: other modes/bots, editors/user imports,
dynamic bots, audio, observers/chat/shared crews, discovery/NAT/Internet service,
prediction/reconciliation/lag compensation and graphical redesign.

For the single-human slice, propose: idle clients send valid heartbeat/input
every tick; **two seconds** without valid traffic triggers neutral drive/fire,
abort of the solo session, cleanup and return to lobby. This reuses the approved
timeout but narrows its release departure policy because no human remains.
Owner approval is required for this phase-specific abort policy. Do not implement
departure via ordinary death if that awards score or permits unintended respawn.

### Exit — all required

1. A clean Windows 11 x64 machine unpacks the archive and starts/connects without
   a compiler, X server, Cygwin or other POSIX runtime. Server instructions
   demonstrably start the Ubuntu package with `DISPLAY` unset and no X service.
2. Through the client UI, select each maze and Vanguard, start, exercise drive,
   steering, turret aim/fire and weapon controls, see all required HUD/map/radar
   fields, experience death/respawn, and reach a correct human or bot winner at
   1,000. **Complete at least one full match on each maze**, then reset/start a
   further match in the same processes. A tick-cap abort is not a completed match.
3. Scripted fixtures separately prove wall/tank/bullet collisions, directional
   damage, mapper acquisition, radar sweep/contact expiry, death, respawn,
   score, win and reset; visual evidence associates those ticks with the UI.
4. Full Phase 1 exit gate and Phase 2 network-path exact-parity tests pass.
   Snapshot/event extraction is read-only; clients never simulate gameplay.
5. At client render rates 30 and 144 FPS, server advances **900 ticks in 60 s
   ±1**, with p99 total tick work (simulation + network integration + view export)
   **<66.7 ms** on a recorded host. Frozen client/output backpressure cannot
   stall the world; finite-tick replay state is unchanged by rendering/export.
6. Protocol/content mismatches, malformed packets, stale epochs, duplicates and
   foreign ownership attempts have explicit failures and no unauthorized state
   effect. Captured outbound views contain no unauthorized live knowledge.
7. Owner plays a solo match and approves controls, reference feel, appearance,
   content selection and installation. `PHASE2-REPORT.md` records versions,
   machine specs, evidence paths, pass/fail and the exact remaining Phase 3 work.

## 3. Owner decisions (D2-A–D2-F) — APPROVED 2026-10-11

These were **recommendations**, not amendments to the eight existing approvals.
The owner approved all six as rulings on 2026-10-11; the recorded disposition is
`docs/PHASE2-DECISIONS.md` and the frozen dependency/toolchain versions are in
`P2.1-REPORT.md`. The table below is retained as the rationale of record.

| Decision | Recommendation and rationale | Approval consequence / alternative |
| --- | --- | --- |
| D2-A: rendering/input | **SDL3**, C frontend, one window with logical panels, full-frame redraw. SDL handles Windows events, resize, DPI, timing and renderer fallback; Linux development can exercise the same frontend. Native Win32/GDI can draw the art but adds Windows-only lifecycle/input/font code and does not solve the snapshot boundary. | Pin a tested SDL3 release and runtime distribution. Choose native Win32 only with revised platform/test estimates. No OpenGL/Direct3D-specific client API is required. |
| D2-B: transport | **ENet over UDP**, direct IP/port, a pinned maintained upstream release. Reliable ordered control/events, unreliable sequenced full snapshots and bounded/redundant inputs fit `PORT-SCOPE.md`'s transport-library recommendation and Phase 3 impairment work. | Verify Windows/Linux build and license early. TCP is simpler for framing/reliability but loss stalls snapshots behind older bytes (head-of-line blocking); choosing TCP requires a documented controlled-slice limit and Phase 3 reassessment. Do not invent reliability over raw UDP or ship a second transport. |
| D2-C: wire/content | Small **explicit binary protocol v1**, fixed-width fields, network byte order, length/count bounds, SHA-256 content manifest; exact supported-version rejection initially. | Owner approves schema and supported op subset in P2.2. JSON is useful for fixture diagnostics, not raw native structs or an undefined gameplay API. Content mismatch fails before start, with no auto-download. |
| D2-D: Windows build | **MinGW-w64 x86-64 cross-build from Linux**, CMake for new client/common protocol/transport only; preserve GNUmakefile server/oracle builds. Pin toolchain, SDL and ENet; also build the frontend natively on Linux for tests. | Native Windows execution is mandatory; Wine is supplemental. MSVC is a possible later toolchain, not a second Phase 2 compatibility commitment. If cross-built DLL/CRT or SDL runtime fails on clean Windows, return to owner before changing primary toolchain. |
| D2-E: presentation timing | Full authorized snapshots at **15 Hz**; initial interpolation delay **one tick (66.7 ms)**, render target 60 FPS, independent 30/144 FPS checks. No extrapolation or local gameplay prediction. | Measure local-control latency; a two-tick buffer is a measured tradeoff requiring owner review, not a silent latency increase. Proposed healthy-LAN p95 input-to-visible-action budget is ≤250 ms, borrowing the release target; failure triggers review, not a physics rewrite. |
| D2-F: session/view minimum | One-peer lobby, first peer administers start/reset; server-enforced ownership and knowledge from the first live snapshot. Two-second solo abort as described above. Ship client visual assets and content IDs, not a full maze-state file. | Approve this slice policy and reference-derived camera/visibility contract. Phase 3 retains the full approved remaining-human departure policy. No omniscient temporary player view. |

### Proposed protocol v1 contract (P2.2 freezes the exact schema)

- Envelope: magic, major/minor, message type, payload byte length, session ID,
  match epoch, message sequence and simulation tick, all explicitly sized.
  Suggested ID/sequence/tick widths: u32, with connection/session lifetime and
  wrap rules defined; engine tick remains signed `int`, so end an epoch before
  overflow rather than changing simulation arithmetic. No `sizeof(struct)`
  transmission, native enum/long, pointers, padding or host-endian bitsets.
- Integers/counts are u8/u16/u32 or signed fixed-width as specified. World
  positions, headings and intent numeric operands use **IEEE-754 binary64 bit
  encoding**, serialized explicitly; reject NaN/Inf and per-op out-of-range
  values. This avoids new input quantization in parity tests. Snapshot fields
  never feed back into physics. Compression/quantization is later measurement work.
- Channel 0 reliable ordered: HELLO/WELCOME/REJECT, bounded content catalog and
  hashes, CONFIG_SELECT/READY/START, EPOCH_BEGIN, RESULTS/RESET/QUIT; gameplay
  events with `(epoch, event_sequence, tick, entity_id, generation)`.
- Channel 1 unreliable sequenced: self-contained authorized VIEW_SNAPSHOT.
  Initial epoch sends a reliable full view barrier; each later snapshot is
  independently decodable, so loss requires no delta-baseline repair.
- Channel 2 unreliable sequenced: input batches with heartbeat, packet sequence,
  last observed server tick and independent command sequence. Each batch repeats
  the previous two tick batches; the server deduplicates commands before admission.
  Held fire/drive/aim state is refreshed; edge actions retain a unique sequence.
  One-shot fire and weapon toggles also travel reliably on channel 0 under those
  same command IDs; a channel-2 duplicate can apply first but never twice. Define
  expiry and rejection for a delayed reliable edge rather than firing it after
  respawn. Redundancy alone is not a delivery guarantee. Do not map a packet
  sequence directly to `Intent.sequence`.
- Session maps connection → controller → current vehicle slot; the peer cannot
  supply an authoritative `Intent.player`. Commands name the controlled generation
  and epoch. Support drive-relative/absolute, heading, world aim, fire/hold and
  weapon enable/fire; target aim/chase/standoff stay test-only until their
  authorization and intended UI are separately specified.
- Live scheduling: at a bounded pre-step admission point, assign newly accepted
  commands to the next unconsumed engine tick; client tick is a freshness hint,
  not permission to backdate or reserve an arbitrary future tick. Preserve
  admitted command order. Acknowledgments distinguish accepted/applied/rejected
  command sequences and report the assigned tick. Suppress duplicates across
  packet boundaries; expire stale/repeated input and reject old generations.
  Exact tick-indexed replay scheduling is a **test-only** mode with controlled
  delivery, not a network client's privilege.
- Client and session queues have declared limits in addition to
  `INTENT_QUEUE_MAX=128`: propose ≤32 commands/batch, ≤64 KiB application message,
  ≤256 KiB reliable queued output/peer and bounded transport service work/tick.
  Reserve admission room for neutralization/release. Capacity rejects are
  observable; no unbounded buffering or silent loss of edge commands. Size
  calculations include 16 vehicles, 511 bullets, 255 explosions and map records;
  if authorized full views exceed bounds, stop and approve a revised bound or
  chunk/reassembly design. A multi-datagram view must be applied atomically;
  verify ENet fragmentation/unreliable behavior rather than assume it.
- Keep at most the newest unsent snapshot; never accumulate old snapshots.
  Reliable queue saturation disconnects/aborts the solo session with a reason.
  ENet service uses nonblocking calls and an explicit receive/work budget; audit
  its internal queues as well as application queues. No socket/DNS wait inside
  `server_step()`; no transport callback mutates engine objects asynchronously.
- Content catalog assigns explicit stable names/IDs (not list indices) to the
  two mazes and Vanguard. Hash exact supplied source bytes, the release-preset
  schema/configuration and the exported sprite/geometry manifest. The server
  owns selection and validates IDs against an allowlist. Client ships visual
  resources and a generated identity manifest, not raw `.m` files; authorized
  map/view records come from the server. Hashes establish version agreement,
  **not secrecy** of historically public supplied maps.

### Snapshot, event and visibility contract

Create protocol/client DTOs separate from `Vehicle`, `Loc`, `Mapper` and
`Radar`. Take an immutable export **after `server_step()` completes**, including
simulation-owned end-of-tick bookkeeping and early game-over exits. Never call
draw callbacks, update specials, RNG or control functions to obtain a view.

- Stable presentation key: `(epoch, entity kind, ID, generation)`. Maintain
  export-side lifetime counters for bullet/explosion slot reuse and vehicle
  respawn; do not derive identity from pointers. Test reuse within/between exports.
- Own-player record: current life/generation, position/body and turret rotations,
  armor, ammo/weapon enabled/reload state, fuel/heat, score and lifecycle flags.
  Other vehicles expose only reference-authorized visual state, not private
  armor/ammo/targeting/bot state. Public scoreboard fields are specified separately.
- World view: authorized geometry/walls/landmarks and visible vehicles, bullets
  and explosions for a reference-sized animation viewport around the controlled
  player; no client-provided arbitrary camera region. Map panel receives mapper
  knowledge, including legitimate stale remembered walls, not `real_map` in full.
  Radar receives existing sweep/blip position, age and allowed classification;
  do not attach precise live vehicle identity/location to anonymous blips.
- P2.5 inventories legacy `display.c`, camera dimensions/clipping, `mapper.c`,
  `radar.c` and HUD/console fields. Visibility is the legacy animation/view rule,
  not an invented wall-occlusion rule. No Camo/Rdf support is implied by this
  release, but their oracle behavior must remain intact. Owner reviews the field
  authorization table, including map and death-camera behavior, before export.
- Events: death, respawn, score change, match end and relevant transient visual
  effects, with stable dedup keys and authorized payloads. Export-side comparison
  can derive persistent transitions; fleeting within-tick effects need reviewed
  read-only capture at their existing mutation site. No premature disc/camo
  bookkeeping, altered collision timing or event-derived scoring.
- Results are copied while combatant scores still exist, delivered reliably,
  and retained until reset. Snapshots carry lifecycle/score truth so lost cosmetic
  events cannot leave stale entities. Event and snapshot channels may arrive in
  either order; apply by epoch/tick/generation, with bounded pending events.

Client keeps a bounded timestamp/tick snapshot ring (initially eight entries),
estimates a monotonic server timeline, and renders at estimated server time
minus the approved buffer. Interpolate positions and shortest-arc headings only
between matching authorized IDs/generations. Snap at respawn/teleport and clear
history on epoch/reset; never interpolate through death or newly hidden state.
Do not blend discrete armor, ammo, walls, scores or results. Freeze the last
authorized pose on underrun, display connection status, and resume from a fresh
view without extrapolating. Own tank follows this same initial policy; camera
and mouse transform use that displayed pose consistently. Measure the resulting
aim/control latency before proposing prediction.

## 4. Agent-sized task contracts

Every implementation task requires **G2** (§5) plus the checks below. S = narrow
session, M = normal session, L = full session; split an L task further if it
cannot finish with evidence in one session. Paths marked **new** are proposals,
not existing files. Each task has one coherent deliverable; no task authorizes
gameplay changes or execution before owner approval. All listed existing paths
are implementation touchpoints, not permission to rewrite entire modules.

### P2.1 — Freeze decisions and executable baseline [S]
- **Files:** this draft; `docs/PHASE2-DECISIONS.md`, `Tests/phase2/README.md` (new);
  existing authority docs and Phase 1 harness read-only.
- **Goal:** resolve D2-A–F, pin build/dependency versions, record Phase 1 manifest
  and execution environment, locate Windows acceptance machine and Linux host.
- **Acceptance:** owner approvals recorded; pre-execution `make phase1-check`
  passes in an isolated candidate; baseline evidence reproducible; 4–6-week
  allowance and Phase 3 boundary agreed. Missing tools/evidence block code work.
- **Constraints:** preserve all eight prior approvals and frozen references;
  no dependency substitution or retroactive baseline blessing.
- **Dependencies:** none; blocks every implementation task.

### P2.2 — Freeze wire schema and content identity [M]
- **Files:** `docs/PROTOCOL-v1.md`, `Protocol/{wire.h,limits.h,content-manifest.*}`,
  `Tests/phase2/fixtures/protocol/*` (new); Phase 1 content/geometry manifests read-only.
- **Goal:** specify exact envelope, IDs, field authorization, op ranges, channel
  use, sequences, epochs, scheduling, results and limits; provide golden bytes
  for HELLO, input, snapshot, event, errors and cross-endian values.
- **Acceptance:** independently reviewable field tables and encoded fixtures;
  explicit version/hash rejection; calculated worst-case message sizes and
  wrap/reset policy; unchanged source asset hashes; owner protocol approval.
- **Constraints:** wire types independent of engine/SDL/ENet headers; no native
  struct copying, auto-download or permissive unknown gameplay messages.
- **Dependencies:** P2.1.

### P2.3 — Portable codec and boundary tests [M]
- **Files:** `Protocol/{encode.c,decode.c,wire.h}`, `Tests/phase2/{codec.*,codec.sh}` (new).
- **Goal:** bounded encode/decode and exact binary64/integer serialization.
- **Acceptance:** golden-byte tests on Linux and MinGW target; truncation at
  every field, oversized counts, unsupported types/version, NaN/Inf, malformed
  strings, wrap and cross-endian fixtures rejected; decoder fuzz/sanitizer run
  with reproducible duration/seed has no invalid access/unbounded allocation.
- **Constraints:** codec does not touch simulation; round-trip tests alone are
  insufficient because shared encoder/decoder bugs can agree.
- **Dependencies:** P2.2; Windows execution check joins P2.9.

### P2.4 — Terminal-free human lifecycle and binding [L]
- **Files:** `Src/{server_core,server_main,setup,vehicle}.c`, `Src/Include/server.h`;
  `Src/server_players.c`, `Tests/phase2/{players.sh,players_fixture.c}` (new);
  inspect `gr.c`, `message.c`, `status.c`, `game.c` consumers.
- **Goal:** explicit human name/controller/vehicle binding for one human and
  one drone, preserving human flags and init/death/respawn semantics with zero
  terminals. Isolate legacy terminal-facing setup behind presentation gates.
- **Acceptance:** scripted human match initializes, dies, respawns, wins/resets
  twice; no terminal allocation/render/input calls; bot order and human defaults
  match independent frozen-X human traces; old X frontend still builds/passes G.
- **Constraints:** no fake `Terminal`, no human-as-bot shortcut, no controller
  reassignment to a respawned/other slot without generation validation; one world.
- **Dependencies:** P2.1, P2.2; early stop/go gate before full networking.

### P2.5 — Authorized read-only view exporter [L]
- **Files:** `Src/server_view.c`, `Src/Include/server_view.h`,
  `Tests/phase2/{view.sh,view_fixture.c}`, `docs/VIEW-v1.md` (new);
  `Src/{display,mapper,radar,hud,newconsole}.c` and their record headers read-only
  unless a separately reviewed const accessor is necessary.
- **Goal:** derive the field/visibility table and export complete per-player
  world, map/radar and own HUD DTOs without any simulation mutation.
- **Acceptance:** owner-reviewed reference view rules; parity of state before/
  after 0/1/multiple exports per tick; independent fixtures for viewport edges,
  unexplored/remembered map, anonymous/expired radar contacts and death camera;
  packet-field allowlist test; maximum authorized view fits approved limits.
- **Constraints:** no full `real_map` or hidden remote private fields; mapper
  knowledge is not synonymous with currently visible geometry; no rule updates.
- **Dependencies:** P2.2, P2.4.

### P2.6 — Entity lifetimes, events and immutable results [M]
- **Files:** `Src/server_events.c`, `Src/Include/server_events.h`,
  `Tests/phase2/{events.sh,events_fixture.c}` (new); `Src/{animate,vehicle,game}.c`
  only for reviewed read-only transient hooks; extend `server_view.c`.
- **Goal:** export generation-safe entities, ticked events and results captured
  before score reset, using simulation transitions as truth.
- **Acceptance:** forced slot reuse, death/respawn, two epochs and early win exits
  yield correct identities/results; duplicate/reordered events do not duplicate
  effects or score; exporter on/off/repeated state dumps are byte-identical.
- **Constraints:** event counters never change RNG/rules; no scoring from events;
  no pointers in DTOs, render callbacks or late reads after cleanup.
- **Dependencies:** P2.3, P2.5.

### P2.7 — Nonblocking transport and input admission [M]
- **Files:** `Transport/{enet_transport.c,transport.h}`,
  `Src/{server_net.c,Include/server_net.h}`,
  `Tests/phase2/{transport.sh,admission_fixture.c}` (new);
  `Src/server_loop.c` for bounded pre/post-step integration; `Src/intent.c` read-only
  unless a separately evidenced adapter seam is needed.
- **Goal:** bounded ENet service, connection ownership, finite/range validation,
  deduplication, assigned-tick acknowledgments and input-to-`intent_push` adapter.
- **Acceptance:** partial/lost/repeated batches, spoofed player/generation,
  queue saturation and stale epoch reject safely; no duplicate edge fire;
  normal release/neutralization cannot be starved; no-peer runs retain parity;
  paused reader for ten seconds cannot stall ticks or grow queues without bound.
- **Constraints:** engine accessed only on simulation thread; no blocking I/O,
  DNS inside ticks, raw UDP reliability implementation or modified consume order.
- **Dependencies:** P2.3, P2.4; snapshot delivery joins P2.6.

### P2.8 — Solo lobby and network-server path [M]
- **Files:** `Src/{server_session.c,Include/server_session.h}`,
  `Tests/phase2/{session.sh,session_fixture.c}` (new);
  `Src/{server_main,server_loop}.c`, `Bin/package-server.sh`, server build lists.
- **Goal:** optional listen mode preserving the Phase 1 local CLI; explicit
  Disconnected→Handshake→Lobby→Running→Results→Lobby flow, allowlisted content,
  ready/start/reset, one peer and one drone, epoch barriers and timeout/quit abort.
- **Acceptance:** mismatched handshake fails before match; second peer rejected;
  start/reset privileges enforced; no controls outside Running; timeout/focus
  release and repeated results/reset clean ownership; existing local package
  tests pass; network package starts with all display-related environment unset.
- **Constraints:** no mid-match join/resume or admin-transfer feature; no new
  gameplay defaults; ordinary completed matches and aborted sessions distinct.
- **Dependencies:** P2.6, P2.7.

### P2.9 — Cross-built Windows shell and native runtime proof [M]
- **Files:** `Client/{CMakeLists.txt,main.c,platform_sdl.c}`,
  root `CMakeLists.txt`, `cmake/mingw-w64.cmake`, `Tests/phase2/windows-smoke.ps1`
  (new); optional forwarding target in root `GNUmakefile`.
- **Goal:** SDL window/event/timer/renderer shell plus portable protocol/ENet
  libraries, built on Linux for Windows x64 with isolated outputs.
- **Acceptance:** native Windows opens, resizes, closes, receives keyboard/mouse,
  reports correct drawable/logical coordinates at 100/150/200% DPI, connects
  and exercises codec fixtures; clean-machine DLL/CRT dependency audit passes;
  Linux frontend build also succeeds. Record exact dependency/toolchain versions.
- **Constraints:** client links no simulation, bots, ucontext, X11 or Unix
  loaders; original server toolchain/flags unaffected. Native Windows evidence
  required before investing in the full frontend.
- **Dependencies:** P2.1, P2.3; connected smoke joins P2.7.

### P2.10 — Original visual assets and full-frame drawing [M]
- **Files:** `Client/{assets.c,render_sdl.c,render.h}`, `Tools/export-client-assets.*`,
  `Tests/phase2/assets.sh` (new); `Src/Display/Objects/*.obj`, `Src/Include/object.h`,
  `Src/geometry.c` and Phase 1 geometry evidence read-only.
- **Goal:** portable checked export of original body/turret/projectile/explosion
  and landmark visuals, palette/font and 16 body views; snapshot-driven drawing.
- **Acceptance:** dimensions, offsets, turret placement and rotation indexing
  compared to frozen X/server geometry; Marauder and Vanguard preserved; fixed
  visual fixtures show matching scale/art without trails after resize/expose.
- **Constraints:** no change to server collision assets; no screen coordinates
  in authoritative state. Use neutral primitive vocabulary where useful, but
  do not link legacy world-reading renderers. If reusing `GraphicsBackend`
  signatures, implement DRAW_XOR on client surfaces or prove those paths unused;
  never silently redefine XOR for legacy callers.
- **Dependencies:** P2.2, P2.9.

### P2.11 — Snapshot model, camera and interpolation [M]
- **Files:** `Client/{model.c,model.h,timeline.c,camera.c,world_render.c}`,
  `Tests/phase2/{timeline.*,timeline.sh}` (new).
- **Goal:** bounded authorized client model, epoch/lifetime handling, monotonic
  render timeline and the one-tick interpolation policy from §3.
- **Acceptance:** recorded views replay at 30/60/144 FPS; loss/reordering,
  underrun, teleport, death/respawn, hidden entity and reset fixtures show no
  stale ghosts, blending across lifetimes or extrapolation; camera follows own
  displayed pose with unchanged logical viewport/world scale.
- **Constraints:** no local movement/damage/RNG simulation; no arbitrary camera
  requests that expose more world data; presentation speed never schedules ticks.
- **Dependencies:** P2.5, P2.6, P2.10.

### P2.12 — Controls and exact world-intent transform [M]
- **Files:** `Client/{input.c,input.h,controls.c}`, `Tests/phase2/{input.*,input.sh}`
  (new); `Src/input.c:input_event_intent()` and legacy mappings read-only references.
- **Goal:** keyboard drive/heading, mouse world aim/fire and weapon controls,
  bounded tick-rate input refresh/redundancy and focus-loss release.
- **Acceptance:** inverse camera/viewport/DPI transform tested at corners,
  nonzero origin and scaled windows; aim/fire hit reference targets; click/hold/
  release and weapon toggles preserve edge order despite retransmission; idle
  heartbeat every tick; key repeat/144 FPS does not multiply command counts.
- **Constraints:** explicit op ranges and server ownership; no screen-relative
  coordinates sent to engine; no target-chase debugging controls in release UI.
- **Dependencies:** P2.7, P2.9, P2.11.

### P2.13 — Essential HUD, mapper and radar [M]
- **Files:** `Client/{hud.c,map.c,radar.c,console.c}`, `Tests/phase2/hud-fixtures/*`
  (new); legacy presentation sources read-only as specifications.
- **Goal:** show all approved console/HUD fields and mapper/radar knowledge,
  readable original-style panels and own lifecycle/winner indications.
- **Acceptance:** known armor/ammo/fuel/heat/score values match view fixtures;
  explored map and stale wall knowledge match reference; radar sweep/blip expiry
  and anonymous contacts work; death/reset clears only appropriate presentation
  history; owner reviews screenshots at normal and high DPI.
- **Constraints:** HUD never reads hidden model data or client-supplied maze files;
  no gameplay-special updates, expanded specials, chat or audio.
- **Dependencies:** P2.5, P2.10, P2.11.

### P2.14 — Connect/start/results/reset playable integration [M]
- **Files:** `Client/{connect_ui.c,lobby_ui.c,results_ui.c,main.c}`,
  `Tests/phase2/{slice.sh,scripted_peer.c}` (new); existing session/renderer modules.
- **Goal:** end-to-end UI with direct address/port, bounded progress/errors,
  approved catalog selection, one-drone start, reliable results and reset/quit.
- **Acceptance:** actual Windows client completes both maze matches, human and
  bot result fixtures show correct winner/score; death/respawn visible; reset
  without process restart; wrong content/protocol and unavailable server yield
  actionable errors; solo timeout abort visibly returns to lobby/disconnected.
- **Constraints:** scripted test completion supplements, not replaces, owner
  live play; forced placements/tick caps are test-only and labeled.
- **Dependencies:** P2.8, P2.12, P2.13.

### P2.15 — Network parity, latency and bounded-load evidence [M]
- **Files:** `Tests/phase2/{regression.sh,network_replay.c,reference_view.c,
  timing.sh,backpressure.sh,latency.*}` (new); Phase 1 harness read-only;
  network/view modules only for diagnosed defects.
- **Goal:** exercise actual network admission/stepping/export path against
  independent legacy reference and direct-intent replay; verify client timing.
- **Acceptance:** §5 three-way byte identity passes for old and new human
  scenarios; 0/1/multiple exports and clients at 30/144 FPS retain simulation
  state; tick-rate/p99 and healthy-LAN latency budgets pass; frozen-reader,
  duplicate/stale/malformed/ownership and view leakage checks pass; measured
  bytes/tick and transport/internal-queue maxima recorded.
- **Constraints:** do not compare wall-clock human matches to deterministic
  goldens; freeze applied command ticks for comparison. Phase 3 impairment and
  two-human release tests remain separately identified.
- **Dependencies:** P2.14; parity harness fixtures start alongside P2.4–P2.7
  so those tasks already have meaningful G2 coverage, not just a final audit.

### P2.16 — Packages, repeatable gate and owner exit review [M]
- **Files:** `Bin/package-client.*`, `Tests/phase2/{package.sh,package.ps1}`,
  `.github/workflows/phase2.yml`, `docs/PHASE2-RUN.md`, `PHASE2-REPORT.md` (new);
  root `GNUmakefile`, Linux server packager as needed.
- **Goal:** relocatable client/server archives, a proposed `make phase2-check`
  automated Linux gate, native-Windows smoke instructions/job and phase hand-off.
- **Acceptance:** clean Windows archive and Ubuntu package pass §2; paths with
  spaces/DPI/focus and repeated sessions checked; notices/runtime manifest
  included; automated G2 checks pass on integrated tip; owner solo sign-off
  and native Windows evidence linked; report lists Phase 3 protocol/session debts.
- **Constraints:** CI service/runners/artifact-retention policy require owner
  agreement (broader Phase 1 §7 step 8 hand-off remains deferred). Cross-build
  success alone cannot mark native Windows exit PASS; no generated binaries,
  large evidence or dependency trees committed.
- **Dependencies:** P2.15.

### Sequencing and timeboxes

Critical integration chain: P2.1→P2.2→P2.3/P2.4→P2.5→P2.6→P2.8→P2.14→P2.15→P2.16,
with P2.7 joining P2.8 and P2.9→P2.10→P2.11→P2.12/P2.13 joining P2.14.
Frontend and server tracks can be worked independently after DTO/fixture approval;
shared headers/build/session files integrate serially with G2 on the merge tip.
This is a work partition, not a request to launch agents before approval.

- Week 1: P2.1–P2.4, P2.9 runtime spike; approve schema and prove a real human
  can live/respawn headlessly. If either gate fails, re-estimate immediately.
- Weeks 2–3: exporter/events/transport/session and art/model/timeline; first
  connected snapshot and controllable rendered tank by end of week 3.
- Weeks 4–5: input, HUD/map/radar, complete/reset matches, parity/load evidence.
- Week 6 allowance: package, clean Windows/owner review and diagnosed fixes.
  Reserve integration capacity; do not use the allowance to add Phase 3 features.

## 5. Regression strategy — G2 before/after every implementation task

**G2 = existing exact simulation guardrail G + task-specific Phase 2 boundary
checks.** Preserve the stronger Phase 1 byte contract; the earlier Phase 0
position tolerance is not permission to weaken it. No golden or seed change may
bless a difference. Run on each task parent/candidate and integrated merge tip.

### A. Unchanged frozen-X guardrail

Use isolated candidate/reference trees and task-unique evidence directories:

```sh
export LC_ALL=C XTANK_SEED=114336
export XTANK_P1_EVIDENCE=/tmp/opencode/xtank-phase2/p2.N/oracle
bash Tests/phase1/regression.sh --suite full --stage before --subject auto
# Implement the approved task.
bash Tests/phase1/regression.sh --suite full --stage after --subject auto
```

Always require `cmp` between frozen reference, pre-change baseline, post-change
candidate and three candidate repeats for each v1/v2 scenario/epoch. Keep frozen
trace/settings/assets/compiler flags identical: Ubuntu 24.04, gcc 13.3.0,
flex 2.6.4, `-O0 -g`, harness-owned Xvfb. Seed injection/preload is reference-only;
candidate uses configured seed. Preserve camo/four-drone/Madman controls even
though they are outside release scope. Run `make phase1-check` at baseline,
server/shared-core integration milestones and final exit, with builds/evidence
outside the checkout. Run appropriate geometry/intent/lifecycle/content/package/
all four X-boundary checks whenever their surface changes.

### B. Prove the new network path, not only the old harness

1. For existing bot-free traces, run **independent frozen-X legacy controls**, a
   direct-intent candidate and the same candidate via real codec/ENet/session
   admission. Pre-deliver tick-indexed batches through a test-only delivery
   barrier; do not modify production scheduling to make the test deterministic.
2. Capture the actual applied `(epoch,tick,vehicle,sequence,op,a,b)` log. Replay
   that log through independent frozen-X legacy operations and direct intents.
   Match initial placements, human flags, settings, asset hashes and seed.
   This separates transport latency from simulation semantics.
3. Add human lifecycle/control and authorized-view cases absent from Phase 1.
   Freeze new reference fixtures **before** changing the corresponding core
   surface, using only reviewed read-only/seed/replay instrumentation on the pin.
   Never link the new command dispatcher/exporter into the reference.
4. Compare per-tick v2 state, final score/results and event occurrence using
   `cmp`, including parent/candidate and three repeats. Compare exporter disabled,
   once-per-tick and repeatedly called. State quantization and interpolation are
   presentation tests, not an excuse for altered authoritative dumps.
5. Independent expected-field fixtures verify view authorization and wire bytes;
   exporter/decoder round trips alone cannot prove secrecy or correctness.
   New oracle schema requires permanent old-format parity plus newly frozen
   reference parity. Report first differing epoch/tick/field on failure.

### C. Presentation and operational checks

- Unit fixtures at real boundaries: codec sizes/numbers, ownership/scheduling,
  input transform, generation/epoch, visibility, interpolation discontinuities.
  Use sanitizers for new Linux codec/session/client-model code; legacy supported
  simulation parity remains on the pinned compiler/flags.
- Record packet traces, applied-input timestamps, server tick timing, client
  receipt/render timestamps and unsynchronized-clock measurement method.
  Healthy-LAN input-to-visible-action is measured locally at the client or with
  external capture, not by subtracting unrelated host clocks.
- `Tests/phase1/timing.sh` still characterizes the original loop. Add a network
  timing harness measuring total work; 30/144 FPS clients must not alter ticks.
  A ten-second paused client must not stall the simulation before/after its
  timeout; compare pre-timeout identical-input runs and separately assert the
  intended session-abort transition rather than demand parity after policy diverges.
- Retain Phase 1 bot evidence; repeat a paced network/export soak (60 minutes,
  twenty resets using finite test epochs) before exit, watching bot progress,
  memory/queue bounds and stale controller generations. Completed owner matches
  remain distinct from capped soak epochs.
- Proposed `make phase2-check`: full Phase 1 gate plus codec, players, view,
  events, admission/session, network replay, model/input and package checks.
  Native Windows, paced timing/latency, soak and owner play evidence are explicit
  additional exit requirements, not implicitly covered by Linux CI.

Evidence goes under `/tmp/opencode/xtank-phase2/p2.N/`, not Git: commands/exit
codes, source manifests, versions, hashes, traces, dumps, captures, screenshots,
machine specs and measurements. Commit only small fixtures, harness source and
concise reports. One task = one reviewed commit/small series during execution;
preserve unrelated working-tree changes and merge dependency-first.

## 6. Stop conditions and risks

### Stop and return to owner with a minimal reproducer

- Any reference/parent/candidate byte difference, nondeterminism, changed RNG
  call order, snapshot/event side effect or instrumented reference rule change.
  Report first differing epoch/tick/fields, manifests/hashes and exact commands;
  do not relax checks, regenerate goldens, pick a new seed or hide failures.
- Human binding requires dummy presentation objects, different human defaults,
  altered respawn/teleport/scoring or an unapproved departure gameplay path.
- Snapshot visibility cannot be specified from legacy evidence, leaks private
  map/contact state, or requires new knowledge rules. Block player export until
  resolved; do not temporarily broadcast the whole world.
- Scheduler stalls, sustained tick-work overruns, unbounded transport/reliable
  queues, or blocked output that stalls simulation. Diagnose before continuing.
- Required view/input bytes exceed approved bounds, transport fragmentation
  reliability differs from assumptions, or the primary Windows toolchain/runtime
  cannot pass native clean-machine tests.
- Control latency/appearance fails owner acceptance, milestones/timeboxes slip,
  an L task cannot finish with evidence without subdivision, or dependency/license
  distribution cannot meet the approved package contract.
- Work requires two-client release behavior, Windows hosting, new modes/bots,
  prediction, user content, observers/chat/audio or an architecture change.
  Re-scope explicitly; protocol additions alone do not authorize broader product work.

### Principal risks and planned mitigation

| Risk | Mitigation / evidence gate |
| --- | --- |
| Terminal assumptions are dormant until humans exist | P2.4 early independent human lifecycle parity; audit naming, activation, specials, death, messages and result paths. |
| Legacy drawing is mistaken for a reusable distributed client | New snapshot DTO/model; renderer never reads authoritative globals; reuse assets/primitive contract selectively. |
| Radar/mapper leak precise or fresh world information | P2.5 field authorization table and negative packet fixtures; reference-derived viewport and remembered-map semantics. |
| 15 Hz feels sluggish with interpolated own tank | One-tick initial buffer, consistent camera/aim transform, measured ≤250 ms healthy-LAN p95 and owner play gate; prediction requires later decision. |
| UDP reliability/fragmentation/backpressure creates stalls or stale views | Tested ENet channels, full replaceable views, atomic reassembly, bounded library/application queues, frozen-reader test. |
| Lost/repeated fire or stale input after respawn/reset | Independent command IDs, redundant bounded inputs, acknowledgments, generation/epoch validation and repeated neutral held state. |
| Cross-compilation passes but Windows deployment fails | P2.9 native runtime gate, bundled audited DLLs/notices, no POSIX dependencies, clean Windows packaging and DPI/focus checks. |
| Short-lived effects/slot reuse disappear between exports | Stable sidecar identities, reviewed transient capture only where necessary, authoritative snapshot lifecycle plus reliable event dedup. |
| Existing oracle misses network-server regressions | Three-way independent X/direct/network replay; read-only export parity; test actual listen mode and total tick cost. |
| Phase boundary expands into full session service | One peer/one drone, explicit slice abort, deferred approved Phase 3 matrix; re-estimate after first playable solo slice. |

## 7. Approval review and Phase 3 hand-off

Owner review resolved D2-A–F, confirmed exit evidence access and accepted task
sizes/timeboxes before P2.1 execution. The owner approved this plan and all six
decisions on 2026-10-11. The planning environment was missing **`flex`** (and
`m4`/`xdpyinfo`/`xrdb`), so the before/after Phase 1 oracle could not build the
reference there; this was an execution prerequisite, not a simulation mismatch
or a replacement for the reported Phase 1 PASS. P2.1 installed the toolchain and
re-ran the full baseline gate clean — see `P2.1-REPORT.md`.

Phase 3 inherits the versioned codec, transport adapter, controller binding,
authorized exporter, lifetimes, timing model and packaged Windows client. It
must still implement/test two-human configuration, fairness across peers,
remaining-human departures/removal without kill credit, admin transfer, lobby
rejoins, four-vehicle bandwidth/timing, the approved impairment matrix and
two-clean-Windows-PC release acceptance. Keep Linux-only hosting and the eight
approved scope decisions unless the owner explicitly amends them.
