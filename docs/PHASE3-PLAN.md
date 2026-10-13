# Phase 3 — Two-PC multiplayer MVP

**DRAFT — owner review required; implementation is not authorized by this
planning document.** Prepared 2026-10-12 from the current working tree.
Planning allowance from `PORT-SCOPE.md`: **4–6 engineer-weeks**, to be
re-estimated after the first two-client match and the departure feasibility gate.

## 1. Goal, authority and scope

Deliver the **two-PC multiplayer MVP** in `docs/PORT-SCOPE.md` Phase 3: two
separate **Windows 11 x64 clients** connect by direct IP/port on a private LAN
to one **Linux authoritative server**, complete Combat matches and restart them
without restarting processes. A lagging, frozen or disconnected client must not
stall the server or the remaining player.

The server owns movement, collisions, weapons, damage, RNG, bots, knowledge,
respawn and scoring. Clients send intent and render authorized views; they do
not simulate gameplay. **No gameplay rule changes; the frozen simulation remains
byte-identical.** Phase 3 extends the delivered Phase 2 seams rather than
rebuilding its protocol, renderer or simulation boundary.

Authority and reconciliation:

- `docs/PORT-SCOPE.md` Phase 3 establishes the multiplayer/resilience goal.
- `docs/DECISIONS.md` and the approved release contract in
  `docs/PHASE0-BRIEF.md` §3 narrow it to **Linux-only hosting, lobby-only joins,
  one controller per tank, no observers/chat**. The scope survey's Windows
  timer/fiber port and Windows host-and-play suggestion is superseded by those
  approvals. Administrator is a session privilege, not a Windows-hosting role.
- `docs/PHASE2-DECISIONS.md` preserves SDL3, ENet/UDP, explicit binary wire
  encoding, exact version/content rejection, MinGW-w64 and 15 Hz snapshots with
  one-tick interpolation. D2-F's provisional solo abort must expand to the
  already-approved remaining-human departure policy.
- `docs/PHASE2-REPORT.md` §7 identifies the inherited seams and Phase 3 debts.
  Its status is **implementation complete / Linux gate green / native Windows
  and owner acceptance outstanding**, not an unconditional product exit PASS.
- `docs/PROTOCOL-v1.md` and `docs/VIEW-v1.md` freeze the existing wire and
  knowledge contracts. The protocol's “awaiting approval” heading is historical:
  D2-C records owner approval on 2026-10-11. New changes still require approval.
- `docs/PHASE1-PLAN.md`, `PHASE1-REPORT.md` and `AGENTS.md` govern G, frozen
  reference provenance, X isolation and evidence discipline.

### Included release configurations and behavior

- Ubuntu 24.04 x86-64 server, one process/world/match; Windows 11 x64 clients.
- **One human + one drone**, or **two humans + 0, 1 or 2 drones**; maximum four
  release vehicles. Keep engine limits (16 vehicles, 511 bullets, 255 explosions)
  and prove maximum authorized-message handling independently of that preset.
- Combat/FFA, 15 ticks/s, respawn, individual winning score 1,000; bots may win.
  Both `combat1` and `Arena`; Vanguard for every combatant; unchanged release
  settings, geometry, controls, ordinary Radar, Mapper, HUD and Console.
- First connected admitted player administers start/reset; authority transfers
  to the remaining human when the administrator departs. Server validation is
  authoritative even if the UI disables a button.
- Valid input/heartbeat every tick, including idle. At two seconds without
  valid input, neutralize drive/fire and remove the departed tank **without kill
  credit**. A remaining human continues; no humans returns the server to lobby.
  QUIT, transport disconnect and output saturation use the same departure path.
- Rejoin only at the next lobby, with fresh connection/ownership/tank identity;
  no mid-match resume, reservation token or retained tank.
- Bounded reliable control/events, per-peer sequenced input, separate authorized
  views, epoch barriers, content/version mismatch failures, measured impairment,
  timing, bandwidth and clean two-PC installation evidence.

Prediction, extrapolation, lag compensation, observers, chat, shared crews,
Internet services/NAT/discovery, Windows hosting, other modes/bots, arbitrary
content, editors, audio and dynamic bots remain outside the approved MVP.
Changing that boundary requires an owner amendment, not a protocol addition.

## 2. Current-tree survey and implementation gaps

This is a source survey, not a fresh test result. The checkout has extensive
modified/untracked Phase 1/2 owner work (and a staged README rename); HEAD alone
does not describe the delivered baseline. No existing file is to be discarded
or reset. P3.1 records the complete starting manifest and re-verifies it.

