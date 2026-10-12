# AGENTS.md — xtank Windows modernization

Working reference for AI coding agents. Factual and concise; the design
documents are authoritative when they disagree with this file.

## What this project is

Xtank is a 1988-era X11 multiplayer tank game. This repository is the
**Windows modernization effort**. Phase 1 is **complete on branch
`windows-phase1`**: the authoritative simulation has been extracted from the
X11 game into a headless Linux server core, proven byte-identical to a frozen
pinned X reference. Phase 2 is **complete (2026-10-12)**: the native Windows
client and single-client vertical slice — protocol v1, ENet transport, solo
lobby/session, snapshot-driven renderer, input, HUD/map/radar, playable
integration, parity audit, exit gate — with the frozen simulation unchanged.
See `docs/PHASE2-REPORT.md`.

The tree builds three binaries from one source base:

- `Src/xtank` — the original X11 front-end (presentation + input + simulation).
- `Src/xtank-headless` — display-free oracle/harness build (null backend).
- `Src/xtank-server` — shipped terminal-free local server (`Src/server_main.c`).

No network protocol, transport, session service, or Windows client exists yet;
those are Phases 2–3. Windows-hosted server/fibers are deferred by the
approved Linux-only MVP. (Phase 2 has since delivered all of these; this line
is kept for historical context on the Phase 1 tree.)

Authoritative docs: `docs/PORT-SCOPE.md`, `docs/PHASE1-PLAN.md`,
`PHASE1-REPORT.md`, plus `docs/DECISIONS.md`, `docs/PHASE0-BRIEF.md`,
`docs/SPIKE-A1-REPORT.md`, `docs/EXTRACT-REPORT.md`, and `P1.*-REPORT.md`.

## Repository layout

- `Src/` — all game source. Subdirectories:
  - `Src/Display/` — graphics backends: `graphics.c` (dispatcher),
    `x11.c`/`x11.h` (X-only), `null.c` (no-op backend), `XMultiSync.c`
    (X-only), `objects.c`, `geometry` assets in `Src/Display/Objects/`.
  - `Src/Programs/` — built-in robot/bot implementations (`drone.c`, etc.).
  - `Src/Threads/` — stackful threader (`thread.c`, `threadglue.c`), Linux
    `swapcontext`/`ucontext`.
  - `Src/Include/` — headers, including the neutral boundary headers
    `graphics_backend.h`, `server.h`, `intent.h`.
  - Key simulation files: `animate.c` (frame order), `update.c`,
    `collision.c`, `hit.c`, `game.c`, `lowlib.c`, `actions.c`, `setup.c`.
  - Server core: `server_core.c` (lifecycle/clock/RNG), `server_loop.c`
    (15 Hz loop), `intent.c` (command queue), `geometry.c`, `server_main.c`.
  - Build files: `Src/GNUmakefile`, `Src/Makefile.inc1`, `Src/Makefile.inc2`
    (shared by the GNU and BSD build).
- `Tests/phase1/` — Phase 1 harness: `regression.sh`, `build-reference.sh`,
  `state_dump.c`, `reference_input.c`, `fixseed.c`, scenario fixtures in
  `Tests/phase1/scenarios/`, frozen goldens (`v2-golden.sha256`,
  `content.golden.sha256`, `geometry.golden.sha256`), and the acceptance
  scripts `geometry.sh`, `intent.sh`, `lifecycle.sh`, `specials.sh`,
  `content.sh`, `package.sh`, `timing.sh`, `bots.sh`, `check-x-boundary.sh`.
- `docs/` — scope/plan/briefs (see above).
- `Bin/` — support scripts, notably `Bin/package-server.sh` (relocatable
  server bundle) and `Bin/newversion.sh` (version generation).
- `Mazes/`, `Vehicles/` — supplied content (`.m`, `.V`); `Mazes.PIX/`,
  `Vehicles.PIX/` are legacy pixmap assets. `Programs/`, `Util/`, `Help/`,
  `Doc/` are legacy content/tools.
- Root `GNUmakefile` — recurses into `Src` and hosts the `phase1-check` gate.
- `.github/workflows/phase1.yml` — CI: `make phase1-check`, plus a
  manually-dispatched timing/bot-evidence job.

## How to build

Run from the repository root (top-level `GNUmakefile` forwards to `Src`):

```
make                 # all three binaries (X front-end, headless, server)
make phase1-headless # Src/xtank-headless only
make phase1-server   # Src/xtank-server only
make phase1-core     # reusable display-free core object set
make phase1-check    # full Phase 1 exit gate (see below)
make clean
```

