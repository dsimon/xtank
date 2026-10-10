# EXTRACT-REPORT — headless extraction feasibility gate (Phase 0)

Baseline pin: commit `14e6d42efdc0318fbf37717b21cc9f6027dc7e9f`, runtime string
`Xtank Release 1.6.1`. The pinned tree was **never modified** (only new report
files were added). All experiments ran in `/home/hatch/workspace/coding/xtank-extract`,
a full copy of the pin. Host: Ubuntu 24.04.5 LTS x86-64, gcc 13.3.0.

## 1. Verdict

**FEASIBLE.**

The gameplay simulation runs headless — with `DISPLAY` unset and no X server
process running — and produces state that is **byte-for-byte identical** to the
unmodified X build under the same fixed seed. A scripted, seeded Combat match
(two `drone` bots on `Quadrant`, Vanguard vehicles) completes with firing,
damage, death, respawn and scoring, and the winner/score/win-tick agree exactly
between the headless and X builds. Three identical headless replays produce
identical state checksums.

The extraction is not free: the X backend must be replaced by a null backend,
and a handful of call sites need explicit handling. But **no gameplay file had
to be changed**, and the presentation/world-state "entanglement" flagged by the
brief is resolvable without altering simulation behaviour.

Confidence caveat: this gate proved one bot (`drone`), Combat and Madman, on one
host and toolchain. Multi-human gameplay, other modes, and Windows hosting were
not exercised (see §7).

## 2. What it took

Two baseline source files were touched, both guarded by `#ifdef HEADLESS` /
`#ifdef STATE_DUMP` so the X build is byte-identical to upstream behaviour:

| Change | File | Lines added |
| --- | --- | ---: |
| No-op Xlib implementation + `XMultiSync` stub | `headless-build/stubs.c` (new) | 674 |
| Shadow X headers (`Xlib.h`, `Xutil.h`, `Xatom.h`, `Xos.h`, `Xresource.h`, `keysym.h`) | `headless-include/X11/` (new) | 429 |
| Mark windows exposed (`make_window`) and return "no events" (`get_events`) under `HEADLESS` | `Src/Display/x11.c` | 15 |
| `STATE_DUMP` debug hook (determinism/fidelity oracle) | `Src/animate.c` | 112 |
| Build scripts, seed shim, scenario files | `build-headless.sh`, `build-x11ref.sh`, `headless-build/` (new) | 124 |

Total ~1,354 new/changed lines, of which only **127 lines are edits to the
baseline**, and **zero lines changed** in `collision.c`, `game.c`, `update.c`,
`hit.c`, `disc.c`, `camo.c`, `box.c`, `intersect.c`, `lowlib.c`, `highlib.c`,
`vehicle.c`, `Src/Programs/*` or any other simulation/bot file.

How it works: `Src/Display/graphics.c` already `#include`s `x11.c` under
`-DX11`. The headless build keeps `-DX11` (so all `#ifdef X11` behaviours,
including `button_up`/`follow_mouse`, are preserved) and adds `-DHEADLESS`,
then compiles against a shadow `<X11/Xlib.h>` whose entry points are the no-ops
in `stubs.c`. The whole of `x11.c` — window flags, GC/font bookkeeping,
`make_picture`, `rotate_pic_90/180`, `get_events`, `display_*` support — runs
unchanged; only the actual X calls vanish. `Display/XMultiSync.c` is excluded
(its private-Xlib use is a known Windows-host build risk) and `XMultiSync` is
stubbed. The resulting `Src/xtank-headless` is **not linked against libX11**
(`ldd` shows no `libX*`), and runs with no `DISPLAY` and no X server.

The `STATE_DUMP` hook prints, every *N* frames, a deterministic line per
vehicle (grid/position/rotation/fuel/heat/armor/discs/ammo/score/kills/deaths/
status) plus an FNV hash over all world state (vehicles, bullets incl.
`thrower`, explosion count). It reads state only, so it cannot perturb a match.

## 3. The three flagged risks

