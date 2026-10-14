#!/usr/bin/env bash
#
# timing.sh -- P3.13 four-vehicle pacing, bandwidth and latency measurement.
#
# Contract (docs/PHASE3-PLAN.md P3.13):
#   bash Tests/phase3/timing.sh [--evidence DIR] [--stage STAGE]
#
# STAGE is one of: build | pace | stress | replay | analyze | all (default).
# Every stage may be run alone and re-entered; evidence is kept outside the
# checkout (default under $XTANK_P3_EVIDENCE or the p3.13 evidence root).
#
# What it proves:
#   pace     two humans + two drones, real-time 15 Hz using the *shipped*
#            absolute-deadline pacer, clients rendering at 30/144/mixed FPS,
#            each producing exactly 900 ticks / 60 s (+/-1).  Records per-tick
#            ingress/service/admission, sim, export and encode wall time (from
#            the bounded read-only instrumentation in Src/), total per-tick
#            work excluding pace sleep, late ticks, per-peer/aggregate bytes,
#            actual ENet wire headers+fragments, entity/view maxima, queue/RSS
#            maxima and same-client input-to-visible latency.
#   stress   fast maximum-density run; records the entity/view maxima reached
#            and independently proves the maximum authorized VIEW_SNAPSHOT
#            bound (all arrays at engine maximum) encodes/round-trips.
#   replay   exact finite applied-log replay invariance: the shipped network
#            leg (and the direct leg, i.e. zero exports) produce byte-identical
#            frozen v2 per-tick dumps at 15/30/144/mixed render rates and
#            across export counts.
#
# The frozen simulation is unchanged; only bounded read-only instrumentation is
# consulted.  Generated binaries, dumps and logs live OUTSIDE the checkout.
#
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HARNESS="$REPO/Tests/phase3"
FIXP="$HARNESS/fixtures/parity"
FIXT="$HARNESS/fixtures/timing"
EVIDENCE="${XTANK_P3_EVIDENCE:-/home/hatch/workspace/coding/xtank-evidence/p3.13}"

ENET_URL="https://github.com/lsalzman/enet/archive/refs/tags/v1.3.18.tar.gz"
ENET_SHA="28603c895f9ed24a846478180ee72c7376b39b4bb1287b73877e5eae7d96b0dd"

STAGE="all"
while [ $# -gt 0 ]; do
	case "$1" in
	  --evidence) EVIDENCE="$2"; shift 2 ;;
	  --stage) STAGE="$2"; shift 2 ;;
	  *) echo "usage: timing.sh [--evidence DIR] [--stage STAGE]" >&2; exit 2 ;;
	esac
done

WORK="$EVIDENCE/timing"
mkdir -p "$EVIDENCE" "$WORK"

log()  { printf '%s\n' "$*"; }
fail() { printf 'TIMING FAIL: %s\n' "$*" >&2; exit 1; }

# ---- host record (D3-G delta is reported explicitly, never claimed) ----
host_record() {
	{
		echo "nproc=$(nproc)"
		echo "ram_total_kb=$(awk '/MemTotal/{print $2}' /proc/meminfo)"
		echo "kernel=$(uname -r)"
		echo "arch=$(uname -m)"
		echo "gcc=$(gcc -dumpversion)"
		echo "date=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
	} > "$WORK/host.txt"
	cat "$WORK/host.txt"
	NPROC=$(nproc)
	[ "$NPROC" -ge 4 ] || \
		log "WARNING: host has $NPROC cores; D3-G reference asks >=4 (DELTA)"
}

# ---- dependencies (pinned ENet v1.3.18) --------------------------------
ENET_DIR="${XTANK_ENET_DIR:-}"
if [ -z "$ENET_DIR" ] || [ ! -f "$ENET_DIR/include/enet/enet.h" ]; then
	for cand in \
	    /home/hatch/workspace/coding/xtank-evidence/p2.9/deps/enet-1.3.18 \
	    "$EVIDENCE/deps/enet-1.3.18" \
	    /tmp/opencode/xtank-phase3/p2.9/deps/enet-1.3.18; do
		if [ -f "$cand/include/enet/enet.h" ]; then ENET_DIR="$cand"; break; fi
	done
