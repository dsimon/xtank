#!/usr/bin/env bash
#
# events.sh -- P3.8 recipient-authorized, bounded event delivery acceptance.
#
# Builds the shipped display-free server core (the P3.8 Src/server_events.c
# exporter), the P3.4 coordinator (Src/server_session.c), the P2.7 admission
# layer (Src/server_net.c), the pinned ENet v1.3.18 transport and the frozen
# v1.1 codec, then links and runs the P3.8 events fixture.  The fixture drives
# two human recipients through a running epoch and asserts:
#
#   - remote SCORE is not broadcast; DEATH/RESPAWN/EXPLOSION obey the approved
#     recipient table; MATCH_END public; RESULTS reliable to both;
#   - per-recipient event_sequence is contiguous (private skips leave no gap);
#   - capture_tick is the occurrence tick through delayed delivery;
#   - >256 events in one epoch deliver without silent loss (bounded cursor),
#     and retirement below a recipient cursor is an explicit slow-peer failure;
#   - RESULTS capture all participants before the score reset, once, and
#     survive a departure;
#   - 0/1/5 observations leave the simulation byte-identical.
#
# It runs the fixture three times and requires byte identity, and statically
# scans Src/{server_events,server_view}.c for draw/special/RNG/control calls.
#
# ENet is pinned (P2.1-REPORT.md): v1.3.18, SHA-256
# 28603c895f9ed24a846478180ee72c7376b39b4bb1287b73877e5eae7d96b0dd.  Set
# XTANK_ENET_DIR to an extracted source tree, else a cached evidence tree is
# used, else the pinned tarball is fetched (out of Git).
#
# All generated binaries/logs live OUTSIDE the checkout.
#
#   bash Tests/phase3/events.sh [--evidence DIR]
#
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HARNESS="$REPO/Tests/phase3"
PROTO="$REPO/Protocol"
EVIDENCE="${XTANK_P3_EVIDENCE:-/tmp/opencode/xtank-phase3/p3.8/events}"

ENET_URL="https://github.com/lsalzman/enet/archive/refs/tags/v1.3.18.tar.gz"
ENET_SHA="28603c895f9ed24a846478180ee72c7376b39b4bb1287b73877e5eae7d96b0dd"

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

DEFS="-DUNIX -DS1024x864 -DKEYPAD_DETECT -DSTINGY_REDRAW -DBATCH_LINES \
-DBATCH_POINTS -DBATCH_COLOR_LINES -DCODE_SIZE=0x20000 -DSTACK_SIZE=0x8000 \
-DNEW_LIMITS -DSLOWER -DMULTI_SYNC -DTHREAD_SWAPCONTEXT -DNULL_BACKEND \
-DSERVER_ONLY"

# ---------------------------------------------------------------------------
# Locate / fetch the pinned ENet source
# ---------------------------------------------------------------------------
ENET_DIR="${XTANK_ENET_DIR:-}"
if [ -z "$ENET_DIR" ]; then
	for cand in \
		"$REPO/../xtank-evidence/p2.9/deps/enet-1.3.18" \
		"/home/hatch/workspace/coding/xtank-evidence/p2.9/deps/enet-1.3.18"; do
		if [ -f "$cand/include/enet/enet.h" ]; then
			ENET_DIR="$cand"
			break
		fi
	done
fi
if [ -z "$ENET_DIR" ]; then
	ENET_DIR="$EVIDENCE/deps/enet-1.3.18"
	if [ ! -f "$ENET_DIR/include/enet/enet.h" ]; then
		mkdir -p "$EVIDENCE/deps"
		TAR="$EVIDENCE/deps/enet-1.3.18.tar.gz"
		if [ ! -f "$TAR" ]; then
			log "== fetching pinned ENet v1.3.18"
			curl -fsSL -o "$TAR" "$ENET_URL" || \
				fail "cannot fetch ENet (set XTANK_ENET_DIR)"
		fi
		echo "$ENET_SHA  $TAR" | sha256sum -c - >/dev/null || \
			fail "ENet tarball SHA-256 mismatch"
		tar xzf "$TAR" -C "$EVIDENCE/deps"
	fi
fi
[ -f "$ENET_DIR/include/enet/enet.h" ] || \
	fail "ENet headers not found at $ENET_DIR"
log "== using ENet at $ENET_DIR"

# ---------------------------------------------------------------------------
# Build the shipped core and link the fixture
# ---------------------------------------------------------------------------
log "== building the shipped server core (make phase1-server phase1-core)"
( cd "$REPO" && make -s phase1-server phase1-core ) > "$WORK/build.out" 2>&1 || \
	fail "server/core build failed (see $WORK/build.out)"
[ -f "$REPO/Src/sobj/server_events.o" ] || fail "server_events.o not built"
[ -f "$REPO/Src/sobj/server_view.o" ] || fail "server_view.o not built"

