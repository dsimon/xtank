#!/usr/bin/env bash
#
# view.sh -- P3.8 per-player view isolation and authorization acceptance.
#
# Builds the shipped display-free server core (including the P3.8
# Src/server_view.c/Src/server_events.c exporters), links the P3.8 view fixture
# against it (Src/sobj minus the server entry point) plus the frozen v1.1 wire
# codec, and:
#
#   1. runs the fixture, which drives two independent human recipients and
#      independently checks recipient bytes: separated cameras, private HUD/
#      mapper/radar, export order A->B and B->A byte identity, 0/1/5-export
#      simulation parity, full-width u32 generations (>255 not truncated),
#      binding/entity generation independence, stale-generation rejection (no
#      arbitrary-camera query) and no full-map leak;
#   2. statically scans Src/server_view.c for draw/special/RNG/control calls;
#   3. runs the fixture three times and requires byte identity (three candidate
#      repeats).
#
# All generated binaries/logs live OUTSIDE the checkout.
#
#   bash Tests/phase3/view.sh [--evidence DIR]
#
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HARNESS="$REPO/Tests/phase3"
PROTO="$REPO/Protocol"
EVIDENCE="${XTANK_P3_EVIDENCE:-/tmp/opencode/xtank-phase3/p3.8/view}"

while [ $# -gt 0 ]; do
	case "$1" in
	  --evidence) EVIDENCE="$2"; shift 2 ;;
	  *) echo "usage: view.sh [--evidence DIR]" >&2; exit 2 ;;
	esac
done

mkdir -p "$EVIDENCE"
WORK="$EVIDENCE/work"
mkdir -p "$WORK"

log()  { printf '%s\n' "$*"; }
fail() { printf 'VIEW FAIL: %s\n' "$*" >&2; exit 1; }

# Same feature defines as the shipped server build (Src/GNUmakefile SCFLAGS).
DEFS="-DUNIX -DS1024x864 -DKEYPAD_DETECT -DSTINGY_REDRAW -DBATCH_LINES \
-DBATCH_POINTS -DBATCH_COLOR_LINES -DCODE_SIZE=0x20000 -DSTACK_SIZE=0x8000 \
-DNEW_LIMITS -DSLOWER -DMULTI_SYNC -DTHREAD_SWAPCONTEXT -DNULL_BACKEND \
-DSERVER_ONLY"

log "== building the shipped server core (make phase1-server phase1-core)"
( cd "$REPO" && make -s phase1-server phase1-core ) > "$WORK/build.out" 2>&1 || \
	fail "server/core build failed (see $WORK/build.out)"

[ -d "$REPO/Src/sobj" ] || fail "server object tree missing"
[ -f "$REPO/Src/sobj/server_view.o" ] || fail "server_view.o not built"
[ -f "$REPO/Src/sobj/server_events.o" ] || fail "server_events.o not built"

# Core objects without the server entry point (the fixture provides main()).
# shellcheck disable=SC2046
CORE_OBJS=$(ls "$REPO"/Src/sobj/*.o | grep -v '/server_main.o$')
[ -n "$CORE_OBJS" ] || fail "no core objects"

log "== static read-only scan of Src/server_view.c"
FORBID='\b(do_special|update_specials|update_vehicle|kill_vehicle|make_bullet|make_explosion|explode_location|server_step|animate|intent_[a-z_]+|play_[a-z_]+|draw_[a-z_]+|rnd|rand|random)[[:space:]]*\('
sed -e 's://.*::' -e 's:/\*.*\*/::' "$REPO/Src/server_view.c" \
	| grep -v '^[[:space:]]*\*' > "$WORK/scan.c"
if grep -nE "$FORBID" "$WORK/scan.c" > "$WORK/scan.out" 2>&1; then
	cat "$WORK/scan.out" >&2
	fail "server_view.c invokes a draw/special/RNG/control function"
fi
log "   Src/server_view.c calls no draw/special/RNG/control function"

log "== compiling the wire codec and the view fixture against the server core"
gcc -O0 -g -w -I"$PROTO" -c "$PROTO/encode.c" -o "$WORK/encode.o" \
	> "$WORK/encode-build.out" 2>&1 || \
	fail "codec build failed (see $WORK/encode-build.out)"
gcc -O0 -g -w -I"$PROTO" -c "$PROTO/decode.c" -o "$WORK/decode.o" \
	>> "$WORK/encode-build.out" 2>&1 || \
	fail "codec build failed (see $WORK/encode-build.out)"

# shellcheck disable=SC2086
gcc -O0 -g -w $DEFS -I"$REPO/Src" -I"$REPO/Src/Include" -iquote"$PROTO" \
	-DXTANK_DIR="\"$REPO/Src/..\"" -DALLDEFINES="\"p3.8 view fixture\"" \
	-o "$WORK/view_fixture" "$HARNESS/view_fixture.c" $CORE_OBJS \
	"$WORK/encode.o" "$WORK/decode.o" -lm \
	> "$WORK/fixture-build.out" 2>&1 || \
	fail "fixture build failed (see $WORK/fixture-build.out)"

log "== running the view fixture (3 candidate repeats)"
for n in 1 2 3; do
	if ! "$WORK/view_fixture" "$REPO" > "$WORK/fixture.$n.out" 2>&1; then
		cat "$WORK/fixture.$n.out" >&2
		fail "view fixture run $n failed"
	fi
	grep -q 'VIEW FIXTURE PASS' "$WORK/fixture.$n.out" || \
		fail "fixture run $n did not report PASS"
done
cmp "$WORK/fixture.1.out" "$WORK/fixture.2.out" >/dev/null || \
	fail "candidate repeats 1 and 2 differ"
cmp "$WORK/fixture.1.out" "$WORK/fixture.3.out" >/dev/null || \
	fail "candidate repeats 1 and 3 differ"
grep -q 'simulation unchanged after 5 exports' "$WORK/fixture.1.out" || \
	fail "fixture did not assert read-only parity"
log "    $(grep -m1 'VIEW FIXTURE PASS' "$WORK/fixture.1.out")"
log "    three candidate repeats identical"

log ""
log "P3.8 VIEW PASS (two recipients; separated cameras/mapper/HUD/radar;"
log "               order-independent recipient bytes; u32 generations;"
log "               stale-binding rejection; no full-map leak; read-only 0/1/5)"