| # | Flagged risk | Verdict | Evidence |
| - | --- | --- | --- |
| 1 | Collision depends on graphical `Picture` geometry (`collision.c:50–73`) | **Resolved — not entangled.** `Picture.width/height/offset_x/offset_y` are static data baked into `Display/Objects/*.obj` (`marauder_pic[...]`) plus the pure-C `rotate_pic_90/180`. `make_picture()` only assigns a pixmap index; it never sets geometry. `coll_vehicle_walls()` reads only those four integers. Stubbing `make_picture` therefore cannot change collision. | Full matches byte-identical; geometry values present and identical headless. |
| 2 | Camouflage bookkeeping follows drawing (`animate.c:222–245`) | **Resolved — presentation-only shadow.** `old_camod` is read only by `display.c` draw decisions. The `old_camod = camod` loop is *outside* the `if (!commandOptions.NoIO)` guard, so it executes headless. The world-state `camod` is set by `update_specials()`/`camo.c`, independent of drawing. | 4-drone Combat with camouflage **enabled**: headless vs X state dumps byte-identical (`sha256 e13b7258…`). |
| 3 | Disc `thrower` state cleared by drawing (`display.c:370–390`) | **Resolved — display bookkeeping, and the display pass still runs.** `b->thrower` is set by `disc.c` and reset to `-1` only in `display_bullets()` on the last terminal. The headless build does **not** use `NoIO`, so `display_terminal()`/`display_bullets()` execute with no-op drawing primitives and perform the identical reset. | Madman (`Disc`) two-drone match: headless and X agree on `b->thrower` (included in the hash) for 328 dumps / 65,600 frames. |

None of the three required a simulation rewrite; all three are artefacts of the
display pass having side effects, and the display pass is retained (only its X
calls are nulled), so its side effects are retained too.

## 4. Determinism (criterion 2, replay clause)

Canonical match: `Mazes/Quadrant`, `Game: Combat`, two `drone` bots, Vanguard,
`Force Off: Camo`, seed `114336`, dumped every 100 frames, no X server,
`DISPLAY` unset, three separate processes.

| Run | Exit | State lines | Win tick | Final hash | SHA-256 of dump |
| --- | ---: | ---: | ---: | --- | --- |
| 1 | 0 | 798 | frame 26569 | `eec7687e82490ac9` | `76f23a333a0907f9bb40fa4d9027feac39f3eccdc11aa85aa34fa33109688c6a` |
| 2 | 0 | 798 | frame 26569 | `eec7687e82490ac9` | `76f23a333a0907f9bb40fa4d9027feac39f3eccdc11aa85aa34fa33109688c6a` |
| 3 | 0 | 798 | frame 26569 | `eec7687e82490ac9` | `76f23a333a0907f9bb40fa4d9027feac39f3eccdc11aa85aa34fa33109688c6a` |

All three dumps are bit-identical (same file SHA-256). Determinism was obtained
by (a) pinning the RNG seed in-process via an `LD_PRELOAD` `time()` shim (the
pin seeds from `time()` in `util.c:init_random`) and (b) `-D` (NoDelay) so the
frame pacer is skipped. Bot CPU-time scheduling did not introduce variance for
`drone`: its decisions complete within the 10 ms `SIGVTALRM` budget, so no
mid-decision preemption occurred in these runs.

## 5. Fidelity vs the A1 X build (criterion 2, reference clause)

`xtank-refdump` is the same source built with the tree's own flags plus
`-DSTATE_DUMP`, linked against real Xlib, run against an `Xvfb :99` server with
the identical settings file and seed. Comparison is on the state dump (which is
what the release acceptance criterion calls for: death/respawn/win ticks, score,
armor, ammo and maze changes exact; positions within 0.01 world unit).

| Scenario | Build | Win/reset tick | Score (Red/Blue or per-vehicle) | Positions | Dump comparison |
| --- | --- | ---: | --- | --- | --- |
| Combat, 2 drones, camo off | headless | 26569 (`status 3`) | 2000 / 0; 2 kills, 6 deaths / 0 kills, 2 deaths | exact | `sha256 76f23a33…` |
| Combat, 2 drones, camo off | X build | 26569 (`status 3`) | 2000 / 0; identical | exact | `sha256 76f23a33…` |
| Combat, 4 drones, camo **on** | headless | 4004 (`status 3`) | 0 / 1000 / 0 / 3320 | exact | `sha256 e13b7258…` |
| Combat, 4 drones, camo **on** | X build | 4004 (`status 3`) | identical | exact | `sha256 e13b7258…` |
| Madman, 2 drones, discs | headless | not finished in timebox | — | exact (common prefix) | 328 dumps, 0 diffs |
| Madman, 2 drones, discs | X build | not finished in timebox | — | exact (common prefix) | 328 dumps, 0 diffs |

Positions, armor, ammo, fuel, heat, score, kills, deaths, disc ownership and
`thrower` all match **exactly** (not merely within 0.01 units) at every dump.
Final `-P` score output is also identical between builds.

Scenario note: two *neutral* drones in `Quadrant` never engage — they wander
apart, run out of fuel and (with camo on) become mutually invisible, so the
match never reaches a winning score. The canonical two-drone gate match
therefore uses Red vs Blue teams (individual scoring; `Team Score: 0`) and
`Force Off: Camo`, matching the MVP release preset which already disables Camo.
The 4-drone run is the camo-enabled control. This is a property of the game and
settings, not of headless operation: the X build behaves identically.

