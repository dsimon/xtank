#!/usr/bin/env bash
#
# session.sh -- P2.8 solo lobby / network-server acceptance.
#
# Builds the shipped display-free server core, the P2.8 session module
# (Src/server_session.c), the P2.7 admission layer (Src/server_net.c), the
# pinned ENet v1.3.18 transport and the frozen P2.3 codec, then links and runs
# the P2.8 fixture in two modes:
#
#   state    handshake and content-digest rejection before any match,
#            second-peer rejection, allowlisted CONFIG_SELECT, admin
#            start/reset, no controls outside Running, completed (win) vs
#            aborted (timeout) results, and repeated results/reset ownership;
#   network  the real ENet path through the production server_session_service:
#            HELLO/WELCOME/CATALOG/CONFIG/READY/START, heartbeat ticks with
#            channel-1 snapshots and channel-0 acknowledgments, and QUIT.
#
# It also proves the network package builds and starts with every
# display-related environment variable unset (DISPLAY/XTANK_DIR/HOME), and
# statically checks the transport/session boundary (only the transport
# implementation includes ENet; the session module is X-free).
#
# ENet is pinned (P2.1-REPORT.md): v1.3.18, SHA-256
# 28603c895f9ed24a846478180ee72c7376b39b4bb1287b73877e5eae7d96b0dd.  Set
# XTANK_ENET_DIR to an extracted source tree, or the script fetches the pinned
# tarball into the evidence directory (out of Git).
#
# All generated binaries/logs live OUTSIDE the checkout.
#
#   bash Tests/phase2/session.sh [--evidence DIR]
#
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HARNESS="$REPO/Tests/phase2"
PROTO="$REPO/Protocol"
EVIDENCE="${XTANK_P2_EVIDENCE:-/tmp/opencode/xtank-phase2/p2.8/session}"

ENET_URL="https://github.com/lsalzman/enet/archive/refs/tags/v1.3.18.tar.gz"
ENET_SHA="28603c895f9ed24a846478180ee72c7376b39b4bb1287b73877e5eae7d96b0dd"

while [ $# -gt 0 ]; do
	case "$1" in
	  --evidence) EVIDENCE="$2"; shift 2 ;;
	  *) echo "usage: session.sh [--evidence DIR]" >&2; exit 2 ;;
	esac
done

mkdir -p "$EVIDENCE"
WORK="$EVIDENCE/work"
mkdir -p "$WORK"

log()  { printf '%s\n' "$*"; }
fail() { printf 'SESSION FAIL: %s\n' "$*" >&2; exit 1; }

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

log "== boundary: session module is display-free"
if grep -nE '#[[:space:]]*include[[:space:]]*<(X11|Xlib)' \
	"$REPO/Src/server_session.c" "$REPO/Src/Include/server_session.h" \
	> "$WORK/scan-x.out" 2>&1; then
	cat "$WORK/scan-x.out" >&2
	fail "server_session includes an X11 header"
fi

# ---------------------------------------------------------------------------
# Build the shipped core + the opt-in network server package
# ---------------------------------------------------------------------------
log "== building the shipped server core (make phase1-server phase1-core)"
( cd "$REPO" && make -s phase1-server phase1-core ) > "$WORK/build.out" 2>&1 || \
	fail "server/core build failed (see $WORK/build.out)"
[ -f "$REPO/Src/sobj/server_events.o" ] || fail "server_events.o not built"
[ -f "$REPO/Src/sobj/server_loop.o" ] || fail "server_loop.o not built"

log "== building the network server (make phase1-server-net)"
( cd "$REPO" && make -s phase1-server-net ENET_DIR="$ENET_DIR" ) \
	> "$WORK/net-build.out" 2>&1 || \
	fail "network server build failed (see $WORK/net-build.out)"
[ -x "$REPO/Src/xtank-server-net" ] || fail "xtank-server-net not built"