| Surface | Delivered seam | Concrete Phase 3 work |
| --- | --- | --- |
| `Src/server_session.c`, `Src/Include/server_session.h` | Solo handshake/lobby/results and optional listen mode | `the_session` singleton, `SERVER_SESSION_MAX_PEERS=1`, slot 0 binding, fixed one-drone combatants, per-peer epoch/world cleanup, auto-start on ready. Separate shared match state from peer state; step/reset/finalize the world once. The listen loop currently exits after its peer leaves. |
| `Src/server_players.c`, `Src/Include/server.h` | Terminal-free human name/controller/slot binding | Table can bind multiple humans, but duplicate names are rejected, binding generations reset on init, and controller APIs use signed `int`. Prove two-human identity, respawn, epoch invalidation and bounded ID wrap behavior. |
| `Src/server_net.c`, `Src/intent.c` | Per-session dedup/acks, validated commands, 128-intent queue and eight-entry release reserve | Admission has an eight-session table but no cross-peer fair quota. Preserve legacy `(player, sequence)` consumption; fairness belongs before it. Session and admission modules both service/drain transport; consolidate routing/budgets so they cannot consume each other's messages. |
| `Src/vehicle.c`, `Src/animate.c` | Inactivate/unmake and legacy live/dead-list lifecycle | No explicit terminal-free departure operation. `kill_vehicle()` explodes, advances RNG/deaths and allows respawn; even a NULL killer is not acceptable removal. Dead-list and outstanding bullet-owner lifetimes need an early feasibility review. |
| `Src/server_view.c`, `Src/Include/server_view.h` | Read-only slot-specific viewport, own HUD, remembered mapper and anonymous radar | Existing exporter accepts any slot and session calls it with generation 0. Enforce controller authorization at delivery, distinguish binding generation from entity-life generation, and prove two different knowledge views without cross-peer reuse. `map_revision` currently reads a global mapper-update counter. |
| `Src/server_events.c`, `Src/Include/server_events.h` | Export-side generations, reviewed spawn/death hooks, results before score reset | Events have no stored occurrence tick; session stamps delivery frame and sends the same event list to its peer. The 256-entry per-epoch append buffer silently stops emitting when full. Add authorized per-peer delivery/cursors and bounded retirement/overflow behavior. |
| `Protocol/` | Allocation-free codec, frozen v1.0 fields/IDs, content digest | No roster/admin transfer, drone-count selection or explicit start request. COMMAND has no life-generation field despite the prose contract. Snapshot vehicles use u8 generations; bullets/explosions omit their own lifetime generations although events expose u32 counters. START's vehicle field is u8 while Vanguard's content ID is 0x0201. |
| `Transport/enet_transport.c`, `Transport/transport.h` | Zero-timeout ENet service, bounded 64-packet receive queue and reliable-backlog inspection | Audit aggregate/per-peer budgets, ENet internal queues and fragmentation. Large channel-1 packets use flags 0; measure ENet's actual fragmentation reliability, not just the nominal channel. Disconnect wrappers are not released on observed disconnect, so repeated reconnects need lifetime/reuse tests. |
| `Client/session_client.c`, `frame.c`, `menu.c` | Live session/frame/lobby/results integration | WELCOME `role==1` means player on wire but client treats it as admin. Auto-config/auto-ready assumes solo; shared roster/config/start/return-to-lobby need explicit UI state. Validate inbound peer/session/epoch/channel before changing session state. |
| `Client/model.c`, `timeline.c`, `input.c`, HUD/map/radar | Eight snapshots, one-tick interpolation, world-intent transform | Bullet matching uses ID+owner, explosions ID only. Prevent blending reused slots; clear histories/edges/redundancy on reset/rejoin. Live frame emits INPUT_BATCH but does not send the reliable companion for one-shot edges. Input must refresh at 15 Hz even during snapshot stalls. |
| `Tests/phase2/` | 13 drivers, Linux client smoke, wire fixtures, packaged server/client and X/direct/network parity | Solo fixtures do not cover two humans, fair admission, removal/admin transfer/rejoin, long event streams or real impairment. Existing parity compares an F2/V/B/E subset for 800 ticks with two direct/network repeats; retain it and add stronger two-human coverage. |
| Root/Src makefiles, CMake and `Bin/package-*.sh` | Isolated client cross-build, GNU server, `make phase2-check` | Phase 2 gate includes Phase 1 only with `PHASE2_WITH_PHASE1=1`. Phase 3 integrated exit must require both inherited gates, new drivers and packages, with explicit operational/native-owner evidence. |

Phase 2's missing native Windows runtime/solo sign-off and network-paced
timing/latency/60-minute twenty-reset soak remain required evidence. Wine and
cross-build success do not close native Windows acceptance.

## 3. Task contracts

Every execution task requires **G3 = G + relevant boundary checks** (§6), before
and after, including documentation/harness tasks under the repository's task
discipline. All tasks inherit the global stop conditions in §7. Files below are
bounded touchpoints, not permission to rewrite modules. Proposed new paths are
marked **new**. S/M are narrow/normal independently verifiable sessions; split
a task before execution if it cannot produce its evidence in one session.

### P3.1 — Freeze executable baseline and acceptance access [S]

- **Objective:** establish reproducible starting state, unresolved rulings and
  actual Linux/two-Windows-machine access before multiplayer work.
- **Files:** `Tests/phase3/README.md`, `docs/PHASE3-DECISIONS.md` (**new**, owner
  rulings only), this plan; existing reports/pins/harness read-only.
- **Acceptance:** source/untracked-file manifest, HEAD, dependency hashes,
  compiler flags and machine specs recorded; isolated `make phase1-check` and
  `make phase2-check PHASE2_WITH_PHASE1=1` pass; G baseline passes; native
  Phase 2 archive smoke and solo owner review recorded or explicitly BLOCKED.
  Owners of two clean Windows PCs and the Linux test host identified; required
  decision dispositions recorded. No missing native evidence labeled PASS.
- **Stop:** baseline differs, required dependencies/machines cannot be obtained,
  or schema/session rulings needed by the next task remain unresolved.
- **Dependencies:** none; baseline/approval gate for implementation.

### P3.2 — Departure feasibility and lifecycle contract [S]

- **Objective:** specify the minimal server-only removal seam before touching
  lifecycle code; prove it can implement the approved departure policy while
  preserving every ordinary simulation path.
- **Files:** `Tests/phase3/{departure-design.sh,departure-design_fixture.c}`
  (**new**), lifecycle section of this plan/owner decisions; inspect
  `Src/{vehicle,animate,setup,intent,game,server_core}.c` read-only.
- **Acceptance:** fixture/audit covers live and respawn-waiting departures,
  pending/held input, live/dead lists, team counts/map occupancy, cleanup and
  existing projectile-owner references. Exact mutation/order/retention contract
  reviewed; no score/death/RNG/explosion/respawn side effect from removal;
  remaining tank/bot and end-of-match semantics specified. Freeze ordinary
  two-human reference fixtures before related core changes. Report whether
  existing primitives suffice and identify the smallest reviewed seam if not.
- **Stop:** removal needs changed game rules/RNG ordering, freeing referenced
  vehicle storage, an unapproved lone-survivor policy or broad lifecycle rewrite.
  Return a minimal reproducer and cost/scope question before P3.7.
- **Dependencies:** P3.1; blocks departure implementation and final schema freeze.

### P3.3 — Approve wire extension and portable codec [M]

- **Objective:** freeze only the approved multiplayer/lifetime extensions in
  §5, with exact offsets, channels, sizes, state transitions and version rejection.
