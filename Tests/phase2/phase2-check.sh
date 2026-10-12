#!/usr/bin/env bash
#
# phase2-check.sh -- P2.16 Phase 2 exit gate.
#
# Contract (docs/PHASE2-PLAN.md P2.16; task contract):
#   bash Tests/phase2/phase2-check.sh [--evidence DIR] [--dist DIR]
#                                     [--with-phase1]
#
# Runs every Phase 2 acceptance driver (P2.2-P2.15) and requires all of them
# green, then builds and verifies the two relocatable release archives:
#
#   P2.2  protocol.sh    frozen wire schema + content identity
#   P2.3  codec.sh       portable codec, golden bytes, MinGW target, fuzz
#   P2.4  players.sh     terminal-free human lifecycle/binding
#   P2.5  view.sh        authorized read-only view exporter
#   P2.6  events.sh      entity lifetimes, events, results
#   P2.7  transport.sh   nonblocking ENet transport + input admission
#   P2.8  session.sh     solo lobby + network-server path
#   P2.9  windows-smoke  cross-built client shell (Linux native + Wine)
#   P2.10 assets.sh      original visual assets + full-frame drawing
#   P2.11 timeline.sh    snapshot model, camera, interpolation
#   P2.12 input.sh       controls + world-intent transform
#   P2.13 ui.sh          HUD, mapper, radar, menu
#   P2.14 playable.sh    connect/start/results/reset playable slice
#   P2.15 parity.sh      three-way network-path byte identity
#
# Then Bin/package-phase2.sh builds xtank-client.zip (Windows x64) and
# xtank-server.tar.gz (Ubuntu network server), and this script verifies the
# archive layout, the client PE import allowlist and a display-free start of the
# packaged server.
#
# The native Windows 11 runtime proof and the owner's solo-play sign-off are
# explicit additional exit requirements and are NOT subsumed by this Linux
# gate; see docs/PHASE2-REPORT.md.
#
# The exact-simulation guardrail G is run separately (before/after):
#   export LC_ALL=C XTANK_SEED=114336
#   bash Tests/phase1/regression.sh --suite full --stage after --subject auto
#
# All dumps, logs, binaries, reference trees and packages live OUTSIDE the
# checkout under ${XTANK_P2_EVIDENCE:-/tmp/opencode/xtank-phase2/p2.16/gate}.
#
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HARNESS="$REPO/Tests/phase2"

EVIDENCE="${XTANK_P2_EVIDENCE:-/tmp/opencode/xtank-phase2/p2.16/gate}"
DIST="${XTANK_P2_DIST:-$EVIDENCE/dist}"
WITH_PHASE1=0
PHASE1_EVIDENCE="${XTANK_P1_EVIDENCE:-$EVIDENCE/phase1-check}"

while [ $# -gt 0 ]; do
	case "$1" in
	  --evidence)    EVIDENCE="$2"; DIST="${XTANK_P2_DIST:-$EVIDENCE/dist}"; shift 2 ;;
	  --dist)        DIST="$2"; shift 2 ;;
	  --with-phase1) WITH_PHASE1=1; shift ;;
	  *) echo "usage: phase2-check.sh [--evidence DIR] [--dist DIR] [--with-phase1]" >&2; exit 2 ;;
	esac
done

mkdir -p "$EVIDENCE"
export LC_ALL=C
export TAR_OPTIONS="${TAR_OPTIONS:---no-same-owner}"
export XTANK_P2_EVIDENCE="$EVIDENCE/scripts"

