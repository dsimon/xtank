#!/usr/bin/env bash
#
# package-client.sh -- assemble the relocatable Windows client archive (P2.16).
#
# The archive places the cross-built front-end at
#
#     <bundle>/bin/xtank-client.exe
#     <bundle>/bin/SDL3.dll              (bundled runtime, D2-A)
#     <bundle>/assets/xtank-assets.bin   (portable visual bundle, P2.10)
#     <bundle>/assets/xtank-assets.manifest
#     <bundle>/fixtures/protocol/*.hex   (committed wire golden fixtures)
#     <bundle>/tools/windows-smoke.ps1   (native Windows runtime smoke, P2.9)
#     <bundle>/README.txt                (launch instructions)
#     <bundle>/BUILD-INFO
#     <bundle>/COPYRIGHT, COPYRIGHT.notes
#     <bundle>/licenses/{SDL3-LICENSE,ENet-LICENSE}
#     <bundle>/THIRD-PARTY-NOTICES.txt
#
# A clean Windows 11 x64 machine unpacks the archive and runs
# `windows-smoke.ps1` without a compiler, X server, Cygwin or other POSIX
# runtime: the exe imports only Win32 system DLLs plus the bundled SDL3.dll
# (no libgcc/libwinpthread/Cygwin/MSYS).  Nothing is signed; nothing is
# installed.
#
# The executable can be supplied (--binary) or cross-built here (--build) with
# the pinned MinGW-w64 toolchain and SDL3 devel package (P2.1/P2.9).  If no
# asset bundle is supplied (--assets), the host exporter
# (Tools/export-client-assets.c) builds one from the read-only source art.
#
# Usage:
#   bash Bin/package-client.sh [--src ROOT] [--binary EXE] [--dll SDL3.dll]
#       [--assets FILE] [--fixtures DIR] [--smoke FILE]
#       [--build] [--mingw-sdl-prefix DIR] [--enet-dir DIR]
#       [--sdl3-license FILE] [--enet-license FILE]
#       [--out DIR] [--name NAME] [--zip]
#
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"

SRC="$REPO"
BINARY=""
DLL=""
ASSETS=""
FIXTURES="$REPO/Tests/phase2/fixtures/protocol"
SMOKE="$REPO/Tests/phase2/windows-smoke.ps1"
OUT=""
NAME="xtank-client"
MAKE_ZIP=0
BUILD=0
MINGW_SDL_PREFIX="${XTK_SDL3_MINGW_PREFIX:-}"
ENET_DIR="${XTANK_ENET_DIR:-}"
SDL3_LICENSE=""
ENET_LICENSE=""

usage() {
	cat >&2 <<'EOF'
usage: package-client.sh [--src ROOT] [--binary EXE] [--dll SDL3.dll]
                         [--assets FILE] [--fixtures DIR] [--smoke FILE]
                         [--build] [--mingw-sdl-prefix DIR] [--enet-dir DIR]
                         [--sdl3-license FILE] [--enet-license FILE]
                         [--out DIR] [--name NAME] [--zip]

  --build             cross-build bin/xtank-client.exe with `make phase2-client-win`
                      (requires --mingw-sdl-prefix / XTK_SDL3_MINGW_PREFIX and
                      --enet-dir / XTANK_ENET_DIR)
  --assets FILE       use FILE as the visual bundle; otherwise one is built
  --zip               also emit <name>.zip next to the bundle directory
EOF
	exit 2
}

while [ $# -gt 0 ]; do
	case "$1" in
	  --src)              SRC="$2"; shift 2 ;;
	  --binary)           BINARY="$2"; shift 2 ;;
	  --dll)              DLL="$2"; shift 2 ;;
	  --assets)           ASSETS="$2"; shift 2 ;;
	  --fixtures)         FIXTURES="$2"; shift 2 ;;
	  --smoke)            SMOKE="$2"; shift 2 ;;
	  --build)            BUILD=1; shift ;;
	  --mingw-sdl-prefix) MINGW_SDL_PREFIX="$2"; shift 2 ;;
	  --enet-dir)         ENET_DIR="$2"; shift 2 ;;
	  --sdl3-license)     SDL3_LICENSE="$2"; shift 2 ;;
	  --enet-license)     ENET_LICENSE="$2"; shift 2 ;;
	  --out)              OUT="$2"; shift 2 ;;
	  --name)             NAME="$2"; shift 2 ;;
	  --zip)              MAKE_ZIP=1; shift ;;
	  *) usage ;;
	esac
