#!/usr/bin/env bash
#
# events.sh -- P2.6 entity-lifetime / event / results acceptance.
#
# Builds the shipped display-free server core, links the P2.6 events fixture
# against it (Src/sobj minus the server entry point) plus the frozen P2.3 wire
# codec, and runs:
#
#   1. the fixture, which drives one terminal-free human plus one drone through
#      two match epochs and asserts: generation-safe identities over respawn and
#      forced bullet/explosion slot reuse; DEATH/RESPAWN/SCORE/EXPLOSION/
#      MATCH_END events carry the right key; the event sequence restarts each
#      epoch; RESULTS are captured before the score reset; duplicate/reordered
#      event delivery does not double score; and 0/1/5 observations leave the
#      simulation state byte-identical;
#   2. a static read-only source scan of Src/server_events.c (no draw/special/
#      RNG/control/step call may appear);
#   3. a determinism repeat: two fixture runs are byte-identical.
#
# All generated binaries/logs live OUTSIDE the checkout.
#
#   bash Tests/phase2/events.sh [--evidence DIR]
#
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HARNESS="$REPO/Tests/phase2"
PROTO="$REPO/Protocol"
EVIDENCE="${XTANK_P2_EVIDENCE:-/tmp/opencode/xtank-phase2/p2.6/events}"

while [ $# -gt 0 ]; do
	case "$1" in
	  --evidence) EVIDENCE="$2"; shift 2 ;;
	  *) echo "usage: events.sh [--evidence DIR]" >&2; exit 2 ;;
	esac
done

mkdir -p "$EVIDENCE"
WORK="$EVIDENCE/work"
mkdir -p "$WORK"

log()  { printf '%s\n' "$*"; }
fail() { printf 'EVENTS FAIL: %s\n' "$*" >&2; exit 1; }

# Same feature defines as the shipped server build (Src/GNUmakefile SCFLAGS).
DEFS="-DUNIX -DS1024x864 -DKEYPAD_DETECT -DSTINGY_REDRAW -DBATCH_LINES \
-DBATCH_POINTS -DBATCH_COLOR_LINES -DCODE_SIZE=0x20000 -DSTACK_SIZE=0x8000 \
-DNEW_LIMITS -DSLOWER -DMULTI_SYNC -DTHREAD_SWAPCONTEXT -DNULL_BACKEND \
-DSERVER_ONLY"

log "== building the shipped server core (make phase1-server phase1-core)"
( cd "$REPO" && make phase1-server phase1-core ) > "$WORK/build.out" 2>&1 || \
	fail "server/core build failed (see $WORK/build.out)"

[ -d "$REPO/Src/sobj" ] || fail "server object tree missing"
[ -f "$REPO/Src/sobj/server_events.o" ] || fail "server_events.o not built"
[ -f "$REPO/Src/sobj/server_view.o" ] || fail "server_view.o not built"

# Core objects without the server entry point (the fixture provides main()).
# shellcheck disable=SC2046
CORE_OBJS=$(ls "$REPO"/Src/sobj/*.o | grep -v '/server_main.o$')
[ -n "$CORE_OBJS" ] || fail "no core objects"

log "== static read-only scan of Src/server_events.c"
FORBID='\b(do_special|update_specials|update_vehicle|kill_vehicle|make_bullet|make_explosion|explode_location|server_step|animate|intent_[a-z_]+|play_[a-z_]+|draw_[a-z_]+|rnd|rand|random)[[:space:]]*\('
# Strip comments first so prose is not a hit.
sed -e 's://.*::' -e 's:/\*.*\*/::' "$REPO/Src/server_events.c" \
	| grep -v '^[[:space:]]*\*' > "$WORK/scan.c"
if grep -nE "$FORBID" "$WORK/scan.c" > "$WORK/scan.out" 2>&1; then
	cat "$WORK/scan.out" >&2
	fail "server_events.c invokes a draw/special/RNG/control/step function"
fi
log "   Src/server_events.c calls no draw/special/RNG/control/step function"

log "== compiling the wire codec and the events fixture against the server core"
gcc -O0 -g -w -I"$PROTO" -c "$PROTO/encode.c" -o "$WORK/encode.o" \
	> "$WORK/encode-build.out" 2>&1 || \
	fail "codec build failed (see $WORK/encode-build.out)"
gcc -O0 -g -w -I"$PROTO" -c "$PROTO/decode.c" -o "$WORK/decode.o" \
	>> "$WORK/encode-build.out" 2>&1 || \
	fail "codec build failed (see $WORK/encode-build.out)"

# shellcheck disable=SC2086
gcc -O0 -g -w $DEFS -I"$REPO/Src" -I"$REPO/Src/Include" -iquote"$PROTO" \
	-DXTANK_DIR="\"$REPO/Src/..\"" -DALLDEFINES="\"p2.6 events fixture\"" \
	-o "$WORK/events_fixture" "$HARNESS/events_fixture.c" $CORE_OBJS \
	"$WORK/encode.o" "$WORK/decode.o" -lm \
	> "$WORK/fixture-build.out" 2>&1 || \
	fail "fixture build failed (see $WORK/fixture-build.out)"

log "== running the events fixture"
if ! "$WORK/events_fixture" "$REPO" > "$WORK/fixture1.out" 2>&1; then
	cat "$WORK/fixture1.out" >&2
	fail "events fixture failed"
fi
grep -q 'EVENTS FIXTURE PASS' "$WORK/fixture1.out" || \
	fail "fixture did not report PASS"
grep -q 'results before reset' "$WORK/fixture1.out" || \
	fail "fixture did not assert results before reset"
log "    $(grep -m1 'EVENTS FIXTURE PASS' "$WORK/fixture1.out")"

log "== determinism: repeat the fixture"
"$WORK/events_fixture" "$REPO" > "$WORK/fixture2.out" 2>&1 || \
	fail "fixture repeat failed"
cmp "$WORK/fixture1.out" "$WORK/fixture2.out" >/dev/null || \
	fail "fixture repeats differ"
log "    two fixture runs identical"

log ""
log "P2.6 EVENTS PASS (generation-safe lifetimes, DEATH/RESPAWN/SCORE/"
log "              EXPLOSION/MATCH_END, results before reset, epoch scoping,"
log "              dedup/reorder safety, read-only 0/1/5-observation parity)"