log()  { printf '%s\n' "$*"; }
fail() { printf 'PHASE2-CHECK FAIL: %s\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------------------
# Pinned dependencies (P2.1/P2.9).  Auto-discover the P2.9 evidence trees.
# ---------------------------------------------------------------------------
ENET_DIR="${XTANK_ENET_DIR:-}"
if [ -z "$ENET_DIR" ]; then
	for cand in \
	    "$REPO/../xtank-evidence/p2.9/deps/enet-1.3.18" \
	    /home/hatch/workspace/coding/xtank-evidence/p2.9/deps/enet-1.3.18 \
	    /tmp/opencode/xtank-phase2/p2.9/deps/enet-1.3.18; do
		[ -f "$cand/include/enet/enet.h" ] && { ENET_DIR="$cand"; break; }
	done
fi
SDL_PREFIX="${XTK_SDL3_LINUX_PREFIX:-}"
if [ -z "$SDL_PREFIX" ]; then
	for cand in \
	    "$REPO/../xtank-evidence/p2.9/deps/linux-prefix" \
	    /home/hatch/workspace/coding/xtank-evidence/p2.9/deps/linux-prefix \
	    /tmp/opencode/xtank-phase2/p2.9/deps/linux-prefix; do
		[ -f "$cand/include/SDL3/SDL.h" ] && { SDL_PREFIX="$cand"; break; }
	done
fi
MINGW_SDL_PREFIX="${XTK_SDL3_MINGW_PREFIX:-}"
if [ -z "$MINGW_SDL_PREFIX" ]; then
	for cand in \
	    "$REPO/../xtank-evidence/p2.9/deps/SDL3-3.4.18/x86_64-w64-mingw32" \
	    /home/hatch/workspace/coding/xtank-evidence/p2.9/deps/SDL3-3.4.18/x86_64-w64-mingw32 \
	    /tmp/opencode/xtank-phase2/p2.9/deps/SDL3-3.4.18/x86_64-w64-mingw32; do
		[ -f "$cand/bin/SDL3.dll" ] && { MINGW_SDL_PREFIX="$cand"; break; }
	done
fi

export XTANK_ENET_DIR="$ENET_DIR"
export XTK_SDL3_LINUX_PREFIX="$SDL_PREFIX"
export XTK_SDL3_MINGW_PREFIX="$MINGW_SDL_PREFIX"

[ -n "$ENET_DIR" ] && [ -f "$ENET_DIR/include/enet/enet.h" ] || \
	fail "pinned ENet source not found (set XTANK_ENET_DIR)"
[ -n "$SDL_PREFIX" ] && [ -f "$SDL_PREFIX/include/SDL3/SDL.h" ] || \
	fail "pinned SDL3 Linux prefix not found (set XTK_SDL3_LINUX_PREFIX)"
[ -n "$MINGW_SDL_PREFIX" ] && [ -f "$MINGW_SDL_PREFIX/bin/SDL3.dll" ] || \
	fail "pinned SDL3 MinGW prefix not found (set XTK_SDL3_MINGW_PREFIX)"

log "== P2.16 phase2-check"
log "   evidence: $EVIDENCE"
log "   ENet:     $ENET_DIR"
log "   SDL3:     $SDL_PREFIX"

# ---------------------------------------------------------------------------
# Optional full Phase 1 exit gate.
# ---------------------------------------------------------------------------
if [ "$WITH_PHASE1" = "1" ]; then
	log "== make phase1-check"
	( cd "$REPO" && LC_ALL=C TAR_OPTIONS="$TAR_OPTIONS" \
	    make phase1-check PHASE1_EVIDENCE="$PHASE1_EVIDENCE" ) \
	    > "$EVIDENCE/phase1-check.log" 2>&1 || \
	    { tail -30 "$EVIDENCE/phase1-check.log" >&2; fail "phase1-check failed"; }
	log "   P1.16 PHASE1-CHECK PASS"
fi

# ---------------------------------------------------------------------------
# P2.2-P2.15 acceptance drivers.
# ---------------------------------------------------------------------------
PASSED=()

run_driver() {
	name="$1"; shift
	drv_ev="$EVIDENCE/scripts/$name"
	mkdir -p "$drv_ev"
	log "== $name"
	if XTANK_P2_EVIDENCE="$drv_ev" "$@" > "$EVIDENCE/$name.log" 2>&1; then
		line="$(grep -m1 -E 'PASS' "$EVIDENCE/$name.log" | tail -1 || true)"
		[ -n "$line" ] && log "   $line"
		PASSED+=("$name")
	else
		tail -40 "$EVIDENCE/$name.log" >&2
		fail "$name did not pass (see $EVIDENCE/$name.log)"
	fi
}

run_driver protocol.sh   bash "$HARNESS/protocol.sh"
run_driver codec.sh      bash "$HARNESS/codec.sh"
run_driver players.sh    bash "$HARNESS/players.sh"
run_driver view.sh       bash "$HARNESS/view.sh"
run_driver events.sh     bash "$HARNESS/events.sh"
run_driver transport.sh  bash "$HARNESS/transport.sh"
run_driver session.sh    bash "$HARNESS/session.sh"
run_driver assets.sh     bash "$HARNESS/assets.sh"
run_driver timeline.sh   bash "$HARNESS/timeline.sh"
run_driver input.sh      bash "$HARNESS/input.sh"
run_driver ui.sh         bash "$HARNESS/ui.sh"
run_driver playable.sh   bash "$HARNESS/playable.sh"
run_driver parity.sh     bash "$HARNESS/parity.sh"

# ---------------------------------------------------------------------------
# P2.9 windows-smoke: Linux-native client self-test (and Wine, if available).
# ---------------------------------------------------------------------------
log "== windows-smoke (P2.9)"
SMOKE_WORK="$EVIDENCE/windows-smoke"
mkdir -p "$SMOKE_WORK"
( cd "$REPO" && make -s phase2-client-linux \
    XTK_CLIENT_BUILD="$SMOKE_WORK/build" \
    XTK_SDL3_LINUX_PREFIX="$SDL_PREFIX" XTK_ENET_DIR="$ENET_DIR" ) \
    > "$SMOKE_WORK/build.log" 2>&1 || \
    { tail -30 "$SMOKE_WORK/build.log" >&2; fail "Linux client build failed"; }
LIN_EXE="$SMOKE_WORK/build/linux/Client/xtank-client"
[ -x "$LIN_EXE" ] || fail "Linux client not built: $LIN_EXE"

# The CI sandbox denies sendmsg()/sendto() with an explicit destination; the
# test-only shim retries with a connected send().  It is a no-op elsewhere.
SHIM="$SMOKE_WORK/sandbox_sendmsg_shim.so"
gcc -shared -fPIC -O2 "$HARNESS/sandbox_sendmsg_shim.c" -o "$SHIM" -ldl \
    > "$SMOKE_WORK/shim.log" 2>&1 || fail "sendmsg shim build failed"

run_smoke() {
	exe="$1"; runner="$2"; out="$3"
	# shellcheck disable=SC2086
	LD_PRELOAD="$SHIM" LD_LIBRARY_PATH="$SDL_PREFIX/lib" \
	  SDL_VIDEODRIVER=dummy $runner "$exe" --self-test \
	  --fixtures "$HARNESS/fixtures/protocol" > "$out" 2>&1
}
run_smoke "$LIN_EXE" "" "$SMOKE_WORK/linux.out" || true
grep -q 'WINDOWS-SMOKE PASS' "$SMOKE_WORK/linux.out" || {
	tail -20 "$SMOKE_WORK/linux.out" >&2
	fail "Linux-native client self-test did not PASS"
}
grep -m1 'checks=' "$SMOKE_WORK/linux.out" | sed 's/^/   /'

if command -v wine >/dev/null 2>&1; then
	WIN_EXE="$DIST/client/xtank-client/bin/xtank-client.exe"
	if [ -x "$WIN_EXE" ]; then
		log "   Wine self-test (supplemental, not native-Windows evidence)"
		export WINEDEBUG=-all
		run_smoke "$WIN_EXE" "wine" "$SMOKE_WORK/wine.out" || true
		if grep -q 'WINDOWS-SMOKE PASS' "$SMOKE_WORK/wine.out"; then
			grep -m1 'checks=' "$SMOKE_WORK/wine.out" | sed 's/^/   /'
		else
			log "   note: Wine self-test did not PASS (supplemental only; see $SMOKE_WORK/wine.out)"
		fi
	fi
else
	log "   wine unavailable; native/Wine runtime smoke recorded as an exit requirement"
fi

# ---------------------------------------------------------------------------
# Packages: build the Windows client archive and the Ubuntu server archive.
# ---------------------------------------------------------------------------
log "== packages (Bin/package-phase2.sh)"
bash "$REPO/Bin/package-phase2.sh" --src "$REPO" --out "$DIST" \
    --mingw-sdl-prefix "$MINGW_SDL_PREFIX" --enet-dir "$ENET_DIR" \
    > "$EVIDENCE/packages.log" 2>&1 || {
	tail -40 "$EVIDENCE/packages.log" >&2
	fail "package-phase2.sh failed"
}
grep -E '^(package-phase2|P2.16)' "$EVIDENCE/packages.log" | sed 's/^/   /'

CLIENT_DIR="$DIST/client/xtank-client"
SERVER_DIR="$DIST/server/xtank-server"
CLIENT_ZIP="$DIST/client/xtank-client.zip"
SERVER_TAR="$DIST/server/xtank-server.tar.gz"

# --- client archive layout + PE import allowlist ---
[ -f "$CLIENT_ZIP" ] || fail "client archive not built: $CLIENT_ZIP"
[ -f "$CLIENT_DIR/bin/xtank-client.exe" ] || fail "client exe missing"
[ -f "$CLIENT_DIR/bin/SDL3.dll" ] || fail "bundled SDL3.dll missing"
[ -s "$CLIENT_DIR/assets/xtank-assets.bin" ] || fail "asset bundle missing"
[ -f "$CLIENT_DIR/fixtures/protocol/hello.hex" ] || fail "protocol fixtures missing"
[ -f "$CLIENT_DIR/tools/windows-smoke.ps1" ] || fail "windows-smoke.ps1 missing"
[ -f "$CLIENT_DIR/THIRD-PARTY-NOTICES.txt" ] || fail "third-party notices missing"
[ -f "$CLIENT_DIR/README.txt" ] || fail "client README.txt missing"

OD="x86_64-w64-mingw32-objdump"
if command -v "$OD" >/dev/null 2>&1; then
	imports="$("$OD" -p "$CLIENT_DIR/bin/xtank-client.exe" | \
	    sed -n 's/.*DLL Name: //p' | tr 'A-Z' 'a-z' | sort -u)"
	log "   PE imports: $(printf '%s ' $imports)"
	bad=""
	for d in $imports; do
		case "$d" in
		  sdl3.dll|kernel32.dll|ntdll.dll|msvcrt.dll|winmm.dll|ws2_32.dll) ;;
		  api-ms-win-*) ;;
		  *) bad="$bad $d" ;;
		esac
	done
	[ -z "$bad" ] || fail "client imports unexpected DLL(s):$bad"
	for pat in libgcc_s libwinpthread libstdc++ cygwin msys- libssp; do
		ls "$CLIENT_DIR/bin/" | grep -qi "$pat" && fail "forbidden runtime DLL staged: $pat"
	done
	log "   client PE imports are Win32 system DLLs + bundled SDL3.dll only"
	[ -f "$CLIENT_DIR/bin/SDL3.dll" ] || fail "SDL3.dll not bundled"
