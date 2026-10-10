# Phase 1 — real headless extraction

## Goal and authority
End with a Linux headless server core and runnable local harness on a new branch from upstream `windows`: no X headers in the simulation's transitive includes, no X library dependency, no display/input/render pass needed to advance a match, a 15 Hz fixed-tick loop, an explicit bounded intent queue, supplied-content loading, cooperative `drone`, and a per-tick regression oracle against the pinned X build. Preserve simulation order, geometry and knowledge rules. No network protocol, transport, session service or Windows client exists at this exit; those belong to Phases 2–3. Windows-hosted server/fibers remain deferred by the approved Linux-only MVP.

- Authority: `PORT-SCOPE.md`, `PHASE0-BRIEF.md`, all eight unchanged approvals in `DECISIONS.md`, `SPIKE-A1-REPORT.md`, `EXTRACT-REPORT.md` §7.
- Pin: `14e6d42efdc0318fbf37717b21cc9f6027dc7e9f`; Ubuntu 24.04 x86-64, gcc 13.3.0, flex 2.6.4, `-O0 -g`. Preserve BSD/file notices.
- This checkout is currently `master` at the pin; the implementer creates the branch from `github.com/dsimon/xtank`'s `windows`. Verify the pin is an ancestor; record the fork SHA and investigate intervening source/asset changes before freezing evidence.
- `/home/hatch/workspace/coding/xtank-extract` is evidence/design input only. Reimplement deliberately; do not import its shadow X headers, 674-line X stubs, binaries or generated objects into the branch.

## Task contracts
Every task below requires **G** (the before/after guardrail below), plus its listed checks. Paths marked **new** are proposed implementation files. Sizes: S = narrow session; M = normal session; L = full session, split further if it cannot finish with evidence. No task authorizes a gameplay rule change.

### §7 step 7 first — establish the oracle before extraction
**P1.1 — Bootstrap frozen reference and legacy guardrail [M; deps: none].**
- Files: `Tests/phase1/regression.sh`, `Tests/phase1/build-reference.sh`, `Tests/phase1/fixseed.c`, `Tests/phase1/scenarios/*.settings` (new); `Src/animate.c` read-only instrumentation hook.
- Goal: recreate `STATE_DUMP` from scratch with the gate's field order, hash and dump positions; build isolated candidate/reference object trees; keep the pinned reference source/binary and evidence outside Git.
- Accept: `bash Tests/phase1/regression.sh --stage before --subject x`, then `--stage after --subject x`, passes §Regression; independently reproduced scratch-headless dumps match X. Legacy canonical SHA/win/hash match the values below. Freeze reference manifest and refuse missing/empty/truncated dumps.

**P1.2 — Per-tick, bot-free characterization [M; deps: P1.1].**
- Files: `Tests/phase1/state_dump.c`, `Tests/phase1/reference_input.c`, `Tests/phase1/scenarios/*`, `Tests/phase1/regression.sh` (new/extend); `Src/animate.c` hook only.
- Goal: retain legacy format v1; add v2 explicit-field dumps for maze mutations, full vehicle/bullet/explosion state, mapper exploration and authorized radar records. No pointers/padding or screen coordinates in v2. Cover all tick exits and reset epochs.
- Accept: `bash Tests/phase1/regression.sh --suite full --stage after --subject x`; period 1, three repeats, identical X/candidate v2 dumps. Bot-free tick-indexed drive/steer/aim/fire/weapon traces prove wall/tank/bullet collisions, directional damage, death/respawn/score/win and reset on both MVP mazes; assertions prove each event occurred. Freeze traces and X goldens before changing simulation.

### §7 step 1 — backend boundary
**P1.3 — Neutral graphics contract, private X types [L; deps: P1.2].**
- Files: `Src/Include/{graphics,x11,proto,tanktypes}.h`, `Src/Display/{graphics,x11,XMultiSync}.c`, `Src/{gr,input,interface,menu,hud}.c`, `Src/Programs/Bootlegger.c`, `Src/Include/graphics_backend.h` (new).
- Goal: move X resources/prototypes into backend-private declarations; define neutral points/segments, window flags and backend operations; make `Video` opaque and migrate direct field consumers to accessors, including backend wrappers needed to keep escape-hatch callers compiling.
- Accept: `make`; G with X subject; existing X event/draw/sync behavior preserved. `bash Tests/phase1/check-x-boundary.sh --headers` (new) preprocesses representative physics/bot headers and fails on transitive `<X11/...>` or X resource types.

