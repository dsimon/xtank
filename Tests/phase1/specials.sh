#!/usr/bin/env bash
#
# specials.sh -- P1.11 knowledge-versus-draw oracle for the xtank Phase 1
# extraction.
#
# Exercises the specials with ZERO presentation (the display-free null
# backend with presentation disabled), using the deterministic bot-free
# trace, and proves:
#
#   * mapper exploration and radar contact acquisition/loss still advance the
#     model (MPH/MP/RNH/RB records and the dispatch counters);
#   * death/respawn/score/win/reset still happen;
#   * every forced draw callback is ELIDED: no draw action reaches a special
#     proc (draw=0) while suppressed>0 under zero presentation, and the same
#     run with presentation enabled does deliver draws (control);
#   * the release/MVP special preset disables New Radar, Tac Link, Camo and
#     Rdf (no NRH records) while Mapper and ordinary Radar remain, and the
#     oracle control still carries New Radar.
#
# It opens no X connection and needs no Xvfb; X presentation itself stays
# covered by the mandatory G (regression.sh --subject x).
#
#   bash Tests/phase1/specials.sh [--evidence DIR]
#
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HARNESS="$REPO/Tests/phase1"
EVIDENCE="${XTANK_P1_EVIDENCE:-/tmp/opencode/xtank-phase1/p1.11}"
WORK="$EVIDENCE/specials"
SCEN=trace-quadrant
TICKLIMIT=400

while [ $# -gt 0 ]; do
	case "$1" in
	  --evidence) EVIDENCE="$2"; shift 2 ;;
	  *) echo "usage: specials.sh [--evidence DIR]" >&2; exit 2 ;;
	esac
done

mkdir -p "$WORK"
SETTINGS="$HARNESS/scenarios/$SCEN.settings"
TRACE="$HARNESS/scenarios/$SCEN.trace"

log()  { printf '%s\n' "$*"; }
fail() { printf 'SPECIALS FAIL: %s\n' "$*" >&2; exit 1; }

[ -f "$SETTINGS" ] || fail "missing settings $SETTINGS"
[ -f "$TRACE" ]    || fail "missing trace $TRACE"

# ---------------------------------------------------------------------------
# 1. Build the display-free (null) candidate with the STATE_DUMP oracle.
# ---------------------------------------------------------------------------
bash "$HARNESS/build-reference.sh" --mode candidate --subject null \
    --stage after --evidence "$EVIDENCE" \
    > "$WORK/build-candidate.out" 2>&1 || fail "candidate build failed (see $WORK/build-candidate.out)"
CAND="$EVIDENCE/candidate-after-null/src"
BIN="$CAND/Src/xtank-headless"
[ -x "$BIN" ] || fail "candidate binary missing: $BIN"

# 2. Build the v2 dump analyzer (event assertions).
ANALYZER="$WORK/state_dump"
gcc -O2 -Wall -o "$ANALYZER" "$HARNESS/state_dump.c" -lm \
    || fail "could not build state_dump analyzer"

# ---------------------------------------------------------------------------
# 3. Run helper.  $1 = tag, $2 = extra env assignments ("" for none),
#    $3 = dump path.
# ---------------------------------------------------------------------------
run_scenario() {   # tag  extra_env  dump
	local tag="$1" extra="$2" dump="$3"
	local report="$WORK/$tag.stats"
	rm -f "$report"
	( cd "$CAND/Src" && \
	  env -u DISPLAY -u LD_PRELOAD LC_ALL=C \
	      XTANK_DIR="$CAND" XTANK_SEED=114336 \
	      XTANK_DUMP="$dump" XTANK_DUMP_PERIOD=1 XTANK_DUMP_FORMAT=2 \
	      XTANK_TICK_LIMIT="$TICKLIMIT" XTANK_TRACE="$TRACE" \
	      XTANK_SPECIALS_PROBE=1 XTANK_SPECIALS_REPORT="$report" \
	      $extra \
	      ./xtank-headless -S -D -P -X -F "$SETTINGS" \
	      -p playerA -p playerB nullterm0 nullterm1 ) \
	    > "$WORK/$tag.stdout" 2> "$WORK/$tag.stderr" || \
	    fail "$tag: run failed (see $WORK/$tag.stderr)"
	[ -s "$report" ] || fail "$tag: no SPECIALS-STATS report"
}

