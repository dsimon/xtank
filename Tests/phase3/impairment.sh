#!/usr/bin/env bash
#
# impairment.sh -- P3.14 approved impairment and slow-client matrix.
#
# Contract (docs/PHASE3-PLAN.md P3.14):
#   bash Tests/phase3/impairment.sh [--evidence DIR] [--stage STAGE]
#
# STAGE is one of:
#   build       compile the shipped core, pinned ENet, the real-UDP impairment
#               relay and the two-human/two-drone fixture (outside the checkout)
#   profiles    baseline / delay-only / loss-only / reorder-only / combined,
#               each recording configured vs *measured* impairment
#   freeze      freeze one client for 10 s; server expires it at 2 s, admin
#               transfers if applicable, the healthy client keeps running
#   backpressure queued reliable edges, fragment losses, send saturation and
#               reset packets exercised across channels
#   replay      capture the applied command log under impairment and replay it
#               through the frozen simulation; require byte identity
#   combined    the mandatory 30-minute four-vehicle combined run
#               (50 ms RTT = symmetric 25 ms each way, 1% loss, 1% +100 ms)
#   analyze     summarize every profile/trace/capture with impairment-analyze.py
#   all         build + profiles + freeze + backpressure + replay + analyze
#               (the 30-minute combined run is a separate, resumable stage)
#
# Impairment is applied by Tests/phase3/impairment_relay.c over *real loopback
# UDP* between the shipped ENet server and clients.  It is not an in-memory
# transport.  Kernel netem/tbf is unavailable in this sandbox; see the S0 note
# in P3.14-CHECKPOINT.md.
#
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HARNESS="$REPO/Tests/phase3"
FIXT="$HARNESS/fixtures/timing"
SETTINGS="$FIXT/p3.13-arena-pace.settings"
EVIDENCE="${XTANK_P3_EVIDENCE:-/home/hatch/workspace/coding/xtank-evidence/p3.14}"

ENET_URL="https://github.com/lsalzman/enet/archive/refs/tags/v1.3.18.tar.gz"
ENET_SHA="28603c895f9ed24a846478180ee72c7376b39b4bb1287b73877e5eae7d96b0dd"

# Mandatory combined profile.
COMBINED_DELAY_MS=25          # symmetric -> ~50 ms measured RTT
COMBINED_LOSS=0.01            # 1% packet loss
COMBINED_EXTRA_MS=100         # extra 100 ms ...
COMBINED_EXTRA_PROB=0.01      # ... on 1% of packets
COMBINED_TICKS=$((30 * 60 * 15))   # 30 minutes at 15 Hz
SEED=114336

STAGE="all"
while [ $# -gt 0 ]; do
	case "$1" in
	  --evidence) EVIDENCE="$2"; shift 2 ;;
	  --stage) STAGE="$2"; shift 2 ;;
	  *) echo "usage: impairment.sh [--evidence DIR] [--stage STAGE]" >&2; exit 2 ;;
	esac
done

WORK="$EVIDENCE/work"
mkdir -p "$EVIDENCE" "$WORK"

log()  { printf '%s\n' "$*"; }
fail() { printf 'IMPAIRMENT FAIL: %s\n' "$*" >&2; exit 1; }

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
	[ "$(nproc)" -ge 4 ] || log "WARNING: host has $(nproc) cores; D3-G reference asks >=4 (DELTA)"
}

# ---- pinned ENet ---------------------------------------------------------
enet_dir() {
	local dir="${XTANK_ENET_DIR:-}"
	if [ -z "$dir" ] || [ ! -f "$dir/include/enet/enet.h" ]; then
		for cand in \
		    /home/hatch/workspace/coding/xtank-evidence/p2.9/deps/enet-1.3.18 \
		    "$EVIDENCE/deps/enet-1.3.18" \
		    /tmp/opencode/xtank-phase3/p2.9/deps/enet-1.3.18; do
			if [ -f "$cand/include/enet/enet.h" ]; then dir="$cand"; break; fi
		done
	fi
	if [ -z "$dir" ] || [ ! -f "$dir/include/enet/enet.h" ]; then
		dir="$EVIDENCE/deps/enet-1.3.18"
		mkdir -p "$EVIDENCE/deps"
		local tar="$EVIDENCE/deps/enet-1.3.18.tar.gz"
		[ -f "$tar" ] || curl -fsSL -o "$tar" "$ENET_URL" || \
			fail "cannot fetch ENet (set XTANK_ENET_DIR)"
		echo "$ENET_SHA  $tar" | sha256sum -c - >/dev/null || \
			fail "ENet tarball SHA-256 mismatch"
		( cd "$EVIDENCE/deps" && tar --no-same-owner xzf "$tar" )
	fi
	echo "$dir"
}

