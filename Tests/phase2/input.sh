#!/usr/bin/env bash
#
# input.sh -- P2.12 client input-controller acceptance.
#
# Contract (docs/PHASE2-PLAN.md P2.12):
#   bash Tests/phase2/input.sh [--evidence DIR]
#
# Builds the standalone input fixture (Client/input.c + camera + the Protocol
# codec; no SDL, no simulation) and runs it twice, requiring byte-identical
# output.  Proves: neutral controls map to authorized wire ops; no emit on tick
# 0 or outside Running; one-shot fire/weapon edges fire once and expire after
# the bounded redundancy window; held drive/fire refresh and release once;
# pause only outside Running; defocus/quit release all held controls; one batch
# per 15 Hz tick with monotonic packet sequence and idle heartbeat; and the
# inverse screen->world camera/DPI transform at corners, a nonzero origin and a
# scaled window.  Static boundary checks keep SDL scancodes and engine intent
# codes out of Client/input.c.
#
# All generated binaries/logs live OUTSIDE the checkout.
#
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HARNESS="$REPO/Tests/phase2"
PROTO="$REPO/Protocol"
EVIDENCE="${XTANK_P2_EVIDENCE:-/tmp/opencode/xtank-phase2/p2.12/input}"

while [ $# -gt 0 ]; do
	case "$1" in
	  --evidence) EVIDENCE="$2"; shift 2 ;;
	  *) echo "usage: input.sh [--evidence DIR]" >&2; exit 2 ;;
	esac
done

mkdir -p "$EVIDENCE"
WORK="$EVIDENCE/work"
mkdir -p "$WORK"

log()  { printf '%s\n' "$*"; }
fail() { printf 'INPUT FAIL: %s\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------------------
# 1. Build and run the input fixture (twice, determinism)
# ---------------------------------------------------------------------------
log "== building the input fixture (pure C: input + camera + codec)"
gcc -O0 -g -Wall -Wextra -I "$PROTO" -I "$REPO/Client" \
	"$HARNESS/input_fixture.c" \
	"$REPO/Client/input.c" "$REPO/Client/camera.c" \
	"$PROTO/encode.c" "$PROTO/decode.c" \
	-o "$WORK/input_fixture" -lm \
	> "$WORK/fixture-build.out" 2>&1 || \
	fail "fixture build failed (see $WORK/fixture-build.out)"

run_fixture() {
	"$WORK/input_fixture"
}

run_fixture > "$WORK/fixture1.out" 2>&1 || \
	{ cat "$WORK/fixture1.out" >&2; fail "fixture run 1 failed"; }
run_fixture > "$WORK/fixture2.out" 2>&1 || \
	{ cat "$WORK/fixture2.out" >&2; fail "fixture run 2 failed"; }
grep -q 'P2.12 INPUT FIXTURE PASS' "$WORK/fixture1.out" || \
	{ cat "$WORK/fixture1.out" >&2; fail "fixture did not report PASS"; }
cmp "$WORK/fixture1.out" "$WORK/fixture2.out" >/dev/null || \
	fail "fixture is not deterministic (two runs differ)"
log "    $(grep -m1 'checks=' "$WORK/fixture1.out")"
log "    two fixture runs byte-identical"

# ---------------------------------------------------------------------------
# 2. Static boundary checks: SDL scancodes and engine intent codes stay out of
#    the client input controller; the controller never blocks.
# ---------------------------------------------------------------------------
log "== boundary checks (input.c is SDL-free, X-free, engine-free, nonblocking)"
for f in Client/input.c Client/input.h; do
	safe=$(printf '%s' "$f" | tr '/' '_')
	if grep -nE '#[[:space:]]*include[[:space:]]*<(SDL3|X11|Xlib)|SDL_SCANCODE_|SDLK_|XOpenDisplay|INTENT_[A-Z_]+|intent_push|server_(init|step|match)|play_game' \
		"$REPO/$f" > "$WORK/scan-$safe.out" 2>&1; then
		cat "$WORK/scan-$safe.out" >&2
		fail "$f references SDL scancodes, X11 or engine intent codes"
	fi
	# Never block on input: no I/O, no sleep, no polling.
	if grep -nE '\b(read|readv|recv|recvfrom|recvmsg|poll|ppoll|select|pselect|accept|epoll_wait|sleep|usleep|nanosleep|SDL_Delay|SDL_WaitEvent)[[:space:]]*\(' \
		"$REPO/$f" > "$WORK/block-$safe.out" 2>&1; then
		cat "$WORK/block-$safe.out" >&2
		fail "$f contains a blocking input call"
	fi
done
log "    input.c/input.h: no SDL/X11/engine symbols, no blocking calls"

# The platform shell is the sole SDL unit; it maps scancodes to neutral
# controls and must not know engine intent codes.
if grep -nE 'INTENT_[A-Z_]+|intent_push' "$REPO/Client/platform_sdl.c" \
	> "$WORK/scan-platform.out" 2>&1; then
	cat "$WORK/scan-platform.out" >&2
	fail "platform_sdl.c references engine intent codes"
fi
grep -q 'xtk_sdl_scancode_to_control' "$REPO/Client/platform_sdl.c" || \
	fail "platform_sdl.c is missing the scancode->control mapping"
log "    platform_sdl.c maps SDL scancodes to neutral controls only"

# The pure input object must have no undefined engine symbol and no SDL/ENet
# reference.
gcc -O0 -g -w -I "$PROTO" -I "$REPO/Client" -c "$REPO/Client/input.c" \
	-o "$WORK/input.o"
if nm -u "$WORK/input.o" 2>/dev/null \
	| grep -qE 'SDL_|enet_|play_game|server_(init|step|match|loop)|animate|intent_push|XOpenDisplay'; then
	fail "input.o references SDL/ENet/simulation"
fi
log "    input.o links no SDL/ENet/simulation symbol"

# The fixture must not include SDL or a simulation header.
if grep -nE '#[[:space:]]*include[[:space:]]*<(SDL3|X11|Xlib)|intent\.h|server\.h' \
	"$HARNESS/input_fixture.c" > "$WORK/scan-fixture.out" 2>&1; then
	cat "$WORK/scan-fixture.out" >&2
	fail "input_fixture.c includes SDL/X11/simulation"
fi
log "    input_fixture.c is SDL-free and simulation-free"

# ---------------------------------------------------------------------------
log ""
log "P2.12 INPUT PASS (edge/held mapping; 15 Hz emit; never tick 0; pause only"
log "                  outside Running; quit/defocus release; no raw keyboard"
log "                  passthrough to the engine)"
