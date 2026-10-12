#!/usr/bin/env bash
#
# ui.sh -- P2.13 HUD/map/radar/menu acceptance.
#
# Contract (docs/PHASE2-PLAN.md P2.13):
#   bash Tests/phase2/ui.sh [--evidence DIR]
#
# Builds the standalone, SDL-free UI fixture (Client/hud.c + map.c + radar.c +
# menu.c + the Protocol codec) and runs it twice, requiring byte-identical
# output.  The fixture proves:
#   * the HUD shows exactly the authorized SelfState values and no player name;
#   * the mapper draws only authorized known MapCell knowledge (remembered/stale
#     walls) plus the own marker, and no unexplored cell;
#   * radar blips are anonymous and shrink/expire with acquisition age;
#   * the lobby reduces the frozen catalog/config messages to content names and
#     reproduces the reference menu texts/layout and selection;
#   * the results/banner come from the server RESULTS (3+ named scores), and
#     RESET clears only the retained results.
# Static boundary checks keep the presentation modules SDL/X/engine-free and
# keep player names out of the HUD, mapper and radar.
#
# All generated binaries/logs live OUTSIDE the checkout.
#
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HARNESS="$REPO/Tests/phase2"
PROTO="$REPO/Protocol"
FIXDIR="$HARNESS/fixtures/protocol"
EVIDENCE="${XTANK_P2_EVIDENCE:-/tmp/opencode/xtank-phase2/p2.13/ui}"

while [ $# -gt 0 ]; do
	case "$1" in
	  --evidence) EVIDENCE="$2"; shift 2 ;;
	  *) echo "usage: ui.sh [--evidence DIR]" >&2; exit 2 ;;
	esac
done

mkdir -p "$EVIDENCE"
WORK="$EVIDENCE/work"
mkdir -p "$WORK"

log()  { printf '%s\n' "$*"; }
fail() { printf 'UI FAIL: %s\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------------------
# 1. Build and run the UI fixture (twice, determinism)
# ---------------------------------------------------------------------------
log "== building the UI fixture (pure C: hud + map + radar + menu + codec)"
gcc -O0 -g -Wall -Wextra -I "$PROTO" -I "$REPO/Client" \
	"$HARNESS/ui_fixture.c" \
	"$REPO/Client/hud.c" "$REPO/Client/map.c" \
	"$REPO/Client/radar.c" "$REPO/Client/menu.c" \
	"$PROTO/encode.c" "$PROTO/decode.c" \
	-o "$WORK/ui_fixture" -lm \
	> "$WORK/fixture-build.out" 2>&1 || \
	fail "fixture build failed (see $WORK/fixture-build.out)"

run_fixture() {
	"$WORK/ui_fixture" "$FIXDIR"
}

run_fixture > "$WORK/fixture1.out" 2>&1 || \
	{ cat "$WORK/fixture1.out" >&2; fail "fixture run 1 failed"; }
run_fixture > "$WORK/fixture2.out" 2>&1 || \
	{ cat "$WORK/fixture2.out" >&2; fail "fixture run 2 failed"; }
grep -q 'P2.13 UI FIXTURE PASS' "$WORK/fixture1.out" || \
	{ cat "$WORK/fixture1.out" >&2; fail "fixture did not report PASS"; }
cmp "$WORK/fixture1.out" "$WORK/fixture2.out" >/dev/null || \
	fail "fixture is not deterministic (two runs differ)"
log "    $(grep -m1 'checks=' "$WORK/fixture1.out")"
log "    two fixture runs byte-identical"

# ---------------------------------------------------------------------------
# 2. Static boundary checks: presentation modules are SDL/X/engine-free
# ---------------------------------------------------------------------------
log "== boundary checks (hud/map/radar/menu are SDL-free, X-free, engine-free)"
for f in Client/ui.h Client/hud.c Client/hud.h Client/map.c Client/map.h \
	 Client/radar.c Client/radar.h Client/menu.c Client/menu.h; do
	safe=$(printf '%s' "$f" | tr '/' '_')
	if grep -nE '#[[:space:]]*include[[:space:]]*<(SDL3|X11|Xlib)|XOpenDisplay|intent_push|server_(init|step|match|loop)|play_game|real_map|actual_vehicles|live_vehicles|do_special|draw_picture' \
		"$REPO/$f" > "$WORK/scan-$safe.out" 2>&1; then
		cat "$WORK/scan-$safe.out" >&2
		fail "$f references SDL/X11 or simulation state"
	fi
done
log "    no SDL/X11/simulation reference in the UI modules"

# Anonymity: the HUD, mapper and radar must never reference a player name.
for f in Client/hud.c Client/map.c Client/radar.c; do
	safe=$(printf '%s' "$f" | tr '/' '_')
	if grep -nE 'owner->name|player_name|->name|\.name\b|strcpy\(.*name' \
		"$REPO/$f" > "$WORK/name-$safe.out" 2>&1; then
		cat "$WORK/name-$safe.out" >&2
		fail "$f references a player name (anonymity rule)"
	fi
done
log "    hud.c/map.c/radar.c read no player name (anonymity kept)"

# The results/menu surface is the only place public names are allowed.
grep -q 'entry\[i\].name' "$REPO/Client/menu.c" || \
	fail "menu.c does not render the authorized results names"
log "    menu.c renders public names only on the results surface"

# The pure UI objects must have no undefined engine/SDL/ENet symbol.
for f in hud map radar menu; do
	gcc -O0 -g -w -I "$PROTO" -I "$REPO/Client" \
		-c "$REPO/Client/$f.c" -o "$WORK/$f.o"
done
if nm -u "$WORK/hud.o" "$WORK/map.o" "$WORK/radar.o" "$WORK/menu.o" \
	2>/dev/null | \
	grep -qE 'SDL_|enet_|play_game|server_(init|step|match|loop)|animate|do_special|XOpenDisplay'; then
	fail "a UI object references SDL/ENet/simulation"
fi
log "    hud/map/radar/menu link no SDL/ENet/simulation symbol"

# The fixture must not include SDL, X11 or a simulation header.
if grep -nE '#[[:space:]]*include[[:space:]]*<(SDL3|X11|Xlib)|intent\.h|server\.h|server_view\.h' \
	"$HARNESS/ui_fixture.c" > "$WORK/scan-fixture.out" 2>&1; then
	cat "$WORK/scan-fixture.out" >&2
	fail "ui_fixture.c includes SDL/X11/simulation"
fi
log "    ui_fixture.c is SDL-free and simulation-free"

# ---------------------------------------------------------------------------
log ""
log "P2.13 UI PASS (authorized HUD values; remembered-only mapper; anonymous"
log "                 radar blips; frozen reference lobby texts/layout;"
log "                 server results/banner with names; reset clears only"
log "                 the retained results)"