**P1.4 — Route macros and X escape hatches [M; deps: P1.3].**
- Files: `Src/Include/graphics_backend.h`, `Src/Display/{graphics,x11}.c`, `Src/{gr,hud,input,interface,menu}.c`, `Src/Programs/Bootlegger.c`; affected `Src/{display,mapper,radar,newradar,newconsole}.c` primitive callers.
- Goal: route `draw_*`, batching, window flags, preferences, beep and synchronization through the backend vtable/accessors; encapsulate direct `XDrawArc`/`XDrawLine` callers.
- Accept: `make`; G with X subject; `bash Tests/phase1/check-x-boundary.sh --sources` permits X calls/types only in backend-private files. Preserve `X11`-conditioned input semantics explicitly before removing that define from server compilation.

**P1.5 — Real null backend and isolated build [M; deps: P1.4].**
- Files: `Src/Display/null.c` (new), `Src/Display/graphics.c`, `Src/{GNUmakefile,Makefile.inc1,Makefile.inc2}`, root `GNUmakefile`, `Tests/phase1/check-x-boundary.sh`.
- Goal: implement neutral no-op backend, initially retaining legacy presentation callers for parity; separate X/server objects and flags. No fake Xlib API or shadow headers.
- Accept: `make phase1-headless`; G with headless subject. `ldd Src/xtank-headless` has no `libX*`; `nm -u Src/xtank-headless` has no Xlib symbols; `bash Tests/phase1/check-x-boundary.sh --build` inspects compiler dependency files for every server translation unit and finds no X headers. Unset `DISPLAY`; use no X connection.

### §7 step 2 — command feed and simulation seam
**P1.6 — Explicit match/player context and tick seam [M; deps: P1.5].**
- Files: `Src/{animate,setup,main,players,lowlib}.c`, `Src/Include/proto.h`, `Src/Include/server.h`, `Src/server_core.c` (new).
- Goal: expose initialize/step/reset/destroy for one match; explicit player-to-vehicle binding, tick/quit-frame and RNG lifecycle. Retain single-world globals behind the API; do not attempt multi-match conversion.
- Accept: `make phase1-headless`; G. `bash Tests/phase1/lifecycle.sh` (new) runs initialize→match→reset twice→destroy in one process, compares each epoch to frozen X, and detects stale `term`/`cv`/ownership. Preserve deaths/respawns→changed boxes/messages→bots→human input→movement/collisions→specials→rules order.

**P1.7 — Bounded intent queue and legacy X adapter [M; deps: P1.6].**
- Files: `Src/input.c`, `Src/{actions,lowlib,server_core}.c`, `Src/Include/{intent,server}.h`, `Src/intent.c` (new), `Tests/phase1/intent.sh` (new).
- Goal: tick/player/sequence-tagged drive, heading, world-coordinate turret aim, fire held/release and weapon enable/select intent; consume once after bots and before movement. One controller/tank; stable player order then sequence; explicit bounded overflow rejection, invalid/stale/foreign-target rejection and held-control semantics.
- Accept: `bash Tests/phase1/intent.sh`; rejected/duplicate commands have no state effect, future commands stay queued, reset clears held/queued input. G's bot-free traces use the candidate queue versus the frozen reference's independent legacy operations (`anim_input`, `map_fire_weapon`, `anim_fire_weapon`, `lowlib`), not a shared new dispatcher.

### §7 step 3 — seed and pacing
**P1.8 — Explicit in-process seed [S; deps: P1.6].**
- Files: `Src/{util,main,server_core}.c`, `Src/Include/{server,proto}.h`, `Tests/phase1/regression.sh`.
- Goal: seed server RNG from config before the first random call; retain the pin's RNG implementation/call order; reset with an explicit seed policy.
- Accept: G; `env -u LD_PRELOAD -u DISPLAY XTANK_SEED=114336 bash Tests/phase1/regression.sh --stage after --subject headless` passes. Shim remains reference-only; malformed/missing configured seeds fail predictably in replay mode.

