#!/usr/bin/env bash
#
# package-phase2.sh -- assemble both Phase 2 release archives (P2.16).
#
# Produces, under --out (default <src>/xtank-phase2-dist):
#
#   xtank-client/  and  xtank-client.zip   the relocatable Windows 11 x64 client
#                                          (Bin/package-client.sh): exe + SDL3.dll
#                                          + asset bundle + fixtures + smoke test
#   xtank-server/  and  xtank-server.tar.gz the Ubuntu network server
#                                          (Bin/package-server.sh --net): binary
#                                          + Mazes/Vehicles + notices
#
# The client is cross-built with the pinned MinGW-w64 toolchain and SDL3 devel
# package; the network server is built with the existing GNUmakefile and the
# pinned ENet source.  No generated binary is copied into the checkout.
#
# Usage:
#   bash Bin/package-phase2.sh [--src ROOT] [--out DIR]
#       [--mingw-sdl-prefix DIR] [--enet-dir DIR]
#       [--client-binary EXE] [--skip-client-build] [--no-client-zip]
#
# Environment fallbacks: XTK_SDL3_MINGW_PREFIX, XTANK_ENET_DIR.
#
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"

SRC="$REPO"
OUT=""
MINGW_SDL_PREFIX="${XTK_SDL3_MINGW_PREFIX:-}"
ENET_DIR="${XTANK_ENET_DIR:-}"
CLIENT_BINARY=""
SKIP_CLIENT_BUILD=0
CLIENT_ZIP=1
CLIENT_NAME="xtank-client"
SERVER_NAME="xtank-server"

usage() {
	cat >&2 <<'EOF'
usage: package-phase2.sh [--src ROOT] [--out DIR]
                         [--mingw-sdl-prefix DIR] [--enet-dir DIR]
                         [--client-binary EXE] [--skip-client-build]
                         [--no-client-zip] [--client-name NAME]
                         [--server-name NAME]
EOF
	exit 2
}

while [ $# -gt 0 ]; do
	case "$1" in
	  --src)               SRC="$2"; shift 2 ;;
	  --out)               OUT="$2"; shift 2 ;;
	  --mingw-sdl-prefix)  MINGW_SDL_PREFIX="$2"; shift 2 ;;
	  --enet-dir)          ENET_DIR="$2"; shift 2 ;;
	  --client-binary)     CLIENT_BINARY="$2"; shift 2 ;;
	  --skip-client-build) SKIP_CLIENT_BUILD=1; shift ;;
	  --no-client-zip)     CLIENT_ZIP=0; shift ;;
	  --client-name)       CLIENT_NAME="$2"; shift 2 ;;
	  --server-name)       SERVER_NAME="$2"; shift 2 ;;
	  *) usage ;;
	esac
done

[ -d "$SRC" ] || { echo "package-phase2: no such source root: $SRC" >&2; exit 2; }
[ -n "$OUT" ] || OUT="$SRC/xtank-phase2-dist"
mkdir -p "$OUT"

# ---------------------------------------------------------------------------
# Discover the pinned dependencies if not supplied.
# ---------------------------------------------------------------------------
if [ -z "$ENET_DIR" ] || [ ! -f "$ENET_DIR/include/enet/enet.h" ]; then
	for cand in \
	    "$SRC/../xtank-evidence/p2.9/deps/enet-1.3.18" \
	    /home/hatch/workspace/coding/xtank-evidence/p2.9/deps/enet-1.3.18 \
	    /tmp/opencode/xtank-phase2/p2.9/deps/enet-1.3.18; do
		[ -f "$cand/include/enet/enet.h" ] && { ENET_DIR="$cand"; break; }
	done
fi
if [ -z "$MINGW_SDL_PREFIX" ] || [ ! -f "$MINGW_SDL_PREFIX/bin/SDL3.dll" ]; then
	for cand in \
	    "$SRC/../xtank-evidence/p2.9/deps/SDL3-3.4.18/x86_64-w64-mingw32" \
	    /home/hatch/workspace/coding/xtank-evidence/p2.9/deps/SDL3-3.4.18/x86_64-w64-mingw32 \
	    /tmp/opencode/xtank-phase2/p2.9/deps/SDL3-3.4.18/x86_64-w64-mingw32; do
		[ -f "$cand/bin/SDL3.dll" ] && { MINGW_SDL_PREFIX="$cand"; break; }
	done
