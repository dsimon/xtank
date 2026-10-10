#!/usr/bin/env bash
#
# geometry.sh -- P1.10 acceptance harness for headless geometry initialization.
#
# Contract (PHASE1-PLAN.md, P1.10):
#   bash Tests/phase1/geometry.sh
#
# Proves that body ordering, all 16 vehicle rotations, picture bounds/offsets,
# turret coordinates and collision segments are initialized without any video
# or pixmap involvement, and that the X front-end build and the display-free
# server build produce the exact same explicit geometry table.
#
# It builds both shipped binaries, dumps the geometry table from the X backend
# (under a harness-owned Xvfb, using the X pixmap upload path) and from the
# null/server backend (no DISPLAY), requires byte identity, checks the frozen
# golden and independently re-derives every rotated view's bounds/offsets from
# the base views.  Marauder (the Vanguard body) is checked explicitly.
#
# Evidence lives OUTSIDE the checkout under
#   ${XTANK_P1_EVIDENCE:-/tmp/opencode/xtank-phase1/p1.10}/geometry
#
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HARNESS="$REPO/Tests/phase1"
EVIDENCE="${XTANK_P1_EVIDENCE:-/tmp/opencode/xtank-phase1/p1.10}"
GE="$EVIDENCE/geometry"
GOLDEN="$HARNESS/geometry.golden.sha256"

mkdir -p "$GE"

