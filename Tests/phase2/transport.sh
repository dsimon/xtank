#!/usr/bin/env bash
#
# transport.sh -- P2.7 nonblocking transport and input-admission acceptance.
#
# Builds the shipped display-free server core, the pinned ENet v1.3.18
# transport wrapper (Transport/enet_transport.c), the admission layer
# (Src/server_net.c) and the P2.3 wire codec, then links and runs the P2.7
# fixture in two modes:
#
#   admission  ownership, finite/range validation, stale epoch/generation,
#              cross-channel dedup (no duplicate edge fire), assigned-tick
#              scheduling and acks, queue saturation with a release reserve,
#              lost/repeated/partial batches, no-peer inertness;
#   transport  real ENet over loopback: connect, reliable/unreliable
#              round-trip, partial decode, a ten-second paused reader that
#              neither stalls ticks nor grows the reliable queue without
#              bound, and the backpressure abort.
#
# It also statically proves the transport boundary (only Transport/
# enet_transport.c includes ENet) and that the admission layer is X-free.
#
# ENet is pinned (P2.1-REPORT.md): v1.3.18, SHA-256
# 28603c895f9ed24a846478180ee72c7376b39b4bb1287b73877e5eae7d96b0dd.  Set
# XTANK_ENET_DIR to an extracted source tree, or the script fetches the pinned
# tarball into the evidence directory (out of Git).
#
# All generated binaries/logs live OUTSIDE the checkout.
#
#   bash Tests/phase2/transport.sh [--evidence DIR]
#
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HARNESS="$REPO/Tests/phase2"
PROTO="$REPO/Protocol"
EVIDENCE="${XTANK_P2_EVIDENCE:-/tmp/opencode/xtank-phase2/p2.7/transport}"

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
[ -z "$ENET_USERS" ] || fail "ENet included outside Transport/enet_transport.c: $ENET_USERS"

log "== boundary: admission layer is display-free"
if grep -nE '#[[:space:]]*include[[:space:]]*<(X11|Xlib)' \
	"$REPO/Src/server_net.c" "$REPO/Src/Include/server_net.h" \
	> "$WORK/scan-x.out" 2>&1; then
	cat "$WORK/scan-x.out" >&2
	fail "server_net includes an X11 header"
fi
FORBID='\b(do_special|update_specials|update_vehicle|kill_vehicle|make_bullet|make_explosion|explode_location|animate|draw_[a-z_]+|rnd|rand|random)[[:space:]]*\('
sed -e 's://.*::' -e 's:/\*.*\*/::' "$REPO/Src/server_net.c" \
	| grep -v '^[[:space:]]*\*' > "$WORK/scan.c"
if grep -nE "$FORBID" "$WORK/scan.c" > "$WORK/scan.out" 2>&1; then
	cat "$WORK/scan.out" >&2
	fail "server_net.c invokes a draw/special/RNG function"
fi

# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------
log "== building the shipped server core (make phase1-server phase1-core)"
( cd "$REPO" && make phase1-server phase1-core ) > "$WORK/build.out" 2>&1 || \
	fail "server/core build failed (see $WORK/build.out)"
[ -d "$REPO/Src/sobj" ] || fail "server object tree missing"

CORE_OBJS=$(ls "$REPO"/Src/sobj/*.o | grep -v '/server_main.o$')
[ -n "$CORE_OBJS" ] || fail "no core objects"

log "== compiling ENet v1.3.18"
ENET_SRCS="callbacks.c compress.c host.c list.c packet.c peer.c protocol.c unix.c"
ENET_OBJS=""
for f in $ENET_SRCS; do
	o="$WORK/enet-${f%.c}.o"
	gcc -O0 -g -w -I"$ENET_DIR/include" -c "$ENET_DIR/$f" -o "$o" \
		>> "$WORK/enet-build.out" 2>&1 || \
		fail "ENet build failed ($f; see $WORK/enet-build.out)"
	ENET_OBJS="$ENET_OBJS $o"
done

log "== compiling the transport wrapper, admission layer and codec"
gcc -O0 -g -w -I"$ENET_DIR/include" -c "$REPO/Transport/enet_transport.c" \
	-o "$WORK/enet_transport.o" > "$WORK/transport-build.out" 2>&1 || \
	fail "enet_transport build failed (see $WORK/transport-build.out)"
# shellcheck disable=SC2086
gcc -O0 -g -w $DEFS -I"$REPO/Src" -I"$REPO/Src/Include" \
	-c "$REPO/Src/server_net.c" -o "$WORK/server_net.o" \
	>> "$WORK/transport-build.out" 2>&1 || \
	fail "server_net build failed (see $WORK/transport-build.out)"
gcc -O0 -g -w -I"$PROTO" -c "$PROTO/encode.c" -o "$WORK/encode.o" \
	>> "$WORK/transport-build.out" 2>&1 || fail "codec build failed"
gcc -O0 -g -w -I"$PROTO" -c "$PROTO/decode.c" -o "$WORK/decode.o" \
	>> "$WORK/transport-build.out" 2>&1 || fail "codec build failed"

log "== linking the admission fixture"
# shellcheck disable=SC2086
gcc -O0 -g -w $DEFS -I"$REPO/Src" -I"$REPO/Src/Include" \
	-I"$REPO/Transport" -I"$ENET_DIR/include" \
	-DXTANK_DIR="\"$REPO/Src/..\"" -DALLDEFINES="\"p2.7 admission fixture\"" \
	-o "$WORK/admission_fixture" "$HARNESS/admission_fixture.c" \
	$CORE_OBJS "$WORK/server_net.o" "$WORK/enet_transport.o" $ENET_OBJS \
	"$WORK/encode.o" "$WORK/decode.o" -Wl,--wrap=sendmsg -lm \
	> "$WORK/fixture-build.out" 2>&1 || \
	fail "fixture build failed (see $WORK/fixture-build.out)"

# ---------------------------------------------------------------------------
# Run
# ---------------------------------------------------------------------------
run_mode() {
	local mode="$1"
	log "== running admission_fixture ($mode)"
	if ! "$WORK/admission_fixture" "$REPO" "$mode" \
		> "$WORK/$mode.1.out" 2>&1; then
		cat "$WORK/$mode.1.out" >&2
		fail "$mode mode failed"
	fi
	grep -q 'ADMISSION FIXTURE PASS' "$WORK/$mode.1.out" || \
		fail "$mode did not report PASS"
	log "    $(grep -m1 'ADMISSION FIXTURE PASS' "$WORK/$mode.1.out")"

	"$WORK/admission_fixture" "$REPO" "$mode" > "$WORK/$mode.2.out" 2>&1 || \
		fail "$mode repeat failed"
	cmp "$WORK/$mode.1.out" "$WORK/$mode.2.out" >/dev/null || \
		fail "$mode repeats differ"
	log "    two $mode runs identical"
}

run_mode admission
run_mode transport

log ""
log "P2.7 TRANSPORT PASS (bounded ENet service, ownership, validation, dedup,"
log "                  assigned-tick acks; lost/repeated/partial, saturation +"
log "                  release reserve, stale epoch/generation, paused reader)"
