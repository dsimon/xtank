# Phase 3 test harness

Harness for the Phase 3 two-PC multiplayer MVP (`docs/PHASE3-PLAN.md`). It
complements — and never replaces or weakens — the frozen Phase 1 oracle in
`Tests/phase1/` and the Phase 2 single-client drivers in `Tests/phase2/`.

This directory was created by **P3.1** (`P3.1-REPORT.md`), which froze the
executable baseline and acceptance access before any multiplayer work. No
Phase 3 driver exists yet; new paths are proposals until their task completes.

## Authority and rules

- `docs/PHASE3-PLAN.md` is the task contract (owner-reviewed; §3 task
  contracts, §6 test/evidence strategy, §7 stop conditions).
- `docs/PHASE3-DECISIONS.md` records the approved D3-A–D3-G rulings. New wire,
  session, ownership, departure or measurement changes require an owner ruling
  recorded there first.
- `docs/PORT-SCOPE.md` Phase 3 and `docs/PHASE0-BRIEF.md` §3 establish the
  Linux-only hosting, lobby-only joins, one-controller-per-tank,
  no-observers/chat boundary. `docs/PHASE2-DECISIONS.md` preserves SDL3,
  ENet/UDP, explicit binary wire, MinGW-w64 and 15 Hz one-tick interpolation.
- `docs/PROTOCOL-v1.md` and `docs/VIEW-v1.md` freeze the inherited wire and
  knowledge contracts; `VIEW-v1.md` knowledge rules remain authoritative.
- **No gameplay rule changes.** The simulation must stay byte-identical to the
  frozen pinned X reference (`XTANK_SEED=114336`, `LC_ALL=C`).
- **G3 before/after every task**: G3 = the Phase 1 exact-simulation guardrail G
  plus the task's relevant Phase 3 boundary checks. Require `cmp` byte identity
  between the frozen reference, the pre-change baseline, the post-change
  candidate and three candidate repeats; never tolerances, hash-only
  assertions or relaxed goldens. Wall-clock diagnostics are separate from
  authoritative dump bytes.

## Baseline (P3.1)

- Branch `windows-phase1`; HEAD `63016bad6781ea238c0e52ea2b6efda1b9a8db45`
  (the Phase 1 plan commit); tree `ee1f9c2551feda21e8c6566055fbe306e15c579a`.
- Phase 1 and Phase 2 are **uncommitted** owner work; the delivered baseline is
  the working tree, not HEAD alone. The complete working-tree manifest (git
  status plus SHA-256 of every modified/untracked non-ignored file) and the
  dependency/flag/environment records are frozen in `P3.1-REPORT.md` and under
  `/tmp/opencode/xtank-phase3/p3.1/` (never in Git).
- `--stage before` snapshots `git archive HEAD`, which predates the
  display-free core and cannot build `-DNULL_BACKEND`. As in P2.1–P2.16, take
  the pre-change baseline with `--stage after` on the unmodified working tree
  and `cmp` the resulting `*.state` dumps across the change; or use two
  isolated working-tree copies with distinct evidence roots.

## Layout (proposed in `docs/PHASE3-PLAN.md` §3; created as tasks land)

| Area | Paths |
| --- | --- |
| Departure feasibility | `Tests/phase3/{departure-design.sh,departure-design_fixture.c}` |
| Wire / codec | `docs/PROTOCOL-v1.1.md`; `Protocol/{wire.h,limits.h,encode.c,decode.c}`; `Tests/phase3/{protocol.sh,codec.sh,codec_fixture.c,fixtures/protocol/*}` |
| Lobby / session | `Src/{server_session.c,server_main.c}`; `Tests/phase3/{lobby.sh,lobby_fixture.c}` |
| Players / binding | `Src/{server_players.c,Include/server.h}`; `Tests/phase3/{players.sh,players_fixture.c,fixtures/players/*}` |
| Admission / timeouts | `Src/{server_net.c,Include/server_net.h}`; `Tests/phase3/{admission.sh,admission_fixture.c}` |
| Departure / rejoin | `Src/{server_session,server_net,server_players,server_core}.c`; `Tests/phase3/{departure.sh,departure_fixture.c}` |
| Views / events | `Src/{server_view,server_events}.c`; `Tests/phase3/{view.sh,events.sh,view_fixture.c,events_fixture.c}` |
| Transport | `Transport/{enet_transport.c,transport.h}`; `Tests/phase3/{transport.sh,transport_fixture.c}` |
| Client lobby / authority | `Client/{session_client,frame,menu,main}.*`; `Tests/phase3/{client-session.sh,client_session_fixture.c}` |
| Client lifetime / input | `Client/{model,timeline,input,frame,world_render}.*`; `Tests/phase3/{timeline.sh,input.sh,timeline_fixture.c,input_fixture.c}` |
| Integration / parity | `Tests/phase3/{playable.sh,playable_fixture.c,parity.sh,parity_fixture.c,fixtures/parity/*}` |
| Timing / latency | `Tests/phase3/{timing.sh,latency_fixture.c,measurements.py}` |
| Impairment | `Tests/phase3/{impairment.sh,backpressure.sh,impairment_fixture.c,impairment-analyze.py}` |
| Soak | `Tests/phase3/{soak.sh,soak_fixture.c,soak-analyze.py}` |
| Exit gate / packaging | `Tests/phase3/{phase3-check.sh,package.sh,windows-multiplayer.ps1}`; `Bin/package-phase3.sh`; `docs/{PHASE3-RUN.md,PHASE3-REPORT.md}` |