done

[ -d "$SRC" ] || { echo "package-client: no such source root: $SRC" >&2; exit 2; }
[ -n "$OUT" ] || OUT="$SRC/xtank-client-dist"

# ---------------------------------------------------------------------------
# Optionally cross-build the Windows executable (D2-D).
# ---------------------------------------------------------------------------
if [ "$BUILD" = "1" ] && [ -z "$BINARY" ]; then
	[ -n "$MINGW_SDL_PREFIX" ] || {
		echo "package-client: --build requires --mingw-sdl-prefix (or XTK_SDL3_MINGW_PREFIX)" >&2
		exit 2
	}
	[ -n "$ENET_DIR" ] || {
		echo "package-client: --build requires --enet-dir (or XTANK_ENET_DIR)" >&2
		exit 2
	}
	BUILDROOT="$OUT/build/win"
	echo "package-client: cross-building xtank-client.exe"
	( cd "$SRC" && make -s phase2-client-win \
	    XTK_CLIENT_BUILD="$OUT/build" \
	    XTK_SDL3_MINGW_PREFIX="$MINGW_SDL_PREFIX" \
	    XTK_ENET_DIR="$ENET_DIR" ) || {
		echo "package-client: phase2-client-win failed" >&2
		exit 2
	}
	BINARY="$BUILDROOT/Client/xtank-client.exe"
fi

if [ -z "$BINARY" ]; then
	for cand in \
	    "$SRC/build/root/win/Client/xtank-client.exe" \
	    "${XTK_CLIENT_BUILD:-/nonexistent}/win/Client/xtank-client.exe"; do
		[ -x "$cand" ] && { BINARY="$cand"; break; }
	done
fi
[ -n "$BINARY" ] && [ -f "$BINARY" ] || {
	echo "package-client: client executable not found (use --binary or --build)" >&2
	exit 2
}

# ---------------------------------------------------------------------------
# Resolve the bundled SDL3 runtime DLL.
# ---------------------------------------------------------------------------
if [ -z "$DLL" ]; then
	exedir="$(cd "$(dirname "$BINARY")" && pwd)"
	if [ -f "$exedir/SDL3.dll" ]; then
		DLL="$exedir/SDL3.dll"
	elif [ -n "$MINGW_SDL_PREFIX" ] && [ -f "$MINGW_SDL_PREFIX/bin/SDL3.dll" ]; then
		DLL="$MINGW_SDL_PREFIX/bin/SDL3.dll"
	fi
fi
[ -n "$DLL" ] && [ -f "$DLL" ] || {
	echo "package-client: SDL3.dll not found (use --dll)" >&2
	exit 2
}

# ---------------------------------------------------------------------------
# Build the visual asset bundle if one was not supplied.
# ---------------------------------------------------------------------------
if [ -z "$ASSETS" ]; then
	WORK="$OUT/build/assets"
	mkdir -p "$WORK"
	echo "package-client: building the portable asset bundle"
	CC="${CC:-gcc}"
	# shellcheck disable=SC2086
	$CC -O0 -g -w -I "$SRC/Src/Include" -I "$SRC/Src/Display" \
	    -I "$SRC/Protocol" -I "$SRC/Client" \
	    "$SRC/Tools/export-client-assets.c" "$SRC/Client/assets.c" \
	    -o "$WORK/export-client-assets" -lm || {
		echo "package-client: asset exporter build failed" >&2
		exit 2
	}
	ASSETS="$WORK/xtank-assets.bin"
	GEOM="$WORK/xtank-assets.geom"
	MANIFEST="$WORK/xtank-assets.manifest"
	"$WORK/export-client-assets" --out "$ASSETS" --geometry "$GEOM" \
	    --manifest "$MANIFEST" || {
		echo "package-client: asset export failed" >&2
		exit 2
	}
else
	MANIFEST=""
fi
[ -s "$ASSETS" ] || { echo "package-client: asset bundle is empty: $ASSETS" >&2; exit 2; }

# ---------------------------------------------------------------------------
# Assemble the bundle.
# ---------------------------------------------------------------------------
ROOT="$OUT/$NAME"
rm -rf "$ROOT"
mkdir -p "$ROOT/bin" "$ROOT/assets" "$ROOT/fixtures/protocol" \
	"$ROOT/tools" "$ROOT/licenses"

