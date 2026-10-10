#!/usr/bin/env bash
#
# regression.sh -- P1.1 bootstrap regression guardrail for the xtank Phase 1
# extraction.  Compares the current working tree (the "candidate") against a
# frozen, pinned X reference for the legacy STATE_DUMP v1 scenarios.
#
# Contract (PHASE1-PLAN.md, P1.1 / "Regression strategy"):
#   bash Tests/phase1/regression.sh --stage before --subject x
#   bash Tests/phase1/regression.sh --stage after  --subject x
#   bash Tests/phase1/regression.sh --suite full --stage after --subject x
#
# It builds an isolated reference and candidate object tree outside the
# checkout, runs the X reference under a harness-owned Xvfb, runs the
# candidate three times, and requires byte identity between every repeat, the
# reference, and the recorded before/after stage dumps.  No gameplay file is
# modified; only the reviewed read-only STATE_DUMP / seed instrumentation is
# overlaid on the pin.
#
# Evidence (dumps, logs, binaries) lives OUTSIDE the checkout under
#   ${XTANK_P1_EVIDENCE:-/tmp/opencode/xtank-phase1/p1.1}
#
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HARNESS="$REPO/Tests/phase1"
EVIDENCE="${XTANK_P1_EVIDENCE:-/tmp/opencode/xtank-phase1/p1.1}"
SCEN_DIR="$HARNESS/scenarios"

STAGE="after"
SUBJECT="x"
SUITE="bootstrap"
SCENARIO_FILTER=""
KEEP_XVFB=0
SELFTEST=0

usage() {
	cat >&2 <<'EOF'
usage: regression.sh [--stage before|after] [--subject x|headless|null|auto]
                     [--suite bootstrap|full] [--scenario NAME ...]
                     [--evidence DIR] [--keep-xvfb]
EOF
	exit 2
}

while [ $# -gt 0 ]; do
	case "$1" in
	  --stage)    STAGE="$2"; shift 2 ;;
	  --subject)  SUBJECT="$2"; shift 2 ;;
	  --suite)    SUITE="$2"; shift 2 ;;
	  --scenario) SCENARIO_FILTER="$SCENARIO_FILTER $2"; shift 2 ;;
	  --evidence) EVIDENCE="$2"; shift 2 ;;
	  --keep-xvfb) KEEP_XVFB=1; shift ;;
	  --selftest-validation) SELFTEST=1; shift ;;
	  *) usage ;;
	esac
done

case "$STAGE" in before|after) ;; *) echo "bad --stage $STAGE" >&2; exit 2 ;; esac
case "$SUITE"  in bootstrap|full) ;; *) echo "bad --suite $SUITE" >&2; exit 2 ;; esac

# 'auto' follows the plan's subject progression: candidate X through P1.5,
# headless thereafter, server after P1.15.  No such backend exists yet in
# P1.1, so 'auto' resolves to X.
if [ "$SUBJECT" = "auto" ]; then SUBJECT="x"; fi
case "$SUBJECT" in x|headless|null) ;; *) echo "bad --subject $SUBJECT" >&2; exit 2 ;; esac

mkdir -p "$EVIDENCE" "$EVIDENCE/runs"

log()  { printf '%s\n' "$*"; }
fail() { printf 'REGRESSION FAIL: %s\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------------------
# Scenario table (legacy v1 scenarios, exact copies of the scratch settings)
# ---------------------------------------------------------------------------
ALL_SCENARIOS="combat-2drone combat-4drone madman-2drone"

scen_period()   { case "$1" in combat-2drone) echo 100;; *) echo 200;; esac; }
scen_ticklim()  { case "$1" in madman-2drone) echo 65600;; *) echo "";; esac; }
scen_veh()      { case "$1" in combat-2drone) echo 2;; combat-4drone) echo 4;; madman-2drone) echo 2;; esac; }
scen_lines()    { case "$1" in combat-2drone) echo 798;; combat-4drone) echo 105;; madman-2drone) echo 984;; esac; }
scen_win()      { case "$1" in combat-2drone) echo 26569;; combat-4drone) echo 4004;; madman-2drone) echo 65600;; esac; }
scen_finished() { case "$1" in madman-2drone) echo no;; *) echo yes;; esac; }
scen_sha() {
	case "$1" in
	  combat-2drone) echo 76f23a333a0907f9bb40fa4d9027feac39f3eccdc11aa85aa34fa33109688c6a ;;
	  combat-4drone) echo e13b72584d6cc6d41ae0c944c7b1235ca88807f23159e7a593fc56f6fe192b6e ;;
	  madman-2drone) echo 440b0e5b7ccd53c6aed3a5b33916d2289c80889446a42b0c399986486b6f9c03 ;;
	esac
}

select_scenarios() {
	if [ -n "$SCENARIO_FILTER" ]; then
		echo $SCENARIO_FILTER
		return
	fi
	# P1.1 bootstrap and P1.2 full both cover the legacy v1 scenarios; 'full'
	# additionally runs the v2 per-tick bot-free characterization.
	echo $ALL_SCENARIOS
}

# ---------------------------------------------------------------------------
# v2 per-tick, bot-free characterization scenarios (P1.2)
# ---------------------------------------------------------------------------
V2_SCENARIOS="trace-quadrant trace-arena trace-arena-win"
V2_PERIOD=1
V2_TICKLIMIT=400

v2_settings() { echo "$SCEN_DIR/$1.settings"; }
v2_trace()    { echo "$SCEN_DIR/$1.trace"; }
v2_winning() {
	case "$1" in
	  trace-arena-win) echo 1000 ;;
	  *) echo 2000 ;;
	esac
}
v2_expect() {
	case "$1" in
	  trace-quadrant)  echo "wall,tank,bullet,directional,death,respawn,score,win,reset" ;;
	  trace-arena)     echo "wall,tank,bullet,directional,death,respawn" ;;
	  trace-arena-win) echo "wall,tank,bullet,directional,death,score,win,reset" ;;
	esac
}

select_v2_scenarios() {
	if [ -n "$SCENARIO_FILTER" ]; then
		echo $SCENARIO_FILTER
		return
	fi
	echo $V2_SCENARIOS
}

validate_dump_v2() {   # $1 = file
	local f="$1"
	[ -f "$f" ] || fail "missing v2 dump: $f"
	[ -s "$f" ] || fail "empty v2 dump: $f"
	local lastc
	lastc=$(tail -c 1 "$f" | od -An -tu1 | tr -d ' ')
	[ "$lastc" = "10" ] || fail "truncated v2 dump (no final newline): $f"
	head -1 "$f" | grep -q '^F2 ' || fail "v2 dump does not start with F2: $f"
	grep -q '^V ' "$f" || fail "v2 dump has no vehicle records: $f"
	# v2 must never carry screen coordinates or pointer/padding bytes.
	if grep -qi 'screen' "$f"; then
		fail "v2 dump contains a screen-coordinate field: $f"
	fi
}

# ---------------------------------------------------------------------------
# Xvfb lifecycle: own exactly one process; clean up only our own PID.
# ---------------------------------------------------------------------------
XDPY=""
XVFB_PID=""
XVFB_OWNED=0
XVFB_LOG=""

XDPY2=""
XVFB2_PID=""
XVFB2_OWNED=0
XVFB2_LOG=""

xvfb_alive() { DISPLAY=":$1" xdpyinfo >/dev/null 2>&1; }

display_free() {
	local d="$1"
	[ -e "/tmp/.X${d}-lock" ] && return 1
	[ -e "/tmp/.X11-unix/X${d}" ] && return 1
	pgrep -f "Xvfb :${d}( |$)" >/dev/null 2>&1 && return 1
	return 0
}

cleanup() {
	local rc=$?
	if [ "$KEEP_XVFB" = "0" ]; then
		if [ "$XVFB_OWNED" = "1" ] && [ -n "$XVFB_PID" ]; then
			kill "$XVFB_PID" 2>/dev/null || true
		fi
		if [ "$XVFB2_OWNED" = "1" ] && [ -n "$XVFB2_PID" ]; then
			kill "$XVFB2_PID" 2>/dev/null || true
		fi
	fi
	exit $rc
}
trap cleanup EXIT INT TERM

ensure_xvfb() {
	if [ -n "${XTANK_REGRESSION_DISPLAY:-}" ]; then
		XDPY="${XTANK_REGRESSION_DISPLAY#:}"
		xvfb_alive "$XDPY" || fail "XTANK_REGRESSION_DISPLAY=:$XDPY is not usable"
		log "== reusing external display :$XDPY"
		return
	fi

	local d
	for d in 99 98 97 96 95 94 93 92 91 90; do
		if display_free "$d"; then
			XDPY="$d"
			break
		fi
	done
	[ -n "$XDPY" ] || fail "no free X display in :90-:99"

	XVFB_LOG="$EVIDENCE/xvfb-$XDPY.log"
	setsid Xvfb ":$XDPY" -screen 0 1024x864x24 -nolisten tcp -noreset \
	    </dev/null >"$XVFB_LOG" 2>&1 &
	XVFB_PID=$!
	XVFB_OWNED=1
	echo "$XVFB_PID" > "$EVIDENCE/xvfb.pid"

	local i
	for i in $(seq 1 50); do
		if ! kill -0 "$XVFB_PID" 2>/dev/null; then
			fail "Xvfb :$XDPY exited during startup (see $XVFB_LOG)"
		fi
		if xvfb_alive "$XDPY"; then
			log "== started Xvfb :$XDPY (pid $XVFB_PID)"
			break
		fi
		sleep 0.2
	done
	xvfb_alive "$XDPY" || fail "Xvfb :$XDPY did not become ready (see $XVFB_LOG)"

	# The v2 bot-free suite drives two human player slots on two displays
	# (the legacy engine only accepts a player whose terminal name matches),
	# so start a second harness-owned server and set each display's
	# xtank.playerName resource to the fixture names used by the traces.
	if [ "$SUITE" = "full" ]; then
		local d
		for d in 89 88 87 86 85 84 83 82 81 80; do
			if display_free "$d"; then
				XDPY2="$d"
				break
			fi
		done
		[ -n "$XDPY2" ] || fail "no free second X display in :80-:89"

		XVFB2_LOG="$EVIDENCE/xvfb-$XDPY2.log"
		setsid Xvfb ":$XDPY2" -screen 0 1024x864x24 -nolisten tcp -noreset \
		    </dev/null >"$XVFB2_LOG" 2>&1 &
		XVFB2_PID=$!
		XVFB2_OWNED=1
		echo "$XVFB2_PID" > "$EVIDENCE/xvfb2.pid"
		for i in $(seq 1 50); do
			if ! kill -0 "$XVFB2_PID" 2>/dev/null; then
				fail "Xvfb :$XDPY2 exited during startup (see $XVFB2_LOG)"
			fi
			if xvfb_alive "$XDPY2"; then
				break
			fi
			sleep 0.2
		done
		xvfb_alive "$XDPY2" || \
		    fail "Xvfb :$XDPY2 did not become ready (see $XVFB2_LOG)"
		log "== started Xvfb :$XDPY2 (pid $XVFB2_PID)"

		printf 'xtank.playerName: playerA\n' | xrdb -display ":$XDPY" -merge
		printf 'xtank.playerName: playerB\n' | xrdb -display ":$XDPY2" -merge
	fi
}

# ---------------------------------------------------------------------------
# Dump validation: refuse missing, empty and truncated output.
# ---------------------------------------------------------------------------
validate_dump() {   # $1 = file, $2 = expected vehicles
	local f="$1" veh="$2"
	[ -f "$f" ] || fail "missing dump: $f"
	[ -s "$f" ] || fail "empty dump: $f"

	# A legacy v1 dump is a sequence of "F ..." headers each followed by one
	# "  V..." line per vehicle, and must end on a completed block with a
	# trailing newline.  Any other shape means truncation/corruption.
	local lastc
	lastc=$(tail -c 1 "$f" | od -An -tu1 | tr -d ' ')
	[ "$lastc" = "10" ] || fail "truncated dump (no final newline): $f"

	local nf nv total
	nf=$(grep -c '^F ' "$f" || true)
	nv=$(grep -c '^  V' "$f" || true)
	total=$(wc -l < "$f")
	[ "$nf" -gt 0 ] || fail "dump has no state frames: $f"
	[ "$((nf + nv))" = "$total" ] || fail "dump has unexpected lines: $f"
	[ "$((nv))" = "$((nf * veh))" ] || \
	    fail "dump block structure wrong (want $veh vehicles/block): $f"
	head -1 "$f" | grep -q '^F ' || fail "dump does not start with a frame: $f"
	tail -1 "$f" | grep -q '^  V' || fail "dump ends mid-block: $f"
}

# ---------------------------------------------------------------------------
# Run helpers
# ---------------------------------------------------------------------------
REF_BIN=""
REF_ROOT=""

run_reference() {   # $1 scen  $2 out
	local scen="$1" out="$2"
	local period tick
	period=$(scen_period "$scen"); tick=$(scen_ticklim "$scen")
	( cd "$REF_ROOT/Src" && \
	  env LC_ALL=C DISPLAY=":$XDPY" XTANK_DIR="$REF_ROOT" XTANK_SEED=114336 \
	      XTANK_DUMP="$out" XTANK_DUMP_PERIOD="$period" \
	      XTANK_TICK_LIMIT="$tick" LD_PRELOAD="$EVIDENCE/fixseed.so" \
	      timeout 900 "$REF_BIN" -S -D -P -X -F "$SCEN_DIR/$scen.settings" \
	      ":$XDPY" ) >"$out.stdout" 2>&1
}

# Seed source (P1.8): the "after" candidate takes its seed from server config
# (XTANK_SEED) with no preload; the pinned "before" baseline predates the
# config seed and still needs the reference-only fixseed.so shim.  The frozen
# X reference always keeps the shim.
run_candidate() {   # $1 scen  $2 out
	local scen="$1" out="$2"
	local cand="$EVIDENCE/candidate-$STAGE-$SUBJECT"
	local src="$cand/src"
	local period tick
	period=$(scen_period "$scen"); tick=$(scen_ticklim "$scen")
	local -a seedenv=()
	if [ "$STAGE" = "before" ]; then
		seedenv=(LD_PRELOAD="$EVIDENCE/fixseed.so")
	fi

	if [ "$SUBJECT" = "headless" ] || [ "$SUBJECT" = "null" ]; then
		( cd "$src/Src" && \
		  env -u DISPLAY -u LD_PRELOAD LC_ALL=C XTANK_DIR="$src" \
		      XTANK_SEED=114336 \
		      XTANK_DUMP="$out" XTANK_DUMP_PERIOD="$period" \
		      XTANK_TICK_LIMIT="$tick" "${seedenv[@]}" \
		      timeout 900 ./xtank-headless -S -D -P -X -F \
		      "$SCEN_DIR/$scen.settings" ) >"$out.stdout" 2>&1
	else
		( cd "$src/Src" && \
		  env -u LD_PRELOAD LC_ALL=C DISPLAY=":$XDPY" XTANK_DIR="$src" \
		      XTANK_SEED=114336 \
		      XTANK_DUMP="$out" XTANK_DUMP_PERIOD="$period" \
		      XTANK_TICK_LIMIT="$tick" "${seedenv[@]}" \
		      timeout 900 ./xtank-xcand -S -D -P -X -F \
		      "$SCEN_DIR/$scen.settings" ":$XDPY" ) >"$out.stdout" 2>&1
	fi
}

run_reference_v2() {   # $1 scen  $2 out
	local scen="$1" out="$2"
	local settings trace
	settings=$(v2_settings "$scen"); trace=$(v2_trace "$scen")
	( cd "$REF_ROOT/Src" && \
	  env LC_ALL=C DISPLAY=":$XDPY" XTANK_DIR="$REF_ROOT" XTANK_SEED=114336 \
	      XTANK_DUMP="$out" XTANK_DUMP_PERIOD="$V2_PERIOD" XTANK_DUMP_FORMAT=2 \
	      XTANK_TICK_LIMIT="$V2_TICKLIMIT" XTANK_TRACE="$trace" \
	      LD_PRELOAD="$EVIDENCE/fixseed.so" \
	      timeout 900 "$REF_BIN" -S -D -P -X -F "$settings" \
	      ":$XDPY" ":$XDPY2" ) >"$out.stdout" 2>&1
}

run_candidate_v2() {   # $1 scen  $2 out
	local scen="$1" out="$2"
	local cand="$EVIDENCE/candidate-$STAGE-$SUBJECT"
	local src="$cand/src"
	local settings trace
	settings=$(v2_settings "$scen"); trace=$(v2_trace "$scen")
	local -a seedenv=()
	if [ "$STAGE" = "before" ]; then
		seedenv=(LD_PRELOAD="$EVIDENCE/fixseed.so")
	fi

	if [ "$SUBJECT" = "null" ]; then
		# The display-free backend has no X resource database; the explicit
		# server config binds the fixture player names per terminal with -p
		# (replacing the former XTANK_NULL_PLAYERS hook) and gives main()
		# two terminal arguments so both human slots exist (the v2 traces
		# bind by name).
		( cd "$src/Src" && \
		  env -u DISPLAY -u LD_PRELOAD LC_ALL=C \
		      XTANK_DIR="$src" XTANK_SEED=114336 \
		      XTANK_DUMP="$out" XTANK_DUMP_PERIOD="$V2_PERIOD" \
		      XTANK_DUMP_FORMAT=2 XTANK_TICK_LIMIT="$V2_TICKLIMIT" \
		      XTANK_TRACE="$trace" "${seedenv[@]}" \
		      timeout 900 ./xtank-headless -S -D -P -X -F \
		      "$settings" -p playerA -p playerB nullterm0 nullterm1 ) \
		    >"$out.stdout" 2>&1
	elif [ "$SUBJECT" = "headless" ]; then
		( cd "$src/Src" && \
		  env -u DISPLAY -u LD_PRELOAD LC_ALL=C XTANK_DIR="$src" \
		      XTANK_SEED=114336 \
		      XTANK_DUMP="$out" XTANK_DUMP_PERIOD="$V2_PERIOD" \
		      XTANK_DUMP_FORMAT=2 XTANK_TICK_LIMIT="$V2_TICKLIMIT" \
		      XTANK_TRACE="$trace" "${seedenv[@]}" \
		      timeout 900 ./xtank-headless -S -D -P -X -F \
		      "$settings" ) >"$out.stdout" 2>&1
	else
		( cd "$src/Src" && \
		  env -u LD_PRELOAD LC_ALL=C DISPLAY=":$XDPY" XTANK_DIR="$src" \
		      XTANK_SEED=114336 \
		      XTANK_DUMP="$out" XTANK_DUMP_PERIOD="$V2_PERIOD" \
		      XTANK_DUMP_FORMAT=2 XTANK_TICK_LIMIT="$V2_TICKLIMIT" \
		      XTANK_TRACE="$trace" "${seedenv[@]}" \
		      timeout 900 ./xtank-xcand -S -D -P -X -F \
		      "$settings" ":$XDPY" ":$XDPY2" ) >"$out.stdout" 2>&1
	fi
}

# ---------------------------------------------------------------------------
# Metric checks against the plan's frozen values.
# ---------------------------------------------------------------------------
check_metrics() {   # $1 scen  $2 file
	local scen="$1" f="$2"
	local want_lines want_win want_sha finished
	want_lines=$(scen_lines "$scen"); want_win=$(scen_win "$scen")
	want_sha=$(scen_sha "$scen"); finished=$(scen_finished "$scen")

	local got_lines got_sha
	got_lines=$(wc -l < "$f")
	got_sha=$(sha256sum "$f" | awk '{print $1}')

	[ "$got_lines" = "$want_lines" ] || \
	    fail "$scen: line count $got_lines != frozen $want_lines"
	[ "$got_sha" = "$want_sha" ] || \
	    fail "$scen: sha256 $got_sha != frozen $want_sha"

	local last_f got_win got_hash last_status
	last_f=$(grep '^F ' "$f" | tail -1)
	got_win=$(printf '%s\n' "$last_f" | awk '{print $2}')
	got_hash=$(printf '%s\n' "$last_f" | sed -n 's/.*hash \([0-9a-fA-F]*\).*/\1/p')
	last_status=$(printf '%s\n' "$last_f" | awk '{print $4}')

	[ "$got_win" = "$want_win" ] || \
	    fail "$scen: final frame $got_win != frozen $want_win"
	if [ "$finished" = "yes" ]; then
		[ "$last_status" != "0" ] || \
		    fail "$scen: match did not terminate (final status 0)"
	fi
	log "    $scen: sha=$got_sha lines=$got_lines frame=$got_win hash=$got_hash"
}

# ---------------------------------------------------------------------------
# Validation self-test: prove the guardrail refuses malformed dumps.
# ---------------------------------------------------------------------------
selftest_validation() {
	local d
	d=$(mktemp -d)
	trap "rm -rf '$d'" EXIT
	local rc=0

	printf 'F 1 status 0 alive 2 dead 0 bullets 0 exp 0 hash 00000000\n  V0 x\n  V1 y\n' \
	    > "$d/good.state"
	printf 'F 1 status 0 alive 2 dead 0 bullets 0 exp 0 hash 00000000\n  V0 x\n' \
	    > "$d/shortblock.state"
	printf 'F 1 status 0 alive 2 dead 0 bullets 0 exp 0 hash 00000000\n  V0 x\n  V1 y' \
	    > "$d/nonewline.state"
	: > "$d/empty.state"

	check() { # $1 desc  $2 expect(accept|refuse)  $3 file  $4 veh
		if ( validate_dump "$3" "$4" ) >/dev/null 2>&1; then
			got=accept
		else
			got=refuse
		fi
		if [ "$got" = "$2" ]; then
			log "  [ok]   $1 -> $got"
		else
			log "  [FAIL] $1 -> $got (want $2)"
			rc=1
		fi
	}
	check "complete dump"            accept "$d/good.state" 2
	check "missing dump"             refuse "$d/absent.state" 2
	check "empty dump"               refuse "$d/empty.state" 2
	check "truncated (no newline)"   refuse "$d/nonewline.state" 2
	check "wrong vehicle count"      refuse "$d/good.state" 4
	check "incomplete final block"   refuse "$d/shortblock.state" 2

	[ "$rc" = "0" ] && log "VALIDATION SELFTEST PASS" || log "VALIDATION SELFTEST FAIL"
	return "$rc"
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
if [ "$SELFTEST" = "1" ]; then
	selftest_validation
	exit $?
fi

log "== P1.1 regression: stage=$STAGE subject=$SUBJECT suite=$SUITE"
log "   repo=$REPO"
log "   evidence=$EVIDENCE"

# 1. Frozen reference (built once; manifest enforced by build-reference.sh).
bash "$HARNESS/build-reference.sh" --mode reference --evidence "$EVIDENCE" \
    > "$EVIDENCE/build-reference.out" 2>&1
REF_ROOT="$EVIDENCE/reference/src"
REF_BIN="$REF_ROOT/Src/xtank-refdump"
[ -x "$REF_BIN" ] || fail "reference binary missing: $REF_BIN"

# 2. Candidate for this stage/subject.
bash "$HARNESS/build-reference.sh" --mode candidate --subject "$SUBJECT" \
    --stage "$STAGE" --evidence "$EVIDENCE" \
    > "$EVIDENCE/build-candidate-$STAGE-$SUBJECT.out" 2>&1

# 3. Build the reference-only seed shim if absent.
if [ ! -f "$EVIDENCE/fixseed.so" ] || [ "$HARNESS/fixseed.c" -nt "$EVIDENCE/fixseed.so" ]; then
	gcc -shared -fPIC -O2 -o "$EVIDENCE/fixseed.so" "$HARNESS/fixseed.c"
fi
[ -f "$EVIDENCE/fixseed.so" ] || fail "could not build fixseed.so"

# 4. Xvfb for the X reference (and for an X candidate).
ensure_xvfb

# 4. Per-scenario comparison.
rc=0
for scen in $(select_scenarios); do
	log "== scenario $scen (period $(scen_period "$scen"), vehicles $(scen_veh "$scen"))"
	veh=$(scen_veh "$scen")
	run_dir="$EVIDENCE/runs"

	run_reference "$scen" "$run_dir/$scen.x.state"
	validate_dump "$run_dir/$scen.x.state" "$veh"

	cmp_reps_ok=1
	for r in 1 2 3; do
		run_candidate "$scen" "$run_dir/$scen.$STAGE.r$r.state"
		validate_dump "$run_dir/$scen.$STAGE.r$r.state" "$veh"
	done
	cmp "$run_dir/$scen.$STAGE.r1.state" "$run_dir/$scen.$STAGE.r2.state" >/dev/null \
	    || cmp_reps_ok=0
	cmp "$run_dir/$scen.$STAGE.r1.state" "$run_dir/$scen.$STAGE.r3.state" >/dev/null \
	    || cmp_reps_ok=0
	[ "$cmp_reps_ok" = "1" ] || fail "$scen: candidate repeats are not identical"

	if ! cmp "$run_dir/$scen.x.state" "$run_dir/$scen.$STAGE.r1.state" >/dev/null; then
		firstdiff=$(cmp "$run_dir/$scen.x.state" "$run_dir/$scen.$STAGE.r1.state" 2>&1 | head -1)
		fail "$scen: candidate differs from X reference ($firstdiff)"
	fi

	# Record the stage dump and cross-check before vs after when both exist.
	cp -f "$run_dir/$scen.$STAGE.r1.state" "$run_dir/$scen.$STAGE.state"
	if [ -f "$run_dir/$scen.before.state" ] && [ -f "$run_dir/$scen.after.state" ]; then
		cmp "$run_dir/$scen.before.state" "$run_dir/$scen.after.state" >/dev/null \
		    || fail "$scen: before and after dumps differ"
		log "    before == after"
	fi

	check_metrics "$scen" "$run_dir/$scen.$STAGE.state"

	# Persist a concise run record.
	{
		echo "scenario=$scen"
		echo "stage=$STAGE"
		echo "subject=$SUBJECT"
		echo "period=$(scen_period "$scen")"
		echo "tick_limit=$(scen_ticklim "$scen")"
		echo "x_sha256=$(sha256sum "$run_dir/$scen.x.state" | awk '{print $1}')"
		echo "candidate_sha256=$(sha256sum "$run_dir/$scen.$STAGE.state" | awk '{print $1}')"
		echo "cmp=identical"
	} > "$run_dir/$scen.$STAGE.$SUBJECT.result"
done

# ---------------------------------------------------------------------------
# 5. v2 per-tick, bot-free characterization (suite full)
# ---------------------------------------------------------------------------
if [ "$SUITE" = "full" ]; then
	log "== v2 per-tick characterization (period $V2_PERIOD, tick-limit $V2_TICKLIMIT)"
	gcc -O2 -Wall -o "$EVIDENCE/state_dump" "$HARNESS/state_dump.c" -lm \
	    || fail "could not build state_dump analyzer"
	GOLDEN="$HARNESS/v2-golden.sha256"

	for scen in $(select_v2_scenarios); do
		settings=$(v2_settings "$scen"); trace=$(v2_trace "$scen")
		[ -f "$settings" ] || fail "$scen: missing settings $settings"
		[ -f "$trace" ]    || fail "$scen: missing trace $trace"
		expect=$(v2_expect "$scen"); winning=$(v2_winning "$scen")
		log "== v2 scenario $scen (winning score $winning)"
		run_dir="$EVIDENCE/runs"

		run_reference_v2 "$scen" "$run_dir/$scen.v2.x.state"
		validate_dump_v2 "$run_dir/$scen.v2.x.state"

		cmp_reps_ok=1
		for r in 1 2 3; do
			run_candidate_v2 "$scen" "$run_dir/$scen.v2.$STAGE.r$r.state"
			validate_dump_v2 "$run_dir/$scen.v2.$STAGE.r$r.state"
		done
		cmp "$run_dir/$scen.v2.$STAGE.r1.state" \
		    "$run_dir/$scen.v2.$STAGE.r2.state" >/dev/null || cmp_reps_ok=0
		cmp "$run_dir/$scen.v2.$STAGE.r1.state" \
		    "$run_dir/$scen.v2.$STAGE.r3.state" >/dev/null || cmp_reps_ok=0
		[ "$cmp_reps_ok" = "1" ] || \
		    fail "$scen: v2 candidate repeats are not identical"

		if ! cmp "$run_dir/$scen.v2.x.state" \
			 "$run_dir/$scen.v2.$STAGE.r1.state" >/dev/null; then
			firstdiff=$(cmp "$run_dir/$scen.v2.x.state" \
			    "$run_dir/$scen.v2.$STAGE.r1.state" 2>&1 | head -1)
			fail "$scen: v2 candidate differs from X reference ($firstdiff)"
		fi

		# Validate the frozen trace and the X golden before trusting events.
		if [ -f "$GOLDEN" ]; then
			want=$(awk -v s="$scen" '$1==s {print $2}' "$GOLDEN")
			got=$(sha256sum "$run_dir/$scen.v2.x.state" | awk '{print $1}')
			[ -n "$want" ] || fail "$scen: no frozen golden entry"
			[ "$want" = "$got" ] || \
			    fail "$scen: X v2 golden sha256 mismatch ($got != $want)"
		fi

		# Event assertions: identical on X and candidate, and complete.
		"$EVIDENCE/state_dump" --file "$run_dir/$scen.v2.x.state" \
		    --winning-score "$winning" --expect "$expect" \
		    > "$run_dir/$scen.v2.x.assert" 2>&1 || \
		    fail "$scen: X assertions failed (see $scen.v2.x.assert)"
		"$EVIDENCE/state_dump" --file "$run_dir/$scen.v2.$STAGE.r1.state" \
		    --winning-score "$winning" --expect "$expect" \
		    > "$run_dir/$scen.v2.$STAGE.assert" 2>&1 || \
		    fail "$scen: candidate assertions failed"
		cmp "$run_dir/$scen.v2.x.assert" \
		    "$run_dir/$scen.v2.$STAGE.assert" >/dev/null || \
		    fail "$scen: X and candidate event assertions differ"

		sed -n '1,9p' "$run_dir/$scen.v2.x.assert" | \
		    while IFS= read -r l; do log "    $l"; done

		cp -f "$run_dir/$scen.v2.$STAGE.r1.state" \
		    "$run_dir/$scen.v2.$STAGE.state"
		{
			echo "scenario=$scen"
			echo "format=v2"
			echo "stage=$STAGE"
			echo "subject=$SUBJECT"
			echo "period=$V2_PERIOD"
			echo "tick_limit=$V2_TICKLIMIT"
			echo "winning_score=$winning"
			echo "trace_sha256=$(sha256sum "$trace" | awk '{print $1}')"
			echo "settings_sha256=$(sha256sum "$settings" | awk '{print $1}')"
			echo "x_sha256=$(sha256sum "$run_dir/$scen.v2.x.state" | awk '{print $1}')"
			echo "candidate_sha256=$(sha256sum "$run_dir/$scen.v2.$STAGE.state" | awk '{print $1}')"
			echo "cmp=identical"
		} > "$run_dir/$scen.v2.$STAGE.$SUBJECT.result"
	done
fi

# 6. Record the frozen reference manifest fingerprint with this run.
sha256sum "$EVIDENCE/reference/MANIFEST" > "$EVIDENCE/reference/MANIFEST.sha256"

log ""
log "P1.2 REGRESSION PASS (stage=$STAGE subject=$SUBJECT suite=$SUITE)"
