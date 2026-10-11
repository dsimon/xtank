# Phase 2 test harness

Harness for the Phase 2 Windows-client / single-client vertical slice
(`docs/PHASE2-PLAN.md`, approved 2026-10-11). It complements the frozen Phase 1
oracle in `Tests/phase1/`; it does not replace or weaken it.

## Authority and rules

- `docs/PHASE2-PLAN.md` is the task contract; `docs/PHASE2-DECISIONS.md` records
  the approved D2-A–D2-F rulings (SDL3, ENet-over-UDP, binary protocol v1,
  MinGW-w64 cross-build, 15 Hz/one-tick interpolation, one-peer lobby with a
  2-second solo abort).
- **No gameplay rule changes.** The simulation must stay byte-identical to the
  frozen pinned X reference (`XTANK_SEED=114336`, `LC_ALL=C`).
- **G2 before/after every task**: G2 = the Phase 1 exact-simulation guardrail G
  plus the task's Phase 2 boundary checks. Require `cmp` byte identity between
  the frozen reference, the pre-change baseline and the post-change candidate
  (plus three candidate repeats); never tolerances, hash-only assertions or
  relaxed goldens.

## Layout (proposed in the plan; created as tasks land)

New Phase 2 paths are proposals until their task completes. Each task in
`docs/PHASE2-PLAN.md` §4 names the exact files it adds.

| Area | Paths |
| --- | --- |
| Wire schema / codec | `Protocol/{wire.h,limits.h,encode.c,decode.c,content-manifest.*}`, fixtures under `Tests/phase2/fixtures/protocol/` |
| Human lifecycle | `Src/{server_core,server_main,setup,vehicle}.c`, `Src/server_players.c`, `Tests/phase2/{players.sh,players_fixture.c}` |
| View / events | `Src/{server_view,server_events}.c`, `Tests/phase2/{view.sh,events.sh,*_fixture.c}` |
| Transport / admission / session | `Transport/enet_transport.c`, `Src/{server_net,server_session}.c`, `Tests/phase2/{transport.sh,session.sh,admission_fixture.c,session_fixture.c}` |
| Windows client | `Client/`, root `CMakeLists.txt`, `cmake/mingw-w64.cmake`, `Tests/phase2/windows-smoke.ps1` |
| Client model / UI | `Client/{model,timeline,camera,input,controls,hud,map,radar,console,connect_ui,lobby_ui,results_ui}.*`, matching shell fixtures |
| Integration / parity / packaging | `Tests/phase2/{regression.sh,network_replay.c,reference_view.c,timing.sh,backpressure.sh,slice.sh,scripted_peer.c,package.sh,package.ps1}`, `docs/{PROTOCOL-v1.md,VIEW-v1.md,PHASE2-RUN.md}` |

`Tests/phase2/` mirrors `Tests/phase1/` conventions: POSIX shell drivers named
`<area>.sh`, small C fixtures named `<area>_fixture.c` (or `*_fixture.c`), and
tiny committed golden/fixture data. `Tests/phase2/fixtures/` holds committed
wire bytes and input/expected-field fixtures.

## Running

The Phase 1 exit gate is unchanged and must keep passing:

```sh
make phase1-check
```

Phase 2 network/codec/session checks are added per task; a consolidated
`make phase2-check` (full Phase 1 gate plus codec, players, view, events,
admission/session, network replay, model/input and package checks) is proposed
for P2.16.

### G2 guardrail

```sh
export LC_ALL=C XTANK_SEED=114336
export XTANK_P1_EVIDENCE=/tmp/opencode/xtank-phase2/p2.<N>/oracle
bash Tests/phase1/regression.sh --suite full --stage before --subject auto
# implement the task
bash Tests/phase1/regression.sh --suite full --stage after --subject auto
```

`--subject auto` resolves to the display-free `null` backend. Note: `--stage
before` snapshots `git archive HEAD`. When Phase 1 work is still uncommitted
(the expected starting state — `docs/PHASE2-PLAN.md` §1), HEAD predates the
display-free core and cannot build `-DNULL_BACKEND`; in that case take the
pre-change baseline with `--stage after` on the unmodified working tree and
`cmp` the resulting `*.after.state` dumps across the change. `make phase1-check`
always evaluates the working-tree candidate (`--stage after`).

## Evidence discipline

- **Keep dumps, logs, binaries, reference worktrees, packages and generated
  flex/version/object files out of Git.** Evidence lives under
  `/tmp/opencode/xtank-phase2/p2.<N>/` (see `${XTANK_P1_EVIDENCE}`); large trees
  may symlink to `/home/hatch/workspace/coding/xtank-evidence/`.
- Commit only harness source, tiny fixtures/goldens and concise reports.
- Preserve the `Tests/phase1/` frozen v1 goldens and v2 goldens permanently.
- Stop and escalate on any byte difference, nondeterminism, hidden display
  dependency or required scope change; never bless a difference.

## Pinned versions

SDL3, ENet and MinGW-w64 build/dependency versions (with URLs and hashes) are
frozen in `P2.1-REPORT.md`; no dependency substitution without a new owner
decision.