Toolchain assumed: Ubuntu 24.04 x86-64, gcc 13.3.0, flex 2.6.4, `-O0 -g`,
`LC_ALL=C`. `Makefile.inc1` defines `COMMON_DEFS`; the X build selects `-DX11`,
the server build selects `-DNULL_BACKEND`, and the shipped server drops
`-DDYNALOAD` and excludes `main.c`/`mdesign.c`/`vdesign.c` plus the non-drone
robots (`SERVER_CORE_SRCS`/`SERVER_ONLY_*` in `Src/Makefile.inc1`).

Builds are isolated by object tree: `Src/xobj` (X), `Src/nobj` (headless
null), `Src/sobj` (server). Keep them out of commits.

## How to test

The Phase 1 exit gate:

```
make phase1-check
```

It runs the full regression oracle (frozen X vs display-free characteri-
zation), then `geometry.sh`, `intent.sh`, `lifecycle.sh`, `content.sh`,
`package.sh`, and all four `check-x-boundary.sh` modes (`--headers`,
`--sources`, `--build`, `--runtime`). `timing.sh` (15 Hz) and
`bots.sh --minutes 60` are recorded separately, not part of `phase1-check`.

### The regression oracle

`Tests/phase1/regression.sh` builds an isolated frozen X reference (pinned
baseline source overlaid with the reviewed **read-only** `STATE_DUMP`/seed
instrumentation) and an isolated candidate **outside the checkout**, runs the
reference under a harness-owned `Xvfb`, runs the candidate three times, and
requires **byte identity** via `cmp` between every repeat, the reference, and
the recorded before/after stage dumps.

```
bash Tests/phase1/regression.sh --suite full --stage before --subject auto
#   ... make a change ...
bash Tests/phase1/regression.sh --suite full --stage after  --subject auto
```

`--subject x|headless|null|auto`; `auto` resolves to the display-free `null`
backend post-P1.5. Two suites: `bootstrap` (legacy v1 scenarios) and `full`
(v1 + v2 per-tick bot-free traces). Also runs the v2 event-assertion analyzer
and checks frozen SHA/line/frame metrics.

### What "G" means

**G** is the mandatory guardrail run **before and after every task**
(`PHASE1-PLAN.md`, "Regression strategy"). It is the full regression oracle
above plus the task's listed checks: build the candidate, run the frozen X
reference and three candidate repeats, and require byte identity between
reference, pre-change baseline, and post-change candidate using `cmp` — never
tolerances, hash-only assertions, or relaxed goldens. If G fails, the task
fails; stop and report rather than bless the difference.

- **Seed: always `114336`.** Set via `XTANK_SEED=114336`. The pinned reference
  also receives it through `LD_PRELOAD=fixseed.so`; post-P1.8 the candidate
  takes it from server config with no preload.
- **Frozen v1 oracle values** (also in `PHASE1-REPORT.md`): `combat-2drone`
  SHA-256 `76f23a33…88c6a`, 798 lines, win frame 26569; `combat-4drone` (camo)
  `e13b7258…92b6e`, win frame 4004; `madman-2drone` (finite 65600-tick replay)
  `440b0e5b…f9c03`, frame 65600. v2 goldens live in
  `Tests/phase1/v2-golden.sha256`.
- Environment: `LC_ALL=C`, identical asset/settings/trace hashes and compiler
  flags, harness-owned `Xvfb`.

## Key conventions

- **No gameplay rule changes.** The simulation must stay byte-identical to the
  frozen pinned X reference. Gameplay formulas, entity limits, knowledge
  rules, and settings defaults are not to be altered. Only reviewed read-only
  dump/seed/replay/tick-limit instrumentation may overlay the pin.
- **Frozen oracle / byte identity.** Never update a golden to bless a
  simulation difference; oracle schema additions require old-format parity
  plus new frozen-reference parity. Preserve legacy v1 dumps permanently.
- **Evidence goes under `/tmp/opencode/`, not Git.** Dumps, logs, screenshots,
  binaries, packages, reference worktrees, and generated flex/version/object
  files stay outside the checkout (`${XTANK_P1_EVIDENCE:-/tmp/opencode/...}`);
  large trees may symlink to `/home/hatch/workspace/coding/xtank-evidence/`.
  Commit only harness source, tiny fixtures, and concise reports.
