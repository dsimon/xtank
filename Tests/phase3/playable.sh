#!/usr/bin/env bash
#
# playable.sh -- P3.12 solo / two-human live-path integration acceptance.
#
# Contract (docs/PHASE3-PLAN.md P3.12):
#   bash Tests/phase3/playable.sh [--evidence DIR]
#
# Drives the shipped coordinator + admission + ENet transport + frozen codec
# and real P3.10 client sessions through every allowed live profile on both
# mazes:
#
#   solo x {0,1,2} drones   one human
#   duo  x {0,1,2} drones   two humans
#
# Each profile must connect/handshake/lobby/config/ready/start, play, finish
# and reset to the lobby barrier, twice (two epochs).  The Arena two-human /
# 0-drone profile must reach a real 1,000-point natural win; the two-human /
# 2-drone profile must retain four-entry results (2 humans + 2 drones).  Any
# other finish is forced through the shipped path and labelled test-only so a
# synthetic result can never be read as an ordinary-match oracle.
#
# Departure/admission/timeouts are tested separately (Tests/phase3/
# departure.sh, admission.sh) and are deliberately absent here.
#
# Generated binaries, oracle trees and logs live OUTSIDE the checkout.
#
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HARNESS="$REPO/Tests/phase3"
FIX="$HARNESS/fixtures/parity"
EVIDENCE="${XTANK_P3_EVIDENCE:-/tmp/opencode/xtank-phase3/p3.12/playable}"

ENET_URL="https://github.com/lsalzman/enet/archive/refs/tags/v1.3.18.tar.gz"
ENET_SHA="28603c895f9ed24a846478180ee72c7376b39b4bb1287b73877e5eae7d96b0dd"

while [ $# -gt 0 ]; do
	case "$1" in
	  --evidence) EVIDENCE="$2"; shift 2 ;;
	  *) echo "usage: playable.sh [--evidence DIR]" >&2; exit 2 ;;
	esac
done

mkdir -p "$EVIDENCE"
WORK="$EVIDENCE/work"
mkdir -p "$WORK"

log()  { printf '%s\n' "$*"; }
fail() { printf 'PLAYABLE FAIL: %s\n' "$*" >&2; exit 1; }

DEFS="-DUNIX -DS1024x864 -DKEYPAD_DETECT -DSTINGY_REDRAW -DBATCH_LINES \
-DBATCH_POINTS -DBATCH_COLOR_LINES -DCODE_SIZE=0x20000 -DSTACK_SIZE=0x8000 \
-DNEW_LIMITS -DSLOWER -DMULTI_SYNC -DTHREAD_SWAPCONTEXT -DNULL_BACKEND \
-DSERVER_ONLY"

# Pinned ENet v1.3.18.
ENET_DIR="${XTANK_ENET_DIR:-}"
if [ -z "$ENET_DIR" ] || [ ! -f "$ENET_DIR/include/enet/enet.h" ]; then
	for cand in \
	    /home/hatch/workspace/coding/xtank-evidence/p2.9/deps/enet-1.3.18 \
	    /tmp/opencode/xtank-phase2/p2.9/deps/enet-1.3.18; do
		if [ -f "$cand/include/enet/enet.h" ]; then
			ENET_DIR="$cand"; break
		fi
	done
fi
if [ -z "$ENET_DIR" ] || [ ! -f "$ENET_DIR/include/enet/enet.h" ]; then
	ENET_DIR="$EVIDENCE/deps/enet-1.3.18"
	if [ ! -f "$ENET_DIR/include/enet/enet.h" ]; then
		mkdir -p "$EVIDENCE/deps"
		TAR="$EVIDENCE/deps/enet-1.3.18.tar.gz"
		[ -f "$TAR" ] || curl -fsSL -o "$TAR" "$ENET_URL" || \
			fail "cannot fetch ENet (set XTANK_ENET_DIR)"
		echo "$ENET_SHA  $TAR" | sha256sum -c - >/dev/null || \
			fail "ENet tarball SHA-256 mismatch"
		tar xzf "$TAR" -C "$EVIDENCE/deps"
	fi
fi
log "== ENet: $ENET_DIR"

log "== building the shipped server core"
( cd "$REPO" && make -s phase1-server phase1-core ) > "$WORK/core.out" 2>&1 || \
	fail "core build failed (see $WORK/core.out)"
