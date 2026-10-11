#!/usr/bin/env bash
#
# package-server.sh -- assemble the relocatable local-server bundle (P1.15).
#
# The bundle places the terminal-free server at
#
#     <bundle>/bin/xtank-server
#
# with all required assets and notices relative to the executable:
#
#     <bundle>/share/xtank/Mazes/*.m
#     <bundle>/share/xtank/Vehicles/*.V
#     <bundle>/COPYRIGHT
#     <bundle>/COPYRIGHT.notes
#
# Src/server_main.c resolves the asset root relative to /proc/self/exe
# (bin/../share/xtank), so a bundle moved to any path -- including one with
# spaces -- runs with DISPLAY, XTANK_DIR and HOME unset.  Only the binary, the
# two supplied mazes used by the acceptance run, and every supplied
# maze/vehicle are copied; no build tree, object, editor, X front-end or
# reference worktree is included.
#
# Usage:
#   bash Bin/package-server.sh [--src ROOT] [--binary PATH] [--out DIR]
#                               [--name NAME] [--tar]
#
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"

SRC="$REPO"
BINARY=""
OUT=""
NAME="xtank-server"
MAKE_TAR=0
NET=0
ENET_DIR="${XTANK_ENET_DIR:-}"

usage() {
	cat >&2 <<'EOF'
usage: package-server.sh [--src ROOT] [--binary PATH] [--out DIR]
                         [--name NAME] [--net] [--enet-dir DIR] [--tar]

  --net          build and package the P2.8 network server
                 (Src/xtank-server-net, requires the pinned ENet source via
                 --enet-dir or XTANK_ENET_DIR) as bin/xtank-server
EOF
	exit 2
}

while [ $# -gt 0 ]; do
	case "$1" in
	  --src)      SRC="$2"; shift 2 ;;
	  --binary)   BINARY="$2"; shift 2 ;;
	  --out)      OUT="$2"; shift 2 ;;
	  --name)     NAME="$2"; shift 2 ;;
	  --net)      NET=1; shift ;;
	  --enet-dir) ENET_DIR="$2"; shift 2 ;;
	  --tar)      MAKE_TAR=1; shift ;;
	  *) usage ;;
	esac
done

[ -d "$SRC" ] || { echo "package-server: no such source root: $SRC" >&2; exit 2; }
if [ "$NET" = "1" ]; then
	[ -n "$ENET_DIR" ] || {
		echo "package-server: --net requires --enet-dir (or XTANK_ENET_DIR)" >&2
		exit 2
	}
	( cd "$SRC" && make -s phase1-server-net ENET_DIR="$ENET_DIR" ) || {
		echo "package-server: network server build failed" >&2
		exit 2
	}
	BINARY="$SRC/Src/xtank-server-net"
fi
[ -n "$BINARY" ] || BINARY="$SRC/Src/xtank-server"
[ -n "$OUT" ] || OUT="$SRC/xtank-server-bundle"

[ -x "$BINARY" ] || { echo "package-server: server binary missing: $BINARY" >&2; exit 2; }
[ -d "$SRC/Mazes" ] || { echo "package-server: missing $SRC/Mazes" >&2; exit 2; }
[ -d "$SRC/Vehicles" ] || { echo "package-server: missing $SRC/Vehicles" >&2; exit 2; }

ROOT="$OUT/$NAME"
rm -rf "$ROOT"
mkdir -p "$ROOT/bin" "$ROOT/share/xtank/Mazes" "$ROOT/share/xtank/Vehicles"

install -m 0755 "$BINARY" "$ROOT/bin/xtank-server"

cp -r "$SRC/Mazes/." "$ROOT/share/xtank/Mazes/"
cp -r "$SRC/Vehicles/." "$ROOT/share/xtank/Vehicles/"

for notice in COPYRIGHT COPYRIGHT.notes; do
	if [ -f "$SRC/$notice" ]; then
		cp -r "$SRC/$notice" "$ROOT/$notice"
	fi
done

{
	echo "name=$NAME"
	echo "binary=bin/xtank-server"
	echo "binary_sha256=$(sha256sum "$ROOT/bin/xtank-server" | awk '{print $1}')"
	echo "asset_root=share/xtank"
	echo "mazes=$(find "$ROOT/share/xtank/Mazes" -type f | wc -l)"
	echo "vehicles=$(find "$ROOT/share/xtank/Vehicles" -type f | wc -l)"
} > "$ROOT/BUILD-INFO"

if [ "$MAKE_TAR" = "1" ]; then
	( cd "$OUT" && tar -czf "$NAME.tar.gz" "$NAME" )
	echo "package-server: $OUT/$NAME.tar.gz"
fi

echo "package-server: $ROOT"