fi

# Optional license texts for distribution notices.
SDL3_LICENSE=""
ENET_LICENSE=""
if [ -n "$MINGW_SDL_PREFIX" ]; then
	sdlroot="$(cd "$MINGW_SDL_PREFIX/.." && pwd)"
	[ -f "$sdlroot/LICENSE.txt" ] && SDL3_LICENSE="$sdlroot/LICENSE.txt"
fi
[ -n "$ENET_DIR" ] && [ -f "$ENET_DIR/LICENSE" ] && ENET_LICENSE="$ENET_DIR/LICENSE"

# ---------------------------------------------------------------------------
# 1. Server package (Ubuntu network server).
# ---------------------------------------------------------------------------
[ -n "$ENET_DIR" ] || { echo "package-phase2: ENet source not found (use --enet-dir)" >&2; exit 2; }
echo "== package-phase2: server package"
bash "$SRC/Bin/package-server.sh" --src "$SRC" --net --enet-dir "$ENET_DIR" \
    --out "$OUT/server" --name "$SERVER_NAME" --tar

# ---------------------------------------------------------------------------
# 2. Client package (Windows x64 archive).
# ---------------------------------------------------------------------------
echo "== package-phase2: client package"
client_args=(
	--src "$SRC"
	--out "$OUT/client"
	--name "$CLIENT_NAME"
	--fixtures "$SRC/Tests/phase2/fixtures/protocol"
	--smoke "$SRC/Tests/phase2/windows-smoke.ps1"
)
[ -n "$SDL3_LICENSE" ] && client_args+=(--sdl3-license "$SDL3_LICENSE")
[ -n "$ENET_LICENSE" ] && client_args+=(--enet-license "$ENET_LICENSE")
[ "$CLIENT_ZIP" = "1" ] && client_args+=(--zip)
if [ -n "$CLIENT_BINARY" ]; then
	client_args+=(--binary "$CLIENT_BINARY")
elif [ "$SKIP_CLIENT_BUILD" = "1" ]; then
	echo "package-phase2: --skip-client-build without --client-binary" >&2
	exit 2
else
	[ -n "$MINGW_SDL_PREFIX" ] || {
		echo "package-phase2: SDL3 MinGW prefix not found (use --mingw-sdl-prefix)" >&2
		exit 2
	}
	client_args+=(--build --mingw-sdl-prefix "$MINGW_SDL_PREFIX" --enet-dir "$ENET_DIR")
fi
bash "$SRC/Bin/package-client.sh" "${client_args[@]}"

# ---------------------------------------------------------------------------
# 3. Basic layout verification.
# ---------------------------------------------------------------------------
CLIENT="$OUT/client/$CLIENT_NAME"
SERVER="$OUT/server/$SERVER_NAME"
[ -f "$CLIENT/bin/xtank-client.exe" ] || { echo "package-phase2: missing client exe" >&2; exit 1; }
[ -f "$CLIENT/bin/SDL3.dll" ] || { echo "package-phase2: missing SDL3.dll" >&2; exit 1; }
[ -s "$CLIENT/assets/xtank-assets.bin" ] || { echo "package-phase2: missing asset bundle" >&2; exit 1; }
[ -x "$SERVER/bin/xtank-server" ] || { echo "package-phase2: missing server binary" >&2; exit 1; }
[ -d "$SERVER/share/xtank/Mazes" ] || { echo "package-phase2: missing server assets" >&2; exit 1; }

echo "package-phase2: client = $CLIENT"
echo "package-phase2: server = $SERVER"
if [ "$CLIENT_ZIP" = "1" ]; then echo "package-phase2: archive = $OUT/client/$CLIENT_NAME.zip"; fi
echo "package-phase2: archive = $OUT/server/$SERVER_NAME.tar.gz"
echo "P2.16 PACKAGES PASS"
