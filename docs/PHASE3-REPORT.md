# Phase 3 report — packages, repeatable gate and two-PC owner exit (P3.16)

**Status: Linux automated gate PASS; packages PASS; operational evidence
attached; two-PC Windows 11 owner exit BLOCKED (owner hardware).** 2026-10-14.
Nothing committed. No gameplay or protocol behavior was changed; the frozen
simulation is byte-identical (G3 below).

Authority: `docs/PHASE3-PLAN.md` §P3.16 and §6, `docs/PHASE3-DECISIONS.md`
(D3-A…D3-G), `docs/PHASE3-RUN.md`. Inputs: `P3.1`–`P3.15` reports and drivers.

> A `P3.16 PHASE3-CHECK PASS` below is the **Linux automated gate**, not the
> two-PC exit. The two-PC exit is BLOCKED until the owner runs the documented
> procedure on two clean Windows 11 PCs.

---

## 1. What was built (new files, harness/config/docs only)

| File | Purpose |
| --- | --- |
| root `GNUmakefile` | new `phase3-check` target (`OPERATIONAL=1` optional) |
| `Tests/phase3/phase3-check.sh` | fail-fast, machine-readable Phase 3 exit gate |
| `Tests/phase3/package.sh` | package acceptance (unpack/relocate/manifests/DLL/X-free) |
| `Tests/phase3/windows-multiplayer.ps1` | native Windows two-PC owner procedure |
| `Bin/package-phase3.sh` | Phase 3 release archives with phase metadata |
| `docs/PHASE3-RUN.md` | operator run guide (gate/packages/operational/Windows) |
| `docs/PHASE3-REPORT.md` | this report |

One product-source change was required by the gate (finding F1):
`Transport/enet_transport.c` — the off-by-default P3.13 timing helper
`tx_now_ns()` now uses `QueryPerformanceCounter` under `_WIN32` and
`clock_gettime` elsewhere. No gameplay/protocol change; Linux/server behavior
and the frozen simulation are unchanged.

---

## 2. Environment (recorded)

| | Value |
| --- | --- |
| branch / HEAD | `windows-phase1` / `63016bad6781ea238c0e52ea2b6efda1b9a8db45` (P1–P3 owner work uncommitted) |
| kernel | `Linux htch-runtime 7.0.0-39-generic x86_64` |
| cores / RAM | **2** (`nproc`) / `MemTotal 8125588 kB` — **below the D3-G ≥4-core reference (DELTA)** |
| gcc | `13.3.0` (`-O0 -g`, `LC_ALL=C`) |
| flex | `2.6.4` |
| MinGW-w64 | `x86_64-w64-mingw32-gcc (GCC) 13-win32`; binutils `2.41.90.20240122` |
| cmake / python3 | `3.28.3` / `3.12.3` |
| SDL3 / ENet / protocol | `3.4.18` / `1.3.18` / `1.1` |
| Wine / valgrind / clang | not installed (Wine supplemental; see F3) |

Gate source manifest SHA-256:
`5392d96b4863d2b24a83e54d1ad1934fb20e336ee2b1087c2dab7ff2bc5b9f0c`
(`gate/manifest/sources.txt`; HEAD, toolchain, `git status`, per-file SHA-256).

---

## 3. Automated exit gate — result

```sh
make phase3-check PHASE3_EVIDENCE=/home/hatch/workspace/coding/xtank-evidence/p3.16/gate OPERATIONAL=1
# bash Tests/phase3/phase3-check.sh --evidence <dir> --operational
```

**`P3.16 PHASE3-CHECK PASS`** — `gate-results.json`:
`status=PASS`, `counts={PASS:31, SUPERSEDED:7, DEFERRED:1, BLOCKED:1}`,
`is_two_pc_exit=false`, 40 steps.

