#!/usr/bin/env bash
#
# view.sh -- P2.5 authorized read-only view exporter acceptance.
#
# Builds the shipped display-free server core, links the P2.5 view fixture
# against it (Src/sobj minus the server entry point) plus the frozen P2.3 wire
# codec, and runs:
#
#   1. the fixture, which drives one terminal-free human plus one drone and
#      asserts the docs/VIEW-v1.md rules: field/visibility allowlist, viewport
#      edges, unexplored vs remembered mapper knowledge, anonymous/expired
#      radar contacts, the death camera, bound-maximized snapshot size, and
#      read-only simulation parity over 0/1/5 exports per tick;
#   2. a static read-only source scan of Src/server_view.c (no draw/special/
#      RNG/control call may appear);
#   3. a determinism repeat: two fixture runs are byte-identical.
#
# All generated binaries/logs live OUTSIDE the checkout.
#
#   bash Tests/phase2/view.sh [--evidence DIR]
#
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HARNESS="$REPO/Tests/phase2"
PROTO="$REPO/Protocol"
EVIDENCE="${XTANK_P2_EVIDENCE:-/tmp/opencode/xtank-phase2/p2.5/view}"

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
( cd "$REPO" && make phase1-server phase1-core ) > "$WORK/build.out" 2>&1 || \
	fail "server/core build failed (see $WORK/build.out)"

[ -d "$REPO/Src/sobj" ] || fail "server object tree missing"
[ -f "$REPO/Src/sobj/server_view.o" ] || fail "server_view.o not built"

# Core objects without the server entry point (the fixture provides main()).
# shellcheck disable=SC2046
CORE_OBJS=$(ls "$REPO"/Src/sobj/*.o | grep -v '/server_main.o$')
[ -n "$CORE_OBJS" ] || fail "no core objects"

log "== static read-only scan of Src/server_view.c"
FORBID='\b(do_special|update_specials|update_vehicle|kill_vehicle|make_bullet|server_step|animate|intent_[a-z_]+|play_[a-z_]+|draw_[a-z_]+|rnd|rand|random|explode_[a-z_]+)[[:space:]]*\('
# Strip comments first so prose (e.g. "called after server_step()") is not a hit.
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
	-DXTANK_DIR="\"$REPO/Src/..\"" -DALLDEFINES="\"p2.5 view fixture\"" \
	-o "$WORK/view_fixture" "$HARNESS/view_fixture.c" $CORE_OBJS \
	"$WORK/encode.o" "$WORK/decode.o" \
	-Wl,--wrap=server_view_export,--wrap=do_special -lm \
	> "$WORK/fixture-build.out" 2>&1 || \
	fail "fixture build failed (see $WORK/fixture-build.out)"

log "== running the authorized view fixture"
if ! "$WORK/view_fixture" "$REPO" > "$WORK/fixture1.out" 2>&1; then
	cat "$WORK/fixture1.out" >&2
	fail "view fixture failed"
fi
grep -q 'VIEW FIXTURE PASS' "$WORK/fixture1.out" || \
	fail "fixture did not report PASS"
grep -q 'simulation state unchanged after 5 exports' "$WORK/fixture1.out" || \
	fail "fixture did not assert read-only parity"
log "    $(grep -m1 'VIEW FIXTURE PASS' "$WORK/fixture1.out")"

log "== determinism: repeat the fixture"
"$WORK/view_fixture" "$REPO" > "$WORK/fixture2.out" 2>&1 || \
	fail "fixture repeat failed"
cmp "$WORK/fixture1.out" "$WORK/fixture2.out" >/dev/null || \
	fail "fixture repeats differ"
log "    two fixture runs identical"

log ""
log "P2.5 VIEW PASS (field/visibility allowlist, viewport edges, mapper vs"
log "              real_map, anonymous/expired radar, death camera, 28524-byte"
log "              maximum, read-only 0/1/5-export parity)"