## 6. Exact reproduction

```sh
# toolchain actually installed during the spike (flex is a hard build dep of the pin):
sudo apt-get install -y flex gdb xvfb

cd /home/hatch/workspace/coding/xtank-extract
./build-headless.sh      # -> Src/xtank-headless  (ldd: no libX11)
./build-x11ref.sh        # -> Src/xtank-refdump   (real Xlib, STATE_DUMP)
gcc -shared -fPIC -O2 -o headless-build/fixseed.so headless-build/fixseed.c

# canonical headless match, no X server, DISPLAY unset, seed 114336
cd Src
env -u DISPLAY XTANK_DIR=$PWD/.. XTANK_SEED=114336 \
    XTANK_DUMP=/tmp/h.state XTANK_DUMP_PERIOD=100 \
    LD_PRELOAD=$PWD/../headless-build/fixseed.so \
    ./xtank-headless -S -D -P -X -F ../headless-build/combat-2drone.settings

# X reference, same seed/settings (Xvfb), for the fidelity diff
setsid Xvfb :99 -screen 0 1024x864x24 -nolisten tcp -noreset </dev/null >/tmp/xvfb99.log 2>&1 &
sleep 3
env XTANK_DIR=$PWD/.. XTANK_SEED=114336 \
    XTANK_DUMP=/tmp/x.state XTANK_DUMP_PERIOD=100 \
    LD_PRELOAD=$PWD/../headless-build/fixseed.so \
    ./xtank-refdump -S -D -P -X -F ../headless-build/combat-2drone.settings :99
sha256sum /tmp/h.state /tmp/x.state    # identical
```

A runnable version of the above (including determinism and all three scenarios)
is `xtank-extract/extract-evidence/repro.sh`; the captured state dumps and score
outputs are in `xtank-extract/extract-evidence/`.

## 7. What the full extraction still requires

The feasibility gate is passed; the following concrete steps convert the spike
into the real extracted core. Each is bounded and independent.

1. **Draw the backend boundary.** Turn the ~30 functions and the `draw_*`/
   window macros of `Src/Include/x11.h` into an opaque backend interface, and
   remove `Display *`, `GC`, `Pixmap`, `Cursor`, `XFontStruct` and `<X11/*.h>`
   from `Video`/`graphics.h`. Today the sim still transitively includes X
   headers; the spike shows none of that data is used by simulation, so this is
   mechanical. (Supersedes the shadow-header/stub trick with a vtable.)
2. **Replace terminal/input with a command feed.** `animate()` currently drives
   input via `get_events()` per terminal and calls vehicle operations in
   `get_input()`/`anim_input()`. Introduce an explicit intent queue (drive,
   turret aim, fire, weapon select) consumed once per tick at the existing
   `animate()` ordering; `map_fire_weapon`/`anim_fire_weapon` already split
   map/anim weapon classes and are the natural seams.
3. **Make pacing and seeding in-process.** Replace `-D`/`setitimer` pacing
   (`icounter.c`) with a fixed-tick loop (15 Hz per the release preset) and
   replace the `LD_PRELOAD` `time()` seed shim with a seed field in server
   config. Preserve the exact movement/collision/special/rule pass order.
4. **Decouple the presentation side-effects deliberately.** Either keep the
   inert `Loc.screen_x/y`, `Vehicle.old_camod` and `Bullet.thrower` fields (they
   are harmless, as shown here) or move them to a client/presentation struct.
   Do not delete them before the client exists; the spike shows deleting/feeding
   them wrongly is the only way to change behaviour.
5. **Deterministic/portable bot scheduling.** `program.c` allots CPU time via
   `ITIMER_VIRTUAL`; make the budget tick-based or cooperative so `drone` (and
   future bots) are bit-stable across hosts, then port `thread.c`'s
   `swapcontext` to fibers for a Windows-hosted server (Linux server MVP can
   keep swapcontext).
6. **Server-side content path.** Confirm `sload.l`/`vload.l`/`newfile.c`
   loaders and `file.c:get_environment()` run with no display (already true),
   and package assets with the server instead of relying on `XTANK_DIR`.
7. **Turn the `STATE_DUMP` hook into the regression oracle.** Promote it to a
   test harness: fixed seed + recorded intent + per-tick checksum, run headless
   vs X in CI. This is the characterization suite `PORT-SCOPE.md` noted was
   missing.
8. **Only then** build the client protocol and Windows frontend (Phases 2–3).

Steps 1–4 are the extraction proper; step 5 is the Windows-host prerequisite;
none require re-deriving gameplay. The 16–28 engineer-week extracted-core
envelope in `PORT-SCOPE.md` remains reasonable, now with the headless risk
retired.
