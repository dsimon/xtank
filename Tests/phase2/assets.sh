#!/usr/bin/env bash
#
# assets.sh -- P2.10 original visual assets and full-frame drawing acceptance.
#
# Contract (docs/PHASE2-PLAN.md P2.10):
#   bash Tests/phase2/assets.sh [--evidence DIR]
#
# Proves that the portable checked export of the original body/turret/
# projectile/explosion and landmark visuals (plus the palette/font and the 16
# body views) is byte-identical to the frozen X/server geometry evidence, that
# the client's loaded bundle reproduces that geometry, that Marauder (the
# Vanguard body) is preserved, and that the snapshot-driven renderer draws the
# art at the original scale with turret placement and no trails after a
# resize/expose.  It also proves the bundle is checksum-checked and that the
# client renderer links no simulation and no X11.
#
# All generated binaries/logs/bundles live OUTSIDE the checkout.
#
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HARNESS="$REPO/Tests/phase2"
PROTO="$REPO/Protocol"
EVIDENCE="${XTANK_P2_EVIDENCE:-/tmp/opencode/xtank-phase2/p2.10/assets}"
GOLDEN="$REPO/Tests/phase1/geometry.golden.sha256"

while [ $# -gt 0 ]; do
	case "$1" in
	  --evidence) EVIDENCE="$2"; shift 2 ;;
	  *) echo "usage: assets.sh [--evidence DIR]" >&2; exit 2 ;;
	esac
done

mkdir -p "$EVIDENCE"
WORK="$EVIDENCE/work"
mkdir -p "$WORK"

log()  { printf '%s\n' "$*"; }
fail() { printf 'ASSETS FAIL: %s\n' "$*" >&2; exit 1; }

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
# 1. Build and run the exporter twice (determinism), dump geometry + manifest
# ---------------------------------------------------------------------------
log "== building the portable asset exporter"
gcc -O0 -g -w -I "$REPO/Src/Include" -I "$REPO/Src/Display" \
	-I "$PROTO" -I "$REPO/Client" \
	"$REPO/Tools/export-client-assets.c" "$REPO/Client/assets.c" \
	-o "$WORK/export-client-assets" -lm > "$WORK/export-build.out" 2>&1 || \
	fail "exporter build failed (see $WORK/export-build.out)"

mkdir -p "$WORK/run1" "$WORK/run2"
"$WORK/export-client-assets" --out "$WORK/run1/assets.bin" \
	--geometry "$WORK/run1/export.geom" --manifest "$WORK/run1/manifest.txt" \
	> "$WORK/run1/export.out" 2>&1 || fail "exporter run 1 failed"
"$WORK/export-client-assets" --out "$WORK/run2/assets.bin" \
	--geometry "$WORK/run2/export.geom" --manifest "$WORK/run2/manifest.txt" \
	> "$WORK/run2/export.out" 2>&1 || fail "exporter run 2 failed"
cmp "$WORK/run1/assets.bin" "$WORK/run2/assets.bin" >/dev/null || \
	fail "exporter is not deterministic (bundles differ)"
log "    $(cat "$WORK/run1/export.out")"
log "    two exporter runs byte-identical"

# ---------------------------------------------------------------------------
# 2. Frozen X/server geometry evidence
# ---------------------------------------------------------------------------
log "== building the display-free server and dumping geometry"
( cd "$REPO" && make phase1-headless ) > "$WORK/headless-build.out" 2>&1 || \
	fail "headless build failed (see $WORK/headless-build.out)"
N_BIN="$REPO/Src/xtank-headless"
[ -x "$N_BIN" ] || fail "missing $N_BIN"
env -u DISPLAY XTANK_GEOMETRY_DUMP="$WORK/server.geom" XTANK_GEOMETRY_EXIT=1 \
	timeout 60 "$N_BIN" > "$WORK/server.stdout" 2>&1 || \
	fail "server geometry dump failed (see $WORK/server.stdout)"
[ -s "$WORK/server.geom" ] || fail "server geometry table is empty"

if ! cmp "$WORK/run1/export.geom" "$WORK/server.geom" >/dev/null; then
	fail "exported geometry != frozen X/server geometry ($(cmp "$WORK/run1/export.geom" "$WORK/server.geom" 2>&1 | head -1))"
fi
log "    exported geometry == frozen X/server geometry (byte identical)"

# Frozen golden.
want=$(sed -n 's/^table //p' "$GOLDEN")
got=$(sha256sum "$WORK/run1/export.geom" | awk '{print $1}')
[ "$want" = "$got" ] || fail "geometry table sha256 $got != frozen $want"
log "    geometry table sha256 $got matches the P1.10 frozen golden"

# Marauder (Vanguard body) preserved.
grep -q '^obj vehicle 6 type marauder pics 16 turrets 2 segs 4$' "$WORK/server.geom" || \
	fail "Marauder geometry table not preserved"
vbody=$(awk -F': *' '/^Body:/{print $2; exit}' "$REPO/Vehicles/Vanguard.V")
[ -n "$vbody" ] || fail "could not read Vanguard body"
grep -qi "^obj vehicle .* type $vbody " "$WORK/server.geom" || \
	fail "Vanguard body '$vbody' missing from geometry table"
log "    Marauder preserved; Vanguard body '$vbody' present"

# ---------------------------------------------------------------------------
# 3. Client loader + snapshot-driven renderer fixture
# ---------------------------------------------------------------------------
log "== building and running the assets fixture (loader + SDL renderer)"
gcc -O0 -g -Wall -Wextra -I "$PROTO" -I "$REPO/Client" \
	-I "$SDL_PREFIX/include" \
	"$HARNESS/assets_fixture.c" "$REPO/Client/assets.c" \
	"$REPO/Client/render_sdl.c" \
	-o "$WORK/assets_fixture" -L "$SDL_PREFIX/lib" -lSDL3 -lm \
	> "$WORK/fixture-build.out" 2>&1 || \
	fail "fixture build failed (see $WORK/fixture-build.out)"

LD_LIBRARY_PATH="$SDL_PREFIX/lib" SDL_VIDEODRIVER=dummy \
	"$WORK/assets_fixture" "$WORK/run1/assets.bin" "$WORK/loaded.geom" \
	> "$WORK/fixture.out" 2>&1 || { cat "$WORK/fixture.out" >&2; fail "fixture failed"; }
grep -q 'ASSETS FIXTURE PASS' "$WORK/fixture.out" || \
	{ cat "$WORK/fixture.out" >&2; fail "fixture did not report PASS"; }
log "    $(grep -m1 'checks=' "$WORK/fixture.out")"

cmp "$WORK/loaded.geom" "$WORK/server.geom" >/dev/null || \
	fail "loaded bundle geometry != frozen X/server geometry"
log "    loaded bundle geometry == frozen X/server geometry (byte identical)"

# ---------------------------------------------------------------------------
# 4. Palette cross-check against the X11 rgb database (when available)
# ---------------------------------------------------------------------------
RGB="/etc/X11/rgb.txt"
if [ -r "$RGB" ]; then
	pal_check() { # name r g b
		awk -v n="$1" -v r="$2" -v g="$3" -v b="$4" \
		    'tolower($4)==tolower(n){ if ($1==r && $2==g && $3==b) found=1 } END{ exit(found?0:1) }' \
		    "$RGB" || fail "palette $1 not $2,$3,$4 in $RGB"
	}
	pal_check White  255 255 255
	pal_check Red    255 0   0
	pal_check Yellow 255 255 0
	pal_check Green  0   255 0
	pal_check orchid 218 112 214
	pal_check grey71 181 181 181
	pal_check cyan   0   255 255
	log "    palette RGB matches the X11 rgb database ($RGB)"
else
	log "    note: $RGB unavailable; palette RGB checked against x11.c only"
fi

# ---------------------------------------------------------------------------
# 5. Boundary checks: no X11, no simulation in the client renderer
# ---------------------------------------------------------------------------
log "== boundary checks (client renderer is X-free and simulation-free)"
for f in Client/render.h Client/assets.c Client/render_sdl.c \
	 Tools/export-client-assets.c; do
	safe=$(printf '%s' "$f" | tr '/' '_')
	if grep -nE '#[[:space:]]*include[[:space:]]*<(X11|Xlib)|XOpenDisplay|XCreateWindow|swapcontext|getcontext|ucontext\.h' \
		"$REPO/$f" > "$WORK/scan-$safe.out" 2>&1; then
		cat "$WORK/scan-$safe.out" >&2
		fail "$f references X11/ucontext"
	fi
done
log "    no X11 or ucontext references"

# render_sdl.o / assets.o must have no undefined engine symbol.
gcc -O0 -g -w -I "$PROTO" -I "$REPO/Client" -I "$SDL_PREFIX/include" \
	-c "$REPO/Client/render_sdl.c" -o "$WORK/render_sdl.o"
gcc -O0 -g -w -I "$PROTO" -I "$REPO/Client" \
	-c "$REPO/Client/assets.c" -o "$WORK/assets.o"
if nm -u "$WORK/render_sdl.o" "$WORK/assets.o" 2>/dev/null \
	| grep -qE 'play_game|server_(init|step|match)|animate|display_vehicle|draw_picture'; then
	fail "client renderer references a simulation/legacy symbol"
fi
log "    renderer/assets link no simulation or legacy renderer symbol"

# ---------------------------------------------------------------------------
log ""
log "P2.10 ASSETS PASS (exported geometry == frozen X/server; 16-view rotation"
log "                 indexing; Marauder/Vanguard preserved; checked bundle;"
log "                 exact scale/art + turret placement; no trails; XOR)"