**P1.9 — Monotonic 15 Hz loop [M; deps: P1.7, P1.8].**
- Files: `Src/{server_core,setup,actions,icounter}.c`, `Src/server_loop.c` (new), `Src/Include/server.h`, `Tests/phase1/timing.sh` (new).
- Goal: absolute monotonic deadlines with rational 1/15-second accumulation; fixed simulation step independent of rendering. Catch up without skipping ticks; bounded work reporting. Fast replay bypasses sleep only, not simulation logic.
- Accept: G without legacy `-D`/signal pacing in server; `bash Tests/phase1/timing.sh --seconds 60 --vehicles 4` measures 900 ticks ±1, p99 tick work <66.7 ms on recorded hardware; paced and fast runs produce identical finite-tick dumps. No `SIGALRM`/`pause()` frame pacer in server path.

### §7 step 4 — remove presentation dependence
**P1.10 — Headless geometry initialization [M; deps: P1.5].**
- Files: `Src/Display/{objects,x11}.c`, `Src/Include/object.h`, `Src/geometry.c` (new), `Src/{collision,server_core}.c`, `Tests/phase1/geometry.sh` (new); `Src/Display/Objects/*.obj` read only.
- Goal: initialize body ordering, all 16 rotations, bounds/offsets, turret coordinates and collision segments without video/pixmap allocation; separate pure rotations from X upload.
- Accept: `bash Tests/phase1/geometry.sh` produces explicit geometry tables for X and server and `cmp` passes for all bodies/rotations, especially Marauder/Vanguard; G. No content conversion or collision formula changes.

**P1.11 — Specials knowledge versus draw [M; deps: P1.6, P1.10].**
- Files: `Src/{mapper,radar,newradar,hud,newconsole,program,vehicle}.c`, `Src/Include/{special,map,proto}.h`, affected specials declarations.
- Goal: preserve activation/update/deactivation and mapper/radar knowledge with zero terminals; separate draw callbacks from model transitions. Disable New Radar/Tac Link/Camo/Rdf only in MVP preset, not oracle controls.
- Accept: G including v2 mapper/radar equality; `bash Tests/phase1/specials.sh` (new) runs zero-terminal exploration/contact acquisition/loss/death/reset and proves no draw callback is invoked; X presentation remains usable.

**P1.12 — Simulation-owned end-of-tick bookkeeping [M; deps: P1.9, P1.11].**
- Files: `Src/{animate,display,disc,camo,setup,server_core}.c`, `Src/Include/{loc,vehicle,bullet}.h`, `Tests/phase1/regression.sh`.
- Goal: keep inert screen/`old_camod`/`thrower` fields; reproduce last-terminal disc clearing and camouflage shadow advancement at their exact legacy phase, including early-return behavior. Server no longer runs `display_terminal`, `update_screen_locs` or terminal synchronization.
- Accept: G's camo and finite Madman controls including `thrower`; `bash Tests/phase1/check-x-boundary.sh --runtime` instruments server calls and proves zero render/event/sync calls. Identical checksums with zero terminals; do not clear thrower earlier than the reference.

### §7 step 5 — cooperative Linux bot scheduling
**P1.13 — Tick-cooperative drone [M; deps: P1.12].**
- Files: `Src/{program,lowlib,highlib,icounter}.c`, `Src/Include/{program,thread}.h`, `Src/Threads/{thread,threadglue}.c`; `Src/Programs/drone.c` only for an evidenced bug fix.
- Goal: stable legacy live-list/program iteration, one `drone` iteration/yield per eligible tick, first-tick/respawn deferral preserved; remove CPU-time decisions from server scheduling. Keep Linux `swapcontext`; no fibers work.
- Accept: G for all frozen drone scenarios; `bash Tests/phase1/bots.sh --repeats 3 --cpu-load` (new) matches dumps and completes. `--minutes 60 --vehicles 4 --resets 20` shows movement/targeting/fire, no stall/crash/stale context. Any watchdog diagnoses failure without changing scheduling or silently yielding a bot.

### §7 step 6 — content and runnable server
**P1.14 — Display-free content/config loading [M; deps: P1.6, P1.8, P1.10].**
- Files: `Src/{file,newfile,setup,init,game}.c`, `Src/{setups,vload,sload}.l`, `Src/Include/server.h`, `Mazes/list` only if needed; `Mazes/{combat1.m,Arena.m}`, `Vehicles/Vanguard.V` read only.
- Goal: explicit asset root and error reporting; loaders independent of environment/X progress UI. Load `combat1.m` by explicit supplied path despite its absence from `Mazes/list`; no RANDOM fallback. Separate score calculation/results from graphical stats waiting.
- Accept: G plus `bash Tests/phase1/content.sh` (new): both exact maze files/Vanguard load with `DISPLAY`, `XTANK_DIR`, `HOME` unset; missing assets fail nonzero; asset SHA manifest matches pin. Config verifies Combat/FFA, ≤4 vehicles, respawn, bots may win, score 1000, 15 Hz, approved special preset and remaining `init_settings()` defaults.

