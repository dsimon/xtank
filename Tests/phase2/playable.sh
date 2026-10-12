#!/usr/bin/env bash
#
# playable.sh -- P2.14 playable-client integration acceptance.
#
# Contract (docs/PHASE2-PLAN.md P2.14; task contract):
#   bash Tests/phase2/playable.sh [--evidence DIR]
#
# Proves the live client frame loop wires the P2.8 session protocol, the P2.11
# snapshot model, the P2.12 input controller and the P2.10/P2.13 renderer,
# HUD/map/radar and menu into a single nonblocking playable slice against a real
# in-process ENet server:
#
#   connect -> handshake -> lobby -> ready -> start -> 60 ticks of play with
#   input flowing to the server -> results -> reset -> clean disconnect
#
# It builds the display-free server core, the P2.8 session, the P2.7 admission
# layer, the pinned ENet transport, the frozen codec and the P2.14 client
# modules, then runs the fixture twice and requires byte-identical output.  It
# also proves (statically and by nm) that the client session/frame modules link
# no simulation, no SDL and no ENet symbol directly, and (optionally) that the
# shipped CMake client still builds.
#
# All generated binaries/logs live OUTSIDE the checkout.
#
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HARNESS="$REPO/Tests/phase2"
PROTO="$REPO/Protocol"
EVIDENCE="${XTANK_P2_EVIDENCE:-/tmp/opencode/xtank-phase2/p2.14/playable}"

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

# ---------------------------------------------------------------------------
# Dependencies: SDL3 (pinned P2.9 build) and ENet (pinned v1.3.18)
# ---------------------------------------------------------------------------
SDL_PREFIX="${XTK_SDL3_LINUX_PREFIX:-}"
if [ -z "$SDL_PREFIX" ] || [ ! -f "$SDL_PREFIX/include/SDL3/SDL.h" ]; then
	for cand in \
	    /tmp/opencode/xtank-phase2/p2.9/deps/linux-prefix \
	    /home/hatch/workspace/coding/xtank-evidence/p2.9/deps/linux-prefix; do
		if [ -f "$cand/include/SDL3/SDL.h" ]; then
			SDL_PREFIX="$cand"
			break
		fi
	done
fi
[ -n "$SDL_PREFIX" ] && [ -f "$SDL_PREFIX/include/SDL3/SDL.h" ] || \
	fail "SDL3 prefix not found (set XTK_SDL3_LINUX_PREFIX)"
log "== SDL3 prefix: $SDL_PREFIX"

ENET_DIR="${XTANK_ENET_DIR:-}"
if [ -z "$ENET_DIR" ] || [ ! -f "$ENET_DIR/include/enet/enet.h" ]; then
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
# Static boundary checks: the client session/frame modules are pure C
# ---------------------------------------------------------------------------
log "== boundary: session_client/frame link no simulation, SDL or ENet"
for f in Client/session_client.c Client/session_client.h \
	 Client/frame.c Client/frame.h; do
	safe=$(printf '%s' "$f" | tr '/' '_')
	if grep -nE '#[[:space:]]*include[[:space:]]*<(SDL3|X11|Xlib|enet)|xtank\.h|server\.h|server_view\.h|server_events\.h|intent\.h|play_game|intent_push|swapcontext|XOpenDisplay|real_map|actual_vehicles' \
		"$REPO/$f" > "$WORK/scan-$safe.out" 2>&1; then
		cat "$WORK/scan-$safe.out" >&2
		fail "$f references SDL/X11/ENet or simulation state"
	fi
done
log "    session_client/frame are SDL/X/ENet/simulation-free"

# Only the transport implementation may include ENet; the P2.14 session/frame
# modules and the P2.9 shell (main.c) do not use ENet directly.
if grep -nE '#[[:space:]]*include[[:space:]]*<enet' \
	"$REPO/Client/session_client.c" "$REPO/Client/session_client.h" \
	"$REPO/Client/frame.c" "$REPO/Client/frame.h" > "$WORK/scan-enet.out" 2>&1; then
	cat "$WORK/scan-enet.out" >&2
	fail "session_client/frame include ENet directly"
