#!/usr/bin/env bash
#
# parity.sh -- P3.12 two-client independent network parity audit.
#
# Contract (docs/PHASE3-PLAN.md P3.12):
#   bash Tests/phase3/parity.sh [--evidence DIR] [--maze combat1|Arena]
#
# Proves that the shipped listen/codec/admission/export/client path does not
# change an outcome.  Two bot-free humans replay the same frozen,
# release-authorized command log three ways and the complete frozen v2
# per-tick state dump is required to be byte-identical:
#
#   leg X        the frozen pinned X reference (Tests/phase1/build-reference.sh)
#                driven by the legacy controls through reference_input.c;
#   leg direct   the shipped terminal-free server core + P3.4 coordinator,
#                commands injected straight into the bounded intent queue;
#   leg network  the same core/coordinator, commands encoded by the frozen
#                v1.1 codec and carried over the pinned ENet transport to two
#                real P3.10 client sessions, admitted by P3.6 and exported by
#                P3.8.
#
# The fixture emits the full v2 record set (F2/V/VW/VT/VC/B/E + MX/MP/ML +
# RNH/RB/NRH/NRB).  Two epochs are run; each reseeds the frozen stream, so both
# epochs must equal the single-match frozen-X dump (the P1.6 pattern).
#
# Generated binaries, oracle trees and logs live OUTSIDE the checkout.
#
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HARNESS="$REPO/Tests/phase3"
FIX="$HARNESS/fixtures/parity"
EVIDENCE="${XTANK_P3_EVIDENCE:-/tmp/opencode/xtank-phase3/p3.12/parity}"

ENET_URL="https://github.com/lsalzman/enet/archive/refs/tags/v1.3.18.tar.gz"
ENET_SHA="28603c895f9ed24a846478180ee72c7376b39b4bb1287b73877e5eae7d96b0dd"

MAZE=""
# Finite per-tick replay bound for scenarios that do not reach a natural win
# (the two *-2000 death/respawn traces).  The frozen X reference and both
# fixture legs must stop at the exact same tick, so a runaway trace can never
# be mistaken for parity (P1.6 finite-replay pattern).  Natural-win scenarios
# end earlier and are unaffected.
LIMIT="${XTANK_P3_TICKS:-4000}"

while [ $# -gt 0 ]; do
	case "$1" in
	  --evidence) EVIDENCE="$2"; shift 2 ;;
	  --maze) MAZE="$2"; shift 2 ;;
	  *) echo "usage: parity.sh [--evidence DIR] [--maze combat1|Arena]" >&2;
	     exit 2 ;;
	esac
done

mkdir -p "$EVIDENCE"
WORK="$EVIDENCE/work"
mkdir -p "$WORK"

log()  { printf '%s\n' "$*"; }
fail() { printf 'PARITY FAIL: %s\n' "$*" >&2; exit 1; }

# Seed the reference's xtank.playerName class on a display.  xrdb is the
# normal tool; sandboxes that ship Xvfb/Xlib but no X utilities get a tiny
# built-in equivalent that sets the root-window RESOURCE_MANAGER property.
seed_name() { # display-suffix name
	local d="$1" n="$2"

	if command -v xrdb >/dev/null 2>&1; then
		printf 'xtank.playerName: %s\n' "$n" | xrdb -display ":$d" -merge
		return
	fi
	if [ ! -x "$WORK/xrdb-shim" ]; then
		cat > "$WORK/xrdb-shim.c" <<'XRDB_EOF'
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <stdio.h>
#include <string.h>
int main(int argc, char **argv)
{
	Display *dpy;
	unsigned char *str;
	if (argc < 3)
		return 2;
	str = (unsigned char *) argv[2];
	dpy = XOpenDisplay(argv[1]);
	if (dpy == NULL) {
		fprintf(stderr, "xrdb-shim: cannot open %s\n", argv[1]);
		return 1;
	}
	XChangeProperty(dpy, DefaultRootWindow(dpy),
			XInternAtom(dpy, "RESOURCE_MANAGER", False),
			XA_STRING, 8, PropModeReplace, str,
			(int) strlen((char *) str));
	XFlush(dpy);
	XCloseDisplay(dpy);
	return 0;
}
XRDB_EOF
		gcc -O2 -o "$WORK/xrdb-shim" "$WORK/xrdb-shim.c" -lX11 \
			>> "$WORK/compile.out" 2>&1 || fail "xrdb-shim build failed"
	fi
	"$WORK/xrdb-shim" ":$d" "xtank.playerName: $n" || \
		fail "cannot seed player name on :$d"
}

DEFS="-DUNIX -DS1024x864 -DKEYPAD_DETECT -DSTINGY_REDRAW -DBATCH_LINES \
-DBATCH_POINTS -DBATCH_COLOR_LINES -DCODE_SIZE=0x20000 -DSTACK_SIZE=0x8000 \
-DNEW_LIMITS -DSLOWER -DMULTI_SYNC -DTHREAD_SWAPCONTEXT -DNULL_BACKEND \
-DSERVER_ONLY"

# ---------------------------------------------------------------------------
# Dependencies (pinned ENet v1.3.18)
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
	-DXTANK_DIR="\"$REPO/Src/..\"" -DALLDEFINES="\"p3.12 parity fixture\"" \
	-o "$WORK/parity_fixture" "$HARNESS/parity_fixture.c" \
	$CORE_OBJS "$WORK/server_session.o" "$WORK/server_net.o" \
	"$WORK/enet_transport.o" $ENET_OBJS "$WORK/encode.o" "$WORK/decode.o" \
	"$WORK/content-manifest.o" "$WORK/model.o" "$WORK/timeline.o" \
	"$WORK/camera.o" "$WORK/session_client.o" \
	-Wl,--wrap=sendmsg -lm > "$WORK/fixture-build.out" 2>&1 || \
	fail "fixture link failed (see $WORK/fixture-build.out)"

# ---------------------------------------------------------------------------
# Frozen reference build (X) + fixseed
# ---------------------------------------------------------------------------
log "== building the frozen X reference"
REF_EV="$EVIDENCE/oracle"
mkdir -p "$REF_EV"
XTANK_P1_EVIDENCE="$REF_EV" \
	bash "$REPO/Tests/phase1/build-reference.sh" --mode reference \
	--evidence "$REF_EV" > "$WORK/build-reference.out" 2>&1 || \
	fail "reference build failed (see $WORK/build-reference.out)"
REF_ROOT="$REF_EV/reference/src"
REF_BIN="$REF_ROOT/Src/xtank-refdump"
[ -x "$REF_BIN" ] || fail "reference binary missing"

# The release binary loads mazes from Mazes/list; expose the shipped combat1
# index entry in the private reference tree (no maze bytes change).
if [ "$MAZE" = "combat1" ]; then
	grep -q '^combat1$' "$REF_ROOT/Mazes/list" || \
		printf 'combat1\n' >> "$REF_ROOT/Mazes/list"
fi

if [ ! -f "$REF_EV/fixseed.so" ]; then
	gcc -shared -fPIC -O2 -o "$REF_EV/fixseed.so" \
		"$REPO/Tests/phase1/fixseed.c"
fi

# Harness-owned Xvfb (two displays for the two human slots).
XDPY=""; XDPY2=""
for d in 99 98 97 96 95 94 93 92 91 90; do
	if [ ! -e "/tmp/.X${d}-lock" ] && \
	   ! pgrep -f "Xvfb :${d}( |$)" >/dev/null 2>&1; then XDPY="$d"; break; fi
done
for d in 89 88 87 86 85 84 83 82 81 80; do
	if [ "$d" = "$XDPY" ]; then continue; fi
	if [ ! -e "/tmp/.X${d}-lock" ] && \
	   ! pgrep -f "Xvfb :${d}( |$)" >/dev/null 2>&1; then XDPY2="$d"; break; fi
done
[ -n "$XDPY" ] && [ -n "$XDPY2" ] || fail "no free X displays"
setsid Xvfb ":$XDPY" -screen 0 1024x864x24 -nolisten tcp -noreset \
	</dev/null > "$WORK/xvfb.log" 2>&1 &
XVFB_PID=$!
setsid Xvfb ":$XDPY2" -screen 0 1024x864x24 -nolisten tcp -noreset \
	</dev/null > "$WORK/xvfb2.log" 2>&1 &
XVFB2_PID=$!
cleanup() { kill "$XVFB_PID" "$XVFB2_PID" 2>/dev/null || true; }
trap cleanup EXIT INT TERM
for i in $(seq 1 100); do
	[ -S "/tmp/.X11-unix/X$XDPY" ] && \
		[ -S "/tmp/.X11-unix/X$XDPY2" ] && break
	sleep 0.2
done
[ -S "/tmp/.X11-unix/X$XDPY" ] || fail "Xvfb :$XDPY not ready"
[ -S "/tmp/.X11-unix/X$XDPY2" ] || fail "Xvfb :$XDPY2 not ready"
seed_name "$XDPY" playerA
seed_name "$XDPY2" playerB

normalise() { sed -E 's/^(F2 [0-9]+) epoch [0-9]+ /\1 epoch 0 /'; }

run_x() { # settings trace out
	local settings="$1" trace="$2" out="$3"
	( cd "$REF_ROOT/Src" && env LC_ALL=C DISPLAY=":$XDPY" XTANK_DIR="$REF_ROOT" \
	    XTANK_SEED=114336 XTANK_DUMP="$out" XTANK_DUMP_PERIOD=1 \
	    XTANK_DUMP_FORMAT=2 XTANK_TICK_LIMIT="$LIMIT" XTANK_TRACE="$trace" \
	    LD_PRELOAD="$REF_EV/fixseed.so" \
	    timeout 600 "$REF_BIN" -S -D -P -X -F "$settings" ":$XDPY" ":$XDPY2" ) \
		> "$out.stdout" 2>&1 || fail "X reference run failed ($out)"
}

# ---------------------------------------------------------------------------
# Scenarios.  Every maze gets a natural trace and a longer finite replay:
#
#   arena        Arena, natural 1,000-point win: both humans drive/aim/fire,
#                bullets/tanks/walls collide, armor takes directional damage,
#                a kill is scored and the match wins (status 3).
#   arena2000    Arena, finite replay: each human kills and respawns once
#                (deaths=1, respawn), score 1000 each, no second kill.
#   combat1      combat1, finite replay: navigation/wall collisions, map and
#                radar state on a second, non-symmetric maze.
#   combat1-2000 combat1, finite replay: as above with the 2000-point setting.
#
# The full kill/win/death coverage is carried by the Arena traces (the union
# of all four exercises every required phenomenon); the combat1 traces add
# maze/geometry and wall-collision coverage.  Non-terminating traces are
# bounded by $LIMIT so both legs stop at the same applied tick.
# ---------------------------------------------------------------------------
scenarios() {
	case "$1" in
	  Arena)   echo "arena arena2000" ;;
	  combat1) echo "combat1 combat1-2000" ;;
	  *) fail "unknown maze $1" ;;
	esac
}

compare_scenario() { # key maze
	local key="$1" maze="$2"
	local settings="$FIX/p3.12-$key.settings" trace="$FIX/p3.12-$key.trace"
	[ -f "$settings" ] || fail "missing $settings"
	[ -f "$trace" ]    || fail "missing $trace"

	log "== scenario $key ($maze)"
	run_x "$settings" "$trace" "$WORK/$key.x.state"
	normalise < "$WORK/$key.x.state" > "$WORK/$key.x.norm"
	[ -s "$WORK/$key.x.norm" ] || fail "$key: empty X dump"

	local leg status
	for leg in direct network; do
		local reps_ok=1 r
		for r in 1 2 3; do
			env XTANK_P3_TICKS="$LIMIT" \
				"$WORK/parity_fixture" "$leg" "$REPO" "$maze" \
				"$settings" "$trace" "$WORK/$key.$leg.r$r" 2 \
				> "$WORK/$key.$leg.r$r.out" 2>&1 || \
				{ cat "$WORK/$key.$leg.r$r.out" >&2; \
				  fail "$key/$leg repeat $r failed"; }
			grep -q 'PARITY FIXTURE PASS' "$WORK/$key.$leg.r$r.out" || \
				fail "$key/$leg repeat $r did not PASS"
		done
		local e
		for e in 0 1; do
			cmp "$WORK/$key.$leg.r1.e$e" "$WORK/$key.$leg.r2.e$e" \
				>/dev/null || reps_ok=0
			cmp "$WORK/$key.$leg.r1.e$e" "$WORK/$key.$leg.r3.e$e" \
				>/dev/null || reps_ok=0
		done
		[ "$reps_ok" = 1 ] || fail "$key/$leg candidate repeats differ"

		for e in 0 1; do
			normalise < "$WORK/$key.$leg.r1.e$e" > "$WORK/$key.$leg.e$e.norm"
			if ! cmp "$WORK/$key.x.norm" "$WORK/$key.$leg.e$e.norm" \
					>/dev/null; then
				local fd
				fd=$(cmp "$WORK/$key.x.norm" \
					"$WORK/$key.$leg.e$e.norm" 2>&1 | head -1)
				fail "$key/$leg epoch $e differs from frozen X ($fd)"
			fi
		done
		log "    $key/$leg: X == epoch 0 == epoch 1"
	done

	{
		echo "scenario=$key"
		echo "maze=$maze"
		echo "settings_sha256=$(sha256sum "$settings" | awk '{print $1}')"
		echo "trace_sha256=$(sha256sum "$trace" | awk '{print $1}')"
		echo "x_sha256=$(sha256sum "$WORK/$key.x.norm" | awk '{print $1}')"
		echo "direct_sha256=$(sha256sum "$WORK/$key.direct.e0.norm" | awk '{print $1}')"
		echo "network_sha256=$(sha256sum "$WORK/$key.network.e0.norm" | awk '{print $1}')"
		echo "cmp=identical"
	} > "$WORK/$key.result"
}

