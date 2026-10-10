#!/usr/bin/env bash
#
# lifecycle.sh -- P1.6 lifecycle oracle for the xtank Phase 1 extraction.
#
# Exercises the explicit server-core match lifecycle (server_match_init ->
# server_step* -> server_match_reset -> ... -> server_destroy) for one match
# twice in a single process, over the retained single-world globals, and
# verifies that:
#
#   * the first epoch is byte-identical to a normal single-match run (which
#     the full regression proves is byte-identical to the frozen X reference);
#   * repeated lifecycle runs of the same scenario+seed are byte-identical;
#   * the reset leaves no stale live/dead vehicles or lowlib current-vehicle
#     (cv) pointer behind (the server_destroy report line).
#
# The lifecycle mode is only active in the STATE_DUMP oracle build and only
# when XTANK_LIFECYCLE is set, so it never affects a normal run.
#
# This is a null-subject (display-free) test: it opens no X connection and
# needs no Xvfb.
#
#   bash Tests/phase1/lifecycle.sh [--evidence DIR] [--scenario NAME]
#
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HARNESS="$REPO/Tests/phase1"
EVIDENCE="${XTANK_P1_EVIDENCE:-/tmp/opencode/xtank-phase1/p1.6}"
SCEN="trace-arena-win"
EXTRA=1		# extra matches beyond the first (so two epochs)

while [ $# -gt 0 ]; do
	case "$1" in
	  --evidence) EVIDENCE="$2"; shift 2 ;;
	  --scenario) SCEN="$2"; shift 2 ;;
	  *) echo "usage: lifecycle.sh [--evidence DIR] [--scenario NAME]" >&2;
	     exit 2 ;;
	esac
done

mkdir -p "$EVIDENCE/lifecycle"
WORK="$EVIDENCE/lifecycle"
SETTINGS="$HARNESS/scenarios/$SCEN.settings"
TRACE="$HARNESS/scenarios/$SCEN.trace"

log()  { printf '%s\n' "$*"; }
fail() { printf 'LIFECYCLE FAIL: %s\n' "$*" >&2; exit 1; }

[ -f "$SETTINGS" ] || fail "missing settings $SETTINGS"
[ -f "$TRACE" ]    || fail "missing trace $TRACE"

# 1. Build the null candidate (display-free, STATE_DUMP oracle).
bash "$HARNESS/build-reference.sh" --mode candidate --subject null \
    --stage after --evidence "$EVIDENCE" \
    > "$WORK/build-candidate.out" 2>&1 || fail "candidate build failed"
CAND="$EVIDENCE/candidate-after-null/src"
BIN="$CAND/Src/xtank-headless"
[ -x "$BIN" ] || fail "candidate binary missing: $BIN"

if [ ! -f "$EVIDENCE/fixseed.so" ] || \
   [ "$HARNESS/fixseed.c" -nt "$EVIDENCE/fixseed.so" ]; then
	gcc -shared -fPIC -O2 -o "$EVIDENCE/fixseed.so" "$HARNESS/fixseed.c"
fi

# 2. Run one epoch (normal), then two epochs (lifecycle).  No tick limit:
#    both scenarios end naturally under this trace, so epoch 0 is not a
#    truncated prefix.
run() {   # $1 out  $2 lifecycle-extra ("" = normal)
	local out="$1" extra="$2"
	local -a lc=()
	if [ -n "$extra" ]; then lc=(XTANK_LIFECYCLE="$extra"); fi
	( cd "$CAND/Src" && \
	  env -u DISPLAY -u XTANK_LIFECYCLE LC_ALL=C \
	      XTANK_DIR="$CAND" XTANK_SEED=114336 \
	      XTANK_DUMP="$out" XTANK_DUMP_PERIOD=1 XTANK_DUMP_FORMAT=2 \
	      XTANK_TRACE="$TRACE" "${lc[@]}" \
	      LD_PRELOAD="$EVIDENCE/fixseed.so" \
	      timeout 900 ./xtank-headless -S -D -P -X -F "$SETTINGS" \
	      -p playerA -p playerB nullterm0 nullterm1 ) \
	    > "$out.stdout" 2>&1
}

log "== P1.6 lifecycle: scenario=$SCEN epochs=$((EXTRA + 1))"
run "$WORK/normal.state" ""
[ -s "$WORK/normal.state" ] || fail "normal run produced no dump"

for r in 1 2 3; do
	run "$WORK/life.$r.state" "$EXTRA"
	[ -s "$WORK/life.$r.state" ] || fail "lifecycle run $r produced no dump"
done
cp -f "$WORK/life.1.state" "$WORK/life.state"
cp -f "$WORK/life.1.state.stdout" "$WORK/life.state.stdout"

# 3. Repeated lifecycle runs must be identical.
cmp "$WORK/life.1.state" "$WORK/life.2.state" >/dev/null \
    || fail "lifecycle runs 1 and 2 differ"
cmp "$WORK/life.1.state" "$WORK/life.3.state" >/dev/null \
    || fail "lifecycle runs 1 and 3 differ"
log "    repeated lifecycle runs: identical"

# 4. Each epoch must equal the normal single-match run.  The lifecycle oracle
#    reseeds every epoch, so both epochs replay the frozen-X stream; the epoch
#    field itself is normalised away before comparison.
epoch0() { awk '/^F2 / { ep=$4 } ep==0 { print }' "$1"; }
epoch1() { awk '/^F2 / { ep=$4 } ep==1 { print }' "$1"; }
normalise() { sed -E 's/^(F2 [0-9]+) epoch [0-9]+ /\1 epoch 0 /'; }
epochs() { awk '/^F2 / { print $4 }' "$1" | sort -u | tr '\n' ' '; }

epoch0 "$WORK/life.state" | normalise > "$WORK/life.epoch0"
epoch1 "$WORK/life.state" | normalise > "$WORK/life.epoch1"
normalise < "$WORK/normal.state" > "$WORK/normal.epoch0"
cmp "$WORK/life.epoch0" "$WORK/normal.epoch0" >/dev/null \
    || fail "lifecycle epoch 0 differs from the normal single-match run"
cmp "$WORK/life.epoch1" "$WORK/normal.epoch0" >/dev/null \
    || fail "lifecycle epoch 1 differs from the normal single-match run"
log "    epoch 0 == epoch 1 == normal single-match run"

got_epochs=$(epochs "$WORK/life.state")
[ "$got_epochs" = "0 1 " ] || fail "expected 2 epochs, got: $got_epochs"
log "    epochs present: $got_epochs"

# 5. If the full regression already produced the frozen X v2 dump, confirm
#    the normal baseline is byte-identical to it (transitive X equality).
XD="$EVIDENCE/runs/$SCEN.v2.x.state"
if [ -f "$XD" ]; then
	cmp "$XD" "$WORK/normal.state" >/dev/null \
	    || fail "normal baseline differs from frozen X reference"
	log "    normal baseline == frozen X reference"
fi

# 6. Reset cleanliness: no stale live/dead vehicles, lowlib cv, or terminal
#    vehicle ownership left behind.
grep -q 'SERVER-LIFECYCLE epochs=2 alive=0 dead=0 cv=null term=null' \
    "$WORK/life.state.stdout" \
    || fail "reset did not leave a clean state (see life.state.stdout)"
log "    reset clean: $(grep -o 'SERVER-LIFECYCLE.*' "$WORK/life.state.stdout")"

log ""
log "P1.6 LIFECYCLE PASS (scenario=$SCEN, 2 epochs, display-free)"
