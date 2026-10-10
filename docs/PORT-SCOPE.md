# Xtank Windows port scope

## Architecture survey

### Summary and evidence boundary

**Xtank is one authoritative simulation process with multiple X11 display connections, not a distributed game with a game client on each player's machine.** A compatibility-based Windows release is the shortest route to play. For a sustainable native Windows multiplayer release, the best investment is a new client/server boundary and Windows frontend while retaining and extracting the existing simulation, rather than immediately rewriting all gameplay.

This survey is based on source inspection, source-line counts, and a successful `make -n` dry run in this checkout. Compilation, gameplay, Windows compatibility, and network performance have not been tested. The owner identifies the source as 1.6.0; `README.md` describes the 1.6.0 BSD relicense, but `Bin/newversion.sh:29` currently generates a **1.6.1** version string. Pin the intended baseline before implementation. `COPYRIGHT` contains the BSD 2-clause terms; preserve the applicable file notices and distribution attribution when reusing code.

### Process, world, and frame loop

- `Src/main.c:main()` initializes settings, robot descriptions, threading, messages, and rotated objects; opens graphics; creates terminals; loads content; collects player information; and enters `main_interface()` in `Src/interface.c`.
- World state is predominantly global: `maze`, `real_map`, `settings`, `frame`, bullet/explosion sets, `actual_vehicles`, and live/dead vehicle lists are defined in `Src/main.c`. Current-context globals include `term` in `Src/gr.c`, `vid` in the X backend, and `cv` in `Src/lowlib.c`. These support sequential context switching, not independently concurrent game instances.
- `Src/setup.c:play_game()` repeatedly calls `animate()` after `setup_game()`, then handles scores, cleanup, and resets. `Src/animate.c:animate()` is the useful simulation seam: pace the frame, increment `frame`, process deaths/respawns, run robots, collect input from each terminal, update vehicles and bullets, resolve collisions, update explosions/maze/specials, apply game rules, render each terminal, then synchronize displays.
- Gameplay is substantial and reusable: `Src/update.c:update_vehicle()`, `update_vector()`, and `update_bullets()`; `Src/collision.c:coll_vehicle_walls()` and the vehicle/bullet collision passes; damage in `Src/hit.c`; maze/landmark behavior in `Src/maze.c` and `Src/box.c`; and weapon/control operations in `Src/actions.c` and `Src/lowlib.c`.
- `Src/game.c:game_rules()` dispatches combat, war, ultimate, capture, race, and madman rules. The same file also implements graphical score presentation, illustrating the mixed responsibilities.
- Logic uses frame-based units, including speed per frame and frame-count lifetimes. `Src/actions.c:set_game_speed()` and `check_game_speed()` use `Src/icounter.c` timers. `MAX_GAME_SPEED` is 30 in `Src/Include/xtank.h`, and the maximum-speed branch skips pacing. A new server must choose and preserve an explicit simulation rate rather than couple simulation to client rendering speed.

### How multi-head multiplayer actually works

1. `Src/main.c:main()` treats positional arguments as X display names and calls `make_terminal()` for each. With no positional display, `Src/file.c:get_environment()` supplies `DISPLAY` or `unix:0.0`.
2. `Src/gr.c:make_terminal()` allocates a `Terminal`, calls `make_video(display_name)`, opens/maps its windows, and creates its graphics objects. `Src/Display/x11.c:make_video()` calls **`XOpenDisplay(name)` once per terminal**. Each `Video` owns a `Display *`, parent window, child windows, GCs, fonts, cursors, and pixmaps (`Src/Include/x11.h:Video`). Different connections can target different machines' X servers, or the same server.
3. `Src/gr.c:set_terminal()` selects global `term` and calls `set_video()` to select global `vid`. `animate()` loops through these contexts for input and again for rendering. There is one shared world and one frame counter, with different cameras/HUDs for each player.
4. `Src/Display/x11.c:get_events()` translates X key, mouse, and expose events into the game's `Event` structure (`Src/Include/graphics.h`). During gameplay it polls with `XPending()`; outside gameplay it can block in `XNextEvent()`. `Src/input.c:get_input()` dispatches events, and `anim_input()` sets `cv` and directly calls vehicle operations such as `aim_all_turrets()`. Input is not serialized as game commands.
5. `Src/display.c:display_terminal()` draws animation, console, map, help, messages, and status. `Src/gr.c:open_windows()` creates one parent layout's child panels, rather than a modern independent client application per player.
6. `Src/gr.c:sync_terminals()` uses `multi_sync()` with the default `MULTI_SYNC` build. `Src/Display/XMultiSync.c:XMultiSync()` sends input-focus requests to all displays before waiting for all replies. This overlaps requests, but still creates a barrier dependent on every display. `animate()` normally performs it every frame (`sync_rate = 1`). A slow/unresponsive X server can delay the whole game.

Thus the wire traffic is **X drawing requests and input events**, not tank-state snapshots or a gameplay protocol. The game process is an X client; the players' display machines run X servers. The words “server/client” in `Src/unix.c:check_internet()` describe this hosting arrangement. That subnet restriction is behind `CHECKNET`; the default make flags do not define it, so the selected `check_internet()` is a no-op. It is not a game networking implementation.

`Src/Include/tanklimits.h` currently permits 14 terminals, 16 vehicles, 511 bullets, and 255 explosions. Terminals need not map one-to-one to tanks: `Src/Include/vehicle.h:Combatant` has a player array, and `Src/setup.c:setup_combatant()` associates multiple players with one vehicle. Observers are also supported. Terminal zero has special host/UI responsibilities; for example, its quit exits the game in `animate()`. These policies need explicit equivalents in a client/server design.

### X11 concentration and coupling

**The low-level implementation is concentrated, but the dependency is broader than that implementation.**

- `Src/Display/graphics.c` selects the backend by literally including `x11.c` under `X11`. The alternative X10/Amiga branches refer to backend files absent from this checkout. The interface suggests a replacement backend is possible, but there is no maintained non-X backend here.
- `Src/Display/x11.c` handles connections, windows, input, X resources, fonts, colors, cursors, pixmaps, drawing, errors, flushing, and synchronization. Relevant functions include `make_video()`, `make_parent()`, `make_window()`, `get_events()`, `make_gcs()`, and `make_picture()`.
- `Src/Include/graphics.h` includes `x11.h`; `Src/Include/x11.h` implements `draw_line`, `draw_point`, `draw_picture`, rectangles, and batched drawing as macros that call Xlib. `Src/Include/proto.h` includes graphics and threading headers and exposes `Display *` in the `XMultiSync` prototype. Even physics files can inherit X types through common declarations.
- Direct Xlib escape hatches include `Src/hud.c:draw_armor()` (`XDrawArc`) and `Src/Programs/Bootlegger.c:erase_graphics()` / `draw_screen_line()` (`XDrawLine`), with terminal lookup for robot debug graphics. `Bootlegger.c` defines `SOFT_DEBUG` itself. UI code also accesses `Video` fields directly: player defaults in `interface.c:get_player_info()`, beep/window flags in `input.c`, and menu styling in `menu.c`.
- `XMultiSync.c` depends on **private Xlib internals** (`Xlibint.h`, `_XFlush`, `_XReply`, `_qfree`, and event-queue fields). The public prototype returns `int`, while the implementation returns `void`. This needs verification/repair on a fresh toolchain; the serial public-API synchronization path already exists in `gr.c`.
- Rendering uses XOR erase/redraw extensively (`display_vehicle()`, `display_bullets()`, and `GXxor` GCs in `make_gcs()`), with expose handling and batched primitives. A native renderer must reproduce those semantics initially, or replace the incremental renderer with full-frame drawing.
- World and presentation are intertwined: `Src/Include/loc.h:Loc` stores screen coordinates for every terminal; `Vehicle` stores an `Object *`; collision bounds in `coll_vehicle_walls()` come from rotated `Picture` dimensions/offsets. These geometry values are gameplay data that must survive even in a headless server.
- `Src/mapper.c:special_mapper()` combines map knowledge updates with drawing; radar, HUD, and console specials use similar action dispatch. `animate()` advances `old_camod` after rendering, and `display_bullets()` clears `b->thrower` only on the last terminal. Rendering has state side effects: suppressing graphics is not sufficient to establish a correct server.
- The `NoIO` option only skips a rendering block in `animate()`; `main()` still opens graphics and requires terminals, and setup still synchronizes displays. It is not an existing headless-server mode.
- `Src/Display/three-d.c` contains empty implementations, including `display_anim_3d()` and `transform_3d()`. The practical baseline is 2D gameplay, despite 3D-related options/fields.

### Approximate size

Counts are **physical lines including comments and blanks**, not executable LOC or estimates of lines requiring changes. Measured using `rg --files Src -g '*.c' -g '*.h' -g '*.l' | xargs wc -l`, with explicit file lists for the responsibility groups below. Generated C, `.obj` bitmap/geometry assets, top-level `Programs/`, utilities, documentation, and data are excluded. Groups describe responsibility and are not a clean dependency partition.

| Surface | Physical lines | Interpretation |
| --- | ---: | --- |
| All checked-in `Src` C/header/lexer source | 49,174 | Includes robot implementations and optional/legacy code. |
| Same source excluding `Src/Programs/` | 34,371 | Approximate application/framework size. |
| `Src/Programs/*.c` | 14,803 | Robot code; one of the twelve files (`shooter.c`) is not in the default build list. |
| `Display/x11.c` + `Display/XMultiSync.c` + `Include/x11.h` | 2,059 | Concentrated X backend, about 4% of all counted source or 6% excluding bots; excludes application escape hatches. |
| Main UI/rendering/input and mixed specials | 11,837 | About 34% of non-bot source: `display`, `gr`, `input`, `interface`, `menu`, `mdesign`, `vdesign`, `escher`, `scroll`, `status`, `message`, `newconsole`, `mapper`, `radar`, `newradar`, `hud` C files. Mostly reusable behind a ported graphics API, largely redesigned for independent clients. |
| Gameplay-oriented candidate reuse | 10,130 | About 29% of non-bot source: `actions`, `animate`, `box`, `camo`, `collision`, `disc`, `explosion`, `game`, `hit`, `intersect`, `maze`, `setup`, `update`, `vehicle`, `lowlib`, `highlib`, `repair` C files. Includes presentation/setup coupling; not an already portable library. |

An include/drawing scan also matched about 26.5k lines of whole C files, including physics, loaders, and a bot. That is evidence of dependency reach, **not** 26.5k lines of X implementation. A reasonable planning model is ~2k lines of backend replacement, ~12k of frontend/mixed responsibilities to audit, and ~10k of gameplay candidates plus ~15k of robot code worth preserving. A backend-only count seriously understates a distributed rewrite; the broad include count seriously overstates a native graphics port.

### Build, runtime, and other platform work