stat() {   # report-file  key
	grep -o "$2=[0-9]*" "$1" | head -1 | cut -d= -f2
}

require_gt() {   # tag  key  value
	[ -n "$3" ] && [ "$3" -gt 0 ] 2>/dev/null || \
	    fail "$1: expected $2 > 0, got '${3:-none}'"
}
require_eq() {   # tag  key  value  want
	[ "$3" = "$4" ] || fail "$1: expected $2 = $4, got '${3:-none}'"
}

# ---------------------------------------------------------------------------
# 4. Oracle control -- zero presentation, all specials (no MVP preset).
# ---------------------------------------------------------------------------
log "== zero-presentation run (oracle control: New Radar/Tac Link/Camo/Rdf present)"
run_scenario oracle "XTANK_NO_PRESENTATION=1" "$WORK/oracle.v2.state"
R="$WORK/oracle.stats"
require_eq oracle presentation "$(stat "$R" presentation)" 0
require_eq oracle draw         "$(stat "$R" draw)"         0
require_gt oracle suppressed   "$(stat "$R" suppressed)"
require_gt oracle model        "$(stat "$R" model)"
require_gt oracle mapper       "$(stat "$R" mapper)"
require_gt oracle radar_acquired "$(stat "$R" radar_acquired)"
require_gt oracle radar_expired  "$(stat "$R" radar_expired)"
grep -q '^MPH ' "$WORK/oracle.v2.state" || fail "oracle: no mapper knowledge (MPH)"
grep -q '^RNH ' "$WORK/oracle.v2.state" || fail "oracle: no radar knowledge (RNH)"
grep -q '^RB '  "$WORK/oracle.v2.state" || fail "oracle: no radar contacts (RB)"
grep -q '^NRH ' "$WORK/oracle.v2.state" || fail "oracle control lost New Radar (NRH)"
log "   report: $(cat "$R")"

# 5. Life-cycle events (exploration/contact/death/respawn/score/win/reset).
log "== event assertions (wall/tank/bullet/death/respawn/score/win/reset)"
"$ANALYZER" --file "$WORK/oracle.v2.state" --winning-score 2000 \
    --expect "wall,tank,bullet,directional,death,respawn,score,win,reset" \
    > "$WORK/oracle.assert" 2>&1 || fail "event assertions failed (see $WORK/oracle.assert)"
sed -n '1,12p' "$WORK/oracle.assert" | while IFS= read -r l; do log "   $l"; done
grep -q '^assertion ok$' "$WORK/oracle.assert" || fail "state_dump assertion not ok"

# ---------------------------------------------------------------------------
# 6. Presentation ON control: the same setup must deliver draw callbacks
#    (proves the gate, not a missing caller, is what elides them).
# ---------------------------------------------------------------------------
log "== presentation-on control (draw callbacks must be delivered)"
run_scenario present "" "$WORK/present.v2.state"
P="$WORK/present.stats"
require_eq present presentation "$(stat "$P" presentation)" 1
require_gt present draw         "$(stat "$P" draw)"
log "   report: $(cat "$P")"

# ---------------------------------------------------------------------------
# 7. Release/MVP preset: New Radar/Tac Link/Camo/Rdf disabled, Mapper and
#    ordinary Radar retained; still zero draw callbacks.
# ---------------------------------------------------------------------------
log "== MVP preset run (New Radar/Tac Link/Camo/Rdf disabled)"
run_scenario mvp "XTANK_NO_PRESENTATION=1 XTANK_MVP_PRESET=1" "$WORK/mvp.v2.state"
M="$WORK/mvp.stats"
require_eq mvp presentation "$(stat "$M" presentation)" 0
require_eq mvp draw         "$(stat "$M" draw)"         0
require_gt mvp suppressed   "$(stat "$M" suppressed)"
grep -q '^MPH ' "$WORK/mvp.v2.state" || fail "mvp: Mapper should remain"
grep -q '^RNH ' "$WORK/mvp.v2.state" || fail "mvp: ordinary Radar should remain"
if grep -q '^NRH ' "$WORK/mvp.v2.state"; then
	fail "mvp: New Radar should be disabled"
fi
log "   report: $(cat "$M")"

log ""
log "SPECIALS PASS (zero-presentation knowledge preserved; draw elided; MVP preset separated)"