# ---------------------------------------------------------------------------
# Coverage audit (P3.12 acceptance).  Byte identity only proves the shipped
# network path changes nothing; this proves the traces actually exercise the
# required authoritative state/phenomena.  It is a union check across the
# scenarios of a full run, so each phenomenon is carried by the most suitable
# maze (Arena carries kill/win/death, the finite replays carry respawn and a
# second maze's map/geometry).  Record tags: F2/V/VW/VT/VC/B/E + maze
# MX/MP/ML + radar RNH/RB (New Radar NRH/NRB is force-disabled by the shipped
# settings profile).
# ---------------------------------------------------------------------------
audit_coverage() {
	local keys="$1"
	local files="" k t tags
	local want="F2 V VW VT VC B E MX MP ML RNH RB"

	for k in $keys; do
		files="$files $WORK/$k.direct.r1.e0"
	done
	# shellcheck disable=SC2086
	tags=$(cat $files | awk '{print $1}' | sort -u | tr '\n' ' ')
	for t in $want; do
		case " $tags " in
		  *" $t "*) ;;
		  *) fail "coverage: missing $t record" ;;
		esac
	done
	# shellcheck disable=SC2086
	grep -aqE '^B [0-9]+ owner 0 ' $files || \
		fail "coverage: no bullets from player A"
	# shellcheck disable=SC2086
	grep -aqE '^B [0-9]+ owner 1 ' $files || \
		fail "coverage: no bullets from player B"
	# shellcheck disable=SC2086
	grep -aq '^E ' $files || fail "coverage: no explosions"
	# directional damage: a vehicle whose six armor sides differ
	# shellcheck disable=SC2086
	cat $files | awk '{for(i=1;i<=NF;i++) if($i=="armor"){for(j=1;j<6;j++) if($(i+j)!=$(i+j+1)) ok=1} if(ok) found=1} END{exit found?0:1}' || \
		fail "coverage: no directional armor damage"
	# shellcheck disable=SC2086
	grep -aqE 'deaths [1-9]' $files || fail "coverage: no deaths"
	# shellcheck disable=SC2086
	grep -aqE 'deathtimer [1-9]' $files || \
		fail "coverage: no death/respawn wait"
	# a natural win (status 3) with a winning score
	# shellcheck disable=SC2086
	cat $files | awk '/^F2 /{if($0~/ status 3 /) w=1} \
	    /^V /{for(i=1;i<=NF;i++) if($i=="score" && $(i+1)>=1000) s=1} \
	    END{exit (w&&s)?0:1}' || fail "coverage: no 1000-point win"
	log "    coverage: required records/phenomena present across:$keys"
}

if [ -n "$MAZE" ]; then
	for key in $(scenarios "$MAZE"); do
		case "$key" in
		  arena|arena2000) compare_scenario "$key" Arena ;;
		  combat1|combat1-2000) compare_scenario "$key" combat1 ;;
		esac
	done
else
	compare_scenario arena Arena
	compare_scenario arena2000 Arena
	compare_scenario combat1 combat1
	compare_scenario combat1-2000 combat1
	audit_coverage "arena arena2000 combat1 combat1-2000"
fi

log ""
log "P3.12 PARITY PASS (frozen-X == direct-intent == shipped network, full v2"
log "                  dump, two epochs, three repeats per leg)"
