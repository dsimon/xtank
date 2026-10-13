#!/usr/bin/env bash
#
# departure.sh -- P3.7 departure removal, admin transfer and lobby rejoin.
#
# Builds the shipped display-free server core, the P3.4 coordinator
# (Src/server_session.c), the P2.7 admission layer (Src/server_net.c), the
# pinned ENet v1.3.18 transport and the frozen v1.1 codec, then links and runs
# the P3.7 fixture.  The fixture drives the shipping coordinator directly and
# asserts the reviewed P3.2 departure contract for every reason:
#
#   - live and respawn-waiting tank removal on QUIT, transport disconnect,
#     valid-input timeout and output/queue saturation, once at a tick boundary;
#   - the departing peer's pending/held controls are cancelled (player-scoped)
#     while the survivor's are not; no ghost projectile;
#   - no kill/score/death/RNG/explosion side effect; retained storage keeps
#     every outstanding projectile owner valid and unchanged projectile rules;
#   - stable surviving slot/ownership and untouched team bookkeeping until the
#     normal reset releases the retained records;
#   - reliable + unique admin transfer; the survivor keeps progressing;
#   - no humans empties the world and returns to the listening lobby;
#   - rejoin while Running fails; the next-lobby rejoin gets fresh
#     connection/controller/tank identity and cannot replay old commands;
#   - simultaneous departures and repeated disconnect/rejoin cycles;
#   - the frozen P3.2 ordinary two-human reference hash is reproduced.
#
# It runs the fixture three times and requires byte identity, then compares the
# ordinary hash to the frozen P3.2/P3.5 reference.  It also statically checks
# that the departure seam and coordinator are display-free and X-free.
#
# ENet is pinned (P2.1-REPORT.md): v1.3.18, SHA-256
# 28603c895f9ed24a846478180ee72c7376b39b4bb1287b73877e5eae7d96b0dd.  Set
# XTANK_ENET_DIR to an extracted source tree, else a cached evidence tree is
# used, else the pinned tarball is fetched (out of Git).
#
# All generated binaries/logs live OUTSIDE the checkout.
#
#   bash Tests/phase3/departure.sh [--evidence DIR]
#
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HARNESS="$REPO/Tests/phase3"
PROTO="$REPO/Protocol"
EVIDENCE="${XTANK_P3_EVIDENCE:-/tmp/opencode/xtank-phase3/p3.7/departure}"
GOLDEN="$HARNESS/fixtures/two-human-ordinary.sha256"

ENET_URL="https://github.com/lsalzman/enet/archive/refs/tags/v1.3.18.tar.gz"
ENET_SHA="28603c895f9ed24a846478180ee72c7376b39b4bb1287b73877e5eae7d96b0dd"

while [ $# -gt 0 ]; do
	case "$1" in
	  --evidence) EVIDENCE="$2"; shift 2 ;;
	  *) echo "usage: departure.sh [--evidence DIR]" >&2; exit 2 ;;
	esac
done

mkdir -p "$EVIDENCE"
WORK="$EVIDENCE/work"
mkdir -p "$WORK"

log()  { printf '%s\n' "$*"; }
fail() { printf 'DEPARTURE FAIL: %s\n' "$*" >&2; exit 1; }

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
# Static boundary checks: the departure seam and coordinator are display-free
# ---------------------------------------------------------------------------
log "== boundary: departure seam and coordinator are display-free"
for f in Src/server_core.c Src/intent.c Src/server_session.c \
	 Src/Include/server.h Src/Include/intent.h \
	 Src/Include/server_session.h; do
	if grep -nE '#[[:space:]]*include[[:space:]]*<(X11|Xlib)' \
		"$REPO/$f" > "$WORK/scan-x.out" 2>&1; then
		cat "$WORK/scan-x.out" >&2
		fail "$f includes an X11 header"
	fi
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

log "== linking the departure fixture"
# shellcheck disable=SC2086
gcc -O0 -g -w $DEFS -DXTANK_WITH_NET -I"$REPO/Src" -I"$REPO/Src/Include" \
	-I"$REPO/Transport" -I"$ENET_DIR/include" \
	-DXTANK_DIR="\"$REPO/Src/..\"" -DALLDEFINES="\"p3.7 departure fixture\"" \
	-o "$WORK/departure_fixture" "$HARNESS/departure_fixture.c" \
	$CORE_OBJS "$WORK/server_session.o" "$WORK/server_net.o" \
	"$WORK/enet_transport.o" $ENET_OBJS "$WORK/encode.o" "$WORK/decode.o" \
	"$WORK/content-manifest.o" -lm \
	> "$WORK/fixture-build.out" 2>&1 || \
	fail "fixture build failed (see $WORK/fixture-build.out)"

# ---------------------------------------------------------------------------
# Run the fixture three times; require byte identity (three candidate repeats)
# ---------------------------------------------------------------------------
log "== running the departure fixture (3 candidate repeats)"
for n in 1 2 3; do
	if ! "$WORK/departure_fixture" "$REPO" > "$WORK/fixture.$n.out" 2>&1; then
		cat "$WORK/fixture.$n.out" >&2
		fail "fixture run $n failed"
	fi
	grep -q 'DEPARTURE-FIXTURE PASS' "$WORK/fixture.$n.out" || \
		fail "fixture run $n did not report PASS"
done
cmp "$WORK/fixture.1.out" "$WORK/fixture.2.out" >/dev/null || \
	fail "candidate repeats 1 and 2 differ"
cmp "$WORK/fixture.1.out" "$WORK/fixture.3.out" >/dev/null || \
	fail "candidate repeats 1 and 3 differ"
log "    $(grep -m1 'DEPARTURE-FIXTURE PASS' "$WORK/fixture.1.out")"
log "    three candidate repeats identical"

# ---------------------------------------------------------------------------
# Frozen ordinary two-human reference (P3.2/P3.5)
# ---------------------------------------------------------------------------
[ -f "$GOLDEN" ] || fail "missing frozen fixture $GOLDEN"
EXPECT="$(sed -n 's/^hash=//p' "$GOLDEN")"
GOT="$(sed -n 's/^DEPARTURE-FIXTURE hash-ordinary=\([0-9a-f]*\).*/\1/p' \
	"$WORK/fixture.1.out" | head -1)"
[ -n "$GOT" ] || fail "fixture did not print the ordinary hash"
[ "$GOT" = "$EXPECT" ] || \
	fail "ordinary two-human hash $GOT != frozen $EXPECT"
log "    ordinary two-human reference $GOT matches frozen fixture"

log ""
log "P3.7 DEPARTURE PASS (live/respawn/timeout/saturation/disconnect removal"
log "                  once at a tick boundary; player-scoped pending/held"
log "                  cancel; no kill/score/death/RNG/explosion; retained"
log "                  projectile owner; stable slots/team bookkeeping; admin"
log "                  transfer; survivor progresses; no-humans lobby return;"
log "                  rejoin-while-running rejected; fresh rejoin identity;"
log "                  simultaneous departures; repeated cycles; frozen"
log "                  ordinary two-human hash reproduced 3x)"