install -m 0755 "$BINARY" "$ROOT/bin/xtank-client.exe"
install -m 0755 "$DLL" "$ROOT/bin/SDL3.dll"
install -m 0644 "$ASSETS" "$ROOT/assets/xtank-assets.bin"
if [ -n "$MANIFEST" ] && [ -s "$MANIFEST" ]; then
	install -m 0644 "$MANIFEST" "$ROOT/assets/xtank-assets.manifest"
fi

if [ -d "$FIXTURES" ]; then
	cp "$FIXTURES"/*.hex "$ROOT/fixtures/protocol/" 2>/dev/null || true
	[ -f "$FIXTURES/README.md" ] && \
	    install -m 0644 "$FIXTURES/README.md" "$ROOT/fixtures/protocol/README.md" || true
fi
[ -f "$SMOKE" ] && install -m 0644 "$SMOKE" "$ROOT/tools/windows-smoke.ps1"

for notice in COPYRIGHT COPYRIGHT.notes; do
	[ -f "$SRC/$notice" ] && install -m 0644 "$SRC/$notice" "$ROOT/$notice" || true
done
[ -n "$SDL3_LICENSE" ] && [ -f "$SDL3_LICENSE" ] && \
	install -m 0644 "$SDL3_LICENSE" "$ROOT/licenses/SDL3-LICENSE"
[ -n "$ENET_LICENSE" ] && [ -f "$ENET_LICENSE" ] && \
	install -m 0644 "$ENET_LICENSE" "$ROOT/licenses/ENet-LICENSE"

cat > "$ROOT/THIRD-PARTY-NOTICES.txt" <<'EOF'
Xtank Windows client -- bundled third-party components

SDL3 (https://libsdl.org)          zlib license.  Bundled as bin/SDL3.dll;
                                   license in licenses/SDL3-LICENSE.
ENet (https://enet.bespin.org)     MIT license.  Statically linked into
                                   bin/xtank-client.exe; license in
                                   licenses/ENet-LICENSE.

The original Xtank source and content are distributed under the terms in
COPYRIGHT and COPYRIGHT.notes.
EOF

cat > "$ROOT/README.txt" <<'EOF'
Xtank Windows client (Phase 2 MVP 0.1.0)
=========================================

Requirements: a 64-bit Windows 11 machine.  No compiler, X server, Cygwin or
other POSIX runtime is required; SDL3.dll is bundled next to the executable.

Unpack this directory anywhere (paths with spaces are fine).  The archive is
self-contained.

  1. Runtime self-test (recommended first run).  In PowerShell:

         .\tools\windows-smoke.ps1 -Exe .\bin\xtank-client.exe `
                                   -Fixtures .\fixtures\protocol

     Expect "WINDOWS-SMOKE PASS".  This audits the PE imports, opens a window,
     exercises keyboard/mouse and DPI handling, decodes the wire fixtures and
     runs an ENet loopback handshake.

  2. Play against a Linux server (start it first; see the server package):

         .\bin\xtank-client.exe --play HOST:PORT --assets .\assets\xtank-assets.bin

     with HOST:PORT the address the server reports (e.g. 192.168.1.20:7777).
     Use --connect HOST:PORT to run only the handshake check.

The client owns presentation and controls only; the Linux server is the sole
authority for movement, collisions, damage, RNG, bots and scoring.  Gameplay
packets are the frozen protocol v1 (docs/PROTOCOL-v1.md).
EOF

{
	echo "name=$NAME"
	echo "binary=bin/xtank-client.exe"
	echo "binary_sha256=$(sha256sum "$ROOT/bin/xtank-client.exe" | awk '{print $1}')"
	echo "sdl3_dll=bin/SDL3.dll"
	echo "sdl3_dll_sha256=$(sha256sum "$ROOT/bin/SDL3.dll" | awk '{print $1}')"
	echo "asset_bundle=assets/xtank-assets.bin"
	echo "asset_sha256=$(sha256sum "$ROOT/assets/xtank-assets.bin" | awk '{print $1}')"
	echo "fixtures=$(find "$ROOT/fixtures/protocol" -name '*.hex' | wc -l)"
	echo "platform=windows-x86_64"
} > "$ROOT/BUILD-INFO"

if [ "$MAKE_ZIP" = "1" ]; then
	( cd "$OUT" && rm -f "$NAME.zip" && zip -qr "$NAME.zip" "$NAME" )
	echo "package-client: $OUT/$NAME.zip"
fi

echo "package-client: $ROOT"
