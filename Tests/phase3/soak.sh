#!/usr/bin/env bash
#
# soak.sh -- P3.15 paced four-vehicle bot/reset soak.
#
# Contract (docs/PHASE3-PLAN.md P3.15):
#   bash Tests/phase3/soak.sh [--evidence DIR] [--stage STAGE]
#                             [--resets N] [--epoch-ticks N] [--rejoin-at LIST]
#
# STAGE is one of: build | run | analyze | status | all (default).
#   build    compile the shipped core (Src/sobj), pinned ENet v1.3.18 and the
#            soak fixture, all outside the checkout.
#   run      build if needed, then run the fixture in the FOREGROUND, writing
#            incremental progress to the evidence work dir.  Launch it
#            detached so an agent-session death does not kill it:
#              setsid nohup bash Tests/phase3/soak.sh --stage run \
#                  > "$WORK/soak.out" 2>&1 < /dev/null &
#   analyze  run soak-analyze.py over the work dir.
#   status   print the running fixture pid (if any) and the progress tail.
#   all      build + run + analyze.
#
# The acceptance soak is 60 minutes: resets=20, epoch-ticks=2700 (180 s at
# 15 Hz) = 54000 paced ticks = 3600 s of paced match time, plus a bounded
# reset transaction per epoch.  A host kill mid-run means the whole run is
# restarted; it is never stitched from segments.
#
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HARNESS="$REPO/Tests/phase3"
FIXT="$HARNESS/fixtures/timing"
SETTINGS="$FIXT/p3.13-arena-pace.settings"
EVIDENCE="${XTANK_P3_EVIDENCE:-/home/hatch/workspace/coding/xtank-evidence/p3.15}"

ENET_URL="https://github.com/lsalzman/enet/archive/refs/tags/v1.3.18.tar.gz"
ENET_SHA="28603c895f9ed24a846478180ee72c7376b39b4bb1287b73877e5eae7d96b0dd"

STAGE="all"
RESETS=20
EPOCH_TICKS=2700
REJOIN_AT="3,8,13,18"
MAZE="Arena"

while [ $# -gt 0 ]; do
	case "$1" in
	  --evidence) EVIDENCE="$2"; shift 2 ;;
	  --stage) STAGE="$2"; shift 2 ;;
	  --resets) RESETS="$2"; shift 2 ;;
	  --epoch-ticks) EPOCH_TICKS="$2"; shift 2 ;;
	  --rejoin-at) REJOIN_AT="$2"; shift 2 ;;
	  --maze) MAZE="$2"; shift 2 ;;
	  *) echo "usage: soak.sh [--evidence DIR] [--stage build|run|analyze|status|all]" >&2; exit 2 ;;
	esac
done

WORK="$EVIDENCE/work"
mkdir -p "$EVIDENCE" "$WORK"

log()  { printf '%s\n' "$*"; }
fail() { printf 'SOAK FAIL: %s\n' "$*" >&2; exit 1; }

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
	( cd "$EVIDENCE/deps" && tar --no-same-owner xzf "$TAR" )
fi

DEFS="-DUNIX -DS1024x864 -DKEYPAD_DETECT -DSTINGY_REDRAW -DBATCH_LINES \
-DBATCH_POINTS -DBATCH_COLOR_LINES -DCODE_SIZE=0x20000 -DSTACK_SIZE=0x8000 \
-DNEW_LIMITS -DSLOWER -DMULTI_SYNC -DTHREAD_SWAPCONTEXT -DNULL_BACKEND \
-DSERVER_ONLY"

BIN="$WORK/soak_fixture"

