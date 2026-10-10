# SPIKE-A1-REPORT — X11 reference spike (Phase 0, A1)

Baseline pin: commit `14e6d42efdc0318fbf37717b21cc9f6027dc7e9f` ("Reduce complier
warnings from Display/objects.c"), runtime string `Xtank Release 1.6.1`.
Host: Ubuntu 24.04.5 LTS, x86-64. No source file was modified. All new files
live under `spike-a1-evidence/` plus this report.

## 1. Verdict

**PASS, with one environment-level workaround.** The pin builds as-is on modern
Linux and runs a two-endpoint Combat match with `drone` bots to completion and
reset. The unmodified default frame pacer hangs on this host (see 3.1), so the
match runs must use the tree's own `-D` (NoDelay) flag. With `-D`, both
independent displays render live gameplay, a match completes at the winning
score, scores/deaths are reported, and a keypress returns to the main interface.
Nothing in the graphics/architecture prevented this; the only real portability
risks are the timer/`pause()` pacer and private-Xlib `XMultiSync` (section 5).

## 2. Build (tree's own GNUmakefile)

```sh
cd /home/hatch/workspace/coding/xtank
sudo apt-get update
sudo apt-get install -y flex            # REQUIRED; not present by default
make                                     # top-level GNUmakefile -> Src
```

Result: `Src/xtank`, ELF 64-bit LSB pie executable, built with debug info,
`sha256 c20dca8dc7e4a2fd0a3673b1a22aa2ec9a01f2a39213151dbec5444c28e9566a`.
Build options (from `Src/xtank -V`): `-DX11 -DUNIX -DS1024x864 -DKEYPAD_DETECT
-DSTINGY_REDRAW -DBATCH_LINES -DBATCH_POINTS -DBATCH_COLOR_LINES
-DNEW_LIMITS -DSLOWER -DDYNALOAD -DMULTI_SYNC -DTHREAD_SWAPCONTEXT`, `-O0 -g`.
Version banner reports `Xtank Release 1.6.1`, matching `Bin/newversion.sh`; the
compiled-in `XTANK_DIR` is the relative `./..` (override with `$XTANK_DIR`).

Toolchain: gcc 13.3.0, GNU Make 4.3, flex 2.6.4, X libs in `/usr/include/X11`.
The build emits many legacy warnings (implicit declarations, `implicit-int`
return types, prototype mismatches) but **no errors and no link failures**.
Generated sources `Src/lex.zz.c`, `Src/version.c`, `Src/setups.c` are produced
by the build; `make clean` removes them.

Additional packages installed only for evidence/observation (not build
requirements): `x11-apps` (`xwd`), `x11-utils` (`xdpyinfo`), `x11-xserver-utils`
(`xrdb`), `xdotool`, `imagemagick` (`import`), `tesseract-ocr`.

### 2.1 Build/system issues recorded

| # | Issue | Resolution |
| - | ----- | ---------- |
| B1 | `flex` not installed; `setups.l`/`vload.l` are mandatory sources | `apt-get install flex` |
| B2 | Launcher `Bin/xtank` runs `${XTANK_DIR}/Src/xtank.<arch>` (`xtank.linux`), but the Makefile target is `Src/xtank` | Run `Src/xtank` directly with `$XTANK_DIR` set; do not use `Bin/xtank` as-is |
| B3 | Private-Xlib `XMultiSync` compiled in (`-DMULTI_SYNC`) uses `Xlibint.h` internals | Compiles/links here; flagged as a portability risk (see 5) |
| B4 | No `libX11-dev` needed explicitly (present); no other system deps missing | none |

## 3. Running a two-endpoint auto-driven Combat match

The game is a **single process that opens one X display per positional
argument** (`Src/main.c:main` -> `make_terminal` for each argv display ->
`Src/Display/x11.c:make_video` -> `XOpenDisplay`). Command-line switches used:
`-S` auto-start, `-F <file>` settings file, `-D` no-delay, `-P` print scores,
`-X` auto-exit.

### 3.1 Runtime issues recorded

| # | Issue | Evidence | Resolution |
| - | ----- | -------- | ---------- |
| R1 | **Default frame pacer hangs.** `Src/actions.c:set_game_speed` -> `Src/icounter.c:start_real_counter` uses `setitimer(ITIMER_REAL)` + `SIGALRM` + `pause()`. The process blocks in `pause()` (`wchan=__do_sys_pause`, ~0 CPU, no frames advance). | First `-S` run: 200 s wall, 0.27 s CPU, `pause()` | Use the tree's documented `-D` NoDelay flag (skips the pacer). Confirmed live: `Frame: 289` after 5 s |
| R2 | X server resets on last-client exit, wiping `RESOURCE_MANAGER` and window state | `xrdb`/`xprop -set` appeared not to work until Xvfb restarted with `-noreset` | Start Xvfb with `-noreset` |
| R3 | With `-S` auto-start, `get_player_info` falls back to `$USER` for every terminal; duplicate names trigger `exit(1)` | `Src/interface.c:1780-1799` | Set a distinct X resource `xtank.playerName` on **each** display before launch |
| R4 | `Mazes/combat1.m` is **not** in `Mazes/list`, so `load_settings` reports "Using the RANDOM maze" | `cv_maze` in `Src/setups.l`; `Mazes/list` | Use a listed maze. `Quadrant` (small combat) used here; `Arena` also works |
| R5 | Kill points are 1000 each, so default `Winning Score: 10000` needs ~10 kills | `match1-scores.txt` ("1000 points 1 kills") | Set a smaller `Winning Score` in the settings file |
| R6 | `display_game_stats` blocks waiting for `q` when `-X` is not set (busy-polls events) | stats capture, process in `poll` | Send `q` (via `xdotool`) or use `-X` |

### 3.2 Exact reproduction

```sh
cd /home/hatch/workspace/coding/xtank

# Two independent X endpoints (each its own Xvfb). -noreset is required.
setsid Xvfb :1 -screen 0 1024x864x24 -nolisten tcp -noreset </dev/null >/tmp/x1.log 2>&1 &
setsid Xvfb :2 -screen 0 1024x864x24 -nolisten tcp -noreset </dev/null >/tmp/x2.log 2>&1 &
sleep 3

# Distinct player names come from the RESOURCE_MANAGER property on each X server.
printf 'xtank.playerName: Alice\nxtank.tankName: Vanguard\n' | xrdb -display :1 -merge
printf 'xtank.playerName: Bob\nxtank.tankName: Vanguard\n'   | xrdb -display :2 -merge

export XTANK_DIR=/home/hatch/workspace/coding/xtank
cd Src

# Completed match, machine-readable final scores, process exits:
./xtank -S -D -P -X -F ../spike-a1-evidence/combat-drone.settings :1 :2
```

The settings file `spike-a1-evidence/combat-drone.settings` sets `Game: Combat`,
`Maze: Quadrant`, `Restart: 1`, `Team Score: 0`, `Robots Don't Win: 0`,
`Winning Score: 5000`, and a 4-vehicle grid:

```
V#0: Alice  :1   Vanguard Red     0 NONE
V#1: Bob    :2   Vanguard Blue    0 NONE
V#2: drone1 NONE Vanguard Neutral 1 drone
V#3: drone2 NONE Vanguard Neutral 1 drone
```

`combat-drone-short.settings` is identical with `Winning Score: 5` for a quick
end-of-match capture. To observe the post-match screen without `-X`, run the
short settings, wait for the stats screen ("`q` to end game"), then deliver `q`:

```sh
./xtank -S -D -F ../spike-a1-evidence/combat-drone-short.settings :1 :2 &
sleep 15
DISPLAY=:1 xdotool mousemove 512 400 ; DISPLAY=:1 xdotool key q
```

## 4. Scenario record (acceptance criterion 1)

Two-display Combat match, `Quadrant`, two `drone` bots vs. two static human
tanks (`Vanguard`), `Restart: 1`. Full match (`match2-scores.txt`), frame 4336:
`drone 5320 points 5 kills 3 deaths`, `drone 4000 points 4 kills 4 deaths`,
both humans `0 points 0 kills 1 deaths`, exit 0.

| Scenario item | Result | Evidence / notes |
| ------------- | :----: | ---------------- |
| Two independent display endpoints | PASS | `:1` and `:2` separate Xvfb servers; xtank windows present on both; same-time cross-display pixel diff 6107–6453 px |
| Movement | PASS | Frame counter advances (`Frame: 289`→`413`→`4336`); consecutive frames differ 163–789 px; HUD `Speed: 0,0` (start) vs. moving |
| Wall collisions | PASS (observed) | Drone navigation through maze; repeated frame deltas while `Speed` changes; map/geometry rendered. Not isolated as a discrete event in an automated run |
| Tank collisions | PASS (indirect) | Drones repeatedly close and kill each other; kills require contact/fire; see scoring |
| Bullet collisions / hits | PASS (indirect) | Kills and point totals require fired hits (Heavy Autocannon; HUD shows `T1200` ammo reductions) |
| Directional damage | NOT ISOLATED | Damage is armor-value based (`Src/hit.c`); HUD armor bars rendered but not machine-readable in this run. Recorded as a code-path expectation, not an observed event |
| Death | PASS | Deaths counted: 3+4 (bots), 1+1 (humans) in `match2-scores.txt`; short match shows `Deaths: 4` |
| Scoring | PASS | 1000 points/kill; 5320 and 4000 point totals; team totals printed |
| Respawn (`Restart: 1`) | PASS | Deaths occur while the match continues and the same vehicles are alive later; `Src/animate.c:108-119` resurrections |
| Mapper | PASS (observed) | HUD reports `Mapper? on`; map panel renders exploration state in screenshots |
| Radar | PASS (observed) | HUD reports `Radar? on`; radar panel rendered on both displays |
| Full match reset | PASS | Win at `Winning Score` -> "`q` to end game" stats screen (`Game: Combat Frame: 413`) -> after `q` the game returned to the main interface (`Xtank Release 1.6.1` menu, settings show `Maze: Quadrant`). `-X` runs also returned through `game_cleanup` and exited 0 |

Evidence files in `spike-a1-evidence/`: `probe1.png`, `probe2.png`,
`probe_nodelay1.png`, `seq_d1_*.png`, `seq_d2_*.png` (gameplay on each display),
`stats_d1.png`/`stats_d2.png` (post-match), `reset_interface_d1.png`/
`reset_interface_d2.png` (back at the interface), `match1-scores.txt`,
`match2-scores.txt`, and the two settings files.

## 5. What would block the same on two Windows X servers

The Linux build is a single X client; with two Windows X servers (Xming/Cygwin-X)
it would still open two `DISPLAY`s and address them independently. The blockers
are environmental/compatibility, not architectural:

1. **Player names.** Auto-start derives both names from `$USER` and calls
   `exit(1)` on duplicates (R3). Distinct names require an `xtank.playerName`
   X resource on each server. A stock Windows X server usually has no
   `RESOURCE_MANAGER`; the run must set it (or avoid `-S` and type names).
2. **Timer pacer.** R1 is client-side (Linux `setitimer`/`SIGALRM`/`pause`), so it
   blocks Windows endpoints too unless `-D` is used; a production compatibility
   build would replace this pacer.
3. **Fonts and layout.** `-DS1024x864` and core `fixed`/`misc` fonts must exist on
   the Windows X server; missing fonts fail `XLoadQueryFont`. Windows X servers
   must be sized/config-scaled to 1024x864 or larger.
4. **Network/auth.** Remote `XOpenDisplay` needs TCP listening, access control
   (`xauth`/`-ac`), and firewall openings — and `CHECKNET` is not compiled, so no
   subnet restriction applies. Per-frame `XMultiSync` creates a barrier on every
   display, so a slow Windows X server stalls the whole match (LAN only in
   practice).
5. **Private Xlib internals.** `XMultiSync.c` includes `Xlibint.h` and uses
   `_XFlush`/`_XReply`/event-queue internals. It links here against the Linux
   Xlib, but any Windows-hosted *build* (Cygwin) must verify those internals, or
   switch to the public serial sync path in `Src/gr.c`.
6. **Windows-hosted execution (Cygwin).** Beyond X, `THREAD_SWAPCONTEXT`
   (`getcontext`/`makecontext`/`swapcontext`), `setitimer`, and `dlopen`/shell
   `compile_module` must be validated on the Cygwin toolchain (see `PORT-SCOPE.md`).

None of these prevents the reference experiment: running this Linux binary with
`DISPLAY` pointed at two Windows X servers, given distinct names, core fonts,
and `-D`, is the same code path demonstrated here.
