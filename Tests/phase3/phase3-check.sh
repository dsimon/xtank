#!/usr/bin/env bash
#
# phase3-check.sh -- P3.16 Phase 3 automated exit gate (fail-fast).
#
# Contract (docs/PHASE3-PLAN.md P3.16 and section 6 "Proposed automated exit
# gate"):
#
#   make phase3-check PHASE3_EVIDENCE=<dir>
#   # equivalent:
#   bash Tests/phase3/phase3-check.sh --evidence <dir> [--operational]
#
# It requires, and never silently skips:
#   1. the full Phase 1 exit gate (`make phase1-check`), and the inherited
#      Phase 2 drivers/package checks (`Tests/phase2/phase2-check.sh`), including
#      v1.0 wire-fixture retention (P3.3 protocol.sh) and an explicit solo
#      compatibility step;
#   2. the Phase 3 drivers P3.2-P3.12, an explicit Linux and MinGW-w64 client
#      build, and the ASan/UBSan codec fuzz (P3.3 codec.sh);
#   3. the Phase 3 packages (Bin/package-phase3.sh) and their acceptance
#      (Tests/phase3/package.sh: unpack/relocate, notices, pinned runtimes,
#      version + content manifests, client DLL import allowlist, X-free Ubuntu
#      network server).
#
# Every step is recorded as PASS / FAIL / BLOCKED with its exact command, exit
# code, wall time, log path and source manifest hash, in both a TSV and a JSON
# (`gate-results.tsv`, `gate-results.json`).  A missing prerequisite or an
# explicitly skipped required step can NEVER yield an overall PASS.
#
# `--operational` additionally enforces the P3.13/P3.14/P3.15 operational
# evidence (timing/impairment/soak) attached against this same manifest by
# re-running each analyzer; without it those are reported NOT-ENFORCED.
#
# The native Windows two-PC exit (ten two-human matches, solo-vs-drone on real
# hardware, install/feel sign-off) requires the owner on real hardware and is
# recorded BLOCKED.  A Linux automated PASS is reported as such and is NEVER
# worded as the two-PC exit PASS.
#
# All dumps, logs, reference trees, build trees and packages live OUTSIDE the
# checkout under the evidence root.
#
set -u

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HARNESS="$REPO/Tests/phase3"

EVIDENCE="${XTANK_P3_EVIDENCE:-/home/hatch/workspace/coding/xtank-evidence/p3.16/gate}"
OPERATIONAL=0
SKIP_PHASE1=0
SKIP_PHASE2=0

while [ $# -gt 0 ]; do
	case "$1" in
	  --evidence)     EVIDENCE="$2"; shift 2 ;;
	  --operational)  OPERATIONAL=1; shift ;;
	  --skip-phase1)  SKIP_PHASE1=1; shift ;;
	  --skip-phase2)  SKIP_PHASE2=1; shift ;;
	  *) echo "usage: phase3-check.sh [--evidence DIR] [--operational] [--skip-phase1] [--skip-phase2]" >&2; exit 2 ;;
	esac
done

mkdir -p "$EVIDENCE/logs" "$EVIDENCE/manifest"
export LC_ALL=C
export TAR_OPTIONS="${TAR_OPTIONS:---no-same-owner}"

TSV="$EVIDENCE/gate-results.tsv"
JSON="$EVIDENCE/gate-results.json"
: > "$TSV"
printf 'step\tstatus\texit_code\tseconds\tcommand\tlog\tdriver_sha256\n' >> "$TSV"

log()  { printf '%s\n' "$*"; }
fail() { printf 'PHASE3-CHECK: %s\n' "$*" >&2; }

# ---------------------------------------------------------------------------
# Pinned dependencies (P2.1/P2.9/P3.1); P3.16 re-verifies them.
# ---------------------------------------------------------------------------
discover() {
	ENET_DIR="${XTANK_ENET_DIR:-}"
	if [ -z "$ENET_DIR" ]; then
		for cand in \
		    "$REPO/../xtank-evidence/p2.9/deps/enet-1.3.18" \
		    /home/hatch/workspace/coding/xtank-evidence/p2.9/deps/enet-1.3.18 \
		    /tmp/opencode/xtank-phase2/p2.9/deps/enet-1.3.18; do
			[ -f "$cand/include/enet/enet.h" ] && { ENET_DIR="$cand"; break; }
		done
	fi
	SDL_LINUX="${XTK_SDL3_LINUX_PREFIX:-}"
	if [ -z "$SDL_LINUX" ]; then
		for cand in \
		    "$REPO/../xtank-evidence/p2.9/deps/linux-prefix" \
		    /home/hatch/workspace/coding/xtank-evidence/p2.9/deps/linux-prefix \
		    /tmp/opencode/xtank-phase2/p2.9/deps/linux-prefix; do
			[ -f "$cand/include/SDL3/SDL.h" ] && { SDL_LINUX="$cand"; break; }
		done
	fi
	SDL_MINGW="${XTK_SDL3_MINGW_PREFIX:-}"
	if [ -z "$SDL_MINGW" ]; then
		for cand in \
		    "$REPO/../xtank-evidence/p2.9/deps/SDL3-3.4.18/x86_64-w64-mingw32" \
		    /home/hatch/workspace/coding/xtank-evidence/p2.9/deps/SDL3-3.4.18/x86_64-w64-mingw32 \
		    /tmp/opencode/xtank-phase2/p2.9/deps/SDL3-3.4.18/x86_64-w64-mingw32; do
			[ -f "$cand/bin/SDL3.dll" ] && { SDL_MINGW="$cand"; break; }
		done
	fi
}
discover
export XTANK_ENET_DIR="$ENET_DIR" XTK_SDL3_LINUX_PREFIX="$SDL_LINUX" \
       XTK_SDL3_MINGW_PREFIX="$SDL_MINGW"

# ---------------------------------------------------------------------------
# Evidence record helpers.
# ---------------------------------------------------------------------------
LAST_STATUS=""
STOP=0
BLOCKED_ANY=0
INCOMPLETE=0
declare -a STEP_NAMES=()

sanitize() { printf '%s' "$1" | tr '\t\n' '  '; }
sha_of()   { [ -f "$1" ] && sha256sum "$1" 2>/dev/null | awk '{print $1}' || true; }

record() { # name status rc seconds command log driver
	local name="$1" status="$2" rc="$3" secs="$4" cmd="$5" logf="$6" drv="$7"
	STEP_NAMES+=("$name")
	printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
	    "$(sanitize "$name")" "$status" "$rc" "$secs" \
	    "$(sanitize "$cmd")" "$(sanitize "$logf")" "$(sha_of "$drv")" >> "$TSV"
}

run_step() { # name driver_path command...
	local name="$1" driver="$2"; shift 2
	local logf="$EVIDENCE/logs/$name.log"
	log "== $name"
	local t0 t1 rc status
	t0=$(date +%s)
	if "$@" > "$logf" 2>&1; then rc=0; status=PASS; else rc=$?; status=FAIL; fi
	t1=$(date +%s)
	LAST_STATUS="$status"
	record "$name" "$status" "$rc" "$((t1 - t0))" "$*" "$logf" "$driver"
	if [ "$status" = PASS ]; then
		local m; m="$(grep -m1 -E 'PASS' "$logf" 2>/dev/null | tail -1)"
		[ -n "$m" ] && log "   $m"
	else
		tail -25 "$logf" >&2
		fail "$name FAILED (rc=$rc, log $logf)"
	fi
}

do_step() { [ "$STOP" = 1 ] && return 0; run_step "$@"; [ "$LAST_STATUS" = PASS ] || STOP=1; }

mark_blocked() { # name reason
	local name="$1" reason="$2"
	local logf="$EVIDENCE/logs/$name.log"
	{ echo "BLOCKED: $reason"; } > "$logf"
	record "$name" BLOCKED "-" "0" "$reason" "$logf" ""
	STEP_NAMES+=("__blocked_$name")
	BLOCKED_ANY=1; STOP=1
	fail "BLOCKED: $name -- $reason"
}

mark_skip() { # name reason
	local name="$1" reason="$2"
	local logf="$EVIDENCE/logs/$name.log"
	{ echo "SKIPPED: $reason"; } > "$logf"
	record "$name" SKIPPED "-" "0" "$reason" "$logf" ""
	fail "SKIPPED: $name -- $reason"
}

# A Phase 2 driver that the P3.3 exact-version-1.1 bump supersedes is not
# "skipped": its coverage is carried by the named Phase 3 successor, which this
# gate requires to PASS (checked by require_successors at the end).
SUPERSEDED_PAIRS=""
register_superseded() { # p2_name successor_name
	local logf="$EVIDENCE/logs/superseded-$1.log"
	{ echo "SUPERSEDED: $1 is carried by Phase 3 driver $2 (required to PASS)."; } > "$logf"
	record "$1" SUPERSEDED "-" "0" "superseded-by:$2" "$logf" ""
	SUPERSEDED_PAIRS="$SUPERSEDED_PAIRS $1:$2"
	log "   superseded: $1 -> $2"
}

require_successors() {
	local pair p2 succ bad=0
	for pair in $SUPERSEDED_PAIRS; do
		p2="${pair%%:*}"; succ="${pair#*:}"
		if ! awk -F'\t' -v s="$succ" '$1==s && $2=="PASS"{f=1} END{exit !f}' "$TSV"; then
			fail "superseded driver $p2 requires $succ to PASS, but it did not"
			bad=1
		fi
	done
	[ "$bad" = 0 ] || { STOP=1; }
}

# ---------------------------------------------------------------------------
# Source manifest (exact checkout manifest this gate ran against).
# ---------------------------------------------------------------------------
gen_source_manifest() {
	{
		echo "date_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
		echo "repo=$REPO"
		echo "branch=$(git -C "$REPO" branch --show-current 2>/dev/null || echo unknown)"
		echo "head=$(git -C "$REPO" rev-parse HEAD 2>/dev/null || echo unknown)"
		echo "gcc=$(gcc --version 2>/dev/null | head -1)"
		echo "mingw=$(x86_64-w64-mingw32-gcc --version 2>/dev/null | head -1)"
		echo "cmake=$(cmake --version 2>/dev/null | head -1)"
		echo "python=$(python3 --version 2>&1)"
		echo "nproc=$(nproc 2>/dev/null)"
		echo "memtotal_kb=$(grep MemTotal /proc/meminfo 2>/dev/null | awk '{print $2}')"
		echo "uname=$(uname -a)"
		echo "# --- working-tree status ---"
		git -C "$REPO" status --porcelain 2>/dev/null
		echo "# --- sha256 of tracked + untracked non-ignored sources ---"
		( cd "$REPO" && { git ls-files; git ls-files --others --exclude-standard; } \
		  | LC_ALL=C sort -u | while IFS= read -r f; do
			[ -f "$f" ] && sha256sum "$f"
		    done )
	} > "$EVIDENCE/manifest/sources.txt"
	sha256sum "$EVIDENCE/manifest/sources.txt" | awk '{print $1}' \
	    > "$EVIDENCE/manifest/sources.sha256"
	{ echo "PASS: source manifest written ($(wc -l < "$EVIDENCE/manifest/sources.txt") lines)"; } \
	    > "$EVIDENCE/logs/source-manifest.log"
	record "source-manifest" PASS 0 0 "gen_source_manifest" "$EVIDENCE/logs/source-manifest.log" ""
	log "== source-manifest"
	log "   $EVIDENCE/manifest/sources.txt"
}
gen_source_manifest

log "== P3.16 phase3-check (Linux automated gate)"
log "   evidence: $EVIDENCE"
log "   ENet:     ${ENET_DIR:-MISSING}"
log "   SDL3 lin: ${SDL_LINUX:-MISSING}"
log "   SDL3 win: ${SDL_MINGW:-MISSING}"
log ""

