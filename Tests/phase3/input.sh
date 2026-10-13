#!/usr/bin/env bash
#
# input.sh -- P3.11 resilient live-input acceptance.
#
# Contract (docs/PHASE3-PLAN.md P3.11):
#   bash Tests/phase3/input.sh [--evidence DIR]
#
# Builds the pure-C input fixture (Client/input.c + camera + the Protocol codec;
# no SDL, no simulation) and runs it twice, requiring byte-identical output.
# Proves: every command carries the controlled entity-life generation; one-shot
# fire/weapon edges and held releases are duplicated into a reliable companion
# sharing their command ids, so the server dedup applies each once; a held
# release survives loss of the unreliable batch; a stale edge cannot fire after
# a life change; input/heartbeat refresh at 15 Hz without any snapshot/model;
# disconnect/reset clears held edges and redundancy while command ids stay
# monotonic; and the camera/DPI world-aim transform is unchanged.  Static
# boundary checks keep SDL scancodes and engine intent codes out of the input
# controller and assert the frame loop emits on an independent 15 Hz schedule.
#
# All generated binaries/logs live OUTSIDE the checkout.
#
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HARNESS="$REPO/Tests/phase3"
PROTO="$REPO/Protocol"
EVIDENCE="${XTANK_P3_EVIDENCE:-/tmp/opencode/xtank-phase3/p3.11/input}"

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
log "== building the resilient input fixture (input + camera + codec)"
gcc -O0 -g -Wall -Wextra -I "$PROTO" -I "$REPO/Client" \
	"$HARNESS/input_fixture.c" \
	"$REPO/Client/input.c" "$REPO/Client/camera.c" \
	"$PROTO/encode.c" "$PROTO/decode.c" \
	-o "$WORK/input_fixture" -lm \
	> "$WORK/fixture-build.out" 2>&1 || \
	fail "fixture build failed (see $WORK/fixture-build.out)"

run_fixture() { "$WORK/input_fixture"; }

run_fixture > "$WORK/fixture1.out" 2>&1 || \
	{ cat "$WORK/fixture1.out" >&2; fail "fixture run 1 failed"; }
run_fixture > "$WORK/fixture2.out" 2>&1 || \
	{ cat "$WORK/fixture2.out" >&2; fail "fixture run 2 failed"; }
grep -q 'P3.11 INPUT FIXTURE PASS' "$WORK/fixture1.out" || \
	{ cat "$WORK/fixture1.out" >&2; fail "fixture did not report PASS"; }
cmp "$WORK/fixture1.out" "$WORK/fixture2.out" >/dev/null || \
	fail "fixture is not deterministic (two runs differ)"
log "    $(grep -m1 'checks=' "$WORK/fixture1.out")"
log "    two fixture runs byte-identical"

# ---------------------------------------------------------------------------
# 2. Static boundary checks: SDL scancodes / engine intent codes stay out of
#    the input controller; the controller never blocks.
# ---------------------------------------------------------------------------
log "== boundary checks (input.c is SDL-free, X-free, engine-free, nonblocking)"
for f in Client/input.c Client/input.h; do
	safe=$(printf '%s' "$f" | tr '/' '_')
	if grep -nE '#[[:space:]]*include[[:space:]]*<(SDL3|X11|Xlib)|SDL_SCANCODE_|SDLK_|XOpenDisplay|INTENT_[A-Z_]+|intent_push|server_(init|step|match)|play_game' \
		"$REPO/$f" > "$WORK/scan-$safe.out" 2>&1; then
		cat "$WORK/scan-$safe.out" >&2
		fail "$f references SDL scancodes, X11 or engine intent codes"
	fi
	if grep -nE '\b(read|readv|recv|recvfrom|recvmsg|poll|ppoll|select|pselect|accept|epoll_wait|sleep|usleep|nanosleep|SDL_Delay|SDL_WaitEvent)[[:space:]]*\(' \
		"$REPO/$f" > "$WORK/block-$safe.out" 2>&1; then
		cat "$WORK/block-$safe.out" >&2
		fail "$f contains a blocking input call"
	fi
done
log "    input.c/input.h: no SDL/X11/engine symbols, no blocking calls"

gcc -O0 -g -w -I "$PROTO" -I "$REPO/Client" -c "$REPO/Client/input.c" \
	-o "$WORK/input.o"
if nm -u "$WORK/input.o" 2>/dev/null \
	| grep -qE 'SDL_|enet_|play_game|server_(init|step|match|loop)|animate|intent_push|XOpenDisplay'; then
	fail "input.o references SDL/ENet/simulation"
fi
log "    input.o links no SDL/ENet/simulation symbol"

# The frame loop stamps the current life and sends the reliable companion.
grep -q 'input_set_life_generation' "$REPO/Client/frame.c" || \
	fail "frame.c does not stamp the controlled entity-life generation"
grep -q 'session_client_send_input_reliable' "$REPO/Client/frame.c" || \
	fail "frame.c does not send the reliable one-shot companion"
grep -q 'input_emit_reliable' "$REPO/Client/frame.c" || \
	fail "frame.c does not use the reliable-emitting input controller"
# Input refresh is on an independent wall-clock schedule, not snapshot-gated.
grep -q 'next_emit_ns' "$REPO/Client/frame.c" || \
	fail "frame.c has no independent 15 Hz input schedule"
if grep -nE 'if \(!\(etick >= 1 && etick > f->last_emit_tick\)\)' \
	"$REPO/Client/frame.c" >/dev/null 2>&1; then
	fail "frame.c still gates emission on snapshot arrival"
fi
log "    frame.c stamps the life and emits reliably on an independent 15 Hz schedule"

# The reliable send exists on the session client and uses the control channel.
grep -q 'session_client_send_input_reliable' "$REPO/Client/session_client.h" || \
	fail "session client has no reliable input companion send"
grep -q 'session_client_send_input_reliable' "$REPO/Client/session_client.c" || \
	fail "session client reliable send is not implemented"

# The fixture must not include SDL or a simulation header.
if grep -nE '#[[:space:]]*include[[:space:]]*<(SDL3|X11|Xlib)|intent\.h|server\.h' \
	"$HARNESS/input_fixture.c" > "$WORK/scan-fixture.out" 2>&1; then
	cat "$WORK/scan-fixture.out" >&2
	fail "input_fixture.c includes SDL/X11/simulation"
fi
log "    input_fixture.c is SDL-free and simulation-free"

# ---------------------------------------------------------------------------
log ""
log "P3.11 INPUT PASS (generation-stamped commands; reliable one-shot edges/"
log "                 held releases sharing command ids apply once; held release"
log "                 survives loss; stale edge dropped on life change; 15 Hz"
log "                 input/heartbeat independent of snapshots; reset clears"
log "                 state with monotonic command ids; transform unchanged)"