`Tests/phase3/` mirrors the Phase 1/2 conventions: POSIX shell drivers named
`<area>.sh`, small C fixtures named `<area>_fixture.c`, and tiny committed
golden/fixture data. `Tests/phase3/fixtures/` holds committed wire/input/
expected-field fixtures. The existing Phase 1/2 goldens are preserved
permanently.

## Running

The inherited gates are unchanged and must keep passing:

```sh
make phase1-check
make phase2-check                 # add PHASE2_WITH_PHASE1=1 to include Phase 1
```

The Phase 3 automated gate (proposed, not implemented until P3.16):

```sh
make phase3-check PHASE3_EVIDENCE=/tmp/opencode/xtank-phase3/p16/gate
# equivalent: bash Tests/phase3/phase3-check.sh --evidence <dir>
```

A fast Linux `phase3-check` PASS is reported as such; it is not two-PC exit
PASS. Native Windows smoke, ten completed two-human matches, solo acceptance
and owner sign-off remain separate exit requirements.

### G3 guardrail

```sh
export LC_ALL=C XTANK_SEED=114336
export XTANK_P1_EVIDENCE=/tmp/opencode/xtank-phase3/p<N>/oracle
bash Tests/phase1/regression.sh --suite full --stage before --subject auto
# implement the task
bash Tests/phase1/regression.sh --suite full --stage after --subject auto
```

`--subject auto` resolves to the display-free `null` backend. Preserve the
seed `114336`, the reference pin, assets/settings/traces and the pinned
compiler/flags (`gcc 13.3.0`, `flex 2.6.4`, `-O0 -g`, Ubuntu 24.04,
harness-owned Xvfb). Never bless a difference.

## Evidence discipline

- **Keep dumps, logs, binaries, reference worktrees, packages and generated
  flex/version/object files out of Git.** Evidence lives under
  `/tmp/opencode/xtank-phase3/p<N>/` (per `docs/PHASE3-PLAN.md` §6). The host
  `/tmp` is a small tmpfs, so the Phase 3 evidence root is symlinked to the
  large overlay tree at `/home/hatch/workspace/coding/xtank-evidence/`
  (`/tmp/opencode/xtank-phase3 -> /home/hatch/workspace/coding/xtank-evidence`,
  so `/tmp/opencode/xtank-phase3/p3.1/` resolves there). Symlinking is the
  owner-sanctioned pattern in `AGENTS.md`.
- Commit only harness source, tiny fixtures/goldens and concise reports.
- Stop and escalate on any byte difference, nondeterminism, hidden display
  dependency or required scope change; never bless a difference or label
  missing native evidence PASS.

## Pinned versions

SDL3 3.4.18, ENet 1.3.18 and the MinGW-w64 cross toolchain (with URLs and
SHA-256s) are frozen in `P2.1-REPORT.md`; P3.1 re-verified every artifact hash
against those pins (`P3.1-REPORT.md` §4). No dependency substitution without a
new owner decision.

## Known harness caveats (recorded by P3.1, not fixed — harness is read-only)

- `make phase2-check PHASE2_WITH_PHASE1=1` run **through `make`** hits a
  pre-existing `Tests/phase1/check-x-boundary.sh --build` parsing bug: the
  nested `make` auto-enables `-w`, and its `make -C Src -s print-server-srcs`
  output gains `make[2]: Entering directory …` lines that the source-list parse
  treats as file names. Invoke with `--no-print-directory` (or run
  `bash Tests/phase2/phase2-check.sh --with-phase1` directly) to avoid it.
- `Tests/phase2/transport.sh` `cmp`s the full fixture stdout, which includes a
  wall-clock `paused-reader: 900 services in N ms` line; the two repeats
  occasionally disagree by 1 ms under load and the driver fails despite all
  assertions passing. This is a diagnostic-only flake.
