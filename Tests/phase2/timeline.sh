#!/usr/bin/env bash
#
# timeline.sh -- P2.11 snapshot model, camera and interpolation acceptance.
#
# Contract (docs/PHASE2-PLAN.md P2.11):
#   bash Tests/phase2/timeline.sh [--evidence DIR]
#
# Proves that the bounded authorized client model, its monotonic render
# timeline and the one-tick interpolation policy (D2-E) replay recorded views
# correctly at 30/60/144 FPS; that loss/reordering, underrun, teleport,
# death/respawn, hidden entities and epoch reset show no stale ghosts, no
# blending across lifetimes and no extrapolation; and that the camera follows
# the own displayed pose with a fixed logical viewport/world scale.  It also
# proves the model/timeline/camera link no simulation, no X11 and no SDL.
#
# All generated binaries/bundles/logs live OUTSIDE the checkout.
#
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HARNESS="$REPO/Tests/phase2"
PROTO="$REPO/Protocol"
EVIDENCE="${XTANK_P2_EVIDENCE:-/tmp/opencode/xtank-phase2/p2.11/timeline}"

while [ $# -gt 0 ]; do
	case "$1" in
	  --evidence) EVIDENCE="$2"; shift 2 ;;
	  *) echo "usage: timeline.sh [--evidence DIR]" >&2; exit 2 ;;
	esac
done

mkdir -p "$EVIDENCE"
WORK="$EVIDENCE/work"
mkdir -p "$WORK"

log()  { printf '%s\n' "$*"; }
fail() { printf 'TIMELINE FAIL: %s\n' "$*" >&2; exit 1; }

# ---- SDL3 (pinned P2.9 build) ---------------------------------------------
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

# ---------------------------------------------------------------------------
# 1. Asset bundle (the renderer needs the original visuals)
# ---------------------------------------------------------------------------
log "== building the portable asset exporter and bundle"
gcc -O0 -g -w -I "$REPO/Src/Include" -I "$REPO/Src/Display" \
	-I "$PROTO" -I "$REPO/Client" \
	"$REPO/Tools/export-client-assets.c" "$REPO/Client/assets.c" \
	-o "$WORK/export-client-assets" -lm > "$WORK/export-build.out" 2>&1 || \
	fail "exporter build failed (see $WORK/export-build.out)"
"$WORK/export-client-assets" --out "$WORK/assets.bin" \
	--geometry "$WORK/export.geom" --manifest "$WORK/manifest.txt" \
	> "$WORK/export.out" 2>&1 || fail "exporter run failed"
[ -s "$WORK/assets.bin" ] || fail "asset bundle is empty"
log "    $(cat "$WORK/export.out")"

# ---------------------------------------------------------------------------
# 2. Build and run the timeline fixture (twice, determinism)
# ---------------------------------------------------------------------------
log "== building the timeline fixture"
gcc -O0 -g -Wall -Wextra -I "$PROTO" -I "$REPO/Client" \
	-I "$SDL_PREFIX/include" \
	"$HARNESS/timeline_fixture.c" \
	"$REPO/Client/model.c" "$REPO/Client/timeline.c" \
	"$REPO/Client/camera.c" "$REPO/Client/world_render.c" \
	"$REPO/Client/assets.c" "$REPO/Client/render_sdl.c" \
	-o "$WORK/timeline_fixture" -L "$SDL_PREFIX/lib" -lSDL3 -lm \
	> "$WORK/fixture-build.out" 2>&1 || \
	fail "fixture build failed (see $WORK/fixture-build.out)"

run_fixture() {
	LD_LIBRARY_PATH="$SDL_PREFIX/lib" SDL_VIDEODRIVER=dummy \
		"$WORK/timeline_fixture" "$WORK/assets.bin"
}

run_fixture > "$WORK/fixture1.out" 2>&1 || \
	{ cat "$WORK/fixture1.out" >&2; fail "fixture run 1 failed"; }
run_fixture > "$WORK/fixture2.out" 2>&1 || \
	{ cat "$WORK/fixture2.out" >&2; fail "fixture run 2 failed"; }
grep -q 'TIMELINE FIXTURE PASS' "$WORK/fixture1.out" || \
	{ cat "$WORK/fixture1.out" >&2; fail "fixture did not report PASS"; }
cmp "$WORK/fixture1.out" "$WORK/fixture2.out" >/dev/null || \
	fail "fixture is not deterministic (two runs differ)"
log "    $(grep -m1 'checks=' "$WORK/fixture1.out")"
log "    two fixture runs byte-identical"

# ---------------------------------------------------------------------------
# 3. Boundary checks: the client model is simulation-free, X-free and SDL-free
# ---------------------------------------------------------------------------
log "== boundary checks (client model is simulation/X/SDL free)"
for f in Client/model.c Client/model.h Client/timeline.c Client/timeline.h \
	 Client/camera.c Client/camera.h Client/world_render.c; do
	safe=$(printf '%s' "$f" | tr '/' '_')
	if grep -nE '#[[:space:]]*include[[:space:]]*<(X11|Xlib|SDL3)|XOpenDisplay|swapcontext|getcontext|ucontext\.h|server_(init|step|match)|play_game|animate\(' \
		"$REPO/$f" > "$WORK/scan-$safe.out" 2>&1; then
		cat "$WORK/scan-$safe.out" >&2
		fail "$f references X11/SDL/simulation"
	fi
done
log "    model/timeline/camera/world_render are X-free and simulation-free"

# The pure model/timeline/camera objects must have no undefined engine symbol.
for f in model timeline camera; do
	gcc -O0 -g -w -I "$PROTO" -I "$REPO/Client" \
		-c "$REPO/Client/$f.c" -o "$WORK/$f.o"
done
if nm -u "$WORK/model.o" "$WORK/timeline.o" "$WORK/camera.o" 2>/dev/null \
	| grep -qE 'play_game|server_(init|step|match|loop)|animate|display_vehicle|draw_picture|do_special|RNG|rand'; then
	fail "client model references a simulation/legacy symbol"
fi
log "    model/timeline/camera link no simulation or legacy symbol"

# ---------------------------------------------------------------------------
log ""
log "P2.11 TIMELINE PASS (bounded model; monotonic timeline; one-tick"
log "                   interpolation at 30/60/144 FPS; snap on teleport/death/"
log "                   respawn; no ghosts/under-run extrapolation; reset)"