- Top-level `GNUmakefile` recurses into `Src`; the BSD `Makefile` uses `bsd.subdir.mk`. `Src/GNUmakefile` and `Src/Makefile` share `Makefile.inc1/2`, compile game, display, threads, and eleven bots into one `xtank` executable, and link X11 and math (also `dl` on Linux). There is no separated engine/server library or Windows build configuration.
- `Makefile.inc1` defaults to `X11`, `UNIX`, `S1024x864`, draw batching, `DYNALOAD`, `MULTI_SYNC`, and `THREAD_SWAPCONTEXT`, with `-O0 -g`. `make -n` confirmed scanner generation from `setups.l`, prefixed scanner generation from `vload.l`, and version generation through `Bin/newversion.sh`. A native build needs replacements for Unix shell/build assumptions and dependency tracking for included backend/assets.
- `Src/Threads/thread.c`'s selected implementation uses `getcontext()`, `makecontext()`, and `swapcontext()`. `Src/program.c:run_all_programs()`, `run_program()`, and `check_time()` schedule stackful robot programs using global `cv`; `icounter.c` uses `setitimer()`, `SIGVTALRM`, `SIGALRM`, and `pause()`. Native Windows requires timer and coroutine work (e.g. fibers), even if X remains. The alternate POSIX-thread branch is not evidence of a ready Windows solution.
- `Src/unix.c:compile_module()` contains shell compiler/linker invocations and platform-specific loading, with a `dlopen()` branch on supported Unix targets. Its linker flags are not a portable plugin pipeline. Built-in bots avoid needing runtime compilation, but still need a working scheduler. Dynamic robot loading is a separate compatibility feature.
- `Src/file.c:get_environment()` selects `XTANK_DIR`, content directories, username, and display; loaders also draw progress messages. `load_mdesc()`, `load_vdesc()`, `Src/newfile.c:new_load_vdesc()`, `vload.l`, and `setups.l` are content-import reuse candidates. Paths, Unix headers/functions, permissions, and Windows text/binary file behavior require an audit. The install recipe installs the executable, not a complete asset bundle.
- `Src/Display/objects.c:rotate_objects()` creates the additional vehicle rotations; `make_objects()` uploads pictures separately to each video. `Objects/*.obj` are included bitmap/geometry definitions, not linker object modules. Preserve body ordering, turret coordinates, dimensions, offsets, and collision segments during asset conversion.
- Optional `Src/sound.c` uses network `rplay`/`rplayd`, rather than local Windows audio, and is not in the default `SRCS` list nor enabled by default flags. Sound is additional work if included in the release.

## Approach A (port) with tradeoffs

### A1: Preserve X11 through a compatibility environment

**Reuse:** virtually all gameplay, robots, rendering, editors, content, and the multi-display architecture. Address compiler/runtime incompatibilities and distribution configuration rather than redesign gameplay.

Two deployment arrangements have different deliverables:

- Compile/run the process under **Cygwin** and display through **Cygwin/X**. Verify selected context APIs, interval timers, Xlib internals, and dynamic loading on the actual Cygwin toolchain; POSIX compatibility does not guarantee every selected mechanism works unchanged.
- Run the process on a Unix host and open displays on Windows X servers such as **Xming**. This can make the game playable from Windows while the game executable remains on Unix. Xming supplies the display server, not the POSIX runtime needed to build/run the game on Windows. A native executable linked to Xlib would still need the non-graphics platform port.

**Benefits:** fastest playable proof, highest behavioral fidelity, and an excellent reference system for later work. Existing multiple Windows display servers preserve remote-player capability without designing a game protocol.

**Risks/tradeoffs:** X server/runtime/font setup, remote display authorization and connectivity, high-DPI/fixed layout behavior, mouse/keypad semantics, and private-Xlib compatibility. All players remain coupled to drawing traffic and synchronization; this is principally a local/LAN solution. X server transport/tunneling arrangements must be tested, and there is no existing disconnect/reconnect protocol. Packaging is less convenient than a standalone Windows game. Verify the distribution terms/version of the selected compatibility packages separately from Xtank's BSD license.

### A2: Replace X11 with native Windows graphics/input

**Reuse:** retain the single-process world, control/robot APIs, gameplay, content, and much of the ~12k-line frontend. Implement the existing graphics/event contract over Win32/GDI or a native Windows SDL frontend; make `Video` platform-neutral and repair direct X escapes. GDI or a software backing surface can implement XOR; a conventional accelerated renderer would likely require replacing incremental redraw behavior.

**Rewrite/adapt:** backend/window lifecycle, fonts/resources/preferences, pixel/pixmap handling, event translation, timer pacing, robot context switching, paths/build/package tooling, and optional audio/plugins. Decouple `UNIX` from basic definitions such as `MAX_TERMINALS` and `update_loc`; simply removing `-DUNIX` breaks unrelated logic. Keep collision geometry independent of graphical resource creation.