**P1.15 — Local server entry and relocatable bundle [M; deps: P1.13, P1.14].**
- Files: `Src/server_main.c` (new), `Src/{main,program,server_core}.c`, root/Src `GNUmakefile`, `Bin/package-server.sh`, `Tests/phase1/package.sh` (new), `Src/Makefile.inc1`.
- Goal: build `Src/xtank-server` plus reusable core target; select supplied maze, seed and local scripted players/0–2 drones without graphical lobby; bundle required assets/notices relative to executable. Link only `drone`, no `DYNALOAD`, editors or X frontend entry point; keep four-drone/Madman oracle access in a test-only harness.
- Accept: `make phase1-server && bash Tests/phase1/package.sh`; relocated bundle in a path with spaces runs both mazes, match/results/reset with env unset, and G. Link/dependency/runtime audit proves no X headers/libs/events/render passes; process opens no X connection.

### §7 steps 7 and 8 — CI gate and handoff, not protocol work
**P1.16 — CI oracle and Phase 1 exit [S; deps: P1.15].**
- Files: `Tests/phase1/regression.sh`, root/Src `GNUmakefile`, `.github/workflows/phase1.yml`, `PHASE1-REPORT.md` (new).
- Goal: run frozen-pin X versus headless/server characterization in CI; report hashes, scenarios, timings, package/bot/lifecycle results and core/intent entry points for Phases 2–3.
- Accept: `make phase1-check` runs full oracle, geometry, intent, lifecycle, content, package and boundary checks successfully; timing/60-minute bot evidence is recorded separately. Report explicitly marks §7 step 8 deferred. No protocol structs, sockets or Windows frontend code in this task.

## Sequencing and critical path
- Oracle first: P1.1→P1.2→P1.3→P1.4→P1.5→P1.6→P1.7→P1.9→P1.12→P1.13→P1.15→P1.16 is the main critical path; P1.8 joins P1.9.
- After P1.5, P1.10 can proceed beside P1.6; then P1.11 must finish before P1.12. This geometry/specials chain can become critical if it exceeds queue/timing work.
- P1.8 and P1.14 can proceed beside queue/specials work once their dependencies finish; P1.14 joins P1.15. Use separate worktrees and agree file ownership for shared `server_core`, headers and Makefiles; merge shared-file changes serially and rerun G after integration.