| Step | Status | rc | s |
| --- | --- | --- | --- |
| source-manifest | PASS | 0 | 0 |
| phase1-check (full oracle + geometry/intent/lifecycle/content/package + 4 X-boundary) | PASS | 0 | 69 |
| p2-protocol-v1.0-retention | PASS | 0 | 0 |
| p2-players | PASS | 0 | 0 |
| p2-events | PASS | 0 | 1 |
| p2-assets | PASS | 0 | 1 |
| p2-timeline | PASS | 0 | 1 |
| p2-input | PASS | 0 | 1 |
| solo-compatibility (P2.14 solo slice) | PASS | 0 | 10 |
| p3.2-departure-design | PASS | 0 | 1 |
| p3.3-protocol-v1.0-retention | PASS | 0 | 0 |
| p3.3-codec-mingw-sanitizer (MinGW + ASan/UBSan fuzz) | PASS | 0 | 24 |
| p3.4-lobby | PASS | 0 | 3 |
| p3.5-players | PASS | 0 | 2 |
| p3.6-admission | PASS | 0 | 1 |
| p3.7-departure | PASS | 0 | 2 |
| p3.8-view | PASS | 0 | 0 |
| p3.8-events | PASS | 0 | 2 |
| p3.9-transport | PASS | 0 | 2 |
| p3.10-client-session | PASS | 0 | 3 |
| p3.11-timeline | PASS | 0 | 0 |
| p3.11-input | PASS | 0 | 1 |
| p3.12-playable | PASS | 0 | 7 |
| p3.12-parity-seed (combat1) | PASS | 0 | 29 |
| p3.12-parity (full 4-scenario, 3 reps) | PASS | 0 | 33 |
| client-build-linux | PASS | 0 | 8 |
| client-build-mingw (x86-64) | PASS | 0 | 19 |
| p3.16-package | PASS | 0 | 42 |
| op-p3.13-timing (analyze) | PASS | 0 | 0 |
| op-p3.14-impairment (analyze) | PASS | 0 | 1 |
| op-p3.15-soak (analyze) | PASS | 0 | 0 |
| mingw-runtime-wine | DEFERRED | - | 0 |
| owner-two-pc-exit | **BLOCKED** | - | - |
| p2-codec / p2-view / p2-transport / p2-session / p2-ui / p2-parity / p2-package | SUPERSEDED | - | 0 |

`SUPERSEDED` is not a skip: each names a required Phase 3 successor
(`require_successors` fails the gate if any successor is not PASS). See F2.

Machine-readable outputs under the evidence root:
`gate-results.tsv`, `gate-results.json`, `gate-summary.txt`,
`manifest/sources.txt`, `logs/<step>.log`.

### Gate behavior that satisfies §6 item 4

- Fail-fast; a missing prerequisite (`ENet`, SDL3 Linux/MinGW, MinGW-w64,
  `flex`, `Xvfb`, `xdpyinfo`, `xrdb`, `cmake`, `python3`, `zip`/`unzip`/`tar`)
  **BLOCKS** the gate; an explicitly skipped required step yields `INCOMPLETE`.
  Neither can ever be a full `PASS`.
- Every step records exact command, exit code, wall time, log path and driver
  SHA-256.
- `--operational` enforces P3.13/P3.14/P3.15 or reports `BLOCKED`; it never
  silently skips.

---

## 4. Packages

`Bin/package-phase3.sh` + `Tests/phase3/package.sh` → **`P3.16 PACKAGE PASS`**:
archives unpack and the bundles run relocated to `a path with spaces`;
`VERSION`, `PINNED-RUNTIMES.txt`, `CONTENT-MANIFEST.sha256`, `MANIFEST.sha256`,
`THIRD-PARTY-NOTICES.txt` and `licenses/` are present on both; the content
manifest hashes match the shipped bytes; the client PE import allowlist passes;
the packaged network server is X-free and starts display-free (`--listen`) and
plays both mazes.

