# Xtank Windows modernization — Phase 0 decision brief

**Recommended approval:** pin the checkout below; choose **B-extracted-core**; ship native Windows LAN clients with a Linux server, Combat and `drone` only.
The owner should mark each numbered decision in §4 **approve / reject / amend**. These are proposed release commitments, not verified capabilities.
Evidence: `PORT-SCOPE.md` plus direct source/Git inspection; no build, reference match, or Windows runtime test was performed for this brief.

## 1. Baseline pin

**Pin commit `14e6d42efdc0318fbf37717b21cc9f6027dc7e9f` from `https://github.com/dsimon/xtank`.**
It is this checkout's HEAD, dated 2017-05-13, with subject “Reduce complier warnings from Display/objects.c”. Pin source and supplied assets together.

- Baseline runtime string: **`Xtank Release 1.6.1`**, exactly what `Bin/newversion.sh:29,45` generates.
- Describe its provenance as **“1.6.0-relicensed lineage, 1.6.1-labeled snapshot at 14e6d42”**. `README.md:12` describes the 1.6.0 relicense; `Src/Programs/README.txt` records 1.6.0 bot observations, not this snapshot's release identity.
- This is a shallow checkout with one visible commit and no local tags. Neither a tagged 1.6.0 release nor an official 1.6.1 release is established by the available Git history. Do not infer either from the mirror label.
- **Why this pin:** it is the exact source already scoped and includes the current warning fixes. Changing only the label to 1.6.0 would conceal the discrepancy without establishing a different baseline.
- Reference artifacts must record the full commit, toolchain, build flags and asset hashes. Generated build number/date/user/path (`Bin/newversion.sh:36–48`) are metadata, not baseline identity.
- Name the modernized MVP **`Xtank Windows MVP 0.1.0`**, reporting the upstream baseline SHA separately; do not present it as an unchanged upstream release.

## 2. Approach confirmation and stop/go gates

**Choose B-extracted-core.** Retain the C simulation on an authoritative server; create the command/snapshot protocol, session management and native Windows 2D frontend.

- **Single strongest reason:** independent native Windows PCs need a game protocol, while `Src/animate.c:181–212` already provides reusable movement/collision/special/rule ordering. A2 cannot reproduce `XOpenDisplay()`-based remote windows (`Src/Display/x11.c:250`); B-full-rewrite needlessly recreates proven gameplay.
- **Single biggest risk:** extraction changes behavior because presentation is part of world state: collision uses `Picture` geometry (`Src/collision.c:50–73`), camouflage bookkeeping follows drawing (`Src/animate.c:222–245`), and disc thrower state is cleared by drawing (`Src/display.c:370–390`). The core is extractable in principle, not already headless.
- **A1 is the reference spike:** allow up to three engineer-weeks to build this pin on Linux and complete a Combat match through two independent Windows X endpoints. Capture controls, motion, collision, damage, scoring, respawn and reset; record toolchain/runtime problems. Use public serial synchronization if private `XMultiSync` blocks the spike (`Src/gr.c`, `Src/Display/XMultiSync.c`).
- **Then allow up to four engineer-weeks for extraction feasibility:** run scripted, seeded Combat without `DISPLAY` or an X connection; preserve rotated geometry, frame ordering and authorized map/radar knowledge; demonstrate one headless `drone` match.
- Reference gate requires acceptance criterion 1 in §3; extraction gate requires criterion 2 and the headless `drone` match. If either timebox expires without its exit evidence, return to the owner with the blocker and revised cost. Switching to A1-only or B-full-rewrite requires a new owner decision.
- Budget against the extracted-core **16–28 engineer-week planning envelope** in `PORT-SCOPE.md`, then re-estimate after the headless gate and first two-client match. These are planning allowances, not a delivery-date promise.

## 3. Precisely bounded minimum viable release

| Dimension | Release commitment |
| --- | --- |
| Modes | **Combat only**, free-for-all, individual score. War, Ultimate, Capture, Race and Madman deferred (`Src/game.c:76–159`). |
| Players/bots | **1 human + 1 `drone`**, or **2 humans with 0–2 `drone`s**; four vehicles maximum. `drone` is the sole built-in offered. No campaign; solo uses the same Linux server. |
| Bot rationale | `Src/Programs/drone.c` declares Combat support and yields via `done()`; `Src/Programs/README.txt` calls it working. That file also reports Diophantine crashes and teammate attacks by spot/Dio: all-eleven parity is a separate commitment. |
| Hosting | **Linux-first, Linux-only MVP server**; Ubuntu 24.04 LTS x86-64. Native Windows 11 x64 clients. Windows-hosted server and standalone offline Windows play follow the MVP. This narrows the Windows-hosting recommendation in `PORT-SCOPE.md`. |
| LAN/session | Direct IP/port on a private LAN; one match per server. Lobby-only joining; one player per tank. First connected player administers start/reset; departing admin transfers authority to the remaining human. No observers or shared-tank crews. |
| Departures | Clients send intent/heartbeat every tick, including while idle. After two seconds without valid input, stop drive/fire and remove the departed tank without kill credit. Remaining session continues; an empty session returns to lobby. Rejoining waits for lobby, with a fresh tank; no mid-match resume. |
| Match preset | Fixed **15 simulation ticks/s**, respawn enabled, individual winning score **1,000**, bots allowed to win. Other applicable settings retain `init_settings()` defaults. `Src/init.c:45` supplies 15 Hz; `Src/actions.c:793–794` shows why the legacy maximum-speed branch is unsuitable. |
| Content | Select `Mazes/combat1.m` or `Mazes/Arena.m`; all players/bots use `Vehicles/Vanguard.V`. Preserve Marauder geometry and Heavy Autocannon behavior. Release preset permits Console, Mapper, HUD and ordinary Radar; disables New Radar, Tac Link, Camo and Rdf listed in that vehicle. |
| Controls/UI | Original 2D art/scale; keyboard drive/steer, mouse turret aim/fire, weapon enable/select; connect/lobby/start/results/reset/quit. Show armor, ammo, fuel, heat, score, explored map and authorized radar contacts. No editor or general settings UI. |
| Packaging | Windows client archive with runtime dependencies/assets; Linux server package with assets and launch instructions. Preserve BSD/file notices. No X server required for either MVP executable. Audio, runtime robot compilation/loading, public-Internet hosting, discovery and NAT traversal deferred. |

