# Phase 3 — how to run it

Operator guide for the **two-PC multiplayer MVP** (`docs/PHASE3-PLAN.md`).
Covers the automated Linux gate, the packages, the operational
(timing/impairment/soak) drivers, and the Windows two-PC owner procedure.

Authority: `docs/PHASE3-PLAN.md` §P3.16 and §6, `docs/PHASE3-DECISIONS.md`
(D3-A…D3-G), `docs/PROTOCOL-v1.1.md`, `docs/VIEW-v1.md`, `Tests/phase3/README.md`.
No gameplay or protocol behaviour is changed by any command here.

---

## 1. Automated Linux exit gate

```sh
make phase3-check PHASE3_EVIDENCE=/tmp/opencode/xtank-phase3/p16/gate
# equivalent:
bash Tests/phase3/phase3-check.sh --evidence <dir> [--operational]
```

`phase3-check.sh` is fail-fast and machine-readable.  It:

1. runs the **full Phase 1 exit gate** (`make phase1-check`);
2. runs the **inherited Phase 2 drivers and package checks**
   (`Tests/phase2/phase2-check.sh`) plus an explicit solo-compatibility step;
3. runs the **P3.2–P3.12 drivers**: `departure-design`, `protocol`
   (v1.0 fixture retention), `codec` (MinGW golden + ASan/UBSan fuzz), `lobby`,
   `players`, `admission`, `departure`, `view`, `events`, `transport`,
   `client-session`, `timeline`, `input`, `playable`, `parity` (seed then full);
4. builds the client on **Linux and MinGW-w64**;
5. builds and verifies the **Phase 3 packages** via `Bin/package-phase3.sh` and
   `Tests/phase3/package.sh` (unpack/relocate, notices, pinned runtimes,
   version + content manifests, client DLL import allowlist, X-free Ubuntu
   network server).

Every step is recorded with its exact command, exit code, wall time, log path
and source-manifest hash:

| Output | Meaning |
| --- | --- |
| `<evidence>/gate-results.tsv` | one row per step: `step status exit_code seconds command log driver_sha256` |
| `<evidence>/gate-results.json` | the same, plus `status` / `counts` / notes; `is_two_pc_exit:false` |
| `<evidence>/gate-summary.txt` | human summary |
| `<evidence>/manifest/sources.txt` | HEAD, toolchain, `git status`, SHA-256 of every tracked/untracked source file |
| `<evidence>/logs/<step>.log` | full log for each step |

Status values: `PASS`, `FAIL`, `BLOCKED`, `SKIPPED`, `NOT-ENFORCED`,
`DEFERRED`.  A missing prerequisite (`ENet`, SDL3 Linux/MinGW, MinGW-w64,
Xvfb, `cmake`, `python3`, `zip`/`unzip`/`tar`) **BLOCKS** the gate, and an
explicitly skipped required step yields `INCOMPLETE`.  Neither can ever be a
full gate `PASS`.

Add `--operational` to also enforce the P3.13/P3.14/P3.15 operational evidence
(see §3); without it they are reported `NOT-ENFORCED`.

### Output wording (important)

A `P3.16 PHASE3-CHECK PASS` is the **Linux automated gate**.  It is **not** the
two-PC exit.  `owner-two-pc-exit` is always recorded `BLOCKED` in the JSON
(owner hardware required).  See §4.

### Prerequisites

Ubuntu 24.04 x86-64 with `gcc` 13.3.0, `make`, `cmake`, `python3`,
`zip`/`unzip`/`tar`, `sha256sum`, `Xvfb`, and the pinned cross toolchain
`x86_64-w64-mingw32-gcc`/`x86_64-w64-mingw32-objdump` (MinGW-w64 `13-win32`,
`binutils` 2.41.90).  The pinned deps auto-discover from
`~/workspace/coding/xtank-evidence/p2.9/deps/` (`enet-1.3.18`,
`SDL3-3.4.18/x86_64-w64-mingw32`, `linux-prefix`); override with
`XTANK_ENET_DIR`, `XTK_SDL3_LINUX_PREFIX`, `XTK_SDL3_MINGW_PREFIX`.

If Wine is present the MinGW `codec.exe` also runs under Wine; if not, the
cross-build/fixtures are still verified and the exe run is recorded
`DEFERRED` (never silent).

---

## 2. Packages

```sh
bash Bin/package-phase3.sh --out <dist>
bash Tests/phase3/package.sh --evidence <dir> --dist <dist>
```

Artifacts under `<dist>`:

```
client/xtank-client/       bin/xtank-client.exe, bin/SDL3.dll, assets/,
                           fixtures/protocol/, tools/{windows-smoke,
                           windows-multiplayer}.ps1, licenses/,
                           VERSION, PINNED-RUNTIMES.txt,
                           CONTENT-MANIFEST.sha256, MANIFEST.sha256,
                           THIRD-PARTY-NOTICES.txt, BUILD-INFO
client/xtank-client.zip    the above, zipped
server/xtank-server/       bin/xtank-server, share/xtank/{Mazes,Vehicles},
                           COPYRIGHT*, licenses/ENet-LICENSE, VERSION,
                           PINNED-RUNTIMES.txt, CONTENT-MANIFEST.sha256,
                           MANIFEST.sha256, THIRD-PARTY-NOTICES.txt
server/xtank-server.tar.gz the above, tarred
```

`Tests/phase3/package.sh` unpacks both archives into a path **with spaces** and
proves they run relocated, that the metadata/manifests are present and the
content hashes match, that the client PE imports only Win32 system DLLs +
`SDL3.dll`, and that the packaged **network server** is X-free (no libX/Xlib/X
header) and starts display-free (`--listen`) and plays both mazes.

Run the Linux server from a package:

```sh
cd server/xtank-server/bin
env -u DISPLAY -u XTANK_DIR -u HOME ./xtank-server --listen 7777
```

---

## 3. Operational drivers (timing / impairment / soak)

These are **not** in the default automated gate; run them on the recorded
capable host and enforce them with `--operational` or attach them to the final
release evidence.

```sh
# P3.13 pacing/bandwidth/latency (fast: build/pace/stress/replay/analyze)
bash Tests/phase3/timing.sh      --stage all --evidence <p3.13>

# P3.14 impairment matrix + mandatory 30-minute combined run
bash Tests/phase3/impairment.sh  --stage all --evidence <p3.14>

# P3.15 60-minute paced two-human/two-drone soak (twenty resets)
#   launch the long run detached so an agent-session death cannot kill it:
setsid nohup bash Tests/phase3/soak.sh --stage run \
    --evidence <p3.15> > <p3.15>/soak.out 2>&1 < /dev/null &
bash Tests/phase3/soak.sh --stage analyze --evidence <p3.15>
```

Each analyzer exits non-zero on a budget failure.  `phase3-check --operational`
re-runs the analyzers over the attached evidence roots
(`XTANK_P3_13_EVIDENCE`, `XTANK_P3_14_EVIDENCE`, `XTANK_P3_15_EVIDENCE`) and
requires them to pass against the same source manifest.

---

## 4. Windows 11 two-PC owner procedure (BLOCKED on the Linux host)

The native two-PC exit requires **two clean, separate Windows 11 x64 PCs** and
the owner; it cannot run on this Linux VM.  It is recorded `BLOCKED` until the
owner performs it.

Setup:

1. Start the authoritative Linux server from the packaged server bundle
   (`xtank-server --listen 7777`); note the server LAN IP.
2. On **each** Windows PC: unzip `xtank-client.zip`, optionally to a path with
   spaces, and open PowerShell in the bundle.
3. Verify the install on each PC:

   ```powershell
   .\tools\windows-smoke.ps1 -Exe .\bin\xtank-client.exe -Fixtures .\fixtures\protocol
   .\tools\windows-multiplayer.ps1 -Mode Verify -Bundle . -Server <SERVER_IP>:7777
   ```

4. Two-human matches (both PCs join the same lobby; the first admitted player is
   the administrator):

   ```powershell
   .\tools\windows-multiplayer.ps1 -Mode Accept -Bundle . `
       -Server <SERVER_IP>:7777 -PlayerName <YourName> -Matches 10
   ```

   The runner records all ten matches spanning both mazes and 0/1/2 drones
   (including four vehicles), and prompts for the required confirmations
   (natural 1,000-point winners, correct results, controls/HUD/map/radar,
   death/respawn, same-process restart/rejoin/admin transfer, paths with
   spaces/DPI/focus).  It writes `xtank-phase3-acceptance.json` and only prints
   `TWO-PC ACCEPTANCE PASS` when all ten matches and every confirmation are
   recorded.

5. Solo-vs-drone on native hardware:

   ```powershell
   .\tools\windows-multiplayer.ps1 -Mode Solo -Bundle . `
       -Server <SERVER_IP>:7777 -PlayerName <YourName>
   ```

6. Record the owner install/feel sign-off (the Accept runner's final prompts
   plus `xtank-phase3-acceptance.json`).

The owner exit is complete only when both PCs pass `Verify`, the ten-match
`Accept` run and the `Solo` run pass, and the owner's install/feel sign-off is
recorded.  Until then the Linux gate's `owner-two-pc-exit` stays `BLOCKED`.

---

## 5. Evidence discipline

- Keep dumps, logs, binaries, reference worktrees, build trees and packages
  **outside Git** (default evidence root
  `~/workspace/coding/xtank-evidence/p3.16/`; `/tmp` is a small tmpfs).
- Commit only harness source, tiny fixtures/goldens and concise reports.
- Never bless a difference, never treat a missing native proof as `PASS`, and
  never report the Linux automated `PASS` as the two-PC exit `PASS`.
