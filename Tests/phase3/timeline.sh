#!/usr/bin/env bash
#
# timeline.sh -- P3.11 lifetime-safe presentation acceptance.
#
# Contract (docs/PHASE3-PLAN.md P3.11):
#   bash Tests/phase3/timeline.sh [--evidence DIR]
#
# Builds the pure-C client model/timeline/camera fixture and runs it twice,
# requiring byte-identical output.  Proves: u32 full entity keys (>255 vehicle
# lives never blend); same-owner bullet reuse and same-kind explosion reuse are
# swapped by their own life generation, never interpolated across lives; loss/
# reorder/death/teleport/departure/epoch replay at 30/60/144 FPS yields the same
# pose with no stale ghost and no extrapolation; a reviewed sidecar capture
# distinguishes same-tick reuse by full key; and the approved one-tick buffer,
# camera viewport and world scale are unchanged.  It also proves the
# model/timeline/camera link no simulation, no X11 and no SDL.
#
# All generated binaries/logs live OUTSIDE the checkout.
#
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HARNESS="$REPO/Tests/phase3"
PROTO="$REPO/Protocol"
EVIDENCE="${XTANK_P3_EVIDENCE:-/tmp/opencode/xtank-phase3/p3.11/timeline}"

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

# ---------------------------------------------------------------------------
# 1. Build and run the fixture (twice, determinism)
# ---------------------------------------------------------------------------
log "== building the lifetime-safe model fixture (model + timeline + camera)"
gcc -O0 -g -Wall -Wextra -I "$PROTO" -I "$REPO/Client" \
	"$HARNESS/timeline_fixture.c" \
	"$REPO/Client/model.c" "$REPO/Client/timeline.c" \
	"$REPO/Client/camera.c" \
	-o "$WORK/timeline_fixture" -lm \
	> "$WORK/fixture-build.out" 2>&1 || \
	fail "fixture build failed (see $WORK/fixture-build.out)"

run_fixture() { "$WORK/timeline_fixture"; }

run_fixture > "$WORK/fixture1.out" 2>&1 || \
	{ cat "$WORK/fixture1.out" >&2; fail "fixture run 1 failed"; }
run_fixture > "$WORK/fixture2.out" 2>&1 || \
	{ cat "$WORK/fixture2.out" >&2; fail "fixture run 2 failed"; }
grep -q 'P3.11 TIMELINE FIXTURE PASS' "$WORK/fixture1.out" || \
	{ cat "$WORK/fixture1.out" >&2; fail "fixture did not report PASS"; }
cmp "$WORK/fixture1.out" "$WORK/fixture2.out" >/dev/null || \
	fail "fixture is not deterministic (two runs differ)"
log "    $(grep -m1 'checks=' "$WORK/fixture1.out")"
log "    two fixture runs byte-identical"

# ---------------------------------------------------------------------------
# 2. Reviewed sidecar capture distinguishes same-tick reuse by full key
# ---------------------------------------------------------------------------
log "== reviewing the full-key sidecar capture"
grep '^CAP ' "$WORK/fixture1.out" > "$WORK/capture.txt"
[ -s "$WORK/capture.txt" ] || fail "no sidecar capture produced"
# The reused vehicle slot carries two distinct generations; the reused bullet
# and explosion carry their own life generation on the same id.
grep -q 'CAP vehicle slot=0 gen=300' "$WORK/capture.txt" || \
	fail "capture missing old vehicle life"
grep -q 'CAP vehicle slot=0 gen=301' "$WORK/capture.txt" || \
	fail "capture missing new vehicle life (>255)"
grep -q 'CAP bullet id=7 owner=0 ogen=300 gen=10' "$WORK/capture.txt" || \
	fail "capture missing reused bullet old life"
grep -q 'CAP bullet id=7 owner=0 ogen=300 gen=11' "$WORK/capture.txt" || \
	fail "capture missing reused bullet new life"
grep -q 'CAP explosion id=3 gen=21' "$WORK/capture.txt" || \
	fail "capture missing reused explosion life"
if grep 'CAP bullet id=7' "$WORK/capture.txt" | \
	awk '{for(i=1;i<=NF;i++) if($i ~ /^gen=/){split($i,a,"=");print a[2]}}' \
	| sort -u | wc -l | grep -qx 2; then
	:
else
	fail "capture does not distinguish the reused bullet by generation"
fi
log "    capture records distinct generations for each reused key"

# ---------------------------------------------------------------------------
# 3. Boundary checks: the client model is simulation-free, X-free and SDL-free
# ---------------------------------------------------------------------------
log "== boundary checks (client model is simulation/X/SDL free)"
for f in Client/model.c Client/model.h Client/timeline.c Client/timeline.h \
	 Client/camera.c Client/camera.h; do
	safe=$(printf '%s' "$f" | tr '/' '_')
	if grep -nE '#[[:space:]]*include[[:space:]]*<(X11|Xlib|SDL3)|XOpenDisplay|swapcontext|getcontext|ucontext\.h|server_(init|step|match)|play_game|animate\(' \
		"$REPO/$f" > "$WORK/scan-$safe.out" 2>&1; then
		cat "$WORK/scan-$safe.out" >&2
		fail "$f references X11/SDL/simulation"
	fi
done
log "    model/timeline/camera are X-free and simulation-free"

for f in model timeline camera; do
	gcc -O0 -g -w -I "$PROTO" -I "$REPO/Client" \
		-c "$REPO/Client/$f.c" -o "$WORK/$f.o"
done
if nm -u "$WORK/model.o" "$WORK/timeline.o" "$WORK/camera.o" 2>/dev/null \
	| grep -qE 'play_game|server_(init|step|match|loop)|animate|display_vehicle|draw_picture|do_special|RNG|rand'; then
	fail "client model references a simulation/legacy symbol"
fi
log "    model/timeline/camera link no simulation or legacy symbol"

# The approved timing policy is unchanged: one-tick buffer, eight-snapshot ring.
grep -q '#define XTK_MODEL_BUFFER_TICKS 1.0' "$REPO/Client/model.h" || \
	fail "approved one-tick interpolation buffer changed"
grep -q '#define XTK_MODEL_RING XTK_SNAPSHOT_RING' "$REPO/Client/model.h" || \
	fail "bounded snapshot ring changed"
log "    one-tick buffer and bounded snapshot ring unchanged"

# ---------------------------------------------------------------------------
log ""
log "P3.11 TIMELINE PASS (u32 full entity keys; >255 lives and reused bullet/"
log "                    explosion slots never blend; FPS-independent replay"
log "                    with no ghosts/extrapolation; sidecar capture; "
log "                    approved timing policy unchanged)"