## Regression strategy — G is mandatory before and after every task
- P1.1 bootstraps against `/home/hatch/workspace/coding/xtank-extract/extract-evidence/` and independently recreates the spike; all subsequent tasks use immutable pin-based real-X goldens, never a reference rebuilt from candidate gameplay. Only reviewed read-only dump/seed/replay/tick-limit instrumentation may overlay the pin.
- Install gcc/make/flex/Xlib development files/Xvfb; isolate objects. Use `LC_ALL=C`, seed **114336**, identical asset/settings/trace hashes and compiler flags. Start a harness-owned `Xvfb :99 -screen 0 1024x864x24 -nolisten tcp -noreset`; clean up only that PID. Candidate headless starts with `DISPLAY` unset. X ref uses `-S -D -P -X -F <settings> :99` (A1 pacer hangs).
- Legacy scenarios are exact copies of scratch `headless-build/{combat-2drone,combat-4drone,madman-2drone}.settings`, not the release preset: Quadrant/Vanguard; two Red/Blue drones, Camo off, score 2000; four neutral drones, Camo on, score 3000; Madman two Red/Blue drones, score 2000. Preserve every other setting byte.
- Reference seed: `XTANK_SEED=114336 LD_PRELOAD=<local>/fixseed.so`; candidate uses shim only until P1.8, then config seed. `XTANK_DUMP=<output> XTANK_DUMP_PERIOD=100` selects v1; P1.2 adds `--dump-format v2 --dump-period 1` harness options. Keep legacy v1 available permanently.
- Exact canonical reference invocation (run in the isolated reference's `Src`; `$R` = reference root, `$T` = absolute harness root): `env LC_ALL=C XTANK_DIR="$R" XTANK_SEED=114336 XTANK_DUMP="$E/x.state" XTANK_DUMP_PERIOD=100 LD_PRELOAD="$E/fixseed.so" ./xtank-refdump -S -D -P -X -F "$T/scenarios/combat-2drone.settings" :99`. Before P1.8 run candidate analogously with `env -u DISPLAY`, candidate asset root and `./xtank-headless`, omitting `:99`; thereafter pass config seed with `LD_PRELOAD` unset. Harness adapters must record the exact evolving candidate CLI.
- Canonical v1 expected SHA-256 **`76f23a333a0907f9bb40fa4d9027feac39f3eccdc11aa85aa34fa33109688c6a`**, 798 lines, win frame **26569**, final hash **`eec7687e82490ac9`**. Camo v1 SHA-256 **`e13b72584d6cc6d41ae0c944c7b1235ca88807f23159e7a593fc56f6fe192b6e`**, win frame **4004** (full checksum verified against local headless/X evidence). Do not invent expanded-v2 goldens.
- Madman is a **finite 65,600-tick replay**, not a wall-time prefix: capture/freeze exactly that bound in both builds (328 period-200 dumps). Fail if either run exits early, exceeds the bound or omits state; compare every bounded tick in v2. Its continued availability is a bookkeeping test, not additional release-mode support.
- P1.2 freezes bot-free traces for both `combat1.m` and `Arena.m`: tick/player/sequence/op/arguments plus exact initial placements/settings. X replay independently calls legacy controls at the old input seam. Candidate direct controls transition to intent queue in P1.7. Freeze event assertions and goldens; run all traces every task.
- Required command contract, implemented by P1.1/P1.2 (these commands do not yet exist): `bash Tests/phase1/regression.sh --suite full --stage before --subject auto`, apply change, then the same command with `--stage after`. `auto` uses candidate X until P1.5, headless thereafter, server after P1.15; always compare against frozen X and the task-parent dump. P1.1 uses bootstrap checks above for its own pre-change evidence.
- For each scenario/format/epoch, runner executes `cmp "$E/x.state" "$E/before.state" && cmp "$E/x.state" "$E/after.state" && cmp "$E/before.state" "$E/after.state"`; also `cmp` normalized final score/event records. Repeat candidate three times and `cmp` each dump; `sha256sum` records all outputs. Require byte identity, not tolerances or hash-only assertions.
- Store `$E` outside the checkout (e.g. `/tmp/opencode/xtank-phase1/<task>`), preserving exit codes, command lines, full baseline/candidate SHAs, toolchain/flags, asset/trace hashes and first differing tick. Oracle schema additions require old-format parity plus new frozen-reference parity; never update goldens to bless a simulation difference.

## Branch / PR discipline
- One task = one reviewed commit or small PR series into the implementer's Phase 1 branch off `windows`; no direct pushes of large blobs. Review status/diff before staging; include only task files and report. Integrate dependency-first; each merge reruns G on the integrated tip.
- Commit harness source, tiny settings/intent fixtures and concise reports. Keep dumps, screenshots, logs, binaries, packages, reference worktrees and generated flex/version/object files local; CI may retain ephemeral artifacts. Evidence reports record paths, hashes and reproducible commands. Preserve unrelated/untracked owner work.

## Risks, unknowns and stop conditions
- Spike retained rendering and sampled state; zero-terminal mapper/radar/disc behavior, early returns and reset statics remain unproved until P1.2/P1.11/P1.12. A1 used Linux Xvfb endpoints, not two Windows PCs; native-client feel remains a later owner acceptance gate.
- Raw legacy FNV uses host-sized values; byte parity is on the pinned supported build, not an asserted cross-ABI result. Geometry `Bits` and input `button_up`/`follow_mouse` depend on `X11`; removing the flag without neutral replacements can silently alter behavior.
- `drone` currently has a potentially uninitialized `enemy_flag`; do not silently fix it or regenerate reference checksums. CPU-time preemption or stack/context behavior may differ under load; any required bot fix must be documented and returned to the owner if it breaks G.
- Stop the task and send owner a minimal reproducer, first differing tick/fields, parent/reference hashes and proposed resolution for any byte difference, nondeterminism, unexplained pin/asset mismatch, oracle instrumentation that mutates state, hidden display dependency requiring rule changes, or inability to complete within one session without splitting.
- Also stop for required scope changes (extra modes/bots, shared controllers, protocol work, Windows hosting), scheduler stalls or sustained timing failure on the recorded host. Do not relax checks, change seed/settings, mask a failure with stubs or choose another architecture without owner approval. Carry the blocked task forward with its evidence.
