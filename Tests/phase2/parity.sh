#!/usr/bin/env bash
#
# parity.sh -- P2.15 three-way network-path parity audit.
#
# Contract (docs/PHASE2-PLAN.md P2.15; task contract):
#   bash Tests/phase2/parity.sh [--evidence DIR]
#
# Proves that the real network path does not change a game outcome.  The same
# match and the same tick-indexed, release-authorized command log are driven
# three ways and the authoritative per-tick state is required to be
# byte-identical:
#
#   leg X       frozen pinned X reference (Tests/phase1/build-reference.sh)
#               driven by the legacy controls through reference_input.c;
#   leg direct  the shipped terminal-free server core + P2.8 session, commands
#               injected straight into the bounded intent queue (no transport);
#   leg network the same core/session, commands encoded by the P2.3 codec and
#               carried over the pinned P2.7 ENet transport to a real P2.14
#               client session; P2.8/P2.7 admit and step them and the P2.11
#               client model consumes the authorized VIEW_SNAPSHOT stream.
#
# The direct and network dumps emit the F2/V/B/E subset of the frozen v2
# STATE_DUMP format, so they are cmp'd against the frozen-X and direct-intent
# oracle dumps.  It also proves the decoded snapshot stream matches the direct
# simulation's own-player position and score.
#
# No gameplay rule is changed.  Generated binaries/logs/reference trees live
# OUTSIDE the checkout.
#
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HARNESS="$REPO/Tests/phase2"
SCEN="$HARNESS/fixtures/parity"
EVIDENCE="${XTANK_P2_EVIDENCE:-/tmp/opencode/xtank-phase2/p2.15/parity}"

ENET_URL="https://github.com/lsalzman/enet/archive/refs/tags/v1.3.18.tar.gz"
ENET_SHA="28603c895f9ed24a846478180ee72c7376b39b4bb1287b73877e5eae7d96b0dd"

TICKS=800
NEXT_TICK=$((TICKS + 1))

while [ $# -gt 0 ]; do
	case "$1" in
	  --evidence) EVIDENCE="$2"; shift 2 ;;
	  *) echo "usage: parity.sh [--evidence DIR]" >&2; exit 2 ;;
	esac
done

mkdir -p "$EVIDENCE"
WORK="$EVIDENCE/work"
mkdir -p "$WORK"

log()  { printf '%s\n' "$*"; }
fail() { printf 'PARITY FAIL: %s\n' "$*" >&2; exit 1; }

DEFS="-DUNIX -DS1024x864 -DKEYPAD_DETECT -DSTINGY_REDRAW -DBATCH_LINES \
-DBATCH_POINTS -DBATCH_COLOR_LINES -DCODE_SIZE=0x20000 -DSTACK_SIZE=0x8000 \
-DNEW_LIMITS -DSLOWER -DMULTI_SYNC -DTHREAD_SWAPCONTEXT -DNULL_BACKEND \
-DSERVER_ONLY"

SETTINGS="$SCEN/p2.15-arena.settings"
TRACE="$SCEN/p2.15-arena.trace"
[ -f "$SETTINGS" ] || fail "missing scenario settings $SETTINGS"
[ -f "$TRACE" ]    || fail "missing scenario trace $TRACE"

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
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

# ---------------------------------------------------------------------------
# Build the shipped core + network modules + client model + fixture
# ---------------------------------------------------------------------------
log "== building the shipped server core"
( cd "$REPO" && make -s phase1-server phase1-core ) > "$WORK/core.out" 2>&1 || \
	fail "core build failed (see $WORK/core.out)"