CORE_OBJS=$(ls "$REPO"/Src/sobj/*.o | grep -v '/server_main.o$')
[ -n "$CORE_OBJS" ] || fail "no core objects"

# ---------------------------------------------------------------------------
# Compile the session/net/transport/codec objects and link the fixture
# ---------------------------------------------------------------------------
log "== compiling session, admission, transport and codec"
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

log "== linking the session fixture"
# shellcheck disable=SC2086
gcc -O0 -g -w $DEFS -DXTANK_WITH_NET -I"$REPO/Src" -I"$REPO/Src/Include" \
	-I"$REPO/Transport" -I"$ENET_DIR/include" \
	-DXTANK_DIR="\"$REPO/Src/..\"" -DALLDEFINES="\"p2.8 session fixture\"" \
	-o "$WORK/session_fixture" "$HARNESS/session_fixture.c" \
	$CORE_OBJS "$WORK/server_session.o" "$WORK/server_net.o" \
	"$WORK/enet_transport.o" $ENET_OBJS "$WORK/encode.o" "$WORK/decode.o" \
	"$WORK/content-manifest.o" -Wl,--wrap=sendmsg -lm \
	> "$WORK/fixture-build.out" 2>&1 || \
	fail "fixture build failed (see $WORK/fixture-build.out)"

# ---------------------------------------------------------------------------
# Run both modes twice; require byte identity
# ---------------------------------------------------------------------------
run_mode() {
	local mode="$1"
	log "== running session_fixture ($mode)"
	if ! "$WORK/session_fixture" "$REPO" "$mode" \
		> "$WORK/$mode.1.out" 2>&1; then
		cat "$WORK/$mode.1.out" >&2
		fail "$mode mode failed"
	fi
	grep -q 'SESSION FIXTURE PASS' "$WORK/$mode.1.out" || \
		fail "$mode did not report PASS"
	log "    $(grep -m1 'SESSION FIXTURE PASS' "$WORK/$mode.1.out")"

	"$WORK/session_fixture" "$REPO" "$mode" > "$WORK/$mode.2.out" 2>&1 || \
		fail "$mode repeat failed"
	cmp "$WORK/$mode.1.out" "$WORK/$mode.2.out" >/dev/null || \
		fail "$mode repeats differ"
	log "    two $mode runs identical"
}

run_mode state
run_mode network

# ---------------------------------------------------------------------------
# Network package starts with all display-related environment unset
# ---------------------------------------------------------------------------
log "== packaging the network server and starting it display-free"
bash "$REPO/Bin/package-server.sh" --src "$REPO" --net \
	--enet-dir "$ENET_DIR" --out "$WORK/bundle" --name xtank-server-net \
	> "$WORK/package.log" 2>&1 || \
	fail "package-server --net failed (see $WORK/package.log)"
QBIN="$WORK/bundle/xtank-server-net/bin/xtank-server"
[ -x "$QBIN" ] || fail "network package binary missing"

env -u DISPLAY -u XTANK_DIR -u HOME -u LD_PRELOAD -u XTANK_ASSET_ROOT \
	-u XTANK_SERVER_ASSETS LC_ALL=C "$QBIN" --version \
	> "$WORK/net-version.out" 2>&1 || fail "network package --version failed"
grep -q 'Xtank Release' "$WORK/net-version.out" || \
	fail "network package did not report version"

env -u DISPLAY -u XTANK_DIR -u HOME -u LD_PRELOAD -u XTANK_ASSET_ROOT \
	-u XTANK_SERVER_ASSETS LC_ALL=C "$QBIN" --help \
	> "$WORK/net-help.out" 2>&1 || fail "network package --help failed"
grep -q -- '--listen' "$WORK/net-help.out" || \
	fail "network package does not expose --listen"

# Start the listener with every display-related variable unset and prove it
# binds (then stop it).
env -u DISPLAY -u XTANK_DIR -u HOME -u LD_PRELOAD -u XTANK_ASSET_ROOT \
	-u XTANK_SERVER_ASSETS LC_ALL=C "$QBIN" --listen 0 --fast \
	> "$WORK/net-listen.out" 2>&1 &
LISTEN_PID=$!
for _ in $(seq 1 50); do
	grep -q '^SERVER-LISTEN ' "$WORK/net-listen.out" 2>/dev/null && break
	sleep 0.1
done
kill "$LISTEN_PID" 2>/dev/null || true
wait "$LISTEN_PID" 2>/dev/null || true
grep -q '^SERVER-LISTEN ' "$WORK/net-listen.out" || \
	fail "network package did not start a display-free listener"
log "    $(grep -m1 '^SERVER-LISTEN ' "$WORK/net-listen.out")"

log ""
log "P2.8 SESSION PASS (handshake+content rejection before match, second peer"
log "                 rejected, allowlist, admin start/reset, no controls"
log "                 outside Running, completed vs abort, 2 s timeout,"
log "                 repeated results/reset; real ENet path; display-free"
log "                 network package)"