- **Files:** `docs/PROTOCOL-v1.1.md` (**proposed new**, version subject to owner),
  `Protocol/{wire.h,limits.h,encode.c,decode.c}`, content manifest only if its
  canonical preset changes; `Tests/phase3/{protocol.sh,codec.sh,codec_fixture.c,
  fixtures/protocol/*}` (**new**).
- **Acceptance:** owner approval precedes codec changes; independent golden-byte
  verifier and Linux/MinGW codec checks; every new field truncation/count/reserved/
  version/nonfinite rejection tested; reproducible ASan/UBSan fuzz passes.
  Original v1.0 fixtures retained and independently verified unchanged; explicit
  1.0↔new-version rejection, no fallback. Worst-case snapshot/control sizes fit
  approved bounds; no allocator/native struct encoding. Content identity policy
  and full-width content IDs agree on both ends.
- **Stop:** unapproved schema/knowledge widening, bounds exceeded, silent reserved
  field reuse, backward-compatible claim without proof or dependency substitution.
- **Dependencies:** P3.1, P3.2 and D3-A/D3-B/D3-C/D3-D rulings (§8).

### P3.4 — Shared lobby and one-world match coordinator [M]

- **Objective:** split shared match/config/epoch/admin state from two bounded
  peer records; keep listen service alive through empty-session lobby cycles.
- **Files:** `Src/{server_session.c,Include/server_session.h,server_main.c}`;
  `Tests/phase3/{lobby.sh,lobby_fixture.c}` (**new**).
- **Acceptance:** two admitted humans in lobby; extra human, observer and running
  join rejected; common maze/Vanguard/drone config and readiness revision agree
  for both; unauthorized config/start/reset has no effect. One start transaction,
  one world init, one `server_step()` per tick and one results finalization/reset,
  independent of peer count. Partial handshake/start failure releases resources.
  Results→lobby opens rejoin before next start under the approved reset contract.
  Empty server remains listening and can accept a fresh solo/two-human session.
- **Stop:** per-peer simulation worlds/steps, admin inferred from player role,
  partial starts leave bindings, or old messages restart/reset a newer epoch.
- **Dependencies:** P3.3; use deterministic bindings until P3.5 joins.

### P3.5 — Two-human / four-vehicle binding and lifecycle [M]

- **Objective:** construct allowed combatant configurations through existing
  human/bot initialization, with unique ownership and stable slots.
- **Files:** `Src/{server_session.c,server_players.c,Include/server.h}`;
  `Tests/phase3/{players.sh,players_fixture.c,fixtures/players/*}` (**new**).
  `Src/{setup,vehicle}.c` only for a separately reviewed accessor if required.
- **Acceptance:** solo+one drone and two humans+0/1/2 drones initialize on each
  maze; slots are unique, all use Vanguard/unchanged human flags and release
  defaults. Both humans die/respawn through normal rules, bind only their tank,
  reset across at least three epochs; no Terminal/input/render allocations.
  Duplicate-name behavior matches the owner ruling; stale controllers and signed
  controller-ID/wrap boundary cases rejected. Bot-free two-human traces reproduce
  independent frozen-X lifecycle/state bytes with three candidate repeats.
- **Stop:** human-as-bot/fake-terminal workaround, changed team/setup/RNG order,
  renamed identity used to claim another controller or ordinary lifecycle parity fails.
- **Dependencies:** P3.2, P3.4.

### P3.6 — Fair bounded admission and valid-input timeouts [M]

- **Objective:** give both peers bounded admission opportunity and neutralization
  capacity while preserving legacy intent ordering and at-most-once edges.
- **Files:** `Src/{server_net.c,Include/server_net.h,server_session.c}`;
  `Tests/phase3/{admission.sh,admission_fixture.c}` (**new**);
  `Src/intent.c` ordering/formulas read-only.
- **Acceptance:** approved round-robin/quota policy has numeric work/queue bounds;
  a flooding peer cannot starve a healthy peer's next-tick admission or either
  peer's drive/fire release. At most 128 engine intents, reserve covers both
  departures. Same command IDs on different peers do not collide; cross-channel
  duplicates on one peer apply once. Peer/session/epoch/life generation, actual
  transport channel, finite/range/expiry and ownership validation precede intents.
  Invalid/replayed/foreign traffic cannot extend the valid-input timeout; tests
  exercise 1,999/2,000/2,001 ms and stale edges across death/respawn/reset.
  Applied log preserves each peer's edge order and existing consume semantics.
- **Stop:** fairness requires changing `intent_consume()` ordering, release can
  be starved, invalid traffic keeps a departed tank alive or queues grow unbounded.
- **Dependencies:** P3.3, P3.4, P3.5; D3-E quota ruling.

### P3.7 — Departure removal, admin transfer and lobby rejoin [M]

- **Objective:** execute P3.2's reviewed server-only lifecycle contract for every
  departure reason, with the approved remaining-human behavior.
- **Files:** `Src/{server_session.c,server_net.c,server_players.c,server_core.c,
  Include/server.h}`; only the reviewed seam in `Src/{vehicle,animate,intent}.c`
  if P3.2 requires it; `Tests/phase3/{departure.sh,departure_fixture.c}` (**new**).
- **Acceptance:** live/dead-waiting tank removal on QUIT/disconnect/timeout/queue
  saturation occurs once at a tick boundary; cancels that peer's pending/held
  controls, prevents respawn, awards no kill/score and emits no artificial death
  or RNG-consuming explosion. Existing projectiles retain valid owner storage
  and follow unchanged projectile rules. Stable slots/remaining ownership and
  reference-derived cleanup/team bookkeeping survive until normal reset.
  Admin transfer is reliable and unique; remaining human keeps progressing;
  no humans cleans world and returns to listening lobby. Rejoin while Running
  fails, next-lobby rejoin gets fresh IDs/tank and cannot replay old commands.
  Test simultaneous departures and repeated disconnect/rejoin cycles.
- **Stop:** ordinary death used as removal, dangling projectile/special references,
  altered surviving-player rule paths or departure behavior outside reviewed contract.
- **Dependencies:** P3.2, P3.5, P3.6; D3-D survivor/retained-record ruling.

