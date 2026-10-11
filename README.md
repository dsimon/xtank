# Xtank — Windows modernization

Xtank is a classic X11 multiplayer vehicle-combat game: you pilot a tank
through a maze, armed with the latest weapons and armor, and fight to satisfy
the objective of whichever game mode you are playing. The original 1.6.0
release and its documentation live alongside this effort — see
[README-1.6.0.md](README-1.6.0.md) for the upstream project.

This repository is the **Windows modernization effort**. Its goal is a native
Windows client/server release that no longer depends on an X11 server. The
strategy is to keep the proven 1988-era gameplay by extracting the simulation
into an authoritative headless server core, then build fresh clients, protocol,
and session management on top of it, rather than rewriting the game.

## Current status

**Phase 1 is complete on the `windows-phase1` branch.** The authoritative
simulation has been extracted from the X11 game into a headless Linux server
core and proven **byte-identical** to a frozen pinned X reference across every
scenario — no gameplay rule was changed.

Phase 1 delivered:

- A display-free **headless server core** (`server_core.c`, `server_loop.c`,
  `intent.c`, `geometry.c`) with an explicit match/player lifecycle, seed,
  game clock and RNG.
- A monotonic **15 Hz fixed-step loop**, decoupled from rendering.
- A neutral **graphics backend contract** (`Src/Include/graphics_backend.h`) so
  the simulation never touches X11 types, plus a genuine no-op null backend.
- A bounded **intent queue** (`Src/Include/intent.h`) carrying player input
  into the simulation once per tick, ready for a future network source.
- Display-free **content/config loading** from an explicit asset root, and a
  terminal-free, relocatable **local server** (`Src/xtank-server`).
- A **CI exit gate** (`make phase1-check`) plus the frozen regression oracle.

There is **no network protocol, transport, session service, or Windows client
yet** — those are Phases 2–3 by the approved scope. Windows-hosted server and
fibers are deferred by the Linux-only MVP decision.

See [PHASE1-REPORT.md](PHASE1-REPORT.md) for the full exit report, frozen
oracle hashes, and per-task results.

## Building

The tree builds three binaries from one source base:

- `Src/xtank` — the original X11 front-end (presentation + input + simulation).
- `Src/xtank-headless` — display-free oracle/harness build (null backend).
- `Src/xtank-server` — shipped terminal-free local server.

From the repository root:

```
make                 # all three binaries
make phase1-headless # Src/xtank-headless only
make phase1-server   # Src/xtank-server only
make phase1-core     # reusable display-free core object set
make clean
```

Toolchain assumed: Ubuntu 24.04 x86-64, gcc 13.3.0, flex 2.6.4, `-O0 -g`,
`LC_ALL=C`. Builds are isolated by object tree (`Src/xobj`, `Src/nobj`,
`Src/sobj`).

## Running a headless match

```
# from the repo root, after `make`
./Src/xtank-server --maze combat1 --vehicle Vanguard --drones 2 --seed 114336 --fast
./Src/xtank-server --maze Arena --drones 1 --matches 2 --max-ticks 4000
```

`Src/xtank-server` builds no `Terminal` and opens no display. It selects a
supplied maze and vehicle by name, sets an explicit seed, builds 0–2 local
`drone` combatants, and runs a match to completion. Assets resolve relative to
the executable, so a relocated bundle runs with `DISPLAY`, `XTANK_DIR` and
`HOME` unset. See `--help` for the full option list (`--maze`, `--vehicle`,
`--drones`, `--seed`, `--matches`, `--max-ticks`, `--assets`, `--fast`,
`--version`).

Bundle the shipped server with `Bin/package-server.sh`.

## Testing

The Phase 1 exit gate:

```
make phase1-check
```

It runs the full regression oracle (frozen X reference vs the display-free
characterization), then the geometry, intent, lifecycle, content, package, and
X-boundary checks. `timing.sh` (15 Hz) and `bots.sh --minutes 60` are recorded
separately.

### The regression oracle

`Tests/phase1/regression.sh` builds an isolated frozen X reference (pinned
baseline source overlaid with reviewed **read-only** dump/seed instrumentation)
and an isolated candidate **outside the checkout**, runs the reference under a
harness-owned `Xvfb`, runs the candidate three times, and requires **byte
identity** via `cmp` between every repeat, the reference, and the recorded
before/after stage dumps.

```
bash Tests/phase1/regression.sh --suite full --stage before --subject auto
#   ... make a change ...
bash Tests/phase1/regression.sh --suite full --stage after  --subject auto
```

Two suites are available: `bootstrap` (legacy v1 scenarios) and `full`
(v1 + v2 per-tick bot-free traces). No golden is ever relaxed to bless a
simulation difference.

## Project structure

- `Src/` — all game source.
  - `Src/Display/` — graphics backends: `graphics.c` (dispatcher),
    `x11.c`/`x11.h` (X-only), `null.c` (no-op backend), assets in
    `Src/Display/Objects/`.
  - `Src/Programs/` — built-in robot/bot implementations (`drone.c`, etc.).
  - `Src/Threads/` — stackful threader (`thread.c`, `threadglue.c`).
  - `Src/Include/` — headers, including the neutral boundary headers
    `graphics_backend.h`, `server.h`, `intent.h`.
  - Key simulation files: `animate.c` (frame order), `update.c`,
    `collision.c`, `hit.c`, `game.c`, `lowlib.c`, `actions.c`, `setup.c`.
  - Server core: `server_core.c`, `server_loop.c`, `intent.c`, `geometry.c`,
    `server_main.c`.
- `Tests/phase1/` — Phase 1 harness, scenarios, frozen goldens, and the
  acceptance scripts.
- `docs/` — scope, plan, briefs, and decisions (`PORT-SCOPE.md`,
  `PHASE1-PLAN.md`, `DECISIONS.md`, and others).
- `Bin/` — support scripts, notably `Bin/package-server.sh`.
- `Mazes/`, `Vehicles/` — supplied content (`.m`, `.V`).
- Root `GNUmakefile` — recurses into `Src` and hosts the `phase1-check` gate.

## Where this is headed

The full scope and rationale are in [docs/PORT-SCOPE.md](docs/PORT-SCOPE.md)
and [docs/DECISIONS.md](docs/DECISIONS.md).

- **Phase 2 — Windows client and single-client vertical slice:** define
  versioned commands/snapshots/events and content IDs, build the Windows
  frontend on a native rendering/input layer, and connect one Windows client to
  the headless Linux server.
- **Phase 3 — Two-PC multiplayer MVP:** direct-connect LAN play with sessions,
  sequenced inputs, snapshot interpolation, lifecycle and disconnect handling,
  and Windows hosting. The MVP targets Combat mode, supplied content, Windows
  11 clients, and a Linux authoritative server first.

## License

The 1.6.0 release of Xtank has been re-licensed under the BSD 2-clause
copyright, sometimes referred to as the "FreeBSD Copyright". Check individual
files for the name of the contributing Author(s). See `COPYRIGHT` for the full
terms.