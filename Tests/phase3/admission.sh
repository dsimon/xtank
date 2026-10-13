#!/usr/bin/env bash
#
# admission.sh -- P3.6 fair bounded admission and valid-input timeouts.
#
# Builds the shipped display-free server core, the P3.4 coordinator
# (Src/server_session.c), the P2.7 admission layer (Src/server_net.c), the
# pinned ENet v1.3.18 transport and the frozen v1.1 codec, then links and runs
# the P3.6 fixture.  The fixture asserts the approved D3-E contract:
#
#   - per-peer per-tick quotas sit inside the global 64-service-event /
#     128-intent bounds; a flooding peer is capped and cannot starve the healthy
#     peer's next-tick admission nor either peer's drive/fire release;
#   - the 8-intent reserve covers BOTH peers' server-side neutralisation;
#   - same command ids on different peers do not collide; a cross-channel
#     duplicate on one peer applies once; the actual transport channel,
#     epoch, ownership/generation, finite/range and expiry validation all
#     precede every intent;
#   - the valid-input timeout boundary is exactly 2,000 ms (1,999/2,000 alive,
#     2,001 removes) and invalid/replayed/foreign traffic cannot extend it.
#
# It runs the fixture three times and requires byte identity (three candidate
# repeats).  It also statically checks that the admission and coordinator
# translation units are display-free.
#
# ENet is pinned (P2.1-REPORT.md): v1.3.18, SHA-256
# 28603c895f9ed24a846478180ee72c7376b39b4bb1287b73877e5eae7d96b0dd.  Set
# XTANK_ENET_DIR to an extracted source tree, else a cached evidence tree is
# used, else the pinned tarball is fetched (out of Git).
#
# All generated binaries/logs live OUTSIDE the checkout.
#
#   bash Tests/phase3/admission.sh [--evidence DIR]
#
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HARNESS="$REPO/Tests/phase3"
PROTO="$REPO/Protocol"
EVIDENCE="${XTANK_P3_EVIDENCE:-/tmp/opencode/xtank-phase3/p3.6/admission}"

ENET_URL="https://github.com/lsalzman/enet/archive/refs/tags/v1.3.18.tar.gz"
ENET_SHA="28603c895f9ed24a846478180ee72c7376b39b4bb1287b73877e5eae7d96b0dd"

while [ $# -gt 0 ]; do
	case "$1" in
	  --evidence) EVIDENCE="$2"; shift 2 ;;
	  *) echo "usage: admission.sh [--evidence DIR]" >&2; exit 2 ;;
	esac
done

mkdir -p "$EVIDENCE"
WORK="$EVIDENCE/work"
mkdir -p "$WORK"

log()  { printf '%s\n' "$*"; }
fail() { printf 'ADMISSION FAIL: %s\n' "$*" >&2; exit 1; }

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
# Static boundary checks: admission + coordinator are display-free
# ---------------------------------------------------------------------------
log "== boundary: admission/coordinator are display-free"
for f in Src/server_net.c Src/server_session.c \
	 Src/Include/server_net.h Src/Include/server_session.h; do
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

log "== linking the admission fixture"
# shellcheck disable=SC2086
gcc -O0 -g -w $DEFS -DXTANK_WITH_NET -I"$REPO/Src" -I"$REPO/Src/Include" \
	-I"$REPO/Transport" -I"$ENET_DIR/include" \
	-DXTANK_DIR="\"$REPO/Src/..\"" -DALLDEFINES="\"p3.6 admission fixture\"" \
	-o "$WORK/admission_fixture" "$HARNESS/admission_fixture.c" \
	$CORE_OBJS "$WORK/server_session.o" "$WORK/server_net.o" \
	"$WORK/enet_transport.o" $ENET_OBJS "$WORK/encode.o" "$WORK/decode.o" \
	"$WORK/content-manifest.o" \
	-Wl,--wrap=make_terminal,--wrap=set_terminal,--wrap=display_terminal \
	-Wl,--wrap=get_events,--wrap=sync_terminals -lm \
	> "$WORK/fixture-build.out" 2>&1 || \
	fail "fixture build failed (see $WORK/fixture-build.out)"

# ---------------------------------------------------------------------------
# Run the fixture three times; require byte identity (three candidate repeats)
# ---------------------------------------------------------------------------
log "== running the admission fixture (3 candidate repeats)"
for n in 1 2 3; do
	if ! "$WORK/admission_fixture" "$REPO" > "$WORK/fixture.$n.out" 2>&1; then
		cat "$WORK/fixture.$n.out" >&2
		fail "fixture run $n failed"
	fi
	grep -q 'ADMISSION FIXTURE PASS' "$WORK/fixture.$n.out" || \
		fail "fixture run $n did not report PASS"
done
cmp "$WORK/fixture.1.out" "$WORK/fixture.2.out" >/dev/null || \
	fail "candidate repeats 1 and 2 differ"
cmp "$WORK/fixture.1.out" "$WORK/fixture.3.out" >/dev/null || \
	fail "candidate repeats 1 and 3 differ"
log "    $(grep -m1 'ADMISSION FIXTURE PASS' "$WORK/fixture.1.out")"
log "    three candidate repeats identical"

log ""
log "P3.6 ADMISSION PASS (per-peer quotas within 64/128 bounds; cross-peer"
log "                   fairness; both-departure reserve; dedup/channel/epoch/"
log "                   ownership/expiry precedence; 2000 ms timeout boundary)"

# The fixture must survive an independent rerun too.
"$WORK/admission_fixture" "$REPO" > "$WORK/fixture.4.out" 2>&1 || \
	fail "fixture rerun failed"
grep -q 'ADMISSION FIXTURE PASS' "$WORK/fixture.4.out" || \
	fail "fixture rerun did not report PASS"
log "    rerun PASS"
