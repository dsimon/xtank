# xtank Windows modernization — Phase 1 exit report

**Phase 1 — real headless extraction.** Final report for the Phase 1 branch
`windows-phase1` (clone of `windows`), covering tasks P1.1–P1.16.

- Working directory: `/home/hatch/workspace/coding/xtank-phase1`
- Baseline tree: `63016bad6781ea238c0e52ea2b6efda1b9a8db45` (the clone is shallow;
  the plan's pin `14e6d42efdc0318fbf37717b21cc9f6027dc7e9f` object is not present,
  but the baseline tree reproduces the canonical oracle byte-for-byte and is
  treated as the pinned baseline — see P1.1).
- Host/toolchain: Ubuntu 24.04 x86-64, gcc 13.3.0, flex 2.6.4, `-O0 -g`,
  `LC_ALL=C`, seed `114336`, harness-owned `Xvfb`.
- Evidence root (outside Git): `/tmp/opencode/xtank-phase1/p1.<task>/`
  (large trees symlinked to `/home/hatch/workspace/coding/xtank-evidence/`).

## Result

**Phase 1 exits PASS.** The tree builds three binaries — the X front-end
(`Src/xtank`), the display-free headless oracle (`Src/xtank-headless`) and the
terminal-free local server (`Src/xtank-server`) — and the simulation is
byte-identical to the frozen pinned X reference on every legacy and
characterization scenario, on both the X and headless subjects. The full exit
gate `make phase1-check` passes (oracle, geometry, intent, lifecycle, content,
package, boundary), and the 15 Hz timing and 60-minute bot evidence are
recorded separately.

No gameplay rule changed in any task. Every task ran the mandatory **G**
guardrail before and after, requiring byte identity between the pinned X
reference, the pre-change baseline and the post-change candidate; no byte
difference was ever observed.

## Task summary

| Task | Title | Result |
| --- | --- | --- |
| P1.1 | Bootstrap frozen reference and legacy guardrail | PASS — `regression.sh`, frozen reference + manifest, canonical v1 SHAs reproduced |
| P1.2 | Per-tick, bot-free characterization | PASS — v2 format, bot-free traces, `state_dump.c` event assertions, v2 goldens |
| P1.3 | Neutral graphics contract, private X types | PASS — `graphics_backend.h`, `--headers` X-free |
| P1.4 | Route macros and X escape hatches | PASS — vtable dispatch, `--sources` X-free |
| P1.5 | Real null backend and isolated build | PASS — `Display/null.c`, X/server split, `--build` X-free; no `libX*` |
| P1.6 | Explicit match/player context and tick seam | PASS — `server_core.c`, `server.h`, lifecycle oracle |
| P1.7 | Bounded intent queue and legacy X adapter | PASS — `intent.h`/`intent.c`, 54 assertions, reference-independent |
| P1.8 | Explicit in-process seed | PASS — `XTANK_SEED` → `ServerConfig.seed`; malformed/missing rejected in replay |
| P1.9 | Monotonic 15 Hz loop | PASS — `server_loop.c`, 900 ticks/60 s, p99 0.44 ms, paced == fast |
| P1.10 | Headless geometry initialization | PASS — `geometry.c`, X == server table, all 16 Marauder views |
| P1.11 | Specials knowledge versus draw | PASS — model/draw split, zero-presentation knowledge preserved |
| P1.12 | Simulation-owned end-of-tick bookkeeping | PASS — zero render/event/sync calls, identical checksum |
| P1.13 | Tick-cooperative drone | PASS — one iteration/yield per eligible tick, CPU-load stable, 60-min soak |
| P1.14 | Display-free content/config loading | PASS — explicit asset root, list-independent, release config verified |
| P1.15 | Local server entry and relocatable bundle | PASS — `xtank-server`, bundle runs both mazes with env unset |
| P1.16 | CI oracle and Phase 1 exit | PASS — `make phase1-check`; this report |

## Frozen oracle values

Legacy v1 (period 100/200/200), byte-identical on reference, X candidate,
headless candidate and before/after; also confirmed by the P1.12 runtime audit
and the P1.15 server package:

| Scenario | Lines | Win/final frame | Final hash | SHA-256 |
| --- | ---: | ---: | --- | --- |
| `combat-2drone` | 798 | 26569 | `eec7687e82490ac9` | `76f23a333a0907f9bb40fa4d9027feac39f3eccdc11aa85aa34fa33109688c6a` |
| `combat-4drone` (camo) | 105 | 4004 | `78bee90daa44a610` | `e13b72584d6cc6d41ae0c944c7b1235ca88807f23159e7a593fc56f6fe192b6e` |
| `madman-2drone` (finite 65600) | 984 | 65600 | `b2424468cbe82c16` | `440b0e5b7ccd53c6aed3a5b33916d2289c80889446a42b0c399986486b6f9c03` |

v2 per-tick bot-free characterization (period 1, tick-limit 400), frozen X
goldens (`Tests/phase1/v2-golden.sha256`):

| Scenario | X v2 dump SHA-256 |
| --- | --- |
| `trace-quadrant` | `284fa217955fbc3a1ccc3dac612b3538819caa31e84e990ccc2d1aeebbb7f776` |
| `trace-arena` | `040bd2518d77bb91a4a9ac2c70807819770bc00ef955380b78909115c5dd7936` |
| `trace-arena-win` | `8dc1d40b14fa06dd27717023ca740d8666f02929ea1063d8953e0d6b6b247d76` |

Other frozen evidence:

- Geometry table (X == server): `9a280594828b29572d51abfb2159ca546d2f785e451b0768a30d5ce8fa51cda1`.
- Content manifest: `Mazes/combat1.m d384f05c…`, `Mazes/Arena.m 288d69fa…`,
  `Vehicles/Vanguard.V 04dc0e29…` (`Tests/phase1/content.golden.sha256`).
- 15 Hz timing: 900 ticks in 60 011 ms, p99 tick work 0.44 ms ≪ 66.7 ms,
  paced == fast (P1.9; later runs 1.07/1.16 ms also pass).
- Bot scheduling: yields exactly `2 × ticks × bots`; watchdog exit 70 on a
  non-yielding bot; 60-minute soak = 21 epochs / 84 084 ticks with movement
  (minmove 522 455), targeting (5 296) and fire (508 704).
- Server runtime: `combat1` 2 matches / 16 971 ticks, `Arena` 2 matches /
  8 834 ticks; `display_terminal=update_screen_locs=sync_terminals=get_events=
  multi_sync=make_terminal=0`; no X connection, no `libX*`, no `libdl`.

No byte difference was observed against a frozen reference anywhere in Phase 1,
so there is no first-differing tick/field to report.

## Architecture delivered

- **Backend boundary (P1.3–P1.5):** `Src/Include/graphics_backend.h` is the
  neutral contract (points/segments, window flags, opaque `Video`, the
  `GraphicsBackend` vtable, neutral `draw_*`/accessors). X types live only in
  `Src/Include/x11.h`/`Src/Display/x11.c`; `Src/Display/null.c` is a genuine
  no-op backend; `Src/Display/graphics.c` dispatches through the vtable.
- **Simulation seam (P1.6–P1.9):** `Src/server_core.c` owns the one-world match
  lifecycle, the game clock and RNG; `Src/server_loop.c` is the monotonic 15 Hz
  fixed-step loop; the X front-end keeps its legacy pacer.
- **Intent queue (P1.7):** `Src/intent.c` carries every player control into the
  simulation once per tick, after bots and before movement.
- **Geometry (P1.10)** and **specials knowledge/draw split (P1.11)** make the
  simulation independent of the presentation pass; **P1.12** moves the inert
  end-of-tick bookkeeping (disc clear, camo shadows, map commit) into the
  simulation at the exact legacy phase.
- **Content (P1.14)** loads supplied assets from an explicit root, list-free and
  with no RANDOM fallback, and expresses the approved release configuration.
