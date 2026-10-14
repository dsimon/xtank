#!/usr/bin/env bash
#
# package.sh -- P3.16 Phase 3 package acceptance (relocatable archives).
#
# Contract (docs/PHASE3-PLAN.md P3.16; §6 automated exit gate item 3):
#   bash Tests/phase3/package.sh [--evidence DIR] [--dist DIR]
#                                [--mingw-sdl-prefix DIR] [--enet-dir DIR]
#
# Builds both release archives with Bin/package-phase3.sh, then proves:
#   * the client .zip and server .tar.gz unpack and the bundles run after being
#     relocated to a different path *with spaces*;
#   * VERSION, PINNED-RUNTIMES.txt, CONTENT-MANIFEST.sha256, MANIFEST.sha256,
#     THIRD-PARTY-NOTICES.txt and licenses/ are present on both bundles and the
#     content manifest hashes match the shipped content bytes;
#   * the client PE import allowlist passes (Win32 system DLLs + bundled SDL3);
#   * the Ubuntu network server is X-free (no libX, no Xlib symbol, no X header
#     in any server TU) and starts display-free (--listen) and plays a match
#     with DISPLAY/XTANK_DIR/HOME unset.
#
# This driver is package-scoped; the Phase 3 exit gate (Tests/phase3/
# phase3-check.sh) additionally requires the full simulation/integration
# drivers and the inherited Phase 1/2 gates.  Native Windows install/feel and
# the ten two-human matches are OWNER exit items, not proven here.
#
# All generated binaries/logs/packages live OUTSIDE the checkout under
# ${XTANK_P3_EVIDENCE:-/home/hatch/workspace/coding/xtank-evidence/p3.16}/package.
#
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
EVIDENCE="${XTANK_P3_EVIDENCE:-/home/hatch/workspace/coding/xtank-evidence/p3.16}"
WORK="$EVIDENCE/package"
DIST="${XTANK_P3_DIST:-$WORK/dist}"
MINGW_SDL_PREFIX="${XTK_SDL3_MINGW_PREFIX:-}"
ENET_DIR="${XTANK_ENET_DIR:-}"
MAX_TICKS=100000

while [ $# -gt 0 ]; do
	case "$1" in
	  --evidence)          EVIDENCE="$2"; WORK="$EVIDENCE/package"; DIST="${XTANK_P3_DIST:-$WORK/dist}"; shift 2 ;;
	  --dist)              DIST="$2"; shift 2 ;;
	  --mingw-sdl-prefix)  MINGW_SDL_PREFIX="$2"; shift 2 ;;
	  --enet-dir)          ENET_DIR="$2"; shift 2 ;;
	  --max-ticks)         MAX_TICKS="$2"; shift 2 ;;
	  *) echo "usage: package.sh [--evidence DIR] [--dist DIR] [--mingw-sdl-prefix DIR] [--enet-dir DIR] [--max-ticks N]" >&2; exit 2 ;;
	esac
done

mkdir -p "$WORK"
export LC_ALL=C
export TAR_OPTIONS="${TAR_OPTIONS:---no-same-owner}"

