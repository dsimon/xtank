#!/usr/bin/env bash
#
# players.sh -- P2.4 terminal-free human lifecycle and binding acceptance.
#
# Builds the shipped display-free server core, links the P2.4 lifecycle fixture
# against it (Src/sobj minus the server entry point), and runs:
#
#   1. the fixture, which drives one explicit terminal-free human (controller ->
#      name -> slot) plus one drone through two full epochs and asserts:
#        init / death / respawn / win / reset twice, human defaults
#        (num_players == 1, Vehicle.teleport == TRUE), the drone bot default
#        (teleport == FALSE), binding ownership/generation validation, and ZERO
#        Terminal allocation / render / event-input calls (link-time --wrap);
#   2. the shipped xtank-server --human smoke: one human + one drone over two
#      matches, with no display and no terminal, completing cleanly;
#   3. a determinism repeat of the fixture.
#
# All generated binaries/logs live OUTSIDE the checkout.
#
#   bash Tests/phase2/players.sh [--evidence DIR]
#
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HARNESS="$REPO/Tests/phase2"
EVIDENCE="${XTANK_P2_EVIDENCE:-/tmp/opencode/xtank-phase2/p2.4/players}"

while [ $# -gt 0 ]; do
	case "$1" in
	  --evidence) EVIDENCE="$2"; shift 2 ;;
	  *) echo "usage: players.sh [--evidence DIR]" >&2; exit 2 ;;
	esac
done

mkdir -p "$EVIDENCE"
WORK="$EVIDENCE/work"
mkdir -p "$WORK"

log()  { printf '%s\n' "$*"; }
fail() { printf 'PLAYERS FAIL: %s\n' "$*" >&2; exit 1; }

# Same feature defines as the shipped server build (Src/GNUmakefile SCFLAGS).
DEFS="-DUNIX -DS1024x864 -DKEYPAD_DETECT -DSTINGY_REDRAW -DBATCH_LINES \
-DBATCH_POINTS -DBATCH_COLOR_LINES -DCODE_SIZE=0x20000 -DSTACK_SIZE=0x8000 \
-DNEW_LIMITS -DSLOWER -DMULTI_SYNC -DTHREAD_SWAPCONTEXT -DNULL_BACKEND \
-DSERVER_ONLY"

log "== building the shipped server core (make phase1-server phase1-core)"
( cd "$REPO" && make phase1-server phase1-core ) > "$WORK/build.out" 2>&1 || \
	fail "server/core build failed (see $WORK/build.out)"

[ -x "$REPO/Src/xtank-server" ] || fail "xtank-server not built"
[ -d "$REPO/Src/sobj" ] || fail "server object tree missing"

# Core objects without the server entry point (the fixture provides main()).
# shellcheck disable=SC2046
CORE_OBJS=$(ls "$REPO"/Src/sobj/*.o | grep -v '/server_main.o$')
[ -n "$CORE_OBJS" ] || fail "no core objects"

log "== compiling the lifecycle fixture against the server core"
# shellcheck disable=SC2086
gcc -O0 -g -w $DEFS -I"$REPO/Src" -I"$REPO/Src/Include" \
	-DXTANK_DIR="\"$REPO/Src/..\"" -DALLDEFINES="\"p2.4 players fixture\"" \
	-o "$WORK/players_fixture" "$HARNESS/players_fixture.c" $CORE_OBJS \
	-Wl,--wrap=make_terminal,--wrap=set_terminal,--wrap=display_terminal \
	-Wl,--wrap=get_events,--wrap=sync_terminals -lm \
	> "$WORK/fixture-build.out" 2>&1 || \
	fail "fixture build failed (see $WORK/fixture-build.out)"

log "== running the terminal-free human lifecycle fixture (2 epochs)"
if ! "$WORK/players_fixture" "$REPO" > "$WORK/fixture1.out" 2>&1; then
	cat "$WORK/fixture1.out" >&2
	fail "lifecycle fixture failed"
fi
grep -q 'PLAYERS FIXTURE PASS' "$WORK/fixture1.out" || \
	fail "fixture did not report PASS"
grep -q 'no Terminal was ever allocated' "$WORK/fixture1.out" || \
	fail "fixture did not assert zero terminal allocation"
log "    $(grep -m1 'PLAYERS FIXTURE PASS' "$WORK/fixture1.out")"

log "== determinism: repeat the fixture"
"$WORK/players_fixture" "$REPO" > "$WORK/fixture2.out" 2>&1 || \
	fail "fixture repeat failed"
cmp "$WORK/fixture1.out" "$WORK/fixture2.out" >/dev/null || \
	fail "fixture repeats differ"
log "    two fixture runs identical"

log "== shipped xtank-server --human smoke (one human + one drone, 2 matches)"
if ! ( cd "$REPO" && env -u DISPLAY -u XTANK_DIR -u HOME \
		timeout 300 ./Src/xtank-server --maze combat1 --vehicle Vanguard \
		--human humanA --drones 1 --seed 114336 --matches 2 \
		--max-ticks 2000 --fast ) > "$WORK/server.out" 2>&1; then
	cat "$WORK/server.out" >&2
	fail "terminal-free server human smoke failed"
fi
grep -q 'human=humanA' "$WORK/server.out" || fail "server did not bind the human"
grep -q 'SERVER-DONE matches=2' "$WORK/server.out" || \
	fail "server did not complete two matches"

log "    $(grep -m1 '^SERVER ' "$WORK/server.out")"
log "    $(grep -m1 'SERVER-DONE' "$WORK/server.out")"

log ""
log "P2.4 PLAYERS PASS (terminal-free human: 2 epochs init/death/respawn/win/reset,"
log "                   zero Terminal/render/input calls; shipped server human smoke)"