# Core objects without the server entry point (the fixture provides main()).
# shellcheck disable=SC2046
CORE_OBJS=$(ls "$REPO"/Src/sobj/*.o | grep -v '/server_main.o$')
[ -n "$CORE_OBJS" ] || fail "no core objects"

log "== static read-only scans of server_events.c and server_view.c"
FORBID='\b(do_special|update_specials|update_vehicle|kill_vehicle|make_bullet|make_explosion|explode_location|server_step|animate|intent_[a-z_]+|play_[a-z_]+|draw_[a-z_]+|rnd|rand|random)[[:space:]]*\('
for f in server_events.c server_view.c; do
	sed -e 's://.*::' -e 's:/\*.*\*/::' "$REPO/Src/$f" \
		| grep -v '^[[:space:]]*\*' > "$WORK/scan-$f"
	if grep -nE "$FORBID" "$WORK/scan-$f" > "$WORK/scan.out" 2>&1; then
		cat "$WORK/scan.out" >&2
		fail "$f invokes a draw/special/RNG/control function"
	fi
done
log "   Src/server_events.c and Src/server_view.c are read-only"

log "== compiling coordinator, admission, transport and codec"
# shellcheck disable=SC2086
gcc -O0 -g -w $DEFS -DXTANK_WITH_NET -I"$REPO/Src" -I"$REPO/Src/Include" \
	-c "$REPO/Src/server_session.c" -o "$WORK/server_session.o" \
	> "$WORK/compile.out" 2>&1 || fail "server_session build failed"
# shellcheck disable=SC2086
gcc -O0 -g -w $DEFS -I"$REPO/Src" -I"$REPO/Src/Include" \
	-c "$REPO/Src/server_net.c" -o "$WORK/server_net.o" \
	>> "$WORK/compile.out" 2>&1 || fail "server_net build failed"
gcc -O0 -g -w -I"$ENET_DIR/include" -c "$REPO/Transport/enet_transport.c" \
	-o "$WORK/enet_transport.o" >> "$WORK/compile.out" 2>&1 || \
	fail "enet_transport build failed"
gcc -O0 -g -w -I"$PROTO" -c "$PROTO/encode.c" -o "$WORK/encode.o" \
	>> "$WORK/compile.out" 2>&1 || fail "codec build failed"
gcc -O0 -g -w -I"$PROTO" -c "$PROTO/decode.c" -o "$WORK/decode.o" \
	>> "$WORK/compile.out" 2>&1 || fail "codec build failed"
gcc -O0 -g -w -I"$PROTO" -c "$PROTO/content-manifest.c" \
	-o "$WORK/content-manifest.o" >> "$WORK/compile.out" 2>&1 || \
	fail "content-manifest build failed"

ENET_SRCS="callbacks.c compress.c host.c list.c packet.c peer.c protocol.c unix.c"
ENET_OBJS=""
for f in $ENET_SRCS; do
	o="$WORK/enet-${f%.c}.o"
	gcc -O0 -g -w -I"$ENET_DIR/include" -c "$ENET_DIR/$f" -o "$o" \
		>> "$WORK/enet-build.out" 2>&1 || \
		fail "ENet build failed ($f; see $WORK/enet-build.out)"
	ENET_OBJS="$ENET_OBJS $o"
done

log "== linking the events fixture"
# shellcheck disable=SC2086
gcc -O0 -g -w $DEFS -DXTANK_WITH_NET -I"$REPO/Src" -I"$REPO/Src/Include" \
	-iquote"$PROTO" -I"$REPO/Transport" -I"$ENET_DIR/include" \
	-DXTANK_DIR="\"$REPO/Src/..\"" -DALLDEFINES="\"p3.8 events fixture\"" \
	-o "$WORK/events_fixture" "$HARNESS/events_fixture.c" \
	$CORE_OBJS "$WORK/server_session.o" "$WORK/server_net.o" \
	"$WORK/enet_transport.o" $ENET_OBJS "$WORK/encode.o" "$WORK/decode.o" \
	"$WORK/content-manifest.o" -lm \
	> "$WORK/fixture-build.out" 2>&1 || \
	fail "fixture build failed (see $WORK/fixture-build.out)"

# ---------------------------------------------------------------------------
# Run the fixture three times; require byte identity
# ---------------------------------------------------------------------------
log "== running the events fixture (3 candidate repeats)"
for n in 1 2 3; do
	if ! "$WORK/events_fixture" "$REPO" > "$WORK/fixture.$n.out" 2>&1; then
		cat "$WORK/fixture.$n.out" >&2
		fail "fixture run $n failed"
	fi
	grep -q 'EVENTS FIXTURE PASS' "$WORK/fixture.$n.out" || \
		fail "fixture run $n did not report PASS"
done
cmp "$WORK/fixture.1.out" "$WORK/fixture.2.out" >/dev/null || \
	fail "candidate repeats 1 and 2 differ"
cmp "$WORK/fixture.1.out" "$WORK/fixture.3.out" >/dev/null || \
	fail "candidate repeats 1 and 3 differ"
log "    $(grep -m1 'EVENTS FIXTURE PASS' "$WORK/fixture.1.out")"
log "    three candidate repeats identical"

log ""
log "P3.8 EVENTS PASS (SCORE/DEATH/RESPAWN/EXPLOSION/MATCH_END authorization;"
log "                 contiguous recipient sequences; occurrence tick; bounded"
log "                 >256 delivery; explicit slow-peer failure; results;"
log "                 read-only 0/1/5)"