log()  { printf '%s\n' "$*"; }
fail() { printf 'GEOMETRY FAIL: %s\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------------------
# Xvfb lifecycle (own exactly one process; clean up only our own PID)
# ---------------------------------------------------------------------------
XDPY=""
XVFB_PID=""
cleanup() {
	local rc=$?
	if [ -n "$XVFB_PID" ]; then kill "$XVFB_PID" 2>/dev/null || true; fi
	exit $rc
}
trap cleanup EXIT INT TERM

display_free() {
	[ -e "/tmp/.X$1-lock" ] && return 1
	[ -e "/tmp/.X11-unix/X$1" ] && return 1
	pgrep -f "Xvfb :$1( |$)" >/dev/null 2>&1 && return 1
	return 0
}

ensure_xvfb() {
	if [ -n "${XTANK_GEOMETRY_DISPLAY:-}" ]; then
		XDPY="${XTANK_GEOMETRY_DISPLAY#:}"
		DISPLAY=":$XDPY" xdpyinfo >/dev/null 2>&1 || \
		    fail "XTANK_GEOMETRY_DISPLAY=:$XDPY is not usable"
		return
	fi
	local d
	for d in 97 96 95 94 93 92 91 90 89 88; do
		if display_free "$d"; then XDPY="$d"; break; fi
	done
	[ -n "$XDPY" ] || fail "no free X display in :88-:97"

	setsid Xvfb ":$XDPY" -screen 0 1024x864x24 -nolisten tcp -noreset \
	    </dev/null >"$GE/xvfb-$XDPY.log" 2>&1 &
	XVFB_PID=$!
	echo "$XVFB_PID" > "$GE/xvfb.pid"

	local i
	for i in $(seq 1 50); do
		kill -0 "$XVFB_PID" 2>/dev/null || \
		    fail "Xvfb :$XDPY exited during startup (see $GE/xvfb-$XDPY.log)"
		if DISPLAY=":$XDPY" xdpyinfo >/dev/null 2>&1; then
			log "== started Xvfb :$XDPY (pid $XVFB_PID)"
			return
		fi
		sleep 0.2
	done
	fail "Xvfb :$XDPY did not become ready"
}

# ---------------------------------------------------------------------------
# 1. Build both shipped binaries
# ---------------------------------------------------------------------------
log "== building X front-end and null/server backend"
( cd "$REPO" && make ) >"$GE/make.log" 2>&1 || fail "make failed (see $GE/make.log)"

X_BIN="$REPO/Src/xtank"
N_BIN="$REPO/Src/xtank-headless"
[ -x "$X_BIN" ] || fail "missing $X_BIN"
[ -x "$N_BIN" ] || fail "missing $N_BIN"

# ---------------------------------------------------------------------------
# 2. Server geometry table (no display, no X connection)
# ---------------------------------------------------------------------------
log "== dumping server geometry table (DISPLAY unset)"
env -u DISPLAY XTANK_GEOMETRY_DUMP="$GE/server.geom" XTANK_GEOMETRY_EXIT=1 \
    timeout 60 "$N_BIN" >"$GE/server.stdout" 2>&1 || \
    fail "server geometry dump failed (see $GE/server.stdout)"
[ -s "$GE/server.geom" ] || fail "server geometry table is empty"

# ---------------------------------------------------------------------------
# 3. X geometry table (real X backend, pixmap upload path)
# ---------------------------------------------------------------------------
ensure_xvfb
log "== dumping X geometry table (DISPLAY=:$XDPY)"
DISPLAY=":$XDPY" XTANK_GEOMETRY_DUMP="$GE/x.geom" XTANK_GEOMETRY_EXIT=1 \
    timeout 60 "$X_BIN" >"$GE/x.stdout" 2>&1 || \
    fail "X geometry dump failed (see $GE/x.stdout)"
[ -s "$GE/x.geom" ] || fail "X geometry table is empty"

# ---------------------------------------------------------------------------
# 4. Byte identity: X == server
# ---------------------------------------------------------------------------
if ! cmp "$GE/x.geom" "$GE/server.geom" >/dev/null; then
	firstdiff=$(cmp "$GE/x.geom" "$GE/server.geom" 2>&1 | head -1)
	fail "X and server geometry tables differ ($firstdiff)"
fi
log "== cmp: X geometry == server geometry (byte identical)"

# ---------------------------------------------------------------------------
# 5. Frozen golden (detects content conversion or formula drift)
# ---------------------------------------------------------------------------
[ -f "$GOLDEN" ] || fail "missing golden $GOLDEN"
want=$(sed -n 's/^table //p' "$GOLDEN")
got=$(sha256sum "$GE/x.geom" | awk '{print $1}')
if [ "${XTANK_P1_REGEN_GEOMETRY_GOLDEN:-0}" = "1" ]; then
	log "== regenerating golden (was $want, now $got)"
	sed -i "s/^table .*/table $got/" "$GOLDEN"
elif [ "$want" != "$got" ]; then
	fail "geometry table sha256 $got != frozen $want"
else
	log "== golden sha256 $got matches"
fi

# ---------------------------------------------------------------------------
# 6. Structural checks
# ---------------------------------------------------------------------------
F="$GE/x.geom"
head -1 "$F" | grep -q '^GEOMETRY-TABLE v1$' || fail "bad table header"
grep -q '^count vehicle 14 turret 1 explosion 10 bullet 25 landmark 3 random 3$' "$F" || \
    fail "object counts do not match the legacy ordering"

nv=$(grep -c '^obj vehicle ' "$F" || true)
[ "$nv" = "14" ] || fail "expected 14 vehicle bodies, found $nv"
bad16=$(awk '/^obj vehicle /{if ($7!=16) print $5}' "$F" || true)
[ -z "$bad16" ] || fail "vehicle bodies without 16 views: $bad16"

# Marauder is body index 6 and the body used by Vehicles/Vanguard.V.
grep -q '^obj vehicle 6 type marauder pics 16 turrets 2 segs 4$' "$F" || \
    fail "Marauder body table is not the expected 16 views / 2 turrets / 4 segs"
vbody=$(awk -F': *' '/^Body:/{print $2; exit}' "$REPO/Vehicles/Vanguard.V")
[ -n "$vbody" ] || fail "could not read Vanguard body"
grep -qi "^obj vehicle .* type $vbody " "$F" || \
    fail "Vanguard body '$vbody' has no geometry table"

# Turret coordinates and collision segments must be present for Marauder.
mar_rot67=$(awk '/^obj vehicle 6 /{f=1} f&&/^obj vehicle 7 /{exit} f&&/^  pic /{c++} END{print c}' "$F")
[ "$mar_rot67" = "16" ] || fail "Marauder does not carry 16 picinfo rotations"
grep -A40 '^obj vehicle 6 ' "$F" | grep -q '^    turret 0 ' || \
    fail "Marauder turret coordinates missing"
grep -A40 '^obj vehicle 6 ' "$F" | grep -q '^    seg 0 ' || \
    fail "Marauder collision segments missing"

# ---------------------------------------------------------------------------
# 7. Independent rotation re-derivation (all 14 vehicles, all 16 views)
# ---------------------------------------------------------------------------
awk '
function verify(   r, bad) {
  if (!isveh) return;
  for (r=0;r<4;r++)
    if (w[r+4]!=h[r]||h[r+4]!=w[r]||ox[r+4]!=h[r]-oy[r]-1||oy[r+4]!=ox[r]) {
      print "ROT90 mismatch " name " view " r; bad=1
    }
  for (r=0;r<8;r++)
    if (w[r+8]!=w[r]||h[r+8]!=h[r]||ox[r+8]!=w[r]-ox[r]-1||oy[r+8]!=h[r]-oy[r]-1) {
      print "ROT180 mismatch " name " view " r; bad=1
    }
  if (np!=16) { print "pics!=16 " name; bad=1 }
  checked++
  if (bad) exit 3
}
/^obj vehicle/ { verify(); delete w;delete h;delete ox;delete oy; np=0; isveh=1; name=$5; next }
/^obj / { verify(); isveh=0; next }
isveh && /^  pic / { r=$2; w[r]=$4; h[r]=$6; ox[r]=$8; oy[r]=$10; np++ }
END { verify(); print "rotation-checked-vehicles=" checked }
' "$F" > "$GE/rotation.check" || fail "rotation re-derivation failed (see $GE/rotation.check)"
grep -q '^rotation-checked-vehicles=14$' "$GE/rotation.check" || \
    fail "rotation re-derivation did not cover all 14 vehicles"
log "== $(cat "$GE/rotation.check")"

log ""
log "P1.10 GEOMETRY PASS (X == server; golden $got)"
