#!/usr/bin/env bash
#
# transport.sh -- P3.9 transport isolation, fragmentation and connection reuse.
#
# Builds the shipped display-free server core, the P3.4 coordinator
# (Src/server_session.c), the P2.7 admission layer (Src/server_net.c), the
# pinned ENet v1.3.18 transport and the frozen v1.1 codec, then links and runs
# the P3.9 fixture in two modes:
#
#   transport  aggregate 64-service-event budget per tick across repeated
#              service calls and two peers (one receive/router owner:
#              server_net_service/server_net_drain); the application receive
#              queue and reliable-backlog caps; a maximum application snapshot
#              fragmented unreliably and reassembled atomically (even under
#              simulated fragment loss or fragment reorder), with a syscall-level
#              packet capture proving the approved reliability choice; 100
#              connect/disconnect/
#              rejoin cycles reusing wrappers with no stale packet, slot
#              exhaustion or growing resources; a frozen reader and a
#              flooding peer isolated to themselves.
#
#   session    real ENet client through the shipping coordinator with forced
#              snapshot send failure: at most the newest unsent snapshot is
#              retained, the peer survives, and recovery sends a full snapshot.
#
# It also statically proves the single-owner rule (transport_service and
# transport_receive are called only from Src/server_net.c) and the transport
# boundary (only Transport/enet_transport.c includes ENet).
#
# ENet is pinned (P2.1-REPORT.md): v1.3.18, SHA-256
# 28603c895f9ed24a846478180ee72c7376b39b4bb1287b73877e5eae7d96b0dd.  Set
# XTANK_ENET_DIR to an extracted source tree, else a cached evidence tree is
# used, else the pinned tarball is fetched (out of Git).
#
# All generated binaries/logs live OUTSIDE the checkout.
#
#   bash Tests/phase3/transport.sh [--evidence DIR]
#
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HARNESS="$REPO/Tests/phase3"
PROTO="$REPO/Protocol"
EVIDENCE="${XTANK_P3_EVIDENCE:-/tmp/opencode/xtank-phase3/p3.9/transport}"

ENET_URL="https://github.com/lsalzman/enet/archive/refs/tags/v1.3.18.tar.gz"
ENET_SHA="28603c895f9ed24a846478180ee72c7376b39b4bb1287b73877e5eae7d96b0dd"

while [ $# -gt 0 ]; do
	case "$1" in
	  --evidence) EVIDENCE="$2"; shift 2 ;;
	  *) echo "usage: transport.sh [--evidence DIR]" >&2; exit 2 ;;
	esac
done

mkdir -p "$EVIDENCE"
WORK="$EVIDENCE/work"
mkdir -p "$WORK"

log()  { printf '%s\n' "$*"; }
fail() { printf 'TRANSPORT FAIL: %s\n' "$*" >&2; exit 1; }

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
# Static boundary checks
# ---------------------------------------------------------------------------
log "== boundary: only the transport implementation includes ENet"
ENET_USERS=$(grep -rl "enet/enet.h" "$REPO/Src" "$REPO/Transport" 2>/dev/null \
	| grep -v "/enet_transport.c$" || true)
[ -z "$ENET_USERS" ] || \
	fail "ENet included outside Transport/enet_transport.c: $ENET_USERS"

log "== boundary: single transport service/receive owner (Src/server_net.c)"
for fn in transport_service transport_receive; do
	BAD=$(grep -rn "${fn}[[:space:]]*(" "$REPO/Src" "$REPO/Transport" \
		--include='*.c' 2>/dev/null \
		| grep -vE '^[^:]*:[0-9]+:[[:space:]]*(\*|/\*)' \
		| grep -v "Transport/enet_transport.c" \
		| grep -v "Src/server_net.c" || true)
	[ -z "$BAD" ] || fail "$fn called outside the single owner: $BAD"
done

# ---------------------------------------------------------------------------
# Build the shipped core and link the fixture
# ---------------------------------------------------------------------------
log "== building the shipped server core (make phase1-server phase1-core)"
( cd "$REPO" && make -s phase1-server phase1-core ) > "$WORK/build.out" 2>&1 || \
	fail "server/core build failed (see $WORK/build.out)"
[ -f "$REPO/Src/sobj/server_events.o" ] || fail "server_events.o not built"

CORE_OBJS=$(ls "$REPO"/Src/sobj/*.o | grep -v '/server_main.o$')
[ -n "$CORE_OBJS" ] || fail "no core objects"

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

log "== linking the transport fixture"
# shellcheck disable=SC2086
gcc -O0 -g -w $DEFS -DXTANK_WITH_NET -I"$REPO/Src" -I"$REPO/Src/Include" \
	-I"$REPO/Transport" -I"$ENET_DIR/include" \
	-DXTANK_DIR="\"$REPO/Src/..\"" -DALLDEFINES="\"p3.9 transport fixture\"" \
	-o "$WORK/transport_fixture" "$HARNESS/transport_fixture.c" \
	$CORE_OBJS "$WORK/server_session.o" "$WORK/server_net.o" \
	"$WORK/enet_transport.o" $ENET_OBJS "$WORK/encode.o" "$WORK/decode.o" \
	"$WORK/content-manifest.o" \
	-Wl,--wrap=sendmsg -Wl,--wrap=recvmsg -Wl,--wrap=transport_send -lm \
	> "$WORK/fixture-build.out" 2>&1 || \
	fail "fixture build failed (see $WORK/fixture-build.out)"

# ---------------------------------------------------------------------------
# Run both modes three times; require byte identity
# ---------------------------------------------------------------------------
run_mode() {
	local mode="$1"
	log "== running transport_fixture ($mode)"
	for n in 1 2 3; do
		if ! "$WORK/transport_fixture" "$REPO" "$mode" \
			> "$WORK/$mode.$n.out" 2>&1; then
			cat "$WORK/$mode.$n.out" >&2
			fail "$mode run $n failed"
		fi
		grep -q 'TRANSPORT-FIXTURE PASS' "$WORK/$mode.$n.out" || \
			fail "$mode run $n did not report PASS"
	done
	cmp "$WORK/$mode.1.out" "$WORK/$mode.2.out" >/dev/null || \
		fail "$mode candidate repeats 1 and 2 differ"
	cmp "$WORK/$mode.1.out" "$WORK/$mode.3.out" >/dev/null || \
		fail "$mode candidate repeats 1 and 3 differ"
	log "    $(grep -m1 'TRANSPORT-FIXTURE PASS' "$WORK/$mode.1.out")"
}

run_mode transport
run_mode session

log ""
log "P3.9 TRANSPORT PASS (single receive/router owner; aggregate 64-event"
log "                  budget; application/ENet queue caps; unreliable"
log "                  fragmentation capture + atomic reassembly under"
log "                  fragment loss/reorder; 100 wrapper-reuse cycles without"
log "                  stale"
log "                  traffic or growth; frozen-reader/flood isolation;"
log "                  newest-unsent-snapshot retention)"
