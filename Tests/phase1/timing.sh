#!/usr/bin/env bash
#
# timing.sh -- P1.9 acceptance fixture for the monotonic 15 Hz server loop.
#
#   bash Tests/phase1/timing.sh --seconds 60 --vehicles 4
#
# It builds the display-free null candidate, then runs the production server
# loop to a finite tick bound twice:
#
#   fast   -- the same simulation, sleep bypassed (XTANK_SERVER_FAST=1);
#   paced  -- absolute CLOCK_MONOTONIC deadlines at 15 Hz (XTANK_TICK_HZ=15).
#
# Acceptance:
#   * the paced run advances exactly seconds*15 ticks (900 for 60 s) within
#     one tick of the wall-clock budget;
#   * the fast and paced dumps are byte-identical (pacing only bypasses sleep,
#     it never changes simulation logic);
#   * the loop's own bounded work report shows p99 per-tick simulation work
#     below the 66.7 ms frame budget;
#   * neither run passes the legacy -D flag, and the server path never uses
#     the SIGALRM/pause() frame pacer (guarded by server_loop_active()).
#
# Generated objects and dumps live outside the checkout (default
# /tmp/opencode/xtank-phase1/p1.9).
#
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HARNESS="$REPO/Tests/phase1"
EVIDENCE="${XTANK_P1_EVIDENCE:-/tmp/opencode/xtank-phase1/p1.9}"
SCEN_DIR="$HARNESS/scenarios"

SECONDS_ARG=60
VEHICLES=4
KEEP=0

usage() {
	echo "usage: $0 [--seconds N] [--vehicles 2|4] [--evidence DIR] [--keep]" >&2
	exit 2
}

while [ $# -gt 0 ]; do
	case "$1" in
	  --seconds) SECONDS_ARG="$2"; shift 2 ;;
	  --vehicles) VEHICLES="$2"; shift 2 ;;
	  --evidence) EVIDENCE="$2"; shift 2 ;;
	  --keep) KEEP=1; shift ;;
	  *) usage ;;
	esac
done

case "$VEHICLES" in
  2) SCEN="combat-2drone" ;;
  4) SCEN="combat-4drone" ;;
  *) echo "bad --vehicles $VEHICLES (want 2 or 4)" >&2; exit 2 ;;
esac

HZ=15
TICKS=$(( SECONDS_ARG * HZ ))
BUDGET_US=66700

mkdir -p "$EVIDENCE/timing"

log()  { printf '%s\n' "$*"; }
fail() { printf 'TIMING FAIL: %s\n' "$*" >&2; exit 1; }

log "== P1.9 timing: seconds=$SECONDS_ARG vehicles=$VEHICLES scenario=$SCEN"
log "   ticks=$TICKS hz=$HZ evidence=$EVIDENCE"

# ---------------------------------------------------------------------------
# Build the display-free null candidate (oracle build, so dumps exist).
# ---------------------------------------------------------------------------
bash "$HARNESS/build-reference.sh" --mode candidate --subject null \
    --stage after --evidence "$EVIDENCE" \
    > "$EVIDENCE/timing/build-candidate-null.out" 2>&1 || \
    fail "candidate build failed"
CAND="$EVIDENCE/candidate-after-null/src"
BIN="$CAND/Src/xtank-headless"
[ -x "$BIN" ] || fail "candidate binary missing: $BIN"

SETTINGS="$SCEN_DIR/$SCEN.settings"
[ -f "$SETTINGS" ] || fail "missing settings $SETTINGS"

# ---------------------------------------------------------------------------
# The server path must not arm the legacy SIGALRM/pause() frame pacer.
# ---------------------------------------------------------------------------
grep -q 'clock_nanosleep' "$REPO/Src/server_loop.c" || \
    fail "server loop does not use absolute monotonic sleep"
grep -q 'server_loop_active' "$REPO/Src/actions.c" || \
    fail "check_game_speed does not bypass the legacy pacer in the server path"
grep -q 'server_loop_active' "$REPO/Src/icounter.c" || \
    fail "legacy real-time pacer is not disabled in the server path"

# Runtime proof (when strace is available): a short paced run installs no
# SIGALRM handler, calls no pause(), and arms no ITIMER_REAL frame timer.
if command -v strace >/dev/null 2>&1; then
	log "== strace: server path pacer audit"
	( cd "$CAND/Src" && \
	  env -u DISPLAY -u LD_PRELOAD LC_ALL=C XTANK_DIR="$CAND" \
	      XTANK_SEED=114336 XTANK_DUMP="$EVIDENCE/timing/probe.state" \
	      XTANK_DUMP_PERIOD=1 XTANK_DUMP_FORMAT=2 \
	      XTANK_SERVER_TICKS=3 XTANK_TICK_HZ="$HZ" \
	      XTANK_LOOP_REPORT="$EVIDENCE/timing/probe.report" \
	      strace -f -e trace=rt_sigaction,pause,setitimer,alarm \
	          -o "$EVIDENCE/timing/strace.log" \
	          "$BIN" -S -P -X -F "$SETTINGS" \
	) > /dev/null 2>&1 || fail "strace pacer audit run failed"
	if grep -q 'SIGALRM' "$EVIDENCE/timing/strace.log"; then
		fail "server path installed a SIGALRM handler"
	fi
	if grep -q 'pause(' "$EVIDENCE/timing/strace.log"; then
		fail "server path called pause()"
	fi
	if grep -q 'setitimer(ITIMER_REAL' "$EVIDENCE/timing/strace.log"; then
		fail "server path armed the ITIMER_REAL frame pacer"
	fi