| Artifact | SHA-256 (first 16) | Notes |
| --- | --- | --- |
| `xtank-client.zip` | `faddc14854cbc166` | Windows 11 x64, 24 fixtures, release metadata |
| `xtank-client.exe` | `cb776ec0baea4f46` | imports `KERNEL32, msvcrt, SDL3, WINMM, WS2_32` only |
| `SDL3.dll` | `4659d5a1e240e7d5` | bundled 3.4.18 (matches P2.16) |
| `xtank-assets.bin` | `9141e3b99ad01a2e` | portable visual bundle (matches P2.16) |
| `xtank-server.tar.gz` | `a158d1db36b037ef` | Ubuntu 24.04 network server |
| `xtank-server` | `4258118f4d632cea` | ENet static, X-free, `SERVER-LISTEN … hz=15 humans=2` |

`VERSION` = `Xtank Phase 3 MVP 0.3.0`; pinned `sdl3=3.4.18 enet=1.3.18
protocol=1.1 mingw=13-win32`.

---

## 5. Operational evidence attached (P3.13 / P3.14 / P3.15)

`phase3-check --operational` re-ran each task's analyzer against its existing
evidence root and required PASS:

| Task | evidence root | analyzer verdict |
| --- | --- | --- |
| P3.13 timing/bandwidth/latency | `~/workspace/coding/xtank-evidence/p3.13` | TIMING PASS (all budgets) |
| P3.14 impairment/slow-client | `~/workspace/coding/xtank-evidence/p3.14` | IMPAIRMENT PASS (all budgets; 30-min combined run) |
| P3.15 paced 60-minute soak | `~/workspace/coding/xtank-evidence/p3.15` | SOAK PASS (all checks; 20 resets) |

**Manifest relationship (stated precisely).** The operational runs predate
P3.16. During P3.16 the only product-source change was
`Transport/enet_transport.c` (F1, Windows-only `_WIN32` branch of the
off-by-default timing helper). All other P3.16 additions are new harness,
config, docs or the `phase3-check` make target. The Linux operational runs
exercise only the Linux `clock_gettime` path, so their measurements apply to
the packaged manifest. This is corroborated byte-for-byte by the gate's frozen
artifacts: the Phase 1 oracle (`combat-2drone 76f23a33…88c6a`, 798 lines / win
26569; `combat-4drone e13b7258…92b6e`; `madman-2drone 440b0e5b…f9c03`) and the
P3.12 three-way parity (`a97df94b…553cea` and the other three scenarios) match
the P3.12–P3.15 reports exactly. Attachment note:
`p3.16/operational/ATTACHMENT.md`.

---

## 6. G3 — frozen simulation byte-identical

Two independent full runs on the same working tree (the uncommitted-work
pattern used by P3.1–P3.15; `--stage before` cannot build here because pinned
HEAD predates the neutral backend), with `LC_ALL=C XTANK_SEED=114336
TAR_OPTIONS=--no-same-owner`:

```
g3/before: P1.2 REGRESSION PASS (stage=after subject=null suite=full)
g3/after:  P1.2 REGRESSION PASS (stage=after subject=null suite=full)
```

`cmp` of the authoritative runs: **42/42 `*.state`/`*.result`/`*.assert` files
byte-identical, 0 differing.** The only 12 differing files are candidate
`.state.stdout` wall-clock diagnostics (`SERVER-LOOP … avg_us/p99_us/max_us`),
identical tick counts — the non-authoritative class recorded since P3.1. Frozen
values reproduce exactly; the substantive after-vs-frozen-reference identity
also passed inside `phase1-check` (gate step 2).

---

## 7. Findings

- **F1 — MinGW client cross-build portability (fixed).** The gate's required
  MinGW-w64 build of the shipped client failed: `Transport/enet_transport.c`
  (P3.13 timing) called `clock_gettime()`, absent in the pinned **win32-thread**
  MinGW model without libwinpthread (forbidden by D2-D). Minimal reviewed fix:
  `tx_now_ns()` uses `QueryPerformanceCounter` under `_WIN32`. This is exactly
  the integration gap the Phase 3 gate exists to catch.