log()  { printf '%s\n' "$*"; }
fail() { printf 'P3.16 PACKAGE FAIL: %s\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------------------
# Discover pinned dependencies (same locations as P2.9/P3.x).
# ---------------------------------------------------------------------------
if [ -z "$ENET_DIR" ]; then
	for cand in \
	    "$REPO/../xtank-evidence/p2.9/deps/enet-1.3.18" \
	    /home/hatch/workspace/coding/xtank-evidence/p2.9/deps/enet-1.3.18 \
	    /tmp/opencode/xtank-phase2/p2.9/deps/enet-1.3.18; do
		[ -f "$cand/include/enet/enet.h" ] && { ENET_DIR="$cand"; break; }
	done
fi
if [ -z "$MINGW_SDL_PREFIX" ]; then
	for cand in \
	    "$REPO/../xtank-evidence/p2.9/deps/SDL3-3.4.18/x86_64-w64-mingw32" \
	    /home/hatch/workspace/coding/xtank-evidence/p2.9/deps/SDL3-3.4.18/x86_64-w64-mingw32 \
	    /tmp/opencode/xtank-phase2/p2.9/deps/SDL3-3.4.18/x86_64-w64-mingw32; do
		[ -f "$cand/bin/SDL3.dll" ] && { MINGW_SDL_PREFIX="$cand"; break; }
	done
fi
[ -n "$ENET_DIR" ] && [ -f "$ENET_DIR/include/enet/enet.h" ] || \
	fail "pinned ENet source not found (set XTANK_ENET_DIR)"
[ -n "$MINGW_SDL_PREFIX" ] && [ -f "$MINGW_SDL_PREFIX/bin/SDL3.dll" ] || \
	fail "pinned SDL3 MinGW prefix not found (set XTK_SDL3_MINGW_PREFIX)"
command -v x86_64-w64-mingw32-objdump >/dev/null 2>&1 || \
	fail "MinGW objdump missing (client PE import allowlist required)"

log "== package-phase3.sh (SSD build)"
bash "$REPO/Bin/package-phase3.sh" --src "$REPO" --out "$DIST" \
    --mingw-sdl-prefix "$MINGW_SDL_PREFIX" --enet-dir "$ENET_DIR" \
    > "$WORK/package-phase3.log" 2>&1 || {
	tail -40 "$WORK/package-phase3.log" >&2
	fail "package-phase3.sh failed (see $WORK/package-phase3.log)"
}
grep -E '^(package-phase3|P3.16)' "$WORK/package-phase3.log" | sed 's/^/   /'

CLIENT_SRC="$DIST/client/xtank-client"
SERVER_SRC="$DIST/server/xtank-server"
CLIENT_ZIP="$DIST/client/xtank-client.zip"
SERVER_TAR="$DIST/server/xtank-server.tar.gz"
[ -f "$CLIENT_ZIP" ] || fail "client archive missing: $CLIENT_ZIP"
[ -f "$SERVER_TAR" ] || fail "server archive missing: $SERVER_TAR"

# ---------------------------------------------------------------------------
# Unpack + relocate to a path with spaces.
# ---------------------------------------------------------------------------
log "== unpack/relocate (path with spaces)"
REL="$WORK/a path with spaces/xtank-phase3"
rm -rf "$WORK/a path with spaces"
mkdir -p "$REL/client" "$REL/server"
unzip -q "$CLIENT_ZIP" -d "$REL/client" || fail "client zip not readable"
tar -xzf "$SERVER_TAR" -C "$REL/server" || fail "server tar not readable"

CLIENT="$REL/client/xtank-client"
SERVER="$REL/server/xtank-server"
[ -f "$CLIENT/bin/xtank-client.exe" ] || fail "relocated client exe missing"
[ -f "$CLIENT/bin/SDL3.dll" ] || fail "relocated SDL3.dll missing"
[ -x "$SERVER/bin/xtank-server" ] || fail "relocated server binary missing"
[ -d "$SERVER/share/xtank/Mazes" ] || fail "relocated server Mazes missing"
log "   client=$CLIENT"
log "   server=$SERVER"

# ---------------------------------------------------------------------------
# Metadata: version / pinned runtimes / content + full manifests / notices.
# ---------------------------------------------------------------------------
log "== release metadata"
for kind in client server; do
	eval "BUNDLE=\$$(echo "$kind" | tr a-z A-Z)"
	[ -s "$BUNDLE/VERSION" ] || fail "$kind: VERSION missing"
	[ -s "$BUNDLE/PINNED-RUNTIMES.txt" ] || fail "$kind: PINNED-RUNTIMES.txt missing"
	[ -s "$BUNDLE/CONTENT-MANIFEST.sha256" ] || fail "$kind: CONTENT-MANIFEST.sha256 missing"
	[ -s "$BUNDLE/MANIFEST.sha256" ] || fail "$kind: MANIFEST.sha256 missing"
	[ -s "$BUNDLE/THIRD-PARTY-NOTICES.txt" ] || fail "$kind: THIRD-PARTY-NOTICES.txt missing"
	[ -d "$BUNDLE/licenses" ] || fail "$kind: licenses/ missing"
	grep -q 'protocol=1.1' "$BUNDLE/PINNED-RUNTIMES.txt" || fail "$kind: pinned protocol not 1.1"
	grep -q 'enet=1.3.18' "$BUNDLE/PINNED-RUNTIMES.txt" || fail "$kind: pinned ENet version missing"
	# every content manifest line must be a real file with a matching hash
	while read -r h f; do
		case "$h" in \#*|"") continue ;; esac
		[ -f "$BUNDLE/$f" ] || fail "$kind: content manifest lists missing file $f"
		[ "$(sha256sum "$BUNDLE/$f" | awk '{print $1}')" = "$h" ] || \
			fail "$kind: content hash mismatch for $f"
	done < "$BUNDLE/CONTENT-MANIFEST.sha256"
	log "   $kind: VERSION=$(cat "$BUNDLE/VERSION")"
done
[ -f "$CLIENT/licenses/SDL3-LICENSE" ] || fail "client: SDL3 license missing"
[ -f "$SERVER/licenses/ENet-LICENSE" ] || fail "server: ENet license missing"
[ -f "$CLIENT/tools/windows-multiplayer.ps1" ] || fail "client: windows-multiplayer.ps1 missing"
[ -f "$CLIENT/tools/windows-smoke.ps1" ] || fail "client: windows-smoke.ps1 missing"

# Server content manifest must match the repository's shipped content bytes.
for pair in "Mazes:share/xtank/Mazes" "Vehicles:share/xtank/Vehicles"; do
	srcdir="$REPO/${pair%%:*}"; dstdir="$SERVER/${pair#*:}"
	mkdir -p "$WORK/content-check"
	checklog="$WORK/content-check/${pair%%:*}.log"
	( cd "$srcdir" && find . -type f -print | LC_ALL=C sort | \
	  while IFS= read -r f; do
		rel="${f#./}"
		a="$(sha256sum "$f" | awk '{print $1}')"
		b="$(sha256sum "$dstdir/$rel" | awk '{print $1}')"
		[ "$a" = "$b" ] || { echo "MISMATCH $rel"; exit 1; }
	  done ) > "$checklog" 2>&1 || \
		{ cat "$checklog" >&2; fail "server content differs from repo: $dstdir"; }
done
log "   server content bytes match the repository content set"

# ---------------------------------------------------------------------------
# Client PE import allowlist.
# ---------------------------------------------------------------------------
log "== client PE import allowlist"
OD="x86_64-w64-mingw32-objdump"
imports="$("$OD" -p "$CLIENT/bin/xtank-client.exe" | sed -n 's/.*DLL Name: //p' | tr 'A-Z' 'a-z' | sort -u)"
log "   imports: $(printf '%s ' $imports)"
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
	ls "$CLIENT/bin/" | grep -qi "$pat" && fail "forbidden runtime DLL staged: $pat"
done
log "   client imports Win32 system DLLs + bundled SDL3.dll only"

# ---------------------------------------------------------------------------
# Server X-free audit + display-free launch.
# ---------------------------------------------------------------------------
log "== server X-free audit"
ldd "$SERVER/bin/xtank-server" > "$WORK/ldd.txt" 2>&1 || true
if grep -qiE 'libX[0-9A-Za-z]*\.so|libxcb|libX11' "$WORK/ldd.txt"; then
	fail "server links an X library: $(grep -iE 'libX' "$WORK/ldd.txt")"
fi
if nm -u "$SERVER/bin/xtank-server" 2>/dev/null | grep -qE 'X[A-Z][A-Za-z0-9_]*'; then
	fail "server has an undefined Xlib symbol"
fi
XHITS=$(grep -lE 'X11/|/x11\.h' "$REPO"/Src/sobj/*.d 2>/dev/null || true)
[ -z "$XHITS" ] || fail "server TUs depend on X headers: $XHITS"
log "   no libX / no Xlib symbol / no X header in server TUs"

log "== packaged network server starts display-free"
( cd "$SERVER/bin" && \
  env -u DISPLAY -u XTANK_DIR -u HOME -u LD_PRELOAD \
      -u XTANK_ASSET_ROOT -u XTANK_SERVER_ASSETS LC_ALL=C \
      timeout 20 ./xtank-server --listen 0 --fast ) \
    > "$WORK/server-listen.out" 2>&1 || true
grep -q '^SERVER-LISTEN port=' "$WORK/server-listen.out" || {
	tail -10 "$WORK/server-listen.out" >&2
	fail "packaged network server did not start display-free"
}
log "   $(grep -m1 'SERVER-LISTEN' "$WORK/server-listen.out")"

log "== packaged network server plays both mazes"
for maze in combat1 Arena; do
	( cd "$SERVER/bin" && \
	  env -u DISPLAY -u XTANK_DIR -u HOME -u LD_PRELOAD \
	      -u XTANK_ASSET_ROOT -u XTANK_SERVER_ASSETS LC_ALL=C \
	      ./xtank-server --maze "$maze" --seed 114336 --drones 2 \
	      --fast --matches 2 --max-ticks "$MAX_TICKS" ) > "$WORK/run-$maze.out" 2>&1 || \
	    fail "packaged server $maze run failed (see $WORK/run-$maze.out)"
	grep -q '^SERVER-MATCH 1 ' "$WORK/run-$maze.out" || fail "$maze: no match result"
	grep -q '^SERVER-DONE matches=2$' "$WORK/run-$maze.out" || fail "$maze: no completion"
	log "   $maze: matches=2 results reported"
done

log ""
log "P3.16 PACKAGE PASS (unpack/relocate, manifests, notices, DLL allowlist, X-free server)"
