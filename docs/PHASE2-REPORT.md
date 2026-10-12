# Xtank Windows — Phase 2 report

**Windows client and single-client vertical slice**

Status: **implementation complete; automated Linux exit gate green; native
Windows and owner sign-off outstanding.** Prepared 2026-10-12 on the
`windows-phase1` working tree (HEAD `63016bad6781ea238c0e52ea2b6efda1b9a8db45`,
Phase 1/2 uncommitted; commits are the owner's).

This report summarises what Phase 2 built, the acceptance evidence, what is
deliberately deferred, and what Phase 3 inherits. Authority: `docs/PORT-SCOPE.md`
(Phase 2), `docs/PHASE2-PLAN.md`, `docs/PHASE2-DECISIONS.md` (D2-A–D2-F),
`docs/PROTOCOL-v1.md`, `docs/VIEW-v1.md`, plus the per-task `P2.N-REPORT.md`.

## 1. What Phase 2 delivered

A native Windows 11 x64 client (C + SDL3 3.4.18) that connects directly over
ENet/UDP to the existing Linux authoritative server, drives a lobby, completes
and resets a Combat match against one `drone`, and renders the match from
authorized snapshots — with the server remaining the sole owner of movement,
collisions, damage, RNG, bots, knowledge, respawn and scoring. The frozen Phase 1
simulation is unchanged.

| Task | Deliverable |
| --- | --- |
| P2.1 | Decisions D2-A–F frozen; pinned SDL3/ENet/MinGW-w64 versions and SHA-256s; Phase 1 baseline re-verified. |
| P2.2 | Frozen binary **protocol v1** (`docs/PROTOCOL-v1.md`, `Protocol/`): 32-octet big-endian envelope, bounded fields, 3 channels, explicit op range, SHA-256 content identity; 16 golden wire fixtures + independent verifier. |
| P2.3 | Portable, allocation-free **codec** (`Protocol/encode.c`, `decode.c`): golden bytes on Linux and MinGW-w64, full rejection/truncation/NaN coverage, ASan+UBSan fuzz, no allocator symbols. |
| P2.4 | **Terminal-free human lifecycle/binding** (`Src/server_players.c`): name→controller→slot with generation-checked rebinding, zero `Terminal` allocation, human defaults preserved; legacy terminal access isolated behind one safe accessor. |
| P2.5 | **Authorized read-only view exporter** (`Src/server_view.c`, `docs/VIEW-v1.md`): world/map+radar/own-HUD DTOs from the legacy visibility rule, no `real_map`, no draw/special/RNG call; read-only parity over 0/1/5 exports. |
| P2.6 | **Entity lifetimes, events and results** (`Src/server_events.c`): export-side generation counters, `DEATH/RESPAWN/SCORE/EXPLOSION/MATCH_END` events with dedup keys, results captured before score reset; reviewed read-only hooks only. |
| P2.7 | **Nonblocking transport + input admission** (`Transport/enet_transport.c`, `Src/server_net.c`): bounded ENet service, ownership/finite/range validation, cross-channel dedup, assigned-tick acks, release reserve and backpressure abort; bounded pre/post-step hooks. |
| P2.8 | **Solo lobby and network-server path** (`Src/server_session.c`): `Disconnected→Handshake→Lobby→Running→Results→Lobby`, allowlisted content (`combat1`/`Arena`, `Vanguard`), first-peer admin start/reset, one peer + one drone, epoch barriers, D2-F two-second solo abort. |
| P2.9 | **Cross-built Windows shell** (`Client/`, root `CMakeLists.txt`, `cmake/mingw-w64.cmake`, `Tests/phase2/windows-smoke.ps1`): SDL3 window/event/timer/renderer + codec + ENet, MinGW-w64 x86-64, Clean-machine DLL audit, Wine runtime smoke. |
| P2.10 | **Original visual assets + full-frame drawing** (`Client/assets.c`, `render_sdl.c`, `Tools/export-client-assets.c`): checked export of body/turret/projectile/explosion/landmark art + palette + 16 body views, byte-identical to frozen geometry; snapshot-driven renderer, no trails, true `DRAW_XOR`. |
| P2.11 | **Snapshot model, camera, interpolation** (`Client/model.c`, `timeline.c`, `camera.c`, `world_render.c`): bounded 8-entry ring, monotonic render timeline, one-tick (66.7 ms) interpolation, snap on teleport/death, no extrapolation, legacy 768×768 viewport. |
| P2.12 | **Controls + exact world-intent transform** (`Client/input.c`): keyboard drive/heading, mouse world aim/fire, weapon select, 15 Hz emit (never tick 0), two-tick redundancy, one-shot edges, pause/defocus/quit release, inverse screen→world/DPI transform. |
| P2.13 | **HUD, mapper, radar, menu** (`Client/hud.c`, `map.c`, `radar.c`, `menu.c`): authorized own-HUD fields, remembered/stale mapper knowledge only, anonymous radar blips, frozen reference lobby texts/layout, server-authored results/banner. |
| P2.14 | **Playable integration** (`Client/session_client.c`, `frame.c`): the live nonblocking frame loop wiring the protocol, model, controller and renderer into one playable slice; no client simulation, presentation never schedules ticks. |
| P2.15 | **Network-path parity audit** (`Tests/phase2/parity.sh`, `parity_fixture.c`): frozen-X == direct-intent == direct == network byte identity over 800 ticks; decoded snapshot stream matches the direct own-player position/score. |
| P2.16 | **Exit gate and packaging**: `Tests/phase2/phase2-check.sh`, `Bin/package-client.sh`, `Bin/package-phase2.sh`, `make phase2-check`, this report. |

## 2. Acceptance results (2026-10-12)

`bash Tests/phase2/phase2-check.sh` → **P2.16 PHASE2-CHECK PASS (13 drivers +
packages)**, exit 0. Every driver green:

| Driver | Task | Result |
| --- | --- | --- |
| `protocol.sh` | P2.2 | PASS — headers standalone, 16 fixtures verified (165 checks), assets unchanged, worst-case snapshot 28 492 B ≤ 65 536. |
| `codec.sh` | P2.3 | PASS — golden bytes Linux + MinGW-w64, rejections, 20 s ASan/UBSan fuzz, no allocator. |
| `players.sh` | P2.4 | PASS — 2 terminal-free human epochs, `make_terminal=0`, `display_terminal=0`, `get_events=0`; `--human` smoke. |
| `view.sh` | P2.5 | PASS — field/visibility allowlist, viewport edges, mapper vs `real_map`, anonymous/expired radar, death camera, 28 524 B max, 0/1/5-export parity. |
| `events.sh` | P2.6 | PASS — death/respawn/score/explosion/match-end, results before reset, epoch scoping, dedup/reorder, read-only parity. |
| `transport.sh` | P2.7 | PASS — ownership, validation, cross-channel dedup, assigned-tick acks, saturation + release reserve, stale epoch/generation, paused-reader bounded, backpressure abort. |
| `session.sh` | P2.8 | PASS — handshake/content rejection before match, second peer rejected, allowlist, admin start/reset, no controls outside Running, completed vs abort, 2 s timeout, repeated reset; display-free network package. |
| `windows-smoke` | P2.9 | PASS — Linux-native `--self-test` (120 checks, 0 failures) with the sandbox `sendmsg` shim; Wine supplemental run also PASS (120 checks). |
| `assets.sh` | P2.10 | PASS — exporter geometry == frozen X/server, 16-view rotation, Marauder/Vanguard, checked bundle, exact scale/art, no trails, XOR. |
| `timeline.sh` | P2.11 | PASS — bounded model, monotonic timeline, one-tick interpolation at 30/60/144 FPS, snap on teleport/death/respawn, no ghosts/extrapolation. |
| `input.sh` | P2.12 | PASS — edge/held mapping, 15 Hz emit, never tick 0, pause only outside Running, quit/defocus release, no raw keyboard passthrough. |
| `ui.sh` | P2.13 | PASS — authorized HUD values, remembered-only mapper, anonymous radar, frozen lobby texts, results/banner with names, reset clears only retained results. |
| `playable.sh` | P2.14 | PASS — 41-check slice: connect/handshake/lobby/ready/start, 60-tick play, 15 Hz input, interpolated render + HUD/map/radar, defocus/pause/quit, win results, epoch-2 reset, clean disconnect. |
| `parity.sh` | P2.15 | PASS — frozen-X == direct-intent == direct == network over 800 ticks (byte identity); decoded snapshot stream matches direct position/score. |

Package verification inside the gate: client PE imports are exactly
`KERNEL32.dll, msvcrt.dll, SDL3.dll, WINMM.dll, WS2_32.dll` (Win32 system DLLs
+ bundled SDL3, no libgcc/libwinpthread/Cygwin/MSYS); client `.zip` unzips with
exe + `SDL3.dll` + asset bundle; the packaged network server starts display-free
(`SERVER-LISTEN port=… hz=15`) with `DISPLAY`, `XTANK_DIR`, `HOME`,
`LD_PRELOAD` and asset-root overrides unset.

The Phase 1 exit gate is unchanged. With `--with-phase1` / `make phase2-check
PHASE2_WITH_PHASE1=1` the gate also runs `make phase1-check`; it passes.

## 3. Regression guardrail G — byte-identical before and after

```sh
export LC_ALL=C XTANK_SEED=114336
XTANK_P1_EVIDENCE=/tmp/opencode/xtank-phase2/p2.16/oracle-before \
  bash Tests/phase1/regression.sh --suite full --stage after --subject auto
XTANK_P1_EVIDENCE=/tmp/opencode/xtank-phase2/p2.16/oracle-after \
  bash Tests/phase1/regression.sh --suite full --stage after --subject auto
```

Both: **P1.2 REGRESSION PASS**. Per-file `cmp` of the two `runs/` trees (66
files): **all 42 authoritative `*.state` / `*.result` / v2 `*.assert` files are
byte-identical**. The only 12 differing files are candidate-repeat
`*.state.stdout` timing lines (`SERVER-LOOP … avg_us/p99_us/max_us`), with
identical tick counts (`combat-2drone` 26 569; `combat-4drone` win 4004;
`madman-2drone` frame 65 600). No golden, seed or flag changed. Frozen v1
values reproduce exactly: `combat-2drone` SHA-256
`76f23a33…88c6a`, 798 lines, win frame 26 569; `combat-4drone`
`e13b7258…92b6e`; `madman-2drone` `440b0e5b…f9c03`.

P2.16 added only harness/packaging files and a `GNUmakefile` target; no
simulation source is in the oracle candidate list, so the change is
structurally simulation-neutral. (Phase 1/2 are uncommitted, so both G runs use
`--stage after` on the working tree, exactly as in P2.1–P2.15.)

## 4. Packaging

`bash Bin/package-phase2.sh` builds both relocatable archives under `--out`:

- **`xtank-client.zip`** (`Bin/package-client.sh`), Windows x64: `bin/xtank-client.exe`
  (cross-built, `-static-libgcc`), bundled `bin/SDL3.dll`, `assets/xtank-assets.bin`
  (180 274 B payload, FNV-1a `03358bb1`, SHA-256 `9141e3b9…da7e81`), the 16 wire
  fixtures, `tools/windows-smoke.ps1`, xtank `COPYRIGHT`/`COPYRIGHT.notes`,
  SDL3 (zlib) and ENet (MIT) licenses, `THIRD-PARTY-NOTICES.txt`, `README.txt`,
  `BUILD-INFO`.
- **`xtank-server.tar.gz`** (`Bin/package-server.sh --net`), Ubuntu network server:
  `bin/xtank-server` + `share/xtank/{Mazes,Vehicles}` + notices, launched with
  `--listen PORT`. The binary is `-DNULL_BACKEND`/`-DXTANK_WITH_NET`, links no
  X11, and resolves assets relative to `/proc/self/exe`.

Gate run archive hashes:

| Archive | SHA-256 |
| --- | --- |
| `xtank-client.zip` | `460740a07d449e29e60b7b32db280385f4b6e473f92aa25781bc7e6b65d2b4df` |
| `xtank-server.tar.gz` | `226d4d011ed15738189686f21655e54f25c9b382c4b4e76974fdcfc5572b6f60` |
| `xtank-client.exe` | `2f9254f39d361a6b7d185d56a2943840c2543520585c705109f45f0dc146edba` |
| `SDL3.dll` | `4659d5a1e240e7d58fb92808f84afeb53395818cec62385d4796b72977a7d859` |

## 5. Exit-criteria assessment (`docs/PHASE2-PLAN.md` §2)

| # | Criterion | State |
| --- | --- | --- |
| 1 | Clean Windows unpack/run without compiler/X/POSIX; server starts `DISPLAY` unset | Server: **proven** (gate). Client cross-build + DLL audit + Wine smoke: **proven**; **native Windows 11 unpack/run outstanding** (no Windows host reachable). |
| 2 | Both mazes, full match each, reset in same processes | Fixture/parity: **proven** on Linux (Arena parity; combat1 playable). **Owner live Windows play outstanding.** |
| 3 | Scripted collision/damage/mapper/radar/death/respawn/score/win/reset fixtures | **Proven** (P2.5/P2.6/P2.14/P2.15 fixtures). Visual-tick association via `--ui-demo`. |
| 4 | Full Phase 1 gate + network-path exact parity | **Proven** (G before/after; `phase2-check --with-phase1`; P2.15 three-way). |
| 5 | 900 ticks/60 s ±1 at 30/144 FPS; p99 < 66.7 ms; backpressure cannot stall | P2.7 bounded transport + paused-reader, P2.11 30/60/144 FPS replay, P2.15 tick-rate checks. **Recorded-host p99 and the 60-minute paced soak remain** (`Tests/phase2/timing.sh` not implemented under the P2.15 contract). |
| 6 | Explicit failures for protocol/content mismatch, malformed/stale/duplicate/foreign ownership; no unauthorized view leakage | **Proven** (P2.7/P2.8 rejection paths; P2.5/P2.15 view-leakage and anonymous-contact checks). |
| 7 | Owner solo-play sign-off; report records versions/specs/evidence | Report: this document. **Owner solo Windows sign-off outstanding.** |

## 6. Deferred / not claimed

- **Native Windows 11 proof** — no Windows host was reachable in this
  environment. The cross-build, PE import allowlist, Wine runtime smoke and
  `Tests/phase2/windows-smoke.ps1` are ready; the owner must run the archive on
  a clean Windows machine (P2.9 §"What remains").
- **Owner live solo play** — controls, reference feel, appearance and content
  selection (exit #7).
- **Paced timing/soak** — `Tests/phase2/timing.sh`/`backpressure.sh`/`latency.*`
  and the 60-minute twenty-reset soak named in the plan P2.15/broad plan were not
  implemented under the explicit P2.15 contract; P2.7/P2.11/P2.15 cover the
  bounded-service, 30/60/144 FPS and parity portions.
- **Phase 3 scope** — two humans / 0–2 drones, four-vehicle acceptance, admin
  transfer, mid-match departures and removal without kill credit, lobby rejoin,
  the 30-minute impairment matrix and two-PC release acceptance. Windows-hosted
  server/fibers and offline host-and-play remain deferred beyond the Linux-only
  MVP. No Phase 2 claim covers these.

## 7. What Phase 3 inherits

Stable seams to build on: the versioned **codec** (`Protocol/`), the **transport
adapter** (`Transport/`), **controller/human binding** (`Src/server_players.c`),
the **authorized exporter** (`Src/server_view.c`), **lifetimes/events/results**
(`Src/server_events.c`), the **session state machine** (`Src/server_session.c`,
extensible from one peer), the client **model/timeline/camera/input/HUD** and the
**packaged Windows client**. Phase 3 must implement/test the two-human
configuration, fairness across peers, remaining-human departure/removal, admin
transfer, lobby rejoins, four-vehicle bandwidth/timing, the approved impairment
matrix and two-clean-Windows-PC release acceptance. Keep Linux-only hosting and
the eight approved scope decisions unless the owner amends them.

## 8. Evidence

All dumps, logs, binaries, reference worktrees and packages are outside Git under
`/tmp/opencode/xtank-phase2/p2.16/` (symlinked to
`/home/hatch/workspace/coding/xtank-evidence/p2.16/`): `phase2-check.log`,
`oracle-before/`, `oracle-after/`, `oracle-diff.txt`, `gate/` (driver logs,
packages, PE audit), and `p2.<N>/` per-task evidence. Committed: the new harness
and packaging source plus this report only.