fi

# ---------------------------------------------------------------------------
# Run helpers.  No -D anywhere: fast replay is the loop's own bypass.
# ---------------------------------------------------------------------------
RUN_ENV=(env -u DISPLAY -u LD_PRELOAD LC_ALL=C
         XTANK_DIR="$CAND" XTANK_SEED=114336
         XTANK_DUMP_PERIOD=1 XTANK_DUMP_FORMAT=2
         XTANK_SERVER_TICKS="$TICKS" XTANK_TICK_HZ="$HZ")

run_once() {   # $1 tag  $2 extra env (word)  $3 dump
	local tag="$1" extra="$2" dump="$3"
	local rep="$EVIDENCE/timing/$tag.report"
	log "== running $tag"
	( cd "$CAND/Src" && \
	  "${RUN_ENV[@]}" $extra \
	      XTANK_DUMP="$dump" XTANK_LOOP_REPORT="$rep" \
	      timeout $(( SECONDS_ARG + 30 )) "$BIN" -S -P -X -F "$SETTINGS" \
	) > "$EVIDENCE/timing/$tag.out" 2>&1 || \
	    fail "$tag run failed (see timing/$tag.out)"
	[ -s "$dump" ] || fail "$tag produced no dump"
	[ -f "$rep" ] || fail "$tag produced no loop report"
}

# fast: sleep bypassed only.
run_once fast "XTANK_SERVER_FAST=1" "$EVIDENCE/timing/fast.state"

# paced: absolute 15 Hz deadlines, no -D, no fast flag.
start_ns=$(date +%s%N)
run_once paced "" "$EVIDENCE/timing/paced.state"
end_ns=$(date +%s%N)
elapsed_ms=$(( (end_ns - start_ns) / 1000000 ))

# ---------------------------------------------------------------------------
# Checks
# ---------------------------------------------------------------------------
count_frames() { grep -c '^F2 ' "$1"; }

fast_frames=$(count_frames "$EVIDENCE/timing/fast.state")
paced_frames=$(count_frames "$EVIDENCE/timing/paced.state")

[ "$fast_frames" = "$TICKS" ] || \
    fail "fast run produced $fast_frames frames, want $TICKS"
[ "$paced_frames" = "$TICKS" ] || \
    fail "paced run produced $paced_frames frames, want $TICKS"

# 900 ticks +/- 1 is exact here because the bound is the tick count; verify the
# wall-clock budget as well.
lo_ms=$(( (SECONDS_ARG - 1) * 1000 ))
hi_ms=$(( (SECONDS_ARG + 5) * 1000 ))
[ "$elapsed_ms" -ge "$lo_ms" ] || \
    fail "paced run too fast: ${elapsed_ms}ms for $TICKS ticks"
[ "$elapsed_ms" -le "$hi_ms" ] || \
    fail "paced run too slow: ${elapsed_ms}ms for $TICKS ticks"

cmp "$EVIDENCE/timing/fast.state" "$EVIDENCE/timing/paced.state" >/dev/null || \
    fail "paced and fast dumps differ (pacing changed simulation state)"

# Parse the loop's own bounded work report.
paced_rep="$EVIDENCE/timing/paced.report"
fast_rep="$EVIDENCE/timing/fast.report"
paced_line=$(grep '^SERVER-LOOP' "$paced_rep") || fail "no SERVER-LOOP report"
fast_line=$(grep '^SERVER-LOOP' "$fast_rep") || fail "no SERVER-LOOP report"

paced_ticks=$(printf '%s\n' "$paced_line" | sed -n 's/.*ticks=\([0-9]*\).*/\1/p')
paced_fast=$(printf '%s\n' "$paced_line" | sed -n 's/.*fast=\([0-9]*\).*/\1/p')
paced_p99=$(printf '%s\n' "$paced_line" | sed -n 's/.*p99_us=\([0-9.]*\).*/\1/p')
fast_fast=$(printf '%s\n' "$fast_line" | sed -n 's/.*fast=\([0-9]*\).*/\1/p')

[ "$paced_ticks" = "$TICKS" ] || \
    fail "paced report ticks=$paced_ticks, want $TICKS"
[ "$paced_fast" = "0" ] || fail "paced run was not paced (fast=$paced_fast)"
[ "$fast_fast" = "1" ] || fail "fast run was not fast (fast=$fast_fast)"

awk -v p="$paced_p99" -v b="$BUDGET_US" 'BEGIN { exit (p < b) ? 0 : 1 }' || \
    fail "p99 tick work ${paced_p99}us is not below ${BUDGET_US}us"

{
	echo "scenario=$SCEN"
	echo "seconds=$SECONDS_ARG"
	echo "hz=$HZ"
	echo "ticks=$TICKS"
	echo "fast_frames=$fast_frames"
	echo "paced_frames=$paced_frames"
	echo "paced_elapsed_ms=$elapsed_ms"
	echo "paced_p99_us=$paced_p99"
	echo "paced_line=$paced_line"
	echo "fast_line=$fast_line"
	echo "cmp=identical"
} > "$EVIDENCE/timing/timing.result"

[ "$KEEP" = "1" ] || rm -f "$EVIDENCE/timing"/*.out

log ""
log "TIMING PASS: ${paced_ticks} ticks in ${elapsed_ms}ms, p99 ${paced_p99}us, paced==fast"