- **The X-boundary.** Server/simulation code must not include X11 headers or
  reference Xlib symbols/types. `Src/Include/graphics_backend.h` is the neutral
  contract; X types (`Xlib.h`, `x11.h`) are reachable only under `-DX11` and
  live only in backend-private files (`Src/Display/x11.c`, `Src/Include/x11.h`).
  Enforced by `Tests/phase1/check-x-boundary.sh` (`--headers`, `--sources`,
  `--build`, `--runtime`).
- **Commit/PR discipline.** One task = one reviewed commit or small PR series;
  inspect `git status`/`git diff` before staging; integrate dependency-first
  and rerun G on the merge tip. Preserve unrelated/untracked owner work.
- Stop and escalate (with a minimal reproducer, first differing
  tick/fields/hashes) on any byte difference, nondeterminism, hidden display
  dependency requiring rule changes, scheduler stall, or required scope change.
  Do not relax checks or pick a different architecture without owner approval.

## Server entry point and running a headless match

`Src/server_main.c:main()` is the shipped server's entry. It builds no
`Terminal`, opens no display, and never enters the graphical lobby. It selects
a supplied maze and vehicle by explicit name, sets an explicit seed, builds
0–2 local `drone` combatants, and runs `play_game()` to completion through the
reusable core (`server_init` / `server_match_init` / `server_step` /
`server_match_reset` / `server_destroy` in `Src/server_core.c`, loop in
`Src/server_loop.c`). Assets (`Mazes/`, `Vehicles/`, `COPYRIGHT*`) resolve
relative to `/proc/self/exe`, so a relocated bundle runs with `DISPLAY`,
`XTANK_DIR`, and `HOME` unset.

```
# from the repo root, after `make` (or `make phase1-server`)
./Src/xtank-server --help
./Src/xtank-server --maze combat1 --vehicle Vanguard --drones 2 --seed 114336 --fast
./Src/xtank-server --maze Arena --drones 1 --matches 2 --max-ticks 4000
```

Options: `--maze NAME` (default `combat1`), `--vehicle NAME` (default
`Vanguard`), `--drones N` (0..2; ≥1 required to play), `--seed N` (default
from `XTANK_SEED`), `--matches N`, `--max-ticks N`, `--assets DIR`, `--fast`
(bypass wall-clock pacing for replay), `--version`, `--help`. At least one
local drone is required because Phase 1 has no local human/network input
source. The headless oracle (`Src/xtank-headless`) uses the legacy CLI
(`-S -D -P -X -F <settings>` plus terminal args) and is driven by the harness.

Bundle the shipped server with `Bin/package-server.sh` (installs
`bin/xtank-server` plus `share/xtank/`); verified by `Tests/phase1/package.sh`.

## What Phase 3 builds on

Phase 2 is complete. The seams Phase 3 (two-PC multiplayer MVP) uses:

- `Src/Include/server.h` — match/lifecycle core: `ServerConfig`,
  `ServerPlayers`, `server_config*`, `server_init`, `server_match_init`,
  `server_step`, `server_match_reset`, `server_destroy`, `server_frame`,
  `server_seed_random`, `server_tick_pace`; loop:
  `server_loop_configure/active/hz/ticks/pace/run` (default 15 Hz);
  `server_consume_intents(int tick)`; `server_presentation_active()`;
  content/config: `content_set_root`, `content_load_maze`,
  `content_load_vehicle`, `content_load_settings`, `content_last_error`,
  `game_results`, `server_config_release`.
- `Src/Include/intent.h` — the player-input contract: `Intent`
  (`tick`/`player`/`sequence`/`op`/`a`/`b`), `IntentOp` (drive, heading,
  aim-world, aim-target, fire, fire-hold, weapon, chase, standoff),
  `IntentStatus`, `intent_init/push/pending/consume/reset`, bound
  `INTENT_QUEUE_MAX = 128`. `Src/input.c:input_event_intent()` is the legacy
  event → intent adapter intended for a future input/transport source.
- `Src/Include/graphics_backend.h` — the neutral backend contract (points,
  segments, window flags, opaque `Video`, `GraphicsBackend` vtable, neutral
  `draw_*`/accessors) that a future Windows presentation backend implements.

Natural hand-offs: `intent_push` from a session/service to the simulation;
`server_step` as the tick boundary; `content_*` as the asset/session loader;
the neutral backend for Windows rendering. Deferred work (do not start here
without an approved scope change): protocol/sockets, session service,
Windows-hosted server/fibers, and the broader §7 step 8 CI handoff.