fi
if [ -z "$ENET_DIR" ] || [ ! -f "$ENET_DIR/include/enet/enet.h" ]; then
	ENET_DIR="$EVIDENCE/deps/enet-1.3.18"
	mkdir -p "$EVIDENCE/deps"
	TAR="$EVIDENCE/deps/enet-1.3.18.tar.gz"
	[ -f "$TAR" ] || curl -fsSL -o "$TAR" "$ENET_URL" || \
		fail "cannot fetch ENet (set XTANK_ENET_DIR)"
	echo "$ENET_SHA  $TAR" | sha256sum -c - >/dev/null || \
		fail "ENet tarball SHA-256 mismatch"
	TAR_OPTIONS="--no-same-owner" tar xzf "$TAR" -C "$EVIDENCE/deps"
fi

DEFS="-DUNIX -DS1024x864 -DKEYPAD_DETECT -DSTINGY_REDRAW -DBATCH_LINES \
-DBATCH_POINTS -DBATCH_COLOR_LINES -DCODE_SIZE=0x20000 -DSTACK_SIZE=0x8000 \
-DNEW_LIMITS -DSLOWER -DMULTI_SYNC -DTHREAD_SWAPCONTEXT -DNULL_BACKEND \
-DSERVER_ONLY"

# ---- build the fixture and the parity fixture --------------------------
stage_build() {
	log "== building the shipped server core"
	( cd "$REPO" && make -s -j1 phase1-server phase1-core ) \
		> "$WORK/core.out" 2>&1 || fail "core build failed ($WORK/core.out)"
	CORE_OBJS=$(ls "$REPO"/Src/sobj/*.o | grep -v '/server_main.o$')
	[ -n "$CORE_OBJS" ] || fail "no core objects"

	: > "$WORK/compile.out"
	compile() { # src out
		gcc -O0 -g -w $DEFS -DXTANK_WITH_NET -I"$REPO/Src" \
			-I"$REPO/Src/Include" -I"$REPO/Protocol" \
			-I"$REPO/Transport" -I"$REPO/Client" \
			-I"$ENET_DIR/include" -c "$1" -o "$2" \
			>> "$WORK/compile.out" 2>&1 || fail "compile failed: $1"
	}
	compile "$REPO/Src/server_session.c" "$WORK/server_session.o"
	compile "$REPO/Src/server_net.c" "$WORK/server_net.o"
	compile "$REPO/Transport/enet_transport.c" "$WORK/enet_transport.o"
	compile "$REPO/Protocol/encode.c" "$WORK/encode.o"
	compile "$REPO/Protocol/decode.c" "$WORK/decode.o"
	compile "$REPO/Protocol/content-manifest.c" "$WORK/content-manifest.o"
	for f in model.c timeline.c camera.c session_client.c; do
		compile "$REPO/Client/$f" "$WORK/${f%.c}.o"
	done
	ENET_OBJS=""
	: > "$WORK/enet-build.out"
	for f in callbacks compress host list packet peer protocol unix; do
		o="$WORK/enet-$f.o"
		gcc -O0 -g -w -I"$ENET_DIR/include" -c "$ENET_DIR/$f.c" -o "$o" \
			>> "$WORK/enet-build.out" 2>&1 || fail "ENet build failed ($f)"
		ENET_OBJS="$ENET_OBJS $o"
	done
	link_one() { # src bin
		gcc -O0 -g -w $DEFS -DXTANK_WITH_NET -I"$REPO/Src" \
			-I"$REPO/Src/Include" -I"$REPO/Protocol" \
			-I"$REPO/Transport" -I"$REPO/Client" \
			-I"$ENET_DIR/include" \
			-DXTANK_DIR="\"$REPO/Src/..\"" -DALLDEFINES="\"p3.13\"" \
			-o "$WORK/$2" "$1" $CORE_OBJS "$WORK/server_session.o" \
			"$WORK/server_net.o" "$WORK/enet_transport.o" $ENET_OBJS \
			"$WORK/encode.o" "$WORK/decode.o" \
			"$WORK/content-manifest.o" "$WORK/model.o" \
			"$WORK/timeline.o" "$WORK/camera.o" \
			"$WORK/session_client.o" -Wl,--wrap=sendmsg -lm \
			> "$WORK/link-$2.out" 2>&1 || fail "link failed: $1"
	}
	link_one "$HARNESS/latency_fixture.c" latency_fixture
	link_one "$HARNESS/parity_fixture.c"  parity_fixture
	[ -x "$WORK/latency_fixture" ] && [ -x "$WORK/parity_fixture" ] || \
		fail "fixtures not built"
	log "== build complete"
}

# ---- paced acceptance: 900 ticks / 60 s at a given render pair ---------
kv() { # file key -> value (handles several pairs packed on one line)
	awk -v k="$2" '{
		n = split($0, a, " ");
		for (i = 1; i <= n; i++) {
			split(a[i], b, "=");
			if (b[1] == k) { print b[2]; exit }
		}
	}' "$1"
}

assert_summary() { # file tag
	local f="$1" tag="$2"
	[ -f "$f" ] || fail "$tag: missing $f"
	local ticks elapsed late p99 lpA lpB lsA lsB
	ticks=$(kv "$f" ticks)
	[ "$ticks" -ge 899 ] && [ "$ticks" -le 901 ] || \
		fail "$tag: ticks=$ticks not 900 +/-1"
	case "$tag" in
	  pace_*)
		elapsed=$(kv "$f" elapsed_ms)
		awk -v e="$elapsed" 'BEGIN{exit !(e>=59000 && e<=62000)}' || \
			fail "$tag: elapsed_ms=$elapsed outside 59-62 s"
		late=$(kv "$f" late_ticks)
		[ "$late" -eq 0 ] || fail "$tag: late_ticks=$late"
		p99=$(kv "$f" total_p99_us)
		awk -v p="$p99" 'BEGIN{exit !(p<66700)}' || \
			fail "$tag: total p99 ${p99}us >= 66.7 ms"
		lpA=$(kv "$f" lat_p95_ms_A); lpB=$(kv "$f" lat_p95_ms_B)
		lsA=$(kv "$f" lat_samples_A); lsB=$(kv "$f" lat_samples_B)
		awk -v l="$lpA" 'BEGIN{exit !(l<=250)}' || \
			fail "$tag: lat p95 A=${lpA}ms > 250 ms"
		awk -v l="$lpB" 'BEGIN{exit !(l<=250)}' || \
			fail "$tag: lat p95 B=${lpB}ms > 250 ms"
		[ "$lsA" -ge 100 ] && [ "$lsB" -ge 100 ] || \
			fail "$tag: too few latency samples ($lsA/$lsB)"
		;;
	esac
}

run_pace() { # tag fpsA fpsB
	local tag="$1" fa="$2" fb="$3"
	log "== pace $tag (fps $fa/$fb, 900 ticks)"
	timeout 180 "$WORK/latency_fixture" pace "$REPO" Arena \
		"$FIXT/p3.13-arena-pace.settings" "$fa" "$fb" 900 \
		"$WORK/$tag" > "$WORK/$tag.out" 2>&1 || \
		{ cat "$WORK/$tag.out" >&2; fail "$tag run failed"; }
	grep -q 'LATENCY FIXTURE PASS' "$WORK/$tag.out" || fail "$tag no PASS"
	assert_summary "$WORK/$tag.summary" "$tag"
	log "   $(grep -E '^ticks=|^elapsed_ms=|^late_ticks=|^total_p99_us=' "$WORK/$tag.summary" | tr '\n' ' ')"
}

# ---- replay invariance -------------------------------------------------
run_replay() {
	local trace="$FIXP/p3.12-arena.trace"
	local settings="$FIXP/p3.12-arena.settings"
	local lim="${XTANK_P3_REPLAY_TICKS:-400}"
	local fps
	log "== replay invariance (finite Arena trace, <=$lim ticks, 2 epochs)"

	for fps in 15 30 144 30,144; do
		env XTANK_P3_TICKS="$lim" XTANK_P3_FPS="$fps" \
			timeout 180 "$WORK/parity_fixture" network "$REPO" Arena \
			"$settings" "$trace" "$WORK/net.$fps" 2 \
			> "$WORK/net.$fps.out" 2>&1 || \
			{ cat "$WORK/net.$fps.out" >&2; fail "replay net $fps failed"; }
		grep -q 'PARITY FIXTURE PASS' "$WORK/net.$fps.out" || \
			fail "replay net $fps no PASS"
	done
	# direct leg = zero per-peer exports; the shared applied log must match.
	env XTANK_P3_TICKS="$lim" timeout 180 "$WORK/parity_fixture" direct \
		"$REPO" Arena "$settings" "$trace" "$WORK/direct" 2 \
		> "$WORK/direct.out" 2>&1 || \
		{ cat "$WORK/direct.out" >&2; fail "replay direct failed"; }
	grep -q 'PARITY FIXTURE PASS' "$WORK/direct.out" || \
		fail "replay direct no PASS"

	local e ref
	for e in 0 1; do
		ref="$WORK/net.15.e$e"
		for fps in 30 144 30,144 direct; do
			cmp "$ref" "$WORK/net.$fps.e$e" >/dev/null 2>&1 || \
			cmp "$ref" "$WORK/direct.e$e" >/dev/null 2>&1 || \
				fail "replay epoch $e differs: $fps"
		done
		# explicit direct==network (export-count invariance)
		cmp "$WORK/direct.e$e" "$WORK/net.15.e$e" >/dev/null || \
			fail "replay epoch $e: direct != network"
	done
	log "   replay: direct == network == 30 == 144 == mixed (epochs 0,1)"
	{
		for f in direct net.15 net.30 net.144 net.30,144; do
			for e in 0 1; do
				printf '%s.e%s %s\n' "$f" "$e" \
					"$(sha256sum "$WORK/$f.e$e" | awk '{print $1}')"
			done
		done
	} > "$WORK/replay.sha256"
}

stage_stress() {
	log "== stress (maximum entity/view density, 4000 ticks)"
	timeout 180 "$WORK/latency_fixture" stress "$REPO" Arena \
		"$FIXT/p3.13-arena-pace.settings" 4000 "$WORK/stress" \
		> "$WORK/stress.out" 2>&1 || \
		{ cat "$WORK/stress.out" >&2; fail "stress failed"; }
	grep -q 'LATENCY FIXTURE PASS' "$WORK/stress.out" || fail "stress no PASS"
	[ "$(awk -F= '/^bound_encode_ok/{print $2}' "$WORK/stress.summary")" = 2 ] \
		|| fail "maximum authorized view bound did not encode/round-trip"
	log "   $(grep -E '^max_vehicles=|^max_bullets=|^max_explosions=|^max_view_bytes=|^bound_view_bytes=|^bound_fragment_est=' "$WORK/stress.summary" | tr '\n' ' ')"
}

# ---- analysis (measurements.py) ---------------------------------------
stage_analyze() {
	local py="python3"
	command -v "$py" >/dev/null 2>&1 || py=python
	"$py" "$HARNESS/measurements.py" --dir "$WORK" \
		--host "$WORK/host.txt" --out "$WORK/report.json" \
		--markdown "$WORK/report.md" || fail "measurements.py failed"
	log "== wrote $WORK/report.md"
	cat "$WORK/report.md"
}

host_record
case "$STAGE" in
  build)   stage_build ;;
  pace)    stage_build; run_pace pace_30 30 30; run_pace pace_144 144 144; \
           run_pace pace_mixed 30 144 ;;
  stress)  stage_build; stage_stress ;;
  replay)  stage_build; run_replay ;;
  analyze) stage_analyze ;;
  all)
	stage_build
	run_pace pace_30 30 30
	run_pace pace_144 144 144
	run_pace pace_mixed 30 144
	stage_stress
	run_replay
	stage_analyze
	;;
  *) echo "timing.sh: unknown stage $STAGE" >&2; exit 2 ;;
esac

log ""
log "TIMING PASS (stage=$STAGE)"