### P3.8 — Per-player views and bounded authorized event delivery [M]

- **Objective:** publish two read-only views and recipient-authorized event
  streams/results without cross-player knowledge or lifetime leakage.
- **Files:** `Src/{server_view.c,server_events.c,server_session.c}`,
  `Src/Include/{server_view.h,server_events.h}`; `Tests/phase3/{view.sh,events.sh,
  view_fixture.c,events_fixture.c}` (**new**), approved event/view addendum
  (**new**). `Src/{vehicle,bullet,explosion}.c` only for reviewed read-only
  spawn/transient lifetime hooks if necessary; verify the actual creation sites
  before selecting the minimal hook locations.
- **Acceptance:** separated cameras/mapper histories/private HUD/radar fixtures
  produce independently checked recipient bytes. Export in A→B and B→A order,
  zero/one/five times each, leaves authoritative per-tick dumps identical.
  Binding and entity generations independently validated; no arbitrary-camera
  query/full map/anonymous radar identity. Remote SCORE is not accidentally
  broadcast when its value is only authorized at RESULTS; respawn coordinates
  and local transient events obey the approved recipient table.
  More than 256 events in one epoch deliver without silent loss: bounded cursors,
  retirement and explicit slow-peer failure; no required lifecycle/result loss.
  Event occurrence tick remains the capture tick through delayed delivery;
  skipped private events have specified sequence-gap semantics. Results capture
  all required participants before score reset, once, and survive departures.
- **Stop:** private event leakage, export mutation/RNG/draw access, generation
  truncation/alias, silent overflow or send failure marked delivered.
- **Dependencies:** P3.3, P3.5, P3.7; approved event authorization in D3-C.

### P3.9 — Transport isolation, fragmentation and connection reuse [M]

- **Objective:** make the shared transport's application and ENet work/memory
  bounds hold across two peers, reconnects and maximum snapshots.
- **Files:** `Transport/{enet_transport.c,transport.h}`,
  `Src/{server_session.c,server_net.c}`; `Tests/phase3/{transport.sh,
  transport_fixture.c}` (**new**).
- **Acceptance:** one receive/router owner and aggregate 64-service-event budget
  per tick unless owner explicitly revises the frozen bound; measured decode/
  command/send budgets also bounded. Both actual ENet and application per-peer/
  host queues have recorded caps. At most newest unsent snapshot retained;
  reliable queued output respects 256 KiB/peer with bounded overshoot specified.
  Frozen-reader/flood/forced-send-failure isolates departure to that peer.
  Maximum application snapshots are reassembled atomically under fragment
  loss/reorder; packet capture proves the approved reliability choice.
  At least 100 connect/disconnect/rejoin cycles reuse wrappers without stale
  queued packets, slot exhaustion or growing resources; healthy peer progresses.
- **Stop:** hidden ENet queue growth/blocking, accidental reliable snapshots,
  partial view application, wrapper reuse aliases old traffic or budget exceeds
  approved limits. Return fragmentation design to owner before changing wire.
- **Dependencies:** P3.3, P3.4, P3.6, P3.7; D3-F fragmentation ruling.

### P3.10 — Multiplayer client lobby, authority and rejoin UI [M]

- **Objective:** present shared server-authored roster/config/ready/start/results/
  lobby state and admin transfer on both clients.
- **Files:** `Client/{session_client.c,session_client.h,frame.c,frame.h,menu.c,
  menu.h,main.c}`; `Tests/phase3/{client-session.sh,client_session_fixture.c}`
  (**new**).
- **Acceptance:** regular player never gains admin from WELCOME player role;
  roster/config/drone count/readiness and transferred admin are visible.
  Only admin config/start/reset controls enabled, with server rejection tested
  independently. Both mazes and all approved configurations selectable; config
  changes invalidate readiness under the approved revision rule. Results→lobby,
  error/disconnect→connect and fresh join work without process restart or solo
  auto-start racing the second player. Inbound wrong peer/channel/session/stale
  epoch cannot regress client state. Native Windows smoke exercises this flow.
- **Stop:** privileges guessed locally, UI cannot open a lobby for rejoin, stale
  results/start regress state or primary toolchain/runtime fails on Windows.
- **Dependencies:** P3.3, P3.4, P3.7; real network checks join P3.9.

### P3.11 — Lifetime-safe presentation and resilient live input [M]

- **Objective:** use full entity keys and generation-stamped live input through
  snapshot stalls, losses, reset and respawn, retaining approved timing policy.
- **Files:** `Client/{model.c,model.h,timeline.c,input.c,input.h,frame.c,
  session_client.c,world_render.c}`; `Tests/phase3/{timeline.sh,input.sh,
  timeline_fixture.c,input_fixture.c}` (**new**).
- **Acceptance:** same-owner bullet reuse, same-kind explosion reuse and >255
  vehicle lives never interpolate old/new entities; reviewed sidecar capture
  distinguishes same-tick reuse. Loss/reorder/death/teleport/departure/epoch replay
  at 30/60/144 FPS has no stale ghosts or extrapolation. Reliable one-shot fire/
  weapon companions share command IDs with redundant batches and apply once.
  Held release survives loss; stale edges cannot fire after a life change.
  Input/heartbeat refreshes at 15 Hz independently of snapshots/render FPS;
  disconnect/reset clears held edges/redundancy/history while command-ID scope
  stays correct. Camera/DPI/world-aim transform and HUD knowledge remain unchanged.
- **Stop:** local gameplay prediction, buffer increase without approval, entity
  matching by slot alone, lost release or unreliable-only one-shot delivery.
- **Dependencies:** P3.3, P3.8, P3.9, P3.10.

### P3.12 — Two-client integration and independent network parity [M]

- **Objective:** prove the actual shipped listen/codec/admission/export/client
  path against direct intents and the independent frozen X reference.
- **Files:** `Tests/phase3/{playable.sh,playable_fixture.c,parity.sh,
  parity_fixture.c,fixtures/parity/*}` (**new**); Phase 1 reference instrumentation
  only by separately reviewed read-only/replay overlays; existing Phase 2
  drivers retained, adapted only for explicit solo/version test profiles.