else
	log "   $OD unavailable; PE import audit deferred to windows-smoke.ps1"
fi

# unzip round-trip
rm -rf "$EVIDENCE/unzip"; mkdir -p "$EVIDENCE/unzip"
unzip -q "$CLIENT_ZIP" -d "$EVIDENCE/unzip" || fail "client zip is not readable"
[ -f "$EVIDENCE/unzip/xtank-client/bin/xtank-client.exe" ] || fail "zip missing client exe"
[ -f "$EVIDENCE/unzip/xtank-client/bin/SDL3.dll" ] || fail "zip missing SDL3.dll"
[ -s "$EVIDENCE/unzip/xtank-client/assets/xtank-assets.bin" ] || fail "zip missing asset bundle"

# --- server archive layout + display-free start ---
[ -f "$SERVER_TAR" ] || fail "server archive not built: $SERVER_TAR"
[ -x "$SERVER_DIR/bin/xtank-server" ] || fail "server binary missing"
[ -d "$SERVER_DIR/share/xtank/Mazes" ] || fail "server Mazes missing"
[ -d "$SERVER_DIR/share/xtank/Vehicles" ] || fail "server Vehicles missing"

( cd "$SERVER_DIR/bin" && \
  env -u DISPLAY -u XTANK_DIR -u HOME -u LD_PRELOAD \
      -u XTANK_ASSET_ROOT -u XTANK_SERVER_ASSETS LC_ALL=C \
      timeout 20 ./xtank-server --listen 0 --fast ) \
    > "$EVIDENCE/server-listen.out" 2>&1 || true
grep -q '^SERVER-LISTEN port=' "$EVIDENCE/server-listen.out" || {
	tail -10 "$EVIDENCE/server-listen.out" >&2
	fail "packaged network server did not start display-free"
}
log "   packaged server starts display-free: $(grep -m1 'SERVER-LISTEN' "$EVIDENCE/server-listen.out")"

# ---------------------------------------------------------------------------
log ""
log "   drivers passed: ${PASSED[*]}"
log ""
log "P2.16 PHASE2-CHECK PASS ($(( ${#PASSED[@]} )) drivers + packages)"