CORE_OBJS=$(ls "$REPO"/Src/sobj/*.o | grep -v '/server_main.o$')
[ -n "$CORE_OBJS" ] || fail "no core objects"

log "== compiling session, admission, transport and codec modules"
: > "$WORK/compile.out"
compile() { # src out
	local src="$1"; local out="$2"

	gcc -O0 -g -w $DEFS -DXTANK_WITH_NET -I"$REPO/Src" -I"$REPO/Src/Include" \
		-I"$REPO/Protocol" -I"$REPO/Transport" -I"$REPO/Client" \
		-I"$ENET_DIR/include" -c "$src" -o "$out" \
		>> "$WORK/compile.out" 2>&1 || fail "compile failed: $src"
}
compile "$REPO/Src/server_session.c" "$WORK/server_session.o"
compile "$REPO/Src/server_net.c" "$WORK/server_net.o"
compile "$REPO/Transport/enet_transport.c" "$WORK/enet_transport.o"
compile "$REPO/Protocol/encode.c" "$WORK/encode.o"
compile "$REPO/Protocol/decode.c" "$WORK/decode.o"
compile "$REPO/Protocol/content-manifest.c" "$WORK/content-manifest.o"
gcc -O0 -g -w -I"$REPO/Protocol" -I"$REPO/Transport" -I"$REPO/Client" \
	-c "$REPO/Client/session_client.c" -o "$WORK/session_client.o" \
	>> "$WORK/compile.out" 2>&1 || fail "client compile failed"

ENET_OBJS=""
for f in callbacks compress host list packet peer protocol unix; do
	o="$WORK/enet-$f.o"
	gcc -O0 -g -w -I"$ENET_DIR/include" -c "$ENET_DIR/$f.c" -o "$o" \
		>> "$WORK/enet-build.out" 2>&1 || fail "ENet build failed ($f)"
	ENET_OBJS="$ENET_OBJS $o"
done

log "== linking the playable fixture"
# shellcheck disable=SC2086
gcc -O0 -g -w $DEFS -DXTANK_WITH_NET -I"$REPO/Src" -I"$REPO/Src/Include" \
	-I"$REPO/Protocol" -I"$REPO/Transport" -I"$REPO/Client" \
	-I"$ENET_DIR/include" \
	-DXTANK_DIR="\"$REPO/Src/..\"" -DALLDEFINES="\"p3.12 playable fixture\"" \
	-o "$WORK/playable_fixture" "$HARNESS/playable_fixture.c" \
	$CORE_OBJS "$WORK/server_session.o" "$WORK/server_net.o" \
	"$WORK/enet_transport.o" $ENET_OBJS "$WORK/encode.o" "$WORK/decode.o" \
	"$WORK/content-manifest.o" "$WORK/session_client.o" \
	-Wl,--wrap=sendmsg -lm > "$WORK/fixture-build.out" 2>&1 || \
	fail "fixture link failed (see $WORK/fixture-build.out)"

# ---------------------------------------------------------------------------
# Run the profile matrix
# ---------------------------------------------------------------------------
run_one() { # mode drones maze settings trace label
	local mode="$1" drones="$2" maze="$3" settings="$4" trace="$5" label="$6"
	local out="$WORK/$label.out"

	"$WORK/playable_fixture" "$mode" "$REPO" "$maze" "$settings" "$trace" \
		"$drones" > "$out" 2>&1 || { cat "$out" >&2; \
		fail "$label failed"; }
	grep -q 'PLAYABLE FIXTURE PASS' "$out" || fail "$label did not PASS"
	log "    $label: PASS"
}

log "== live profiles"
for maze in Arena combat1; do
	case "$maze" in
	  Arena)   key=arena   ;;
	  combat1) key=combat1 ;;
	esac
	settings="$FIX/p3.12-$key.settings"
	trace="$FIX/p3.12-$key.trace"
	[ -f "$settings" ] && [ -f "$trace" ] || fail "missing fixtures for $maze"
	for d in 0 1 2; do
		run_one solo "$d" "$maze" "$settings" "$trace" "solo-$key-$d"
	done
	for d in 0 1 2; do
		run_one duo "$d" "$maze" "$settings" "$trace" "duo-$key-$d"
	done
done

log ""
log "P3.12 PLAYABLE PASS (solo/duo x 0/1/2 drones x both mazes; start/finish/"
log "                   reset; natural 1000-point win; four-entry results)"