- **Acceptance:** bot-free two-human command traces on both mazes include
  simultaneous drive/aim/fire, tank/wall/bullet collision, directional damage,
  deaths/respawns/score/win and multiple epochs. Frozen-X == direct-intent ==
  shipped network per-tick authoritative dumps/results via `cmp`, with three
  candidate repeats and parent/candidate identity. Cover map/radar/RNG/control
  state as well as existing F2/V/B/E; keep old dump formats permanently.
  Both decoded views match independent expected fields. Solo and two-human
  0/1/2-drone live paths start/finish/reset; natural 1,000-point winner and
  retained four-entry results checked. Synthetic placements/forced results are
  labeled test-only. Departure tested separately from ordinary-match oracle.
- **Stop:** only testing direct fixture entry, reference linked to new dispatcher,
  nondeterminism/byte mismatch or changed applied ticks hidden as sim parity.
- **Dependencies:** P3.5–P3.11.

### P3.13 — Four-vehicle pacing, bandwidth and latency measurement [M]

- **Objective:** close inherited timing debt and measure total shipped-loop cost
  and input-to-visible latency, not just core step or theoretical packet size.
- **Files:** `Tests/phase3/{timing.sh,latency_fixture.c,measurements.py}` (**new**);
  `Src/{server_session.c,server_loop.c}`, transport/client statistics only for
  bounded read-only instrumentation.
- **Acceptance:** recorded ≥four-core/8 GB Linux host, two humans/two drones;
  30/144 FPS and mixed-rate clients each produce 900 ticks/60 s ±1; p99 total
  work <66.7 ms. Measure ingress/service/admission, sim, exports for both peers,
  event/ack encoding and ENet service/output together, excluding deliberate pace
  sleep. Record p50/p95/p99/max, late ticks, bytes/s per peer and aggregate,
  actual wire fragment overhead, entity counts, all queue/RSS maxima.
  Healthy-LAN input-to-visible-action p95 ≤250 ms with same-client monotonic
  timestamps or external capture; attach clock/measurement method. Stress
  maximum entity/view bounds separately; exact finite applied-log replay is
  unchanged at all render rates/export counts.
- **Stop:** sustained overruns, latency budget failure, unbounded buffers or
  required compression/quantization/prediction/rate change; seek owner review.
- **Dependencies:** P3.12; D3-G measurement/environment agreement.

### P3.14 — Approved impairment and slow-client matrix [M]

- **Objective:** prove resilience using real UDP/ENet on reproducible impaired
  links and a frozen client while the healthy player continues.
- **Files:** `Tests/phase3/{impairment.sh,backpressure.sh,impairment_fixture.c,
  impairment-analyze.py}` (**new**), test run instructions.
- **Acceptance:** baseline, delay-only, loss-only, reorder-only and combined
  profiles recorded; mandatory **30-minute four-vehicle combined run: 50 ms RTT,
  1% packet loss, extra 100 ms delay on 1% of packets**. Configure symmetric
  delay to reach measured RTT (not 50 ms each direction); record random seed,
  actual impairment rates and packet trace. No duplicate kill/respawn or entity
  desync >1 s; p95 input-to-visible ≤250 ms. Freeze one client for ten seconds,
  expire/remove it at two seconds, transfer admin if applicable; healthy client
  and server continue. Resume cannot reclaim tank; next-lobby fresh join works.
  Test queued reliable edges, fragment losses, send saturation and reset packets
  across channels. Captured applied logs replay exactly; packet captures contain
  only recipient-authorized view/event information.
- **Stop:** budgets fail, healthy peer stalls, unauthorized packets or impairment
  silently skipped due to sandbox/network privileges. Missing real network access
  is BLOCKED, not replaced by an in-memory transport PASS.
- **Dependencies:** P3.13.

### P3.15 — Paced four-vehicle bot/reset soak [M]

- **Objective:** verify long-lived drone progress, event retention, connection
  reuse and reset cleanup under the delivered multiplayer loop.
- **Files:** `Tests/phase3/{soak.sh,soak_fixture.c,soak-analyze.py}` (**new**).
- **Acceptance:** **60 minutes paced** with two humans/two drones and **twenty
  resets**, no bot crash, scheduler stall, stale binding/edge/entity or lost
  mandatory event/result. Record drone movement/acquisition/fire, queue/RSS
  limits after warm-up, resource counts after each reset and admin/rejoin cycles.
  Capped fixture epochs may drive reset coverage but are labeled aborts, not
  completed wins; ordinary bot-free replay remains byte-identical. Retain Phase 1
  bot evidence; CPU-time-dependent drone choices need not be byte-identical.
- **Stop:** bot/scheduler failure, resource growth, lifetime alias or event buffer
  exhaustion; no new bot algorithm or golden changes to repair the evidence.
- **Dependencies:** P3.12, P3.13, P3.14.

### P3.16 — Packages, repeatable gate and two-PC owner exit [M]

- **Objective:** ship reproducible test archives and a repeatable Phase 3 gate,
  then obtain clean native two-PC release acceptance and a factual hand-off.
- **Files:** root `GNUmakefile`; `Tests/phase3/{phase3-check.sh,package.sh,
  windows-multiplayer.ps1}`; `Bin/package-phase3.sh`,
  `docs/{PHASE3-RUN.md,PHASE3-REPORT.md}` (**new**); existing client/server
  packagers; optional CI changes only after owner runner/retention approval.
- **Acceptance:** proposed gate in §6 passes on integrated manifest; packages
  unpack/relocate, notices/pinned runtimes/version/content manifests present,
  client DLL import allowlist passes and Ubuntu network server is X-free.
  On **two clean separate Windows 11 PCs**, connect/install through documented
  UI without compiler/POSIX/X; complete solo-vs-drone and **ten two-human matches**
  spanning both mazes and 0/1/2 drones, including four vehicles. Natural winners
  at 1,000, correct results, controls/HUD/map/radar/death/respawn and same-process
  restart/rejoin/admin transfer verified; paths with spaces/DPI/focus smoke.
  P3.13–P3.15 operational evidence attached. Owner solo and two-PC feel/install
  sign-off recorded. Report identifies exact versions/specs/manifests, every
  PASS/BLOCKED/FAIL, evidence paths and Phase 4 hardening estimate.
- **Stop:** no native proof/sign-off, mismatch between tested and packaged source,
  missing required operational evidence or any inherited/new gate failure.
- **Dependencies:** P3.1–P3.15; final release gate, not merely Linux CI completion.

## 4. Dependencies and execution checkpoints

Critical gates:

1. **P3.1 → P3.2 → P3.3:** baseline/native access, departure feasibility and owner
   protocol/session decisions before changing the shared contracts.
2. **P3.3 → P3.4 → P3.5 → P3.6 → P3.7:** shared lobby, real ownership, fair input
   and safe departure/admin transfer. Never retrofit departure after exit tests.
3. **P3.7 → P3.8/P3.9/P3.10 → P3.11 → P3.12:** authorized delivery, isolated
   transport and multiplayer client join into independent network parity.
4. **P3.12 → P3.13 → P3.14 → P3.15 → P3.16:** measured timing, impairment, soak,
   packages and native release evidence. Packaging scaffolding/native playtest
   preparation can start early; final acceptance waits for integrated results.

P3.8/P3.9/P3.10 have separable exporter/transport/UI work after their listed
prerequisites, but shared headers/session files integrate serially with G3 on
each integrated tip. This is sequencing, not an instruction to launch agents.

Indicative checkpoints: week 1 baseline/rulings/removal/codec; weeks 2–3 first
two-client start/respawn/reset/departure; weeks 4–5 parity/load/impairment/soak;
week 6 native packages/owner review and diagnosed fixes. Re-estimate if the early
departure gate or first two-client match exceeds its allowance. Missing native
Phase 2 proof blocks claiming readiness; document any owner-authorized sequencing
exception without treating the missing acceptance as passed.

## 5. Protocol v1 extensions — proposals, not rulings

Existing v1.0 already supplies sequenced inputs, reliable companion commands,
acks, events/results, epochs and per-player snapshots. **No new gameplay op is
needed:** drive/heading/world-aim/fire/weapon/heartbeat remain the release set;
target/chase/standoff remain test-only. Multi-peer tables, fair quotas and
read-only export are principally server changes.

The current frozen schema cannot express every required session/lifetime
boundary. Recommend a **separately specified v1.1, exact-version-only release**,
subject to D3-A. Do not reinterpret reserved fields, change old fixture bytes or
pretend existing 1.0 clients accept unknown messages. New client/server upgrade
together; older versions fail with an actionable mismatch before creating a tank.
If the owner chooses a major bump, use that label consistently instead. No
negotiated dual-version production stack is proposed.

| Proposed addition/change | Minimum fields/semantics to freeze in P3.3 | Rationale |
| --- | --- | --- |
| Reliable `LOBBY_STATE` / session-status message | Shared config revision, state, maze/vehicle u16 IDs, drone count 0–2, bounded human roster (≤2), stable connection/controller identity, self identity, names, readiness, admin identity and departure reason. Valid in lobby/results/running for appropriate status changes. | Player role is not admin; both clients need consistent roster/config and reliable admin-transfer/return-to-lobby notification. Reuse one revisioned message rather than unrelated admin/roster updates. No hidden vehicle/HUD/score data in roster. |
| Reliable explicit `START_REQUEST` | Expected config revision; admin-only; every admitted human ready; valid population/preset checked. Existing START remains server-authored. | Solo READY auto-start otherwise races the second join. Recommendation needs D3-B; an approved auto-start contract could omit this message. |
| Configuration/ready/reset extension | Drone count in CONFIG_SELECT/status; config revision in READY; explicit ready/unready if owner approves; RESET returns through a lobby barrier before a later start. WELCOME advertises human capacity accurately. | Existing len-0 READY and fixed one-drone config cannot describe all release combinations or guard config races; current immediate RESET→START never offers a rejoin window. |
| Input life stamp | Full-width controlled **entity-life generation** in INPUT_BATCH and RELIABLE_CMD alongside epoch. Connection maps owner; no client authoritative slot. ACK reasons specify stale life. | Binding generation does not advance on ordinary respawn. Delayed edge from the previous life must fail even inside the existing 15-tick expiry window. |
| Full entity lifetime keys | u32 life generation for own/visible vehicles/results/owner reference and u32 **own** generation for bullet/explosion records; landmark full grid key if identity is used. Keep numeric positions/operands binary64. | u8 vehicle truncation and owner-only projectile matching cannot distinguish all slot reuse. Event generation alone cannot repair snapshot-first/lost-event interpolation. Export-side same-tick lifetime hooks may be needed; they must be read-only. Landmark's current masked u8 grid key can collide. |
| Correct full-width START content ID | Vehicle ID u16 consistently with catalog/config IDs. | Current u8 field truncates Vanguard 0x0201; widening is a wire change, not a gameplay/content substitution. |
| Reliable initial view barrier | Explicit bounded control-channel full-view/keyframe message, or an approved reliable-channel exception with exact codec/transport rules; per-recipient epoch/view ordering. | v1 prose calls the first snapshot reliable while schema places VIEW_SNAPSHOT only on ch1 and implementation sends it unreliably. Freeze a consistent contract; lost first view must neither strand input nor admit stale world state. |
| Authorized event/lobby departure semantics | Capture tick, per-recipient ordered event sequence or explicitly permitted filtered gaps, removal distinct from DEATH, retained result-participant treatment and explicit overflow/queue error reasons. | Global fanout can reveal remote score/spawn location. Removal is not a kill. Event retirement and result completion cannot depend on a blocked peer. A separate entity-remove event is optional if fresh snapshots plus session status prove removal adequately. |

`VIEW-v1.md`'s **knowledge rules remain authoritative**: own private HUD only,
reference 768×768 half-open viewport, remembered mapper, anonymous ordinary
radar, frozen death camera. Identity widths do not widen knowledge. A reviewed
addendum reconciles its historical provisional-generation text with Phase 2
implementation and the approved wire changes; never invent occlusion/special rules.