**All acceptance criteria must pass:**

1. **Reference evidence:** the pinned X build completes a two-Windows-endpoint Combat match and reset; an owner-reviewed scenario record covers movement, wall/tank/bullet collisions, directional damage, death, scoring, respawn, mapper and radar behavior.
2. **Core fidelity:** fixed seed and tick-indexed commands, without bots, exercise those scenarios in reference and headless builds. Death/respawn/win ticks, score, armor, ammo and maze changes agree exactly; positions agree within 0.01 world unit at checkpoints. Any deviation requires a recorded owner-approved rule change. Three identical headless replays produce identical state checksums on the supported server build.
3. **Installation:** on two clean Windows 11 x64 machines, unpack and connect using the documented steps with no compiler, POSIX runtime or X installation. The Ubuntu server starts with `DISPLAY` unset and no X service, offering both named mazes and the release preset.
4. **Playable completion:** complete a solo-vs-`drone` match and ten two-human matches, including both mazes and the four-vehicle configuration. Verify firing/aiming, collision/damage, all required HUD fields, death/respawn, correct winner at 1,000 points, and results/reset into another match without restarting processes.
5. **Bot stability:** a 60-minute four-vehicle session and twenty match resets produce no bot crash, scheduler stall or stale ownership; `drone` moves, acquires enemies and fires without graphics access. Preserve its algorithm/API subset; identical CPU-time-dependent decisions are not required.
6. **Network resilience:** for 30 minutes at four vehicles, inject 50 ms round-trip latency, 1% packet loss and delay 1% of packets by an extra 100 ms to induce reordering. No duplicate kills/respawns or entity desynchronization lasting over one second; 95th-percentile input-to-visible-action latency is at most 250 ms. Freeze one client for ten seconds, disconnect it and rejoin at the next lobby: the other client and simulation continue, with the departure policy above.
7. **Timing/information:** run clients at 30 and 144 render FPS; the server still advances 900 ticks in 60 seconds ± one tick, with 99th-percentile tick work below 66.7 ms on a recorded four-core/8 GB test host. Packet capture confirms no unexplored map data or unauthorized vehicle/contact data; foreign-tank commands and protocol/content mismatches are rejected before affecting play.
8. **Owner sign-off:** the owner plays one solo and one two-PC LAN match and approves reference feel, controls, supported content and documented installation. Record tested machine/toolchain specifications and results with the release.

## 4. Owner decisions — approve, reject or amend

1. **Baseline and release naming:** approve the exact SHA/1.6.1 runtime baseline and separate MVP name in §1. If authentic 1.6.0 is mandatory, supply or identify that revision; re-scope differences and regenerate all reference evidence before implementation.
2. **Product approach and investment:** approve B-extracted-core and the bounded A1/extraction gates in §2. If X-assisted LAN play is sufficient, choose A1 as the product and replace native/protocol acceptance; A2 fits local native play, while B-full-rewrite requires a larger budget and gameplay re-characterization.
3. **MVP breadth and server OS:** approve the exact §3 modes, limits, content/preset, session policy and Linux-only hosting. Additional modes/content require their own rule/asset/visibility scenarios. Windows hosting from day one adds timer/context/build/package acceptance (`Src/icounter.c`, `Src/Threads/thread.c`) and an offline host-and-play workflow before release.
4. **Bot compatibility target:** approve `drone`'s behavior and required `xtanklib` subset only, with necessary bug fixes documented. All eleven built-ins, external C source compatibility or exact historical scheduling each expand scheduler/API work and bot-specific tests (`Src/program.c`, `Src/Include/xtanklib.h`); no bots would remove the solo release commitment.
5. **Audio:** approve a silent MVP. Local Windows audio adds asset sourcing, event-to-sound mapping and volume/device tests; `Src/sound.c` is optional network `rplay` support, not a ready native audio implementation.
6. **Editors and user content:** approve supplied-content selection only; defer maze/vehicle editors and arbitrary imports. Requiring them adds client editor UI, save/import validation and vehicle-design parity (`Src/mdesign.c`, `Src/vdesign.c`, `Src/vload.l`); editable content also needs client/server content identity handling.
7. **Dynamic robot loading:** approve built-in-only server bots, no runtime compiler/plugin workflow. Requiring legacy loading adds an agreed OS/compiler/API target and module lifecycle tests; `Src/unix.c:compile_module()` is Unix shell/loading machinery, not a portable extension system.
8. **Legacy social/session features:** approve one controller per tank, lobby joins and no observers/chat. Shared crews require command arbitration (`Src/setup.c:setup_combatant()`); observers, team messaging or live reconnect require explicit privileges, knowledge filtering and lifecycle acceptance before inclusion.

**Approval authorizes the Phase 0 reference spike and establishes the proposed MVP contract; proceeding beyond each gate requires its exit evidence.**