fi

# ---------------------------------------------------------------------------
# Build the shipped core + the opt-in network pieces
# ---------------------------------------------------------------------------
log "== building the shipped server core (make phase1-server phase1-core)"
( cd "$REPO" && make -s phase1-server phase1-core ) > "$WORK/build.out" 2>&1 || \
	fail "server/core build failed (see $WORK/build.out)"
CORE_OBJS=$(ls "$REPO"/Src/sobj/*.o | grep -v '/server_main.o$')
[ -n "$CORE_OBJS" ] || fail "no core objects"

log "== compiling session, admission, transport, codec and client modules"
compile() { # src out extra...
	local src="$1"; local out="$2"; shift 2
	gcc -O0 -g -w $DEFS -DXTANK_WITH_NET -I"$REPO/Src" -I"$REPO/Src/Include" \
		-I"$REPO/Protocol" -I"$REPO/Transport" -I"$REPO/Client" \
		-I"$ENET_DIR/include" "$@" -c "$src" -o "$out" \
		>> "$WORK/compile.out" 2>&1 || fail "compile failed: $src"
}
: > "$WORK/compile.out"
compile "$REPO/Src/server_session.c" "$WORK/server_session.o"
compile "$REPO/Src/server_net.c" "$WORK/server_net.o"
compile "$REPO/Transport/enet_transport.c" "$WORK/enet_transport.o"
compile "$REPO/Protocol/encode.c" "$WORK/encode.o"
compile "$REPO/Protocol/decode.c" "$WORK/decode.o"
compile "$REPO/Protocol/content-manifest.c" "$WORK/content-manifest.o"

for f in session_client.c frame.c model.c timeline.c camera.c input.c \
	 hud.c map.c radar.c menu.c world_render.c assets.c render_sdl.c; do
	gcc -O0 -g -w -I"$PROTO" -I"$REPO/Transport" -I"$REPO/Client" \
		-I"$SDL_PREFIX/include" \
		-c "$REPO/Client/$f" -o "$WORK/${f%.c}.o" \
		>> "$WORK/compile.out" 2>&1 || fail "client compile failed: $f"
done

# nm boundary: the pure session/frame objects have no simulation/SDL/ENet symbol.
if nm -u "$WORK/frame.o" "$WORK/session_client.o" 2>/dev/null | \
	grep -qE 'SDL_|enet_|play_game|server_(init|step|match|loop)|animate|do_special|intent_push'; then
	fail "frame/session_client reference SDL/ENet/simulation"
fi
log "    frame.o/session_client.o link no SDL/ENet/simulation symbol"

ENET_SRCS="callbacks.c compress.c host.c list.c packet.c peer.c protocol.c unix.c"
ENET_OBJS=""
for f in $ENET_SRCS; do
	o="$WORK/enet-${f%.c}.o"
	gcc -O0 -g -w -I"$ENET_DIR/include" -c "$ENET_DIR/$f" -o "$o" \
		>> "$WORK/enet-build.out" 2>&1 || \
		fail "ENet build failed ($f; see $WORK/enet-build.out)"
	ENET_OBJS="$ENET_OBJS $o"
done

# ---------------------------------------------------------------------------
# Asset bundle (the renderer draws the original visuals)
# ---------------------------------------------------------------------------
log "== building the portable asset exporter and bundle"
gcc -O0 -g -w -I "$REPO/Src/Include" -I "$REPO/Src/Display" \
	-I "$PROTO" -I "$REPO/Client" \
	"$REPO/Tools/export-client-assets.c" "$REPO/Client/assets.c" \
	-o "$WORK/export-client-assets" -lm >> "$WORK/compile.out" 2>&1 || \
	fail "exporter build failed"
"$WORK/export-client-assets" --out "$WORK/assets.bin" \
	--geometry "$WORK/export.geom" --manifest "$WORK/manifest.txt" \
	> "$WORK/export.out" 2>&1 || fail "exporter run failed"
[ -s "$WORK/assets.bin" ] || fail "asset bundle is empty"

# ---------------------------------------------------------------------------
# Link and run the fixture twice (determinism)
# ---------------------------------------------------------------------------
log "== linking the playable fixture"
# shellcheck disable=SC2086
gcc -O0 -g -w $DEFS -DXTANK_WITH_NET -I"$REPO/Src" -I"$REPO/Src/Include" \
	-I"$REPO/Protocol" -I"$REPO/Transport" -I"$REPO/Client" \
	-I"$ENET_DIR/include" -I"$SDL_PREFIX/include" \
	-DXTANK_DIR="\"$REPO/Src/..\"" -DALLDEFINES="\"p2.14 playable fixture\"" \
	-o "$WORK/playable_fixture" "$HARNESS/playable_fixture.c" \
	$CORE_OBJS "$WORK/server_session.o" "$WORK/server_net.o" \
	"$WORK/enet_transport.o" $ENET_OBJS "$WORK/encode.o" "$WORK/decode.o" \
	"$WORK/content-manifest.o" \
	"$WORK/frame.o" "$WORK/session_client.o" "$WORK/model.o" \
	"$WORK/timeline.o" "$WORK/camera.o" "$WORK/input.o" "$WORK/hud.o" \
	"$WORK/map.o" "$WORK/radar.o" "$WORK/menu.o" "$WORK/world_render.o" \
	"$WORK/assets.o" "$WORK/render_sdl.o" \
	-L"$SDL_PREFIX/lib" -lSDL3 -Wl,--wrap=sendmsg -lm \
	> "$WORK/fixture-build.out" 2>&1 || \
	fail "fixture link failed (see $WORK/fixture-build.out)"

run_fixture() {
	LD_LIBRARY_PATH="$SDL_PREFIX/lib" SDL_VIDEODRIVER=dummy \
		"$WORK/playable_fixture" "$WORK/assets.bin"
}

log "== running the playable fixture (twice)"
run_fixture > "$WORK/fixture1.out" 2>&1 || \
	{ cat "$WORK/fixture1.out" >&2; fail "fixture run 1 failed"; }
run_fixture > "$WORK/fixture2.out" 2>&1 || \
	{ cat "$WORK/fixture2.out" >&2; fail "fixture run 2 failed"; }
grep -q 'PLAYABLE FIXTURE PASS' "$WORK/fixture1.out" || \
	{ cat "$WORK/fixture1.out" >&2; fail "fixture did not report PASS"; }
cmp "$WORK/fixture1.out" "$WORK/fixture2.out" >/dev/null || \
	fail "fixture is not deterministic (two runs differ)"
log "    $(grep -m1 'PLAYABLE FIXTURE PASS' "$WORK/fixture1.out")"
log "    two fixture runs byte-identical"

# ---------------------------------------------------------------------------
# The shipped CMake client must still build with the new modules.
# ---------------------------------------------------------------------------
if [ -z "${XTK_SKIP_CLIENT_BUILD:-}" ]; then
	log "== building the shipped client (make phase2-client-linux)"
	( cd "$REPO" && make -s phase2-client-linux \
	    XTK_SDL3_LINUX_PREFIX="$SDL_PREFIX" XTK_ENET_DIR="$ENET_DIR" \
	    XTK_CLIENT_BUILD="$EVIDENCE/build/root" ) \
		> "$WORK/client-build.out" 2>&1 || \
		fail "shipped client build failed (see $WORK/client-build.out)"
	[ -x "$EVIDENCE/build/root/linux/Client/xtank-client" ] || \
		fail "xtank-client binary missing after build"
	log "    xtank-client built"
fi

# ---------------------------------------------------------------------------
log ""
log "P2.14 PLAYABLE PASS (connect/handshake/lobby/ready/start; 60-tick play"
log "                   with 15 Hz input never on tick 0; interpolated model"
log "                   rendered to world + HUD/map/radar; defocus/pause/quit"
log "                   release; win RESULTS/banner; epoch-2 reset; clean"
log "                   disconnect; unavailable server yields no session)"