Content hashes remain byte-for-byte unless canonical release-preset bytes actually
change (for example to describe variable population). If they change, record an
explicit synchronized content-identity revision; preserve prior manifests and
all asset/geometry hashes. Session roster/drone selection does not silently
authorize new game settings or content. No snapshot compression/delta/rate change
is assumed; P3.13 measurements may justify a separate owner proposal.

## 6. Test and evidence strategy

### G3: frozen simulation before/after every execution task

All evidence lives under **`/tmp/opencode/xtank-phase3/p<N>/`**, where `p1`
corresponds to P3.1, etc. Nested oracle/build/package/capture folders stay there.
No dumps, binary packages, dependency trees, screenshots or generated objects
enter Git. This planning change creates only this document and commits nothing;
no implementation or fresh acceptance/G result is claimed here.

Normal execution pattern (approved task N):

```sh
export LC_ALL=C XTANK_SEED=114336
export XTANK_P1_EVIDENCE="/tmp/opencode/xtank-phase3/p<N>/oracle"
bash Tests/phase1/regression.sh --suite full --stage before --subject auto
# Execute the approved task and its boundary checks.
bash Tests/phase1/regression.sh --suite full --stage after --subject auto
```

Require `cmp` of frozen reference, actual pre-change parent, post-change candidate
and three candidate repeats for every authoritative v1/v2 state/result/assertion
artifact. Preserve seed 114336, reference pin, assets/settings/traces and pinned
compiler/flags (`gcc 13.3.0`, flex 2.6.4, `-O0 -g`, Ubuntu 24.04, harness-owned
Xvfb). Never bless differences with hash-only/tolerance checks or new goldens;
wall-clock diagnostics are separate from authoritative dump bytes.

Because inherited work is uncommitted, verify what `--stage before` actually
builds. If it selects committed HEAD rather than the delivered working-tree
parent, use **two isolated copies of the actual parent/candidate manifests**,
each with `--stage after` and distinct evidence roots, then explicitly `cmp`
their authoritative artifacts. This preserves before/after parity rather than
mislabeling an upstream-versus-Phase-2 comparison. Record the adaptation.

Full Phase 1 gate at baseline, shared-core/lifecycle milestones and final tip;
relevant geometry/intent/lifecycle/content/package and all four X-boundary checks
when those surfaces change. Client/package tasks also run their meaningful
boundary drivers and inherited client checks. Protocol changes retain an
isolated v1.0 baseline/fixtures; Phase 2 solo assertions such as second-peer
rejection remain tested in an explicit solo profile, while the new release
profile tests two peers. Do not delete old acceptance coverage to make new code pass.

### New-path parity and policy boundaries

- Test actual shipped `--listen` routing, not only fixture helpers. Admit at the
  bounded pre-step seam; production client ticks are freshness hints, never
  authority to select application ticks.
- For deterministic comparison, use bot-free controlled delivery and capture
  actual `(epoch, tick, slot, sequence, op, a, b)` applied commands. Independent
  frozen-X legacy controls and direct intents replay the **same applied log**.
  Freeze new reference scenarios before related core work, with reviewed seed/
  read-only/replay instrumentation only; never link the new dispatcher/exporter
  into the pin. Require three candidate repeats and per-tick `cmp`, not just
  final positions or the existing partial F2/V/B/E comparison.
- Exact replay under impairment compares identical applied commands, not two
  wall-clock delivery schedules. Bot CPU scheduling is stability evidence;
  it is not deterministic network-play parity.
- Departure is an approved session action absent from frozen ordinary-match
  traces. Compare ordinary paths fully and the prefix before departure; then
  assert the reviewed removal contract, score/death/RNG neutrality of the
  removal operation, valid remaining world state and continuing legacy rules.
  Do not demand identity to a reference where the departed tank remains, or
  modify the pin's rules to manufacture departure parity.
- Independent expected-field/wire fixtures and packet captures prove both
  recipients' view/event allowlists; encode/decode round trips alone are
  insufficient. Export count/order cannot influence simulation.

### Proposed automated exit gate

Mirror the Phase 2 driver/log/package structure with a new, fail-fast target:

```sh
make phase3-check PHASE3_EVIDENCE=/tmp/opencode/xtank-phase3/p16/gate
# Equivalent planned driver:
bash Tests/phase3/phase3-check.sh --evidence /tmp/opencode/xtank-phase3/p16/gate
```

These commands/targets **do not exist yet**. P3.16 implements them to require:

1. Full `make phase1-check` and inherited Phase 2 drivers/package checks,
   including v1.0 fixture preservation and explicit solo compatibility coverage.
2. New protocol/codec, lobby, players, admission, departure, view/events,
   transport/reconnect, client-session, model/input and shipped playable/parity
   drivers (P3.2–P3.12), Linux/MinGW builds, meaningful sanitizer/fuzz coverage.
3. Package layout, relocation, pinned dependency/notices/content/build manifests,
   DLL audit and X-free Linux network launch; all outputs rooted in p16 evidence.
4. Machine-readable per-driver PASS/FAIL/BLOCKED with exact commands/exit codes,
   source manifests and log paths. Missing prerequisites or skipped required
   drivers cannot produce a full gate PASS.

Separate explicit operational drivers (`timing.sh`, `impairment.sh`,
`backpressure.sh`, `soak.sh`) run on the recorded capable hosts. The final release
gate requires their evidence against the **same tested/package manifest**, plus
native Windows smoke, ten completed two-human matches, solo acceptance and owner
sign-off. A fast Linux automated PASS is reported as such; it is not two-PC exit
PASS. If an optional operational mode is added to `phase3-check`, it must enforce
these drivers or report BLOCKED, never silently skip them.

Each p<N> stores parent/candidate manifests, versions/flags, command logs, `cmp`
results and first differences, packet/applied-input logs, queue/timing/RSS
measurements, render captures, machine specs and package hashes. Small harness
source/fixtures and concise reports are future review artifacts; this request
authorizes writing the plan only and **no commits**.

## 7. Global stop conditions

Stop the affected task and return a minimal reproducer, exact manifests/commands,
first differing epoch/tick/fields and evidence path when:

- G or new-path parity differs, repeats are nondeterministic, reference
  instrumentation changes rules, or exports/events affect RNG/simulation.
- Departure requires changed ordinary gameplay, scoring, respawn, collision,
  team/game-rule logic or unsafe object lifetime. P3.2/P3.7 approval is a bounded
  session seam, not permission for broad changes to `animate.c`/`vehicle.c`.
- Ownership/knowledge cannot be proven; private events/map/contact state leak;
  slot/life/epoch reuse aliases old commands or presentation.
- A blocked/flooding client stalls another peer/world, queues are unbounded,
  mandatory events/edges silently drop, ENet fragmentation violates the approved
  delivery contract, or service/tick/latency budgets fail.
- Required wire/count/queue bounds must change, unknown messages require
  permissive acceptance, or protocol/content/dependency changes lack approval.
- Clean native Windows fails, native access/impairment tools are unavailable,
  required evidence remains missing, or the task/timebox requires re-estimation.
- Implementation needs prediction, broader modes/content/social features,
  Windows hosting or another scope/architecture change.

No relaxed goldens, different seed, tolerance comparison, speculative omniscient
view or unapproved transport/toolchain substitution resolves a stop condition.

## 8. Open owner decisions — questions with recommendations

**All questions below remain OPEN.** Recommendations are planning assumptions,
not decisions. Record the owner's ruling before dependent tasks; already-approved
two-second removal, admin transfer, Linux hosting and no-observer policies are
not reopened by this list.

### D3-A — Which wire version and compatibility policy should carry Phase 3?

**Recommendation:** exact-version **1.1**, new authoritative spec/fixtures,
simultaneous client/server upgrade, explicit 1.0 mismatch; retain old fixtures
and isolated baseline coverage. This follows the approved major/minor envelope
and avoids a dual-stack service. Snapshot widths/semantics do break 1.0, so if
the owner treats minor versions as backward-compatible, call it **2.0** instead.
Approve the exact schema in P3.3; the plan does not assign message numeric IDs.

### D3-B — What are ready/start/reset and drone-selection semantics?

**Recommendation:** admin chooses maze and 0–2 drones for two humans; solo fixes
one drone. Both humans explicitly ready to the current config revision, admin
sends explicit start; config change invalidates all readiness. RESET from
results/live match leads to shared lobby (live reset is a labeled abort), giving
rejoins a real window, then a new ready/start transaction increments epoch.
This preserves admin authority and avoids READY auto-start racing joins. Should
ready/unready be an explicit field, and must reset preserve the prior selection?
Recommend yes to both; require readiness again.

### D3-C — Do you approve the identity/barrier fixes and event recipient table?

**Recommendation:** u32 full entity-life generations in snapshots/input/results,
true bullet/explosion generations, full-width content IDs, explicit reliable
initial-view barrier, and occurrence-ticked recipient-authorized events.
Keep VIEW-v1 knowledge unchanged: own SCORE before results, visible local
effects/spawns only where reference-authorized; public winner/results to both.
Use per-recipient event sequencing to avoid interpreting filtered global gaps
as loss. These close concrete lifetime/privacy gaps without changing gameplay.
Approve whether session status + snapshot disappearance suffices for departure
or a distinct REMOVE event is required, and whether map_revision may disclose
the existing global update count or should be recipient-local export metadata.

### D3-D — What happens to a lone survivor and departed participant records?

**Recommendation:** never invent a network forfeit winner or add bots mid-match.
Continue through unchanged legacy Combat/end conditions; if no scored winner
exists when ordinary rules end, show an abort/no-winner result. Preserve stable
departed combatant/vehicle-owner storage until outstanding projectile references
and match cleanup complete; keep their earned score/name in final results with
a departure marker if approved. This avoids pointer lifetime changes and fake
kill credit. Confirm this interpretation for two humans/zero drones and approve
P3.2's exact live/dead removal bookkeeping before implementation.

### D3-E — What numeric fairness, identity and pre-match timeout policy applies?

**Recommendation:** rotating peer-first bounded admission with per-peer quotas
within the existing global 64-service-event/128-intent bounds, guaranteed reserve
for both neutralizations, explicit saturation acks, no changed consume ordering.
P3.6 presents exact quota numbers against worst-case legitimate input before
freeze. Reject duplicate player names with an actionable lobby error rather
than the solo fallback name; keep connection identity independent of names.
Use a bounded handshake deadline (recommend three seconds, matching the client)
and valid lobby heartbeats to reclaim abandoned peer slots. Confirm timeout
boundary (`>=2,000 ms` recommended) and which accepted fresh traffic qualifies;
malformed/stale/replayed traffic must not count as valid input.

### D3-F — How should oversized snapshot fragmentation preserve sequencing?

**Recommendation:** keep self-contained 15 Hz full views and existing 64 KiB
application bounds; explicitly verify pinned ENet unreliable fragmentation and
atomic reassembly, choosing its supported unreliable-fragment flag where needed.
The initial reliable barrier is separate. Do not allow flags-0 oversized views
to silently become reliable backlog. If measured fragment loss fails the approved
matrix, bring an exact bounded chunk/reassembly or compression proposal back to
the owner with measurements; no such redesign is pre-approved here.

### D3-G — Which native hosts, operational runs and CI hand-off are authorized?

**Recommendation:** secure two clean Windows 11 x64 PCs plus the recorded
≥four-core/8 GB Ubuntu host now; close Phase 2 native/solo debt before large
Phase 3 integration. Require the existing 250 ms p95, 15 Hz/p99, 30-minute
impairment and 60-minute/twenty-reset acceptance unchanged. Run operational/native
checks explicitly on these hosts; automate fast Linux checks in the current
runner arrangement only after CI ownership/artifact retention is agreed. Who
supplies machines/network impairment privileges and owner play sessions? If
latency fails, recommend diagnosis first and an owner-reviewed presentation
tradeoff; do not silently increase interpolation or introduce prediction.

Owner approval of this plan and the necessary rulings starts execution.
Re-estimate broader parity/hardening after the first verified two-PC match;
Phase 4 remains a separate release-hardening scope.