- **Server (P1.15):** `Src/server_main.c` is a terminal-free entry point; the
  bundle (`Bin/package-server.sh`) is relocatable and links only the `drone`
  program with no `DYNALOAD`, editors or X front end.

## What Phases 2–3 inherit (core and intent entry points)

The headless/server core API is declared in `Src/Include/server.h`; the
player-input contract is in `Src/Include/intent.h`; the neutral backend contract
is in `Src/Include/graphics_backend.h`.

Match/lifecycle core (`server.h`):

- Config: `ServerConfig`, `ServerPlayers`, `server_config()`,
  `server_config_init()`, `server_config_add_player()`, `server_player_name()`.
- Lifecycle: `server_init()`, `server_match_init()`, `server_step()`,
  `server_match_reset()`, `server_destroy()`, `server_frame()`,
  `server_frame_reset()`, `server_seed_random()`, `server_tick_pace()`.
- Loop/pacing: `server_loop_configure/active/hz/ticks/pace/run`, default 15 Hz.
- Intent seam: `server_consume_intents(int tick)`.
- Presentation gate: `server_presentation_active()`.
- Release config/content: `server_release_settings()`,
  `server_config_release()`, `content_set_root()`, `content_load_maze()`,
  `content_load_vehicle()`, `content_load_settings()`, `content_last_error()`,
  `game_results()`.

Intent queue (`intent.h`):

- `Intent` (`tick`, `player`, `sequence`, `op`, `a`, `b`), `IntentOp`
  (drive/heading/aim-world/aim-target/fire/fire-hold/weapon/chase/standoff),
  `IntentStatus`; `intent_init()`, `intent_push()`, `intent_pending()`,
  `intent_consume(tick)`, `intent_reset()`; bound `INTENT_QUEUE_MAX = 128`.
- `Src/input.c:input_event_intent()` is the legacy-event → intent adapter for a
  future (Phase 2/3) input/transport source; it is not yet the active source.

These are the seams Phase 2 (network protocol/transport/session service) and
Phase 3 (Windows client/front end) are expected to build on. `intent_push` is
the natural hand-off from a session/service to the simulation; `server_step` is
the tick boundary; `content_*` is the asset/session loader; `graphics_backend.h`
is the neutral contract a future Windows presentation backend implements.

## Regression strategy

`bash Tests/phase1/regression.sh --suite full --stage before|after --subject
x|headless|null` builds an isolated frozen reference (pin overlay + read-only
`STATE_DUMP`/seed/replay instrumentation) and an isolated candidate outside the
checkout, starts harness-owned Xvfb servers, and requires byte identity
(`cmp`) between the reference, three candidate repeats, and the recorded
before/after stage dumps, plus frozen line/frame/SHA metrics and the v2 event
assertions. `--subject auto` now resolves to the display-free `null` backend
(post-P1.5). No golden is ever relaxed to bless a simulation difference.

## Deferred (explicit)

- **§7 step 8 (CI gate and handoff): deferred.** Phase 1 ships the CI oracle
  (`.github/workflows/phase1.yml` → `make phase1-check`) but the broader §7
  step 8 handoff items (hosted/remote CI service, cross-platform runners,
  artifact retention policy) remain with the owner.
- **No protocol, transport, session service, sockets or Windows frontend code
  exists at this exit** — these are Phases 2–3 by the approved scope. No
  protocol structs, sockets or Windows frontend code were added in P1.16.
- **Windows-hosted server / fibers** remain deferred by the Linux-only MVP;
  P1.13 keeps the Linux `swapcontext` threader.
- `timing.sh` (60 s) and `bots.sh --minutes 60` are recorded separately from
  `phase1-check` (CI job `timing-and-bot-evidence`, manual dispatch).
- The shipped server supports 1–2 local `drone` combatants; the "2 humans +
  0–2 drones" mode has no local human/network input source in Phase 1.
- Enabled backend batching, removal of dead null-backend rotation helpers, and
  a settings-file CLI path for the server are cosmetic/future cleanups only.

## Report index

Per-task reports are in the working directory root: `P1.1-REPORT.md` …
`P1.16-REPORT.md`.
