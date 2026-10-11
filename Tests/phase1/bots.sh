#!/usr/bin/env bash
#
# bots.sh -- P1.13 acceptance fixture for tick-cooperative drone scheduling.
#
#   bash Tests/phase1/bots.sh --repeats 3 --cpu-load
#   bash Tests/phase1/bots.sh --minutes 60 --vehicles 4 --resets 20
#
# Repeat mode (--repeats N, optional --cpu-load):
#   * builds the display-free (null) candidate;
#   * replays every frozen drone scenario N times, optionally with a pool of
#     CPU hogs saturating the host;
#   * requires every run to complete and to be byte-identical to the frozen
#     reference SHA-256 and to every other repeat.  Because bot scheduling is
#     tick-cooperative (no CPU-time decisions), host load must not change a
#     single byte of simulation state.
#   * when strace is available, audits the yield cadence: over a bounded run
#     every eligible bot run arms and disarms exactly one virtual timer, so the
#     number of ITIMER_VIRTUAL setitimer() calls is exactly 2 * ticks * bots
#     (one iteration/yield per eligible tick).
#
# Long mode (--minutes M --vehicles 4 --resets R):
#   * runs the 4-drone match through R lifecycle resets in one process with
#     the server's fast replay, so simulated time is at least M minutes;
#   * proves movement (positions change), targeting (turret angles change) and
#     fire (bullets are spawned), that every epoch makes progress (no stall),
#     that the process exits cleanly (no crash), and that each reset leaves no
#     stale vehicle/context (SERVER-LIFECYCLE cv=null term=null).
#
# Generated objects and dumps live outside the checkout (default
# /tmp/opencode/xtank-phase1/p1.13).
#
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HARNESS="$REPO/Tests/phase1"
EVIDENCE="${XTANK_P1_EVIDENCE:-/tmp/opencode/xtank-phase1/p1.13}"
SCEN_DIR="$HARNESS/scenarios"
WORK="$EVIDENCE/bots"

REPEATS=3
CPU_LOAD=0
MINUTES=0
VEHICLES=4
RESETS=0
KEEP=0

usage() {
	echo "usage: $0 [--repeats N] [--cpu-load]" >&2
	echo "          [--minutes M --vehicles 2|4 --resets R]" >&2
	echo "          [--evidence DIR] [--keep]" >&2
	exit 2
}

while [ $# -gt 0 ]; do
	case "$1" in
	  --repeats) REPEATS="$2"; shift 2 ;;
	  --cpu-load) CPU_LOAD=1; shift ;;
	  --minutes) MINUTES="$2"; shift 2 ;;
	  --vehicles) VEHICLES="$2"; shift 2 ;;
	  --resets) RESETS="$2"; shift 2 ;;
	  --evidence) EVIDENCE="$2"; shift 2 ;;
	  --keep) KEEP=1; shift ;;
	  *) usage ;;
	esac
done
WORK="$EVIDENCE/bots"

mkdir -p "$WORK"

log()  { printf '%s\n' "$*"; }
fail() { printf 'BOTS FAIL: %s\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------------------
# Frozen scenario table (kept in step with regression.sh).
# ---------------------------------------------------------------------------
ALL_SCENARIOS="combat-2drone combat-4drone madman-2drone"
scen_period()  { case "$1" in combat-2drone) echo 100;; *) echo 200;; esac; }
scen_ticklim() { case "$1" in madman-2drone) echo 65600;; *) echo "";; esac; }
scen_bots()    { case "$1" in combat-2drone) echo 2;; combat-4drone) echo 4;; madman-2drone) echo 2;; esac; }
scen_sha() {
	case "$1" in
	  combat-2drone) echo 76f23a333a0907f9bb40fa4d9027feac39f3eccdc11aa85aa34fa33109688c6a ;;
	  combat-4drone) echo e13b72584d6cc6d41ae0c944c7b1235ca88807f23159e7a593fc56f6fe192b6e ;;
	  madman-2drone) echo 440b0e5b7ccd53c6aed3a5b33916d2289c80889446a42b0c399986486b6f9c03 ;;
	esac
}

# ---------------------------------------------------------------------------
# CPU load: a pool of busy loops, killed on exit.
# ---------------------------------------------------------------------------
HOGS=()
start_cpu_load() {
	local n i
	n=$(nproc 2>/dev/null || echo 2)
	[ "$n" -gt 8 ] && n=8
	log "== starting $n cpu-load hogs"
	for i in $(seq 1 "$n"); do
		( while :; do :; done ) &
		HOGS+=("$!")
	done
}
stop_cpu_load() {
	local p
	for p in "${HOGS[@]:-}"; do
		[ -n "$p" ] && kill "$p" 2>/dev/null || true
	done
	HOGS=()
}
trap 'stop_cpu_load' EXIT INT TERM

# ---------------------------------------------------------------------------
# Build the display-free (null) oracle candidate.
# ---------------------------------------------------------------------------
build_candidate() {
	bash "$HARNESS/build-reference.sh" --mode candidate --subject null \
	    --stage after --evidence "$EVIDENCE" \
	    > "$WORK/build-candidate.out" 2>&1 || \
	    fail "candidate build failed (see $WORK/build-candidate.out)"
	CAND="$EVIDENCE/candidate-after-null/src"
	BIN="$CAND/Src/xtank-headless"
	[ -x "$BIN" ] || fail "candidate binary missing: $BIN"
}

# ---------------------------------------------------------------------------
# One frozen-scenario replay (fast: the loop's own -D bypass).
# ---------------------------------------------------------------------------
run_one() {   # scen out tag
	local scen="$1" out="$2" tag="$3"
	local period tick
	period=$(scen_period "$scen"); tick=$(scen_ticklim "$scen")
	log "== $tag: scenario=$scen period=$period ticks=${tick:-auto}"
	( cd "$CAND/Src" && \
	  env -u DISPLAY -u LD_PRELOAD LC_ALL=C \
	      XTANK_DIR="$CAND" XTANK_SEED=114336 \
	      XTANK_DUMP="$out" XTANK_DUMP_PERIOD="$period" \
	      XTANK_TICK_LIMIT="$tick" \
	      timeout 900 "$BIN" -S -D -P -X -F "$SCEN_DIR/$scen.settings" \
	) > "$out.stdout" 2>&1 || fail "$tag run failed (see $out.stdout)"
	[ -s "$out" ] || fail "$tag produced no dump"
}

check_frozen() {   # scen file
	local scen="$1" f="$2" want got
	want=$(scen_sha "$scen")
	got=$(sha256sum "$f" | awk '{print $1}')
	[ "$got" = "$want" ] || \
	    fail "$scen: $f sha256 $got != frozen $want"
}

# ---------------------------------------------------------------------------
# Yield cadence audit: exactly one arm + one disarm per eligible bot run.
# ---------------------------------------------------------------------------
yield_audit() {
	command -v strace >/dev/null 2>&1 || return 0
	local scen=combat-2drone ticks=50 bots period out log
	period=$(scen_period "$scen"); bots=$(scen_bots "$scen")
	out="$WORK/yield-audit.state"; log="$WORK/yield-audit.strace"
	( cd "$CAND/Src" && \
	  env -u DISPLAY -u LD_PRELOAD LC_ALL=C \
	      XTANK_DIR="$CAND" XTANK_SEED=114336 \
	      XTANK_DUMP="$out" XTANK_DUMP_PERIOD=1 XTANK_TICK_LIMIT="$ticks" \
	      XTANK_SERVER_TICKS="$ticks" XTANK_SERVER_FAST=1 \
	      strace -f -e trace=setitimer -o "$log" \
	          "$BIN" -S -D -P -X -F "$SCEN_DIR/$scen.settings" \
	) > "$WORK/yield-audit.stdout" 2>&1 || fail "yield audit run failed"
	local calls want
	calls=$(grep -c 'setitimer(ITIMER_VIRTUAL' "$log" || true)
	want=$(( 2 * ticks * bots ))
	[ "$calls" = "$want" ] || \
	    fail "yield cadence: $calls ITIMER_VIRTUAL calls, want $want (2*$ticks*$bots)"
	log "   yield audit: $calls ITIMER_VIRTUAL calls == 2 * $ticks ticks * $bots bots"
}

# ---------------------------------------------------------------------------
# Watchdog diagnosis: a bot that never yields is reported and aborted; it is
# never preempted or silently yielded.  Self-contained fixture over the real
# icounter.c watchdog.
# ---------------------------------------------------------------------------
watchdog_check() {
	local fix="$WORK/watchdog_fixture.c" bin="$WORK/watchdog_fixture"
	cat > "$fix" <<'FIXTURE'
/* P1.13 watchdog acceptance fixture: a "bot" that never yields must be
   diagnosed and aborted, not preempted. */
#include <stdio.h>
#include <stdlib.h>
#include "common.h"
#include "tanktypes.h"
#include "clfkr.h"
#include "vehicle.h"
#include "program.h"
#include "server.h"
#include "icounter.h"

int server_loop_active(void) { return 1; }
CLFkr command_options;
static Vehicle self;
Vehicle *cv = &self;
int frame = 42;
static Prog_desc desc;
static Program prog;

int
main(void)
{
	setvbuf(stderr, NULL, _IONBF, 0);
	/* 1 ms budget: the first 10 ms virtual-timer tick trips the watchdog. */
	setenv("XTANK_BOT_WATCHDOG_MS", "1", 1);
	desc.name = "spin";
	prog.desc = &desc;
	self.number = 3;
	self.current_prog = &prog;
	setup_counter();
	program_watchdog_start();
	for (;;)			/* never yields */
		;
}
FIXTURE
	gcc -O0 -g -w -DUNIX -I"$REPO/Src" -I"$REPO/Src/Include" \
	    -o "$bin" "$REPO/Src/icounter.c" "$fix" || \
	    fail "watchdog fixture did not build"

	local rc=0
	timeout 30 "$bin" > "$WORK/watchdog.out" 2> "$WORK/watchdog.err" || rc=$?
	[ "$rc" = "70" ] || fail "watchdog did not abort a non-yielding bot (rc=$rc)"
	grep -q "XTANK WATCHDOG: bot 'spin' (vehicle 3) did not yield" \
	    "$WORK/watchdog.err" || \
	    fail "watchdog diagnosis missing: $(cat "$WORK/watchdog.err")"
	log "   watchdog: non-yielding bot diagnosed and aborted (rc=70): $(cat "$WORK/watchdog.err")"
}

# ---------------------------------------------------------------------------
# Repeat mode.
# ---------------------------------------------------------------------------
repeat_mode() {
	local scen r out
	log "== P1.13 repeat mode: repeats=$REPEATS cpu_load=$CPU_LOAD"
	build_candidate
	[ "$CPU_LOAD" = "1" ] && start_cpu_load

	for scen in $ALL_SCENARIOS; do
		for r in $(seq 1 "$REPEATS"); do
			out="$WORK/$scen.r$r.state"
			if [ "$CPU_LOAD" = "1" ]; then
				run_one "$scen" "$out" "$scen repeat $r (cpu-load)"
			else
				run_one "$scen" "$out" "$scen repeat $r"
			fi
			check_frozen "$scen" "$out"
		done
		for r in $(seq 2 "$REPEATS"); do
			cmp "$WORK/$scen.r1.state" "$WORK/$scen.r$r.state" >/dev/null || \
			    fail "$scen: repeat 1 != repeat $r"
		done
		log "   $scen: $REPEATS repeats identical to frozen reference"
	done

	watchdog_check
	yield_audit

	stop_cpu_load
	log ""
	log "BOTS PASS (repeat mode: $REPEATS x ${ALL_SCENARIOS// /,} byte-identical to frozen; cpu-load=$CPU_LOAD)"
}

# ---------------------------------------------------------------------------
# Long soak mode: R resets in one process over R+1 matches.
# ---------------------------------------------------------------------------
long_mode() {
	local scen out period hz=15 need_ticks
	case "$VEHICLES" in
	  2) scen=combat-2drone ;;
	  4) scen=combat-4drone ;;
	  *) fail "bad --vehicles $VEHICLES (want 2 or 4)" ;;
	esac
	period=15
	need_ticks=$(( MINUTES * 60 * hz ))
	out="$WORK/long-$scen.state"
	log "== P1.13 long mode: scenario=$scen minutes=$MINUTES resets=$RESETS"
	log "   need >= $need_ticks simulated ticks (${MINUTES}m @ ${hz}Hz)"
	build_candidate

	log "== running ${RESETS} lifecycle resets (fast replay)"
	( cd "$CAND/Src" && \
	  env -u DISPLAY -u LD_PRELOAD LC_ALL=C \
	      XTANK_DIR="$CAND" XTANK_SEED=114336 \
	      XTANK_DUMP="$out" XTANK_DUMP_PERIOD="$period" XTANK_DUMP_FORMAT=2 \
	      XTANK_LIFECYCLE="$RESETS" \
	      timeout 3600 "$BIN" -S -D -P -X -F "$SCEN_DIR/$scen.settings" \
	) > "$WORK/long-$scen.stdout" 2>&1 || \
	    fail "long run failed (see $WORK/long-$scen.stdout)"
	[ -s "$out" ] || fail "long run produced no dump"

	# Clean exit / no stale context across resets.
	grep -q "SERVER-LIFECYCLE epochs=$(( RESETS + 1 )) alive=0 dead=0 cv=null term=null" \
	    "$WORK/long-$scen.stdout" || \
	    fail "long run did not leave a clean reset state (see long-$scen.stdout)"
	log "   clean lifecycle: $(grep -o 'SERVER-LIFECYCLE.*' "$WORK/long-$scen.stdout")"

	# Movement / targeting / fire / progress, from the v2 dump.
	local stats="$WORK/long-$scen.stats"
	awk '
	  BEGIN { prev_ep = -999 }
	  /^F2 / {
	      ep=$4; tk=$2
	      if (ep != prev_ep) { nepoch++; prev_ep=ep }
	      if (tk > maxt) maxt=tk
	      ticks++
	      next
	  }
	  /^V / {
	      num=$2; px=""; py=""
	      for (k=1;k<=NF;k++){ if($k=="pos"){px=$(k+1); py=$(k+2); break} }
	      if (px!="") {
	          if (have[num]) { dx=px-lx[num]; dy=py-ly[num]; if(dx<0)dx=-dx; if(dy<0)dy=-dy; disp[num]+=dx+dy }
	          lx[num]=px; ly[num]=py; have[num]=1; seen[num]=1
	      }
	      next
	  }
	  /^VT / {
	      ang=""; for (k=1;k<=NF;k++){ if($k=="angle"){ang=$(k+1); break} }
	      key=$2 SUBSEP ang
	      if (!(key in seenang)) { seenang[key]=1; nangle++ }
	      next
	  }
	  /^B / { nbullets++; next }
	  END {
	      minv=1e18
	      for (i=0;i<16;i++) if (seen[i] && disp[i]<minv) minv=disp[i]
	      printf "epochs=%d ticks=%d maxtick=%d nangle=%d nbullets=%d minmove=%.1f\n",
	             nepoch, ticks, maxt, nangle, nbullets, (minv==1e18?0:minv)
	  }
	' "$out" > "$stats"
	cat "$stats" | while IFS= read -r l; do log "   $l"; done

	eval "$(sed -E 's/([a-z]+)=([0-9.]+)/\1=\2 /g' "$stats")"
	: "${epochs:=0}"; : "${ticks:=0}"; : "${nangle:=0}"; : "${nbullets:=0}"
	: "${minmove:=0}"; : "${maxtick:=0}"

	[ "$((epochs))" -ge "$(( RESETS + 1 ))" ] || \
	    fail "long run produced $epochs epochs, want >= $(( RESETS + 1 ))"
	[ "$((ticks))" -ge "$need_ticks" ] || \
	    fail "long run advanced $ticks ticks, want >= $need_ticks (${MINUTES}m)"
	[ "$((nangle))" -gt "$VEHICLES" ] || fail "no turret-angle change (targeting)"
	[ "$((nbullets))" -gt 0 ] || fail "no bullets spawned (fire)"
	awk -v m="$minmove" 'BEGIN { exit (m > 0) ? 0 : 1 }' || \
	    fail "a vehicle never moved (minmove=$minmove)"

	log ""
	log "BOTS PASS (long mode: $epochs epochs/$ticks ticks, targeting=$nangle, bullets=$nbullets, minmove=$minmove)"
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
if [ "$MINUTES" -gt 0 ]; then
	long_mode
else
	repeat_mode
fi

[ "$KEEP" = "1" ] || rm -f "$WORK"/*.stdout
