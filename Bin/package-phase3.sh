#!/usr/bin/env bash
#
# package-phase3.sh -- assemble both Phase 3 release archives (P3.16).
#
# Wraps the proven Phase 2 packagers and adds the Phase 3 release metadata the
# exit gate requires on *both* bundles:
#
#   xtank-client/ + xtank-client.zip   relocatable Windows 11 x64 client
#                                      (Bin/package-client.sh): exe + SDL3.dll
#                                      + asset bundle + wire fixtures + smoke
#                                      + windows-multiplayer.ps1
#   xtank-server/ + xtank-server.tar.gz relocatable Ubuntu network server
#                                      (Bin/package-server.sh --net): binary
#                                      + Mazes/Vehicles + ENet license
#
# On each bundle this adds:
#   VERSION                 exact Phase 3 release label
#   PINNED-RUNTIMES.txt     exact dependency/toolchain versions
#   CONTENT-MANIFEST.sha256 per-file SHA-256 of the shipped content bytes
#   MANIFEST.sha256         per-file SHA-256 of the whole bundle
#   THIRD-PARTY-NOTICES.txt (client from package-client.sh; server added here)
#   licenses/               SDL3 (client) / ENet (both)
#
# The client is cross-built with the pinned MinGW-w64 toolchain and SDL3 devel
# package; the network server is built with the existing GNUmakefile and the
# pinned ENet source.  No generated binary is copied into the checkout.
#
# Usage:
#   bash Bin/package-phase3.sh [--src ROOT] [--out DIR]
#       [--mingw-sdl-prefix DIR] [--enet-dir DIR]
#       [--client-binary EXE] [--skip-client-build] [--no-archives]
#
# Environment fallbacks: XTK_SDL3_MINGW_PREFIX, XTANK_ENET_DIR,
#                        XTANK_PHASE3_VERSION.
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
MAKE_ARCHIVES=1
CLIENT_NAME="xtank-client"
SERVER_NAME="xtank-server"
VERSION="${XTANK_PHASE3_VERSION:-0.3.0}"
RELEASE_LABEL="Xtank Phase 3 MVP ${VERSION}"
BUILT_UTC="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

usage() {
	cat >&2 <<'EOF'
usage: package-phase3.sh [--src ROOT] [--out DIR]
                         [--mingw-sdl-prefix DIR] [--enet-dir DIR]
                         [--client-binary EXE] [--skip-client-build]
                         [--no-archives] [--client-name NAME]
                         [--server-name NAME] [--version VER]
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
	  --no-archives)       MAKE_ARCHIVES=0; shift ;;
	  --client-name)       CLIENT_NAME="$2"; shift 2 ;;
	  --server-name)       SERVER_NAME="$2"; shift 2 ;;
	  --version)           VERSION="$2"; RELEASE_LABEL="Xtank Phase 3 MVP ${VERSION}"; shift 2 ;;
	  *) usage ;;
	esac
done

[ -d "$SRC" ] || { echo "package-phase3: no such source root: $SRC" >&2; exit 2; }
[ -n "$OUT" ] || OUT="$SRC/xtank-phase3-dist"
mkdir -p "$OUT"

# ---------------------------------------------------------------------------
# Discover the pinned dependencies if not supplied (same locations as P2.9).
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

SDL3_LICENSE=""
ENET_LICENSE=""
if [ -n "$MINGW_SDL_PREFIX" ]; then
	sdlroot="$(cd "$MINGW_SDL_PREFIX/.." && pwd)"
	[ -f "$sdlroot/LICENSE.txt" ] && SDL3_LICENSE="$sdlroot/LICENSE.txt"
fi
[ -n "$ENET_DIR" ] && [ -f "$ENET_DIR/LICENSE" ] && ENET_LICENSE="$ENET_DIR/LICENSE"

[ -n "$ENET_DIR" ] || { echo "package-phase3: ENet source not found (use --enet-dir)" >&2; exit 2; }

# ---------------------------------------------------------------------------
# Helpers.
# ---------------------------------------------------------------------------
emit_content_manifest() { # bundle_dir manifest_path "label:path" ...
	manifest="$2"; shift 2
	{
		echo "# Xtank Phase 3 content manifest -- ${RELEASE_LABEL}"
		echo "# built=${BUILT_UTC}"
		for spec in "$@"; do
			label="${spec%%:*}"
			rel="${spec#*:}"
			if [ -d "$1/$rel" ]; then
				( cd "$1" && find "$rel" -type f -print | LC_ALL=C sort | \
				  while IFS= read -r f; do
					printf '%s  %s\n' "$(sha256sum "$f" | awk '{print $1}')" "$f"
				  done )
			elif [ -f "$1/$rel" ]; then
				printf '%s  %s\n' "$(sha256sum "$1/$rel" | awk '{print $1}')" "$rel"
			else
				echo "# MISSING $label: $rel"
			fi
		done
	} > "$manifest"
}

emit_bundle_manifest() { # bundle_dir
	( cd "$1" && find . -type f ! -name MANIFEST.sha256 -print | LC_ALL=C sort | \
	  while IFS= read -r f; do
		sha256sum "$f"
	  done ) | sed 's| \./| |' > "$1/MANIFEST.sha256"
}