**Benefits:** feasible, smaller than recreating all gameplay, good fidelity, and a standalone native experience for one player plus bots. Existing editors can potentially survive behind the new primitives.

**Critical limit:** a process cannot create ordinary Win32 windows on other players' PCs in the way it opens remote X displays. Multiple local windows/monitors also do not automatically provide independent player keyboards/mice. A pure backend port loses the current remote multiplayer mechanism unless X endpoints remain, remote presentation is added, or a game protocol is designed. That networking work moves the project toward Approach B.

**Risks:** global terminal assumptions, XOR/expose fidelity, display-dependent geometry, old C/prototype and 64-bit ABI assumptions, and bots whose control flow depends on stackful scheduling. The X backend size makes this tractable, but not a weekend API substitution.

## Approach B (client/server rewrite) with tradeoffs

### Proposed architecture

An authoritative server owns maze mutations, movement/collisions, weapons, damage, RNG, bots, scores, respawns, and game rules. Each native Windows client owns its window, camera, controls, renderer, HUD, preferences, and local audio. A host can run both processes locally; a remote server can remain on Linux initially. Delivering a Windows-hosted server is a distinct acceptance criterion.

Clients send bounded, sequenced **intent** (drive, heading, turret aim, firing, weapon/special selection), not authoritative positions or damage. The server consumes commands at fixed tick boundaries and publishes tick-numbered state snapshots plus events. Use stable entity IDs/generations and explicit wire fields, not memory dumps of `Vehicle`, `Loc`, pointers, platform `long`, or enums. Handle content/protocol versions, ownership, host privileges, observers, joining, departures, and reset transitions explicitly.

Start with direct-connect LAN play and a proven transport library supporting unreliable sequenced snapshots and reliable control/events. Render interpolated remote state independently of server tick rate; test before adding local prediction/reconciliation. Internet hosting, discovery/relay/NAT traversal, and extensive lag compensation expand scope beyond that MVP. A blocked client must never block the simulation.

Snapshot design must preserve Xtank's information rules: mapper exploration, radar/IFF/stealth, camouflage, team messaging, and observer privileges are not merely cosmetic. Server-authorized per-player views should expose the appropriate knowledge rather than blindly broadcasting every private field. `special_mapper()` and radar code are useful specifications for this split. Preserve or deliberately revise multiple-player control of one tank, with explicit command arbitration.

### Reuse versus rewrite

**Literal from-scratch version:** rewrite the engine, UI, bot interfaces/scheduler, and all networking. Reuse the rules/formulas as specifications, weapon/vehicle tables (e.g. `Src/Include/weapon-defs.h`), maze/vehicle data, sprite/geometry assets, and documentation; retain import compatibility or provide converters. Existing C bots cannot be presumed source-compatible with a new engine. Porting all eleven built-ins and the old robot API can become a major project by itself.

**Lower-risk variant:** extract the existing C simulation into a headless server core, keeping functions and tables where their behavior is valuable, while creating fresh commands, snapshots, client rendering, and session management. `animate()` supplies ordering, `lowlib.c` supplies control operations, and `game_rules()` supplies mode dispatch. Replace `term`/`vid` dependencies with explicit player state, move graphical screen coordinates into client state, split special update/draw responsibilities, and replace rendering side effects with simulation-owned transitions/events. Retain body geometry in headless assets. This is a client/server architectural rewrite with gameplay reuse, not a literal from-scratch game.

### Benefits and technical risks

**Benefits:** native clients on independent Windows PCs, no X server requirement, simulation isolated from display latency, explicit sessions/reconnection policy, and a sound base for modern rendering and eventual Internet play. Server authority also avoids needing deterministic full-world simulation on all clients.