# ---------------------------------------------------------------------------
# Prerequisite gate.  Missing prerequisites BLOCK the gate (never a PASS).
# ---------------------------------------------------------------------------
prereq_failed=0
need_file() { [ -n "$2" ] && [ -f "$2" ] || { fail "missing prerequisite: $1 ($2)"; prereq_failed=1; }; }
need_cmd()  { command -v "$1" >/dev/null 2>&1 || { fail "missing prerequisite command: $1"; prereq_failed=1; }; }
need_file "pinned ENet source"   "${ENET_DIR:+$ENET_DIR/include/enet/enet.h}"
need_file "pinned SDL3 Linux"    "${SDL_LINUX:+$SDL_LINUX/include/SDL3/SDL.h}"
need_file "pinned SDL3 MinGW"    "${SDL_MINGW:+$SDL_MINGW/bin/SDL3.dll}"
need_cmd gcc; need_cmd make; need_cmd python3; need_cmd cmake
need_cmd x86_64-w64-mingw32-gcc; need_cmd x86_64-w64-mingw32-objdump
need_cmd zip; need_cmd unzip; need_cmd tar; need_cmd sha256sum
need_cmd flex; need_cmd Xvfb; need_cmd xdpyinfo; need_cmd xrdb
WINE="$(command -v wine 2>/dev/null || true)"
if [ -n "$WINE" ]; then WINE_ENV=(); else WINE_ENV=(XTK_ALLOW_NO_WINE=1); fi
if [ "$prereq_failed" = 1 ]; then
	mark_blocked "prerequisites" "one or more pinned dependencies/tools are missing"
fi

# ---------------------------------------------------------------------------
# 1. Full Phase 1 exit gate.
# ---------------------------------------------------------------------------
if [ "$SKIP_PHASE1" = 1 ]; then
	mark_skip "phase1-check" "--skip-phase1 requested (required for a full gate)"
	INCOMPLETE=1
else
	do_step "phase1-check" "$REPO/Tests/phase1/regression.sh" \
	    env -u MAKEFLAGS -u MFLAGS make -C "$REPO" --no-print-directory \
	        phase1-check PHASE1_EVIDENCE="$EVIDENCE/phase1-check"
fi

# ---------------------------------------------------------------------------
# 2. Inherited Phase 2 drivers + package checks.
#
# The P3.3 exact-version-1.1 protocol bump supersedes the v1.0 byte-golden
# Phase 2 drivers (codec/view/transport/session/ui/parity) - P3.3-REPORT.md §7
# records this as expected debt and proves the v1.0 fixtures are retained by an
# independent verifier.  The Phase 3 gate therefore runs the still-valid
# inherited Phase 2 drivers here and requires each superseded driver's Phase 3
# successor to PASS (require_successors below); nothing is silently dropped.
# ---------------------------------------------------------------------------
if [ "$SKIP_PHASE2" = 1 ]; then
	mark_skip "phase2-check" "--skip-phase2 requested (required for a full gate)"
	INCOMPLETE=1
else
	# Still-valid inherited Phase 2 drivers (v1.0 fixture retention, identity,
	# assets, model/input, and the explicit solo compatibility slice).
	do_step "p2-protocol-v1.0-retention" "$REPO/Tests/phase2/protocol.sh" \
	    env -u MAKEFLAGS -u MFLAGS XTANK_P2_EVIDENCE="$EVIDENCE/phase2/protocol" \
	        bash "$REPO/Tests/phase2/protocol.sh"
	do_step "p2-players" "$REPO/Tests/phase2/players.sh" \
	    env XTANK_P2_EVIDENCE="$EVIDENCE/phase2/players" \
	        bash "$REPO/Tests/phase2/players.sh"
	do_step "p2-events" "$REPO/Tests/phase2/events.sh" \
	    env XTANK_P2_EVIDENCE="$EVIDENCE/phase2/events" \
	        bash "$REPO/Tests/phase2/events.sh"
	do_step "p2-assets" "$REPO/Tests/phase2/assets.sh" \
	    env XTANK_P2_EVIDENCE="$EVIDENCE/phase2/assets" \
	        bash "$REPO/Tests/phase2/assets.sh"
	do_step "p2-timeline" "$REPO/Tests/phase2/timeline.sh" \
	    env XTANK_P2_EVIDENCE="$EVIDENCE/phase2/timeline" \
	        bash "$REPO/Tests/phase2/timeline.sh"
	do_step "p2-input" "$REPO/Tests/phase2/input.sh" \
	    env XTANK_P2_EVIDENCE="$EVIDENCE/phase2/input" \
	        bash "$REPO/Tests/phase2/input.sh"
	# Explicit solo compatibility (P2.14 solo connect/start/results/reset).
	do_step "solo-compatibility" "$REPO/Tests/phase2/playable.sh" \
	    env XTANK_P2_EVIDENCE="$EVIDENCE/phase2/solo" \
	        bash "$REPO/Tests/phase2/playable.sh"

	# Superseded version-sensitive / flaky v1.0 drivers -> Phase 3 successors.
	register_superseded "p2-codec"            "p3.3-codec-mingw-sanitizer"
	register_superseded "p2-view"             "p3.8-view"
	register_superseded "p2-transport"        "p3.9-transport"
	register_superseded "p2-session"          "p3.4-lobby"
	register_superseded "p2-ui"               "p3.10-client-session"
	register_superseded "p2-parity"           "p3.12-parity"
	register_superseded "p2-package"          "p3.16-package"
fi

# ---------------------------------------------------------------------------
# 3. Phase 3 drivers P3.2-P3.12.
# ---------------------------------------------------------------------------
do_step "p3.2-departure-design" "$HARNESS/departure-design.sh" \
    bash "$HARNESS/departure-design.sh" --evidence "$EVIDENCE/drivers/p3.2"
do_step "p3.3-protocol-v1.0-retention" "$HARNESS/protocol.sh" \
    bash "$HARNESS/protocol.sh" --evidence "$EVIDENCE/drivers/p3.3-protocol"
do_step "p3.3-codec-mingw-sanitizer" "$HARNESS/codec.sh" \
    env "${WINE_ENV[@]}" XTANK_P3_EVIDENCE="$EVIDENCE/drivers/p3.3-codec" \
        bash "$HARNESS/codec.sh"
if [ "$STOP" = 0 ] && [ -z "$WINE" ]; then
	record "mingw-runtime-wine" DEFERRED "-" "0" \
	    "codec.exe under wine (cross-build verified; no Wine runner)" \
	    "$EVIDENCE/logs/p3.3-codec-mingw-sanitizer.log" "$HARNESS/codec.sh"
	log "   note: no Wine runner; MinGW cross-build/fixtures verified, exe run deferred"
fi
do_step "p3.4-lobby" "$HARNESS/lobby.sh" \
    bash "$HARNESS/lobby.sh" --evidence "$EVIDENCE/drivers/p3.4"
do_step "p3.5-players" "$HARNESS/players.sh" \
    bash "$HARNESS/players.sh" --evidence "$EVIDENCE/drivers/p3.5"
do_step "p3.6-admission" "$HARNESS/admission.sh" \
    bash "$HARNESS/admission.sh" --evidence "$EVIDENCE/drivers/p3.6"
do_step "p3.7-departure" "$HARNESS/departure.sh" \
    bash "$HARNESS/departure.sh" --evidence "$EVIDENCE/drivers/p3.7"
do_step "p3.8-view" "$HARNESS/view.sh" \
    bash "$HARNESS/view.sh" --evidence "$EVIDENCE/drivers/p3.8-view"
do_step "p3.8-events" "$HARNESS/events.sh" \
    bash "$HARNESS/events.sh" --evidence "$EVIDENCE/drivers/p3.8-events"
do_step "p3.9-transport" "$HARNESS/transport.sh" \
    bash "$HARNESS/transport.sh" --evidence "$EVIDENCE/drivers/p3.9"
do_step "p3.10-client-session" "$HARNESS/client-session.sh" \
    bash "$HARNESS/client-session.sh" --evidence "$EVIDENCE/drivers/p3.10"
do_step "p3.11-timeline" "$HARNESS/timeline.sh" \
    bash "$HARNESS/timeline.sh" --evidence "$EVIDENCE/drivers/p3.11-timeline"
do_step "p3.11-input" "$HARNESS/input.sh" \
    bash "$HARNESS/input.sh" --evidence "$EVIDENCE/drivers/p3.11-input"
do_step "p3.12-playable" "$HARNESS/playable.sh" \
    bash "$HARNESS/playable.sh" --evidence "$EVIDENCE/drivers/p3.12-playable"

# P3.12 parity: seed the private reference Mazes/list with combat1 first (a
# known harness quirk), then the full four-scenario run.
do_step "p3.12-parity-seed" "$HARNESS/parity.sh" \
    bash "$HARNESS/parity.sh" --maze combat1 --evidence "$EVIDENCE/drivers/p3.12-parity"
do_step "p3.12-parity" "$HARNESS/parity.sh" \
    bash "$HARNESS/parity.sh" --evidence "$EVIDENCE/drivers/p3.12-parity"

# ---------------------------------------------------------------------------
# 4. Explicit Linux and MinGW-w64 client builds (D3-D toolchain).
# ---------------------------------------------------------------------------
do_step "client-build-linux" "$REPO/GNUmakefile" \
    env -u MAKEFLAGS -u MFLAGS make -C "$REPO" --no-print-directory phase2-client-linux \
        XTK_CLIENT_BUILD="$EVIDENCE/build" \
        XTK_SDL3_LINUX_PREFIX="$SDL_LINUX" XTK_ENET_DIR="$ENET_DIR"
do_step "client-build-mingw" "$REPO/GNUmakefile" \
    env -u MAKEFLAGS -u MFLAGS make -C "$REPO" --no-print-directory phase2-client-win \
        XTK_CLIENT_BUILD="$EVIDENCE/build" \
        XTK_SDL3_MINGW_PREFIX="$SDL_MINGW" XTK_ENET_DIR="$ENET_DIR"

# ---------------------------------------------------------------------------
# 5. Phase 3 packages + acceptance.
# ---------------------------------------------------------------------------
do_step "p3.16-package" "$HARNESS/package.sh" \
    env XTANK_P3_EVIDENCE="$EVIDENCE/package" \
        bash "$HARNESS/package.sh" --evidence "$EVIDENCE/package" \
            --mingw-sdl-prefix "$SDL_MINGW" --enet-dir "$ENET_DIR"

# Every superseded Phase 2 driver must have a PASSing Phase 3 successor.
require_successors

# ---------------------------------------------------------------------------
# 6. Operational evidence (optional, enforced when --operational is given).
# ---------------------------------------------------------------------------
OPS_P313="${XTANK_P3_13_EVIDENCE:-/home/hatch/workspace/coding/xtank-evidence/p3.13}"
OPS_P314="${XTANK_P3_14_EVIDENCE:-/home/hatch/workspace/coding/xtank-evidence/p3.14}"
OPS_P315="${XTANK_P3_15_EVIDENCE:-/home/hatch/workspace/coding/xtank-evidence/p3.15}"

if [ "$OPERATIONAL" = 1 ]; then
	if [ "$STOP" = 1 ]; then :; else
	do_step "op-p3.13-timing" "$HARNESS/timing.sh" \
	    env XTANK_P3_EVIDENCE="$OPS_P313" \
	        bash "$HARNESS/timing.sh" --stage analyze --evidence "$OPS_P313"
	do_step "op-p3.14-impairment" "$HARNESS/impairment.sh" \
	    env XTANK_P3_EVIDENCE="$OPS_P314" \
	        bash "$HARNESS/impairment.sh" --stage analyze --evidence "$OPS_P314"
	do_step "op-p3.15-soak" "$HARNESS/soak.sh" \
	    env XTANK_P3_EVIDENCE="$OPS_P315" \
	        bash "$HARNESS/soak.sh" --stage analyze --evidence "$OPS_P315"
	fi
else
	record "op-p3.13-timing" NOT-ENFORCED - "-" \
	    "timing.sh --stage analyze" "$OPS_P313" "$HARNESS/timing.sh"
	record "op-p3.14-impairment" NOT-ENFORCED - "-" \
	    "impairment.sh --stage analyze" "$OPS_P314" "$HARNESS/impairment.sh"
	record "op-p3.15-soak" NOT-ENFORCED - "-" \
	    "soak.sh --stage analyze" "$OPS_P315" "$HARNESS/soak.sh"
	log "== operational evidence NOT-ENFORCED (run with --operational to require it)"
fi

# Owner two-PC exit: always BLOCKED on this Linux host.
record "owner-two-pc-exit" BLOCKED - "-" \
    "Tests/phase3/windows-multiplayer.ps1 on two clean Windows 11 PCs" \
    "$HARNESS/windows-multiplayer.ps1" ""

# ---------------------------------------------------------------------------
# Machine-readable summary.
# ---------------------------------------------------------------------------
if [ "$BLOCKED_ANY" = 1 ]; then
	GATE_STATUS="BLOCKED"
elif [ "$STOP" = 1 ]; then
	GATE_STATUS="FAIL"
elif [ "$INCOMPLETE" = 1 ]; then
	GATE_STATUS="INCOMPLETE"
else
	GATE_STATUS="PASS"
fi

python3 - "$TSV" "$JSON" "$GATE_STATUS" "$EVIDENCE" <<'PY'
import json, sys
tsv, out, status, evidence = sys.argv[1:5]
steps = []
with open(tsv) as fh:
    header = fh.readline()
    for line in fh:
        line = line.rstrip("\n")
        if not line:
            continue
        parts = line.split("\t")
        while len(parts) < 7:
            parts.append("")
        name, st, rc, secs, cmd, logf, drv = parts[:7]
        try:
            rc_i = int(rc)
        except ValueError:
            rc_i = None
        try:
            secs_f = float(secs)
        except ValueError:
            secs_f = None
        steps.append({
            "step": name, "status": st, "exit_code": rc_i,
            "seconds": secs_f, "command": cmd, "log": logf,
            "driver_sha256": drv,
        })
counts = {}
for s in steps:
    counts[s["status"]] = counts.get(s["status"], 0) + 1
doc = {
    "gate": "phase3-check",
    "task": "P3.16",
    "status": status,
    "evidence": evidence,
    "is_two_pc_exit": False,
    "counts": counts,
    "steps": steps,
    "notes": [
        "A PASS here is the Linux automated gate, not the two-PC exit PASS.",
        "owner-two-pc-exit is BLOCKED pending owner hardware: two clean "
        "Windows 11 PCs, ten two-human matches, solo-vs-drone on native "
        "hardware and install/feel sign-off (Tests/phase3/windows-multiplayer.ps1).",
        "Operational P3.13/P3.14/P3.15 evidence is enforced only with "
        "--operational and is attached against the same source manifest.",
    ],
}
with open(out, "w") as fh:
    json.dump(doc, fh, indent=2)
print("gate-status=%s counts=%s" % (status, counts))
PY

{
	echo "P3.16 PHASE3-CHECK RESULT: $GATE_STATUS"
	echo "  evidence:     $EVIDENCE"
	echo "  results tsv:  $TSV"
	echo "  results json: $JSON"
	echo "  source manifest: $EVIDENCE/manifest/sources.txt ($(cat "$EVIDENCE/manifest/sources.sha256" 2>/dev/null))"
	echo "  steps:"
	awk -F'\t' 'NR>1 {printf "    %-28s %-12s rc=%-4s %ss\n",$1,$2,$3,$4}' "$TSV"
	echo
	if [ "$GATE_STATUS" = "PASS" ]; then
		echo "P3.16 PHASE3-CHECK PASS (Linux automated gate)"
		echo "NOTE: the two-PC Windows 11 owner exit is BLOCKED (owner hardware);"
		echo "      this PASS is NOT the two-PC exit PASS."
	elif [ "$GATE_STATUS" = "BLOCKED" ]; then
		echo "P3.16 PHASE3-CHECK BLOCKED (missing prerequisite); not a PASS"
	elif [ "$GATE_STATUS" = "INCOMPLETE" ]; then
		echo "P3.16 PHASE3-CHECK INCOMPLETE (a required step was skipped); not a PASS"
	else
		echo "P3.16 PHASE3-CHECK FAIL"
	fi
} | tee "$EVIDENCE/gate-summary.txt"

[ "$GATE_STATUS" = "PASS" ] && exit 0 || exit 1