emit_pinned_runtimes() { # bundle_dir kind
	dest="$1/PINNED-RUNTIMES.txt"
	mingw_ver="$(x86_64-w64-mingw32-gcc --version 2>/dev/null | head -1 || echo 'n/a')"
	obj_ver="$(x86_64-w64-mingw32-objdump --version 2>/dev/null | head -1 || echo 'n/a')"
	{
		echo "# Xtank Phase 3 pinned runtimes/toolchain -- ${RELEASE_LABEL}"
		echo "# built=${BUILT_UTC}"
		echo "release=$RELEASE_LABEL"
		echo "sdl3=3.4.18"
		echo "enet=1.3.18"
		echo "protocol=1.1"
		echo "mingw=$mingw_ver"
		echo "mingw_binutils=$obj_ver"
		echo "server_runtime=ubuntu-24.04-x86_64-glibc"
		echo "client_runtime=windows-11-x86_64 (bundled SDL3.dll:3.4.18)"
	} > "$dest"
}

notices_server() { # bundle_dir
	dest="$1/THIRD-PARTY-NOTICES.txt"
	{
		echo "Xtank Linux network server -- bundled third-party components"
		echo
		echo "ENet (https://enet.bespin.org)   MIT license.  Statically linked"
		echo "                                into bin/xtank-server; license in"
		echo "                                licenses/ENet-LICENSE."
		echo
		echo "The original Xtank source and content are distributed under the"
		echo "terms in COPYRIGHT and COPYRIGHT.notes."
	} > "$dest"
}

# ---------------------------------------------------------------------------
# 1. Server package (Ubuntu network server).
# ---------------------------------------------------------------------------
echo "== package-phase3: server package"
bash "$SRC/Bin/package-server.sh" --src "$SRC" --net --enet-dir "$ENET_DIR" \
    --out "$OUT/server" --name "$SERVER_NAME"
SERVER="$OUT/server/$SERVER_NAME"
[ -x "$SERVER/bin/xtank-server" ] || { echo "package-phase3: missing server binary" >&2; exit 1; }

mkdir -p "$SERVER/licenses"
[ -n "$ENET_LICENSE" ] && install -m 0644 "$ENET_LICENSE" "$SERVER/licenses/ENet-LICENSE"
printf '%s\n' "$RELEASE_LABEL" > "$SERVER/VERSION"
notices_server "$SERVER"
emit_pinned_runtimes "$SERVER" server
emit_content_manifest "$SERVER" "$SERVER/CONTENT-MANIFEST.sha256" \
    "mazes:share/xtank/Mazes" "vehicles:share/xtank/Vehicles"
emit_bundle_manifest "$SERVER"

# ---------------------------------------------------------------------------
# 2. Client package (Windows x64 archive).
# ---------------------------------------------------------------------------
echo "== package-phase3: client package"
client_args=(
	--src "$SRC"
	--out "$OUT/client"
	--name "$CLIENT_NAME"
	--fixtures "$SRC/Tests/phase3/fixtures/protocol"
	--smoke "$SRC/Tests/phase2/windows-smoke.ps1"
)
[ -n "$SDL3_LICENSE" ] && client_args+=(--sdl3-license "$SDL3_LICENSE")
[ -n "$ENET_LICENSE" ] && client_args+=(--enet-license "$ENET_LICENSE")
if [ -n "$CLIENT_BINARY" ]; then
	client_args+=(--binary "$CLIENT_BINARY")
elif [ "$SKIP_CLIENT_BUILD" = "1" ]; then
	echo "package-phase3: --skip-client-build without --client-binary" >&2
	exit 2
else
	[ -n "$MINGW_SDL_PREFIX" ] || {
		echo "package-phase3: SDL3 MinGW prefix not found (use --mingw-sdl-prefix)" >&2
		exit 2
	}
	client_args+=(--build --mingw-sdl-prefix "$MINGW_SDL_PREFIX" --enet-dir "$ENET_DIR")
fi
bash "$SRC/Bin/package-client.sh" "${client_args[@]}"
CLIENT="$OUT/client/$CLIENT_NAME"
[ -f "$CLIENT/bin/xtank-client.exe" ] || { echo "package-phase3: missing client exe" >&2; exit 1; }

# Phase 3 additions: multiplayer PowerShell procedure + release metadata.
install -m 0644 "$SRC/Tests/phase3/windows-multiplayer.ps1" \
    "$CLIENT/tools/windows-multiplayer.ps1"
printf '%s\n' "$RELEASE_LABEL" > "$CLIENT/VERSION"
emit_pinned_runtimes "$CLIENT" client
emit_content_manifest "$CLIENT" "$CLIENT/CONTENT-MANIFEST.sha256" \
    "assets:assets" "fixtures:fixtures"
emit_bundle_manifest "$CLIENT"

# ---------------------------------------------------------------------------
# 3. Archives (created after augmentation so they contain the metadata).
# ---------------------------------------------------------------------------
if [ "$MAKE_ARCHIVES" = "1" ]; then
	( cd "$OUT/client" && rm -f "$CLIENT_NAME.zip" && \
	  zip -qr "$CLIENT_NAME.zip" "$CLIENT_NAME" )
	( cd "$OUT/server" && rm -f "$SERVER_NAME.tar.gz" && \
	  tar -czf "$SERVER_NAME.tar.gz" "$SERVER_NAME" )
	echo "package-phase3: archive = $OUT/client/$CLIENT_NAME.zip"
	echo "package-phase3: archive = $OUT/server/$SERVER_NAME.tar.gz"
fi

echo "package-phase3: client = $CLIENT"
echo "package-phase3: server = $SERVER"
echo "P3.16 PACKAGES PASS"