- **F2 — inherited Phase 2 drivers superseded by v1.1 (reconciled).** The P3.3
  exact-version-1.1 bump (documented in `P3.3-REPORT.md` §7) makes the v1.0
  byte-golden Phase 2 drivers obsolete. Probed post-v1.1: PASS = `protocol`
  (v1.0 independent verifier), `players`, `events`, `assets`, `timeline`,
  `input`, `playable`; superseded = `codec` (v1.0 goldens), `view`
  (`SelfState.reserved` scalar), `transport` (documented wall-clock
  `paused-reader` flake), `session` (solo epoch/abort semantics changed by
  P3.4), `ui` (segfault on changed client structs), `parity` (P3.12 successor).
  The gate runs the valid drivers and records each superseded driver with its
  **required** Phase 3 successor; `require_successors` fails the gate if any
  successor is not PASS. v1.0 fixtures remain independently verified (P3.3).
- **F3 — fresh-VM prerequisites (installed).** This host lacked `flex`,
  `xdpyinfo`, `xrdb` and MinGW-w64. Installed flex 2.6.4, x11-utils,
  x11-xserver-utils and the pinned MinGW-w64 toolchain; all are now gate
  prerequisites, so a missing tool BLOCKS rather than PASSes.

---

## 8. Two-PC Windows 11 owner exit — BLOCKED

The native exit cannot run on this Linux VM and is **not claimed**. `owner-two-pc-exit`
is `BLOCKED` in `gate-results.json`, and the gate's PASS text explicitly states
it is not the two-PC exit. Exact procedure ready in `docs/PHASE3-RUN.md` §4 and
`Tests/phase3/windows-multiplayer.ps1`:

1. Linux server from the packaged bundle: `xtank-server --listen 7777`.
2. Each Windows PC: unpack `xtank-client.zip` (paths with spaces fine) and run
   `windows-smoke.ps1` + `windows-multiplayer.ps1 -Mode Verify -Server …`.
3. `windows-multiplayer.ps1 -Mode Accept -Matches 10`: ten two-human matches
   across both mazes and 0/1/2 drones (four vehicles), recording natural
   1,000-point winners, results, controls/HUD/map/radar/death/respawn,
   same-process restart/rejoin, administrator transfer, paths/DPI/focus.
   It writes `xtank-phase3-acceptance.json` and prints PASS only when all ten
   matches and every confirmation are recorded.
4. `windows-multiplayer.ps1 -Mode Solo -Server …` for solo-vs-drone.
5. Owner install/feel sign-off.

---

## 9. Evidence (outside Git)

`~/workspace/coding/xtank-evidence/p3.16/`:
`gate/` (accepted gate run: `gate-results.{tsv,json}`, `gate-summary.txt`,
`manifest/`, `logs/`, `phase1-check/`, `drivers/`, `build/`, `package/`),
`g3/` (`before/`, `after/`, logs, 42/42 cmp), `operational/ATTACHMENT.md`,
`manifest/host.txt`. `/tmp/opencode/xtank-phase3` symlinks to this root.

---

## 10. Phase 4 hardening estimate

Indicative, not committed:

- **Owner/native acceptance:** 1–2 owner sessions (two Windows 11 PCs) + a
  diagnosed-fix loop; the procedure is ready, so this is hardware time, not
  engineering.
- **Resolve F2 debt (1–2 weeks):** adapt the superseded Phase 2 drivers to the
  v1.1 profile (fixture regen, `view`/`ui` struct updates, `session` epoch
  semantics) or formally retire them into Phase 3 successors; ideally restore a
  single `make phase2-check`-equivalent profile so the composition is not
  needed.
- **Protocol/session hardening (2–4 weeks):** packet-capture fuzzing of the
  shipped listen path, ENet fragmentation under adversarial loss at packet
  level, long-horizon reconnect churn, and CI wiring of `phase3-check`
  (including `--operational`) once runner ownership/retention is agreed.
- **Broader parity (3–5 weeks):** extend independent network parity to
  four-vehicle scenarios with departure/impairment bounded separately, and
  re-measure latency/bandwidth on a ≥4-core/8 GB D3-G host (this host is a
  2-core DELTA).

Phase 4 remains a separate release-hardening scope and needs owner approval.