stage_build() {
	log "== building the shipped server core"
	( cd "$REPO" && make -s -j1 phase1-server phase1-core ) \
		> "$WORK/core.out" 2>&1 || fail "core build failed ($WORK/core.out)"
	local core_objs
	core_objs=$(ls "$REPO"/Src/sobj/*.o | grep -v '/server_main.o$')
	[ -n "$core_objs" ] || fail "no core objects"

	: > "$WORK/compile.out"
	compile() {
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
	local enet_objs=""
	: > "$WORK/enet-build.out"
	for f in callbacks compress host list packet peer protocol unix; do
		gcc -O0 -g -w -I"$ENET_DIR/include" -c "$ENET_DIR/$f.c" \
			-o "$WORK/enet-$f.o" >> "$WORK/enet-build.out" 2>&1 || \
			fail "ENet build failed ($f)"
		enet_objs="$enet_objs $WORK/enet-$f.o"
	done
	# shellcheck disable=SC2086
	gcc -O0 -g -w $DEFS -DXTANK_WITH_NET -I"$REPO/Src" \
		-I"$REPO/Src/Include" -I"$REPO/Protocol" \
		-I"$REPO/Transport" -I"$REPO/Client" -I"$ENET_DIR/include" \
		-DXTANK_DIR="\"$REPO/Src/..\"" -DALLDEFINES="\"p3.15\"" \
		-o "$BIN" "$HARNESS/soak_fixture.c" \
		$core_objs "$WORK/server_session.o" "$WORK/server_net.o" \
		"$WORK/enet_transport.o" $enet_objs "$WORK/encode.o" \
		"$WORK/decode.o" "$WORK/content-manifest.o" "$WORK/model.o" \
		"$WORK/timeline.o" "$WORK/camera.o" "$WORK/session_client.o" \
		-Wl,--wrap=sendmsg -lm > "$WORK/link-fixture.out" 2>&1 || \
		fail "fixture link failed ($WORK/link-fixture.out)"
	[ -x "$BIN" ] || fail "fixture not built"
	log "== build complete"
}

stage_run() {
	[ -x "$BIN" ] || stage_build
	mkdir -p "$WORK/run"
	local out="$WORK/run/soak"
	log "== soak: resets=$RESETS epoch_ticks=$EPOCH_TICKS maze=$MAZE"
	log "   pid=$$ ppid=$PPID start=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
	date -u +%Y-%m-%dT%H:%M:%SZ > "$WORK/run/started.txt"
	echo "resets=$RESETS epoch_ticks=$EPOCH_TICKS rejoin_at=$REJOIN_AT maze=$MAZE" \
		> "$WORK/run/config.txt"
	# Run in the foreground; the caller detaches this script (setsid nohup).
	exec "$BIN" run "$REPO" "$MAZE" "$SETTINGS" "$RESETS" "$WORK/run" soak \
		--epoch-ticks "$EPOCH_TICKS" --rejoin-at "$REJOIN_AT"
}

stage_analyze() {
	local py="python3"
	command -v "$py" >/dev/null 2>&1 || py=python
	"$py" "$HARNESS/soak-analyze.py" --work "$WORK" --host "$WORK/host.txt" \
		--json "$WORK/report.json" --markdown "$WORK/report.md" || \
		fail "soak-analyze.py failed"
	log "== wrote $WORK/report.md"
	cat "$WORK/report.md"
}

stage_status() {
	log "== soak status"
	pgrep -af soak_fixture || log "   (no soak_fixture process)"
	[ -f "$WORK/run/soak.summary" ] && log "   summary: $WORK/run/soak.summary"
	if [ -f "$WORK/run/progress.txt" ]; then
		log "   progress tail:"
		tail -5 "$WORK/run/progress.txt"
	fi
	if [ -f "$WORK/run/resets.txt" ]; then
		log "   resets so far: $(wc -l < "$WORK/run/resets.txt")"
	fi
}

host_record
case "$STAGE" in
  build)   stage_build ;;
  run)     stage_run ;;
  analyze) stage_analyze ;;
  status)  stage_status ;;
  all)     stage_build; stage_run; stage_analyze ;;
  *) echo "soak.sh: unknown stage $STAGE" >&2; exit 2 ;;
esac