**Risks:** gameplay/feel regression across many weapons and modes; hidden render-state dependencies; collision geometry conversion; fair command ordering; aim/camera coordinate conversion; latency/interpolation artifacts; transient event loss/duplication; visibility leakage; and bot compatibility. Fixed-step replays need a controllable seed and bot scheduling because the current RNG seeds from time (`Src/util.c:init_random()`) and bot budgets depend on CPU time. No automated gameplay regression suite was evident in the surveyed build/source layout, so characterization is part of the work. A full rewrite incurs all these risks while also recreating proven physics and AI behavior.

### Rough relative effort

Indicative **engineer-weeks**, assuming one experienced C/game-networking developer, occasional owner playtesting, preserved 2D art, and no graphics redesign. These are planning ranges, not validated quotations. MVP means installation, selecting supplied content, and completing a match; native network MVP means two Windows PCs, direct-connect LAN, basic Combat mode, and essential HUD. Feature-complete estimates include remaining modes, built-in bots, editors, and packaging; legacy dynamic robot loading requires a separately agreed compatibility target.

| Deliverable | Rough effort | Main uncertainty |
| --- | ---: | --- |
| X-compatible Windows play demonstration | 1–3 | Fresh build, context/timer APIs, fonts, multiple displays. |
| Packaged compatibility/LAN release | 3–8 total | Runtime deployment and robust display setup; minimal gameplay changes. |
| Native single-process port, local play/bots | 8–16 MVP; 16–28 broader parity | Graphics semantics and non-X OS layer; remote native multiplayer not included. |
| Literal from-scratch native client/server | 24–40 MVP; 50–90+ broader parity | Recreating gameplay, bot compatibility, editor/UI work, and network feel. |
| Client/server architecture with extracted existing core | 16–28 MVP; 30–55 broader parity | How much gameplay can run correctly without terminals/rendering. |

The important relative ordering is compatibility first, native local port next, then native network architecture, with a literal gameplay rewrite highest risk. Adding client/server later to a finished native local port is additional work, not included in its estimate. More people can parallelize frontend and server work after agreeing the protocol, but effort does not translate linearly into calendar speed. Re-estimate after a headless-core spike and the first two-client match.

## Recommendation with rationale

**Recommend a staged client/server modernization that reuses the existing gameplay core, preceded by a short X-compatible reference/playability spike.** Allocate against the extracted-core range, not the cheaper backend-only port range. The default target should be standalone Windows clients and direct-connect LAN multiplayer, with a Linux authoritative server first and Windows hosting included in the release plan.

The owner's instinct is correct that independent native clients need a different architecture: the existing multiplayer transport is X11, and replacing drawing calls cannot reproduce remote Win32 displays. However, the inspected code does not justify throwing away all gameplay. It contains a recognizable central frame order, substantial collision/weapon/rule logic, content, and nearly 15k lines of robot implementations. Recreating these would increase fidelity risk more than it reduces porting work.

The key feasibility question is whether that logic can be separated economically from terminal state and graphical side effects. Answer it early with a bounded extraction spike. If it fails, choose a fresh gameplay implementation using the old game as a behavioral reference and revise the budget toward the full-rewrite range. If the owner only needs occasional Windows LAN play and accepts X runtime installation, the compatibility release alone can satisfy that narrower goal at much lower cost.

## Phased work plan

### Phase 0 — Baseline and Windows proof (1–3 engineer-weeks)

- Pin the release/commit and resolve the 1.6.0/1.6.1 labeling. Agree MVP controls, supplied content, server OS, modes, bot/API compatibility, and same-tank multi-player policy.
- Build and run the original on a supported Unix environment. Record representative movement, collisions, weapons, camouflage, radar, deaths, scoring, and reset behavior.
- Demonstrate a local Windows X endpoint and two independent Windows display endpoints; try Cygwin-hosted execution if a Windows-hosted compatibility deliverable is required. Verify context/timer support and replace private multi-sync with the existing public serial path if necessary for the experiment.
- **Exit:** a completed two-player reference match and an observed toolchain/runtime issue list; a compatibility package may already be a useful interim release.