CORE_OBJS=$(ls "$REPO"/Src/sobj/*.o | grep -v '/server_main.o$')
[ -n "$CORE_OBJS" ] || fail "no core objects"

log "== compiling session, admission, transport, codec and client model"
compile() { # src out include_flags...
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
compile "$REPO/Protocol/encode.c" "$WORK/encode.o" -I"$REPO/Protocol"
compile "$REPO/Protocol/decode.c" "$WORK/decode.o" -I"$REPO/Protocol"
compile "$REPO/Protocol/content-manifest.c" "$WORK/content-manifest.o" \
	-I"$REPO/Protocol"
for f in model.c timeline.c camera.c session_client.c; do
	gcc -O0 -g -w -I"$REPO/Protocol" -I"$REPO/Transport" -I"$REPO/Client" \
		-c "$REPO/Client/$f" -o "$WORK/${f%.c}.o" >> "$WORK/compile.out" 2>&1 || \
		fail "client compile failed: $f"
done

ENET_OBJS=""
for f in callbacks compress host list packet peer protocol unix; do
	o="$WORK/enet-$f.o"
	gcc -O0 -g -w -I"$ENET_DIR/include" -c "$ENET_DIR/$f.c" -o "$o" \
		>> "$WORK/enet-build.out" 2>&1 || fail "ENet build failed ($f)"
	ENET_OBJS="$ENET_OBJS $o"
done

log "== linking the parity fixture"
# shellcheck disable=SC2086
gcc -O0 -g -w $DEFS -DXTANK_WITH_NET -I"$REPO/Src" -I"$REPO/Src/Include" \
	-I"$REPO/Protocol" -I"$REPO/Transport" -I"$REPO/Client" \
	-I"$ENET_DIR/include" \
	-DXTANK_DIR="\"$REPO/Src/..\"" -DALLDEFINES="\"p2.15 parity fixture\"" \
	-o "$WORK/parity_fixture" "$HARNESS/parity_fixture.c" \
	$CORE_OBJS "$WORK/server_session.o" "$WORK/server_net.o" \
	"$WORK/enet_transport.o" $ENET_OBJS "$WORK/encode.o" "$WORK/decode.o" \
	"$WORK/content-manifest.o" "$WORK/model.o" "$WORK/timeline.o" \
	"$WORK/camera.o" "$WORK/session_client.o" \
	-Wl,--wrap=sendmsg -lm > "$WORK/fixture-build.out" 2>&1 || \
	fail "fixture link failed (see $WORK/fixture-build.out)"

run_leg() { # mode out [snap]
	if [ "$1" = "network" ]; then
		timeout 300 "$WORK/parity_fixture" network "$REPO" "$SETTINGS" \
			"$TRACE" "$2" "$3"
	else
		timeout 300 "$WORK/parity_fixture" direct "$REPO" "$SETTINGS" \
			"$TRACE" "$2"
	fi
}

# ---------------------------------------------------------------------------
# 1. direct vs network (twice each, deterministic), byte identity
# ---------------------------------------------------------------------------
log "== running the direct and network legs (twice each)"
run_leg direct  "$WORK/direct.1.state" > "$WORK/direct.1.out" 2>&1 || \
	{ cat "$WORK/direct.1.out" >&2; fail "direct leg failed"; }
run_leg direct  "$WORK/direct.2.state" > "$WORK/direct.2.out" 2>&1 || \
	fail "direct leg repeat failed"
run_leg network "$WORK/network.1.state" "$WORK/network.1.snap" \
	> "$WORK/network.1.out" 2>&1 || \
	{ cat "$WORK/network.1.out" >&2; fail "network leg failed"; }
run_leg network "$WORK/network.2.state" "$WORK/network.2.snap" \
	> "$WORK/network.2.out" 2>&1 || fail "network leg repeat failed"

grep -q 'PARITY FIXTURE PASS' "$WORK/direct.1.out" || fail "direct did not PASS"
grep -q 'PARITY FIXTURE PASS' "$WORK/network.1.out" || fail "network did not PASS"

cmp "$WORK/direct.1.state"  "$WORK/direct.2.state"  >/dev/null || \
	fail "direct leg is not deterministic"
cmp "$WORK/network.1.state" "$WORK/network.2.state" >/dev/null || \
	fail "network leg is not deterministic"
cmp "$WORK/network.1.snap"  "$WORK/network.2.snap"  >/dev/null || \
	fail "network snapshot stream is not deterministic"

if ! cmp "$WORK/direct.1.state" "$WORK/network.1.state" >/dev/null; then
	firstdiff=$(cmp "$WORK/direct.1.state" "$WORK/network.1.state" 2>&1 | head -1)
	fail "direct and network authoritative state differ ($firstdiff)"
fi
log "    direct == network authoritative state ($(wc -l <"$WORK/direct.1.state") lines)"

# ---------------------------------------------------------------------------
# 2. decoded snapshot stream == direct simulation (own position/score)
# ---------------------------------------------------------------------------
log "== comparing the client snapshot stream to the direct simulation"
awk '
/^F2 /{tick=$2}
/^V 0 /{x="";y="";sc="";
	for(i=1;i<=NF;i++){ if($i=="pos"){x=$(i+1);y=$(i+2)} if($i=="score"){sc=$(i+1)} }
	printf "S %s 0 %s %s %s\n", tick, x, y, sc}
' "$WORK/direct.1.state" > "$WORK/direct.v0"
got=$(wc -l < "$WORK/network.1.snap")
[ "$got" -ge "$TICKS" ] || fail "too few snapshots ($got < $TICKS)"
# The compared window is frames 1..TICKS; the forced-result frame after it is
# where the server legitimately snapshots the pre-reset winning score.
head -n "$TICKS" "$WORK/direct.v0" > "$WORK/direct.v0.head"
head -n "$TICKS" "$WORK/network.1.snap" > "$WORK/network.1.snap.head"
if ! cmp "$WORK/direct.v0.head" "$WORK/network.1.snap.head" >/dev/null; then
	firstdiff=$(cmp "$WORK/direct.v0.head" "$WORK/network.1.snap.head" 2>&1 | head -1)
	fail "snapshot stream diverges from the direct simulation ($firstdiff)"
fi
log "    $TICKS snapshots match the direct own-player position/score exactly"

# ---------------------------------------------------------------------------
# 3. leg X: frozen pinned X reference vs the direct-intent oracle
# ---------------------------------------------------------------------------
log "== building the frozen X reference and the direct-intent candidate"
REF_EV="$EVIDENCE/oracle"
mkdir -p "$REF_EV"
XTANK_P1_EVIDENCE="$REF_EV" \
	bash "$REPO/Tests/phase1/build-reference.sh" --mode reference \
	--evidence "$REF_EV" > "$WORK/build-reference.out" 2>&1 || \
	fail "reference build failed (see $WORK/build-reference.out)"
XTANK_P1_EVIDENCE="$REF_EV" \
	bash "$REPO/Tests/phase1/build-reference.sh" --mode candidate \
	--subject null --stage after --evidence "$REF_EV" \
	> "$WORK/build-candidate.out" 2>&1 || \
	fail "candidate build failed (see $WORK/build-candidate.out)"

REF_ROOT="$REF_EV/reference/src"
REF_BIN="$REF_ROOT/Src/xtank-refdump"
CAN_ROOT="$REF_EV/candidate-after-null/src"
CAN_BIN="$CAN_ROOT/Src/xtank-headless"
[ -x "$REF_BIN" ] || fail "reference binary missing"
[ -x "$CAN_BIN" ] || fail "candidate binary missing"

if [ ! -f "$REF_EV/fixseed.so" ]; then
	gcc -shared -fPIC -O2 -o "$REF_EV/fixseed.so" \
		"$REPO/Tests/phase1/fixseed.c"
fi

# Harness-owned Xvfb for the frozen X reference.
XDPY=""
XVFB_PID=""
for d in 99 98 97 96 95 94 93 92 91 90; do
	if [ ! -e "/tmp/.X${d}-lock" ] && \
	   ! pgrep -f "Xvfb :${d}( |$)" >/dev/null 2>&1; then
		XDPY="$d"; break
	fi
done
[ -n "$XDPY" ] || fail "no free X display"
setsid Xvfb ":$XDPY" -screen 0 1024x864x24 -nolisten tcp -noreset \
	</dev/null > "$WORK/xvfb.log" 2>&1 &
XVFB_PID=$!
cleanup() { [ -n "$XVFB_PID" ] && kill "$XVFB_PID" 2>/dev/null || true; }
trap cleanup EXIT INT TERM
for i in $(seq 1 50); do
	DISPLAY=":$XDPY" xdpyinfo >/dev/null 2>&1 && break
	sleep 0.2
done
DISPLAY=":$XDPY" xdpyinfo >/dev/null 2>&1 || fail "Xvfb :$XDPY not ready"
printf 'xtank.playerName: pilot\n' | xrdb -display ":$XDPY" -merge

log "== running the frozen X reference (leg X)"
( cd "$REF_ROOT/Src" && env LC_ALL=C DISPLAY=":$XDPY" XTANK_DIR="$REF_ROOT" \
	  XTANK_SEED=114336 XTANK_DUMP="$WORK/x.state" XTANK_DUMP_PERIOD=1 \
	  XTANK_DUMP_FORMAT=2 XTANK_TICK_LIMIT="$TICKS" XTANK_TRACE="$TRACE" \
	  LD_PRELOAD="$REF_EV/fixseed.so" \
	  timeout 300 "$REF_BIN" -S -D -P -X -F "$SETTINGS" ":$XDPY" ) \
	> "$WORK/x.stdout" 2>&1 || fail "X reference run failed"

log "== running the direct-intent candidate (leg direct-intent)"
( cd "$CAN_ROOT/Src" && env -u DISPLAY -u LD_PRELOAD LC_ALL=C \
	  XTANK_DIR="$CAN_ROOT" XTANK_SEED=114336 XTANK_DUMP="$WORK/c.state" \
	  XTANK_DUMP_PERIOD=1 XTANK_DUMP_FORMAT=2 XTANK_TICK_LIMIT="$TICKS" \
	  XTANK_TRACE="$TRACE" \
	  timeout 300 "$CAN_BIN" -S -D -P -X -F "$SETTINGS" -p pilot nullterm0 ) \
	> "$WORK/c.stdout" 2>&1 || fail "direct-intent candidate run failed"

grep -E '^(F2|V|B|E) ' "$WORK/x.state" > "$WORK/x.filtered"
grep -E '^(F2|V|B|E) ' "$WORK/c.state" > "$WORK/c.filtered"
# The fixture dumps include a deterministic forced result after the compared
# window; truncate to the same window for the X comparison.
awk -v lim="$NEXT_TICK" \
	'$1=="F2" && $2==lim {exit} {print}' \
	"$WORK/direct.1.state" > "$WORK/direct.win.state"
awk -v lim="$NEXT_TICK" \
	'$1=="F2" && $2==lim {exit} {print}' \
	"$WORK/network.1.state" > "$WORK/network.win.state"

[ -s "$WORK/x.filtered" ] || fail "X reference produced no state"
if ! cmp "$WORK/x.filtered" "$WORK/c.filtered" >/dev/null; then
	firstdiff=$(cmp "$WORK/x.filtered" "$WORK/c.filtered" 2>&1 | head -1)
	fail "X reference and direct-intent candidate differ ($firstdiff)"
fi
if ! cmp "$WORK/x.filtered" "$WORK/direct.win.state" >/dev/null; then
	firstdiff=$(cmp "$WORK/x.filtered" "$WORK/direct.win.state" 2>&1 | head -1)
	fail "X reference and direct leg differ ($firstdiff)"
fi
if ! cmp "$WORK/x.filtered" "$WORK/network.win.state" >/dev/null; then
	firstdiff=$(cmp "$WORK/x.filtered" "$WORK/network.win.state" 2>&1 | head -1)
	fail "X reference and network leg differ ($firstdiff)"
fi
log "    X == direct-intent == direct == network ($TICKS ticks, $(wc -l <"$WORK/x.filtered") records)"

{
	echo "scenario=Arena,1 human+1 drone"
	echo "trace_sha256=$(sha256sum "$TRACE" | awk '{print $1}')"
	echo "settings_sha256=$(sha256sum "$SETTINGS" | awk '{print $1}')"
	echo "ticks=$TICKS"
	echo "x_sha256=$(sha256sum "$WORK/x.filtered" | awk '{print $1}')"
	echo "direct_sha256=$(sha256sum "$WORK/direct.win.state" | awk '{print $1}')"
	echo "network_sha256=$(sha256sum "$WORK/network.win.state" | awk '{print $1}')"
	echo "snapshots=$got"
	echo "cmp=identical"
} > "$WORK/parity.result"

log ""
log "P2.15 PARITY PASS (three-way byte identity: frozen-X == direct-intent =="
log "                  direct == network over $TICKS ticks; decoded snapshot"
log "                  stream matches the direct position/score exactly)"