DEFS="-DUNIX -DS1024x864 -DKEYPAD_DETECT -DSTINGY_REDRAW -DBATCH_LINES \
-DBATCH_POINTS -DBATCH_COLOR_LINES -DCODE_SIZE=0x20000 -DSTACK_SIZE=0x8000 \
-DNEW_LIMITS -DSLOWER -DMULTI_SYNC -DTHREAD_SWAPCONTEXT -DNULL_BACKEND \
-DSERVER_ONLY"

stage_build() {
	local enet; enet="$(enet_dir)"
	log "== building shipped server core"
	( cd "$REPO" && make -s -j1 phase1-server phase1-core ) \
		> "$WORK/core.out" 2>&1 || fail "core build failed ($WORK/core.out)"
	local core_objs; core_objs=$(ls "$REPO"/Src/sobj/*.o | grep -v '/server_main.o$')
	[ -n "$core_objs" ] || fail "no core objects"

	: > "$WORK/compile.out"
	compile() {
		gcc -O0 -g -w $DEFS -DXTANK_WITH_NET -I"$REPO/Src" \
			-I"$REPO/Src/Include" -I"$REPO/Protocol" \
			-I"$REPO/Transport" -I"$REPO/Client" \
			-I"$enet/include" -c "$1" -o "$2" \
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
	local enet_objs=""
	: > "$WORK/enet-build.out"
	for f in callbacks compress host list packet peer protocol unix; do
		gcc -O0 -g -w -I"$enet/include" -c "$enet/$f.c" -o "$WORK/enet-$f.o" \
			>> "$WORK/enet-build.out" 2>&1 || fail "ENet build failed ($f)"
		enet_objs="$enet_objs $WORK/enet-$f.o"
	done

	gcc -O0 -g -w $DEFS -DXTANK_WITH_NET -I"$REPO/Src" \
		-I"$REPO/Src/Include" -I"$REPO/Protocol" \
		-I"$REPO/Transport" -I"$REPO/Client" -I"$enet/include" \
		-DXTANK_DIR="\"$REPO/Src/..\"" -DALLDEFINES="\"p3.14\"" \
		-o "$WORK/impairment_fixture" "$HARNESS/impairment_fixture.c" \
		$core_objs "$WORK/server_session.o" "$WORK/server_net.o" \
		"$WORK/enet_transport.o" $enet_objs "$WORK/encode.o" \
		"$WORK/decode.o" "$WORK/content-manifest.o" "$WORK/model.o" \
		"$WORK/timeline.o" "$WORK/camera.o" "$WORK/session_client.o" \
		-Wl,--wrap=sendmsg -lm > "$WORK/link-fixture.out" 2>&1 || \
		fail "fixture link failed ($WORK/link-fixture.out)"

	# Reuse the P3.12 parity fixture for the applied-log exact-replay check.
	gcc -O0 -g -w $DEFS -DXTANK_WITH_NET -I"$REPO/Src" \
		-I"$REPO/Src/Include" -I"$REPO/Protocol" \
		-I"$REPO/Transport" -I"$REPO/Client" -I"$enet/include" \
		-DXTANK_DIR="\"$REPO/Src/..\"" -DALLDEFINES="\"p3.14\"" \
		-o "$WORK/parity_fixture" "$HARNESS/parity_fixture.c" \
		$core_objs "$WORK/server_session.o" "$WORK/server_net.o" \
		"$WORK/enet_transport.o" $enet_objs "$WORK/encode.o" \
		"$WORK/decode.o" "$WORK/content-manifest.o" "$WORK/model.o" \
		"$WORK/timeline.o" "$WORK/camera.o" "$WORK/session_client.o" \
		-Wl,--wrap=sendmsg -lm > "$WORK/link-parity.out" 2>&1 || \
		fail "parity fixture link failed ($WORK/link-parity.out)"

	gcc -O0 -g -w -o "$WORK/impairment_relay" \
		"$HARNESS/impairment_relay.c" > "$WORK/link-relay.out" 2>&1 || \
		fail "relay build failed ($WORK/link-relay.out)"

	[ -x "$WORK/impairment_fixture" ] && [ -x "$WORK/impairment_relay" ] && \
		[ -x "$WORK/parity_fixture" ] || fail "binaries not built"
	log "== build complete"
}

# run_one <label> <ticks> [extra fixture flags ...]
run_one() {
	local label="$1" ticks="$2"; shift 2
	local dir="$WORK/$label"
	mkdir -p "$dir"
	log "== run $label ($ticks ticks)"
	env XTANK_P3_WORKDIR="$dir" XTANK_P3_RELAY_BIN="$WORK/impairment_relay" \
		"$WORK/impairment_fixture" run "$REPO" Arena "$SETTINGS" "$ticks" \
		"$dir" "$label" --seed "$SEED" "$@" \
		> "$dir/$label.out" 2>&1 || \
		{ tail -20 "$dir/$label.out" >&2; fail "$label run failed"; }
	grep -q 'IMPAIRMENT FIXTURE PASS' "$dir/$label.out" || \
		fail "$label did not PASS"
	[ -f "$dir/$label.summary" ] || fail "$label missing summary"
	[ -f "$dir/relay-$label.stats" ] || fail "$label missing relay stats"
}

stage_profiles() {
	local ticks="${XTANK_P3_PROFILE_TICKS:-300}"
	run_one baseline    "$ticks" --delay-ms 0 --loss 0 --reorder 0
	run_one delay-only  "$ticks" --delay-ms 25
	run_one loss-only   "$ticks" --loss 0.01
	run_one reorder-only "$ticks" --reorder 0.05 --reorder-ms 20
	run_one combined    "$ticks" --delay-ms 25 --loss 0.01 \
		--extra-delay-ms 100 --extra-prob 0.01
	log "== profiles complete"
}

stage_freeze() {
	# Freeze the administrator (client A) for 10 s (150 ticks) starting at
	# t=120 (after the match has settled).
	run_one freeze 600 --freeze 1 --freeze-client 0 --freeze-at 120 \
		--freeze-dur 150
	log "== freeze complete"
}

stage_backpressure() {
	# Sustained impairment (fragment/reliable retransmit pressure) plus a
	# mid-run reset transaction, exercising queued reliable edges,
	# fragment losses, bounded send queues and reset packets across
	# channels.  Kept within the delivered transport's approved operating
	# envelope: beyond ~10% loss the fixed-tick bring-up and ENet peer
	# keepalive are stressed past release conditions (that is the job of
	# the mandatory 30-minute combined profile, not this queue reset probe).
	run_one backpressure 600 --delay-ms 25 --loss 0.05 --reorder 0.03 \
		--reorder-ms 20 --extra-delay-ms 50 --extra-prob 0.05 \
		--reset-at 300
	local dir="$WORK/backpressure"
	local before after done
	# The fixture writes the reset fields on one summary line, so parse the
	# key=value tokens rather than anchoring to the start of a line.
	before=$(awk '{for(i=1;i<=NF;i++) if($i ~ /^reset_epoch_before=/) {sub(/^reset_epoch_before=/,"",$i); print $i}}' "$dir/backpressure.summary")
	after=$(awk '{for(i=1;i<=NF;i++) if($i ~ /^reset_epoch_after=/) {sub(/^reset_epoch_after=/,"",$i); print $i}}' "$dir/backpressure.summary")
	done=$(awk '{for(i=1;i<=NF;i++) if($i ~ /^reset_tick_done=/) {sub(/^reset_tick_done=/,"",$i); print $i}}' "$dir/backpressure.summary")
	[ -n "$done" ] && [ "$done" -ge 0 ] || fail "backpressure: reset not exercised"
	[ -n "$after" ] && [ "$after" -gt "$before" ] || \
		fail "backpressure: epoch did not advance on restart ($before -> $after)"
	log "   reset at tick $done, epoch $before -> $after"
	log "== backpressure complete"
}

stage_replay() {
	local dir="$WORK/replay"
	local ticks="${XTANK_P3_REPLAY_TICKS:-400}"
	mkdir -p "$dir"
	log "== applied-log replay under impairment ($ticks ticks)"
	env XTANK_P3_WORKDIR="$dir" XTANK_P3_RELAY_BIN="$WORK/impairment_relay" \
		"$WORK/impairment_fixture" run "$REPO" Arena "$SETTINGS" "$ticks" \
		"$dir" applied --seed "$SEED" --delay-ms 25 --loss 0.01 \
		--extra-delay-ms 100 --extra-prob 0.01 \
		--applied-log "$dir/applied.trace" \
		> "$dir/capture.out" 2>&1 || \
		{ tail -30 "$dir/capture.out" >&2; fail "replay capture failed"; }
	grep -q 'IMPAIRMENT FIXTURE PASS' "$dir/capture.out" || \
		fail "replay capture did not PASS"
	[ -s "$dir/applied.trace" ] || fail "empty applied log"
	# Replay the captured applied commands twice through the frozen sim.
	local fp
	for fp in A B; do
		env XTANK_P3_TICKS="$ticks" "$WORK/parity_fixture" direct "$REPO" \
			Arena "$SETTINGS" "$dir/applied.trace" "$dir/replay$fp" 1 \
			> "$dir/replay$fp.out" 2>&1 || \
			{ tail -20 "$dir/replay$fp.out" >&2; fail "replay $fp failed"; }
		grep -q 'PARITY FIXTURE PASS' "$dir/replay$fp.out" || \
			fail "replay $fp did not PASS"
	done
	cmp "$dir/replayA.e0" "$dir/replayB.e0" >/dev/null || \
		fail "captured applied log did not replay byte-identically"
	log "   applied commands: $(wc -l < "$dir/applied.trace"); replays identical"
	log "== replay complete"
}

stage_combined() {
	log "== MANDATORY 30-minute combined run ($COMBINED_TICKS ticks)"
	log "   config: symmetric ${COMBINED_DELAY_MS} ms each way (~50 ms RTT),"
	log "   loss ${COMBINED_LOSS}, extra ${COMBINED_EXTRA_MS} ms on ${COMBINED_EXTRA_PROB}"
	run_one combined30 "$COMBINED_TICKS" --delay-ms "$COMBINED_DELAY_MS" \
		--loss "$COMBINED_LOSS" --extra-delay-ms "$COMBINED_EXTRA_MS" \
		--extra-prob "$COMBINED_EXTRA_PROB"
	log "== combined 30-minute run complete"
}

stage_analyze() {
	local py="python3"
	command -v "$py" >/dev/null 2>&1 || py=python
	"$py" "$HARNESS/impairment-analyze.py" --work "$WORK" \
		--host "$WORK/host.txt" --json "$WORK/report.json" \
		--markdown "$WORK/report.md" || fail "impairment-analyze.py failed"
	log "== wrote $WORK/report.md"
	cat "$WORK/report.md"
}

host_record
case "$STAGE" in
  build)       stage_build ;;
  profiles)    stage_build; stage_profiles ;;
  freeze)      stage_build; stage_freeze ;;
  backpressure) stage_build; stage_backpressure ;;
  replay)      stage_build; stage_replay ;;
  combined)    stage_build; stage_combined ;;
  analyze)     stage_analyze ;;
  all)         stage_build; stage_profiles; stage_freeze; stage_backpressure; \
               stage_replay; stage_analyze ;;
  *) echo "impairment.sh: unknown stage $STAGE" >&2; exit 2 ;;
esac

log ""
log "IMPAIRMENT PASS (stage=$STAGE)"