### Phase 1 — Headless extraction feasibility (2–4 weeks)

- Separate `animate()`'s simulation passes from pacing, terminal input, presentation, and X synchronization. Introduce explicit match/player context and a command boundary around existing vehicle operations.
- Split loaders from progress drawing, preserve collision/body/turret geometry without pixmaps, and separate specials' knowledge updates from drawing. Move `old_camod`/disc render bookkeeping to appropriate simulation or presentation state.
- Establish controlled RNG seeding and scripted commands for repeatable characterization. Initially run without bots, then prove one built-in bot through a headless-compatible scheduler/API; suppress or replace Bootlegger's debug display path.
- **Exit/decision gate:** headless Combat matches exercise movement, wall/tank/bullet collisions, damage, respawn, and scoring without an X connection; outcomes are stable under scripted runs and consistent with the reference. Re-budget or switch to fresh gameplay implementation if extraction remains dominated by UI assumptions.

### Phase 2 — Windows client and single-client vertical slice (4–6 weeks)

- Define versioned commands/snapshots/events and content IDs. Build the Windows frontend using SDL or an equivalent native rendering/input layer, retaining original 2D appearance and collision scale.
- Connect one Windows client to the headless Linux server in a controlled local test setup; use loopback when client and server can run on the same host. Implement aim, drive/fire, essential console/HUD/map, deaths, respawn, and match results. Keep camera/render rate independent of simulation.
- Provide a basic connect/start flow; translate screen-relative mouse controls into explicit world/vehicle intent.
- **Exit:** a Windows user can install/start, select a supplied maze/vehicle, and complete a Combat match against a bot or scripted opponent without X11.

### Phase 3 — Two-PC multiplayer MVP (4–6 weeks)

- Add reliable session/configuration/events, sequenced inputs, snapshot interpolation, entity lifecycle, content/version mismatch handling, and host/observer roles. Implement ownership validation and appropriate per-player knowledge views.
- Exercise disconnects, bounded reconnect/resume policy, input timeouts, reset transitions, and a deliberately slow client. Measure bandwidth and tick time with the existing entity limits rather than inferring them from source size.
- Test direct-connect LAN matches under simulated delay/loss/reordering; add prediction only where measured control latency warrants it. Port server timer/coroutine/build dependencies, add host-and-play launch flow, and verify Windows hosting for the recommended release target.
- **Exit:** two separate Windows PCs complete and restart matches; one lagging/disconnected client does not stall the server. Re-estimate feature parity from this playable result.

### Phase 4 — MVP hardening and distribution (3–6 weeks)

- Package clients/server, required assets, writable user settings, and notices. Verify clean Windows installation, DPI/window focus/keypad/mouse behavior, repeated sessions, and resource cleanup.
- Add focused gameplay/protocol regression coverage for real boundaries: collision/damage behavior, command ownership/order, lifecycle resets, packet impairment, and knowledge visibility. Compare with owner reference playtests.
- **Exit:** a repeatable, documented native Windows LAN release. Phases 0–4 total roughly 14–25 weeks of identified work; the 16–28-week planning envelope reserves additional integration/uncertainty allowance.

### Phase 5 — Broader compatibility and reach (separately scheduled)

- Restore remaining modes, weapon/special edge cases, all agreed built-in bots, same-tank crew behavior, richer messaging/observers, and native vehicle/maze editors. Preserve data import/export and vehicle-design calculations such as `Src/vdesign.c:compute_vdesc()`.
- Add local audio if desired. Decide whether to preserve source-level robot APIs and runtime compilation/loading or introduce a new extension mechanism; budget this explicitly.
- Pursue Internet hosting/discovery/relay and additional latency handling after the LAN release. Each feature group gets reference scenarios and an owner acceptance match; broader parity is the 30–55-week extracted-core envelope, subject to the earlier gates.
