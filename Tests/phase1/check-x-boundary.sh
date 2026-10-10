#!/bin/bash
#
# check-x-boundary.sh -- guard the simulation/bot <-> presentation boundary.
#
# P1.3 implements the --headers mode: preprocess representative physics and
# bot headers (plus the neutral boundary headers they transitively include)
# and fail if any of them pull in <X11/...> or expose an X resource type.
#
# P1.4 adds the --sources mode: preprocess every C source/header in Src and
# fail if any non-backend-private file references an Xlib call or type (or
# pulls in <X11/...>).  Backend-private presentation files are the only place
# Xlib may appear.
#
# P1.5 adds:
#   --build     no X header in any server translation unit's deps.
#               The explicit server source list comes from the build
#               (Src/GNUmakefile: print-server-srcs); each server TU is asked
#               for its compiler dependency list (-M) with the exact server
#               flags (no -DX11, -DNULL_BACKEND) and the deps must contain no
#               <X11/...> header and no backend-private Include/x11.h.
#
# Usage: bash Tests/phase1/check-x-boundary.sh --headers
#        bash Tests/phase1/check-x-boundary.sh --sources
#        bash Tests/phase1/check-x-boundary.sh --build
# Exit:  0 clean, 1 boundary violation, 2 usage/unsupported mode.

set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SRC="$ROOT/Src"
INC="$SRC/Include"
CC="${CC:-gcc}"

# Same feature defines as the supported build (Src/Makefile.inc1).
COMMON_DEFS=(
	-DUNIX -DS1024x864 -DKEYPAD_DETECT -DSTINGY_REDRAW
	-DBATCH_LINES -DBATCH_POINTS -DBATCH_COLOR_LINES
	-DCODE_SIZE=0x20000 -DSTACK_SIZE=0x8000
	-DNEW_LIMITS -DSLOWER
	-DDYNALOAD -DMULTI_SYNC -DTHREAD_SWAPCONTEXT
)
DEFS=( -DX11 "${COMMON_DEFS[@]}" )
# Server build selects the null backend and no window-system define (P1.5).
NULL_DEFS=( "${COMMON_DEFS[@]}" -DNULL_BACKEND )

# Representative physics/bot headers, plus the neutral boundary headers that
# the simulation and bot translation units include transitively.
HEADERS=(
	tanktypes.h
	vehicle.h
	bullet.h
	object.h
	loc.h
	vstructs.h
	maze.h
	lowlib.h
	program.h
	special.h
	graphics_backend.h
	graphics.h
	proto.h
)

# X resource types that must never leak into simulation/bot code.
X_TOKEN_RE='Display|Window|Pixmap|GC|XFontStruct|XPoint|XSegment|Cursor|XErrorEvent|KeySym|XImage|XEvent|Colormap|XColor|XrmName|XrmClass|XrmRepresentation|XrmValue|XrmDatabase|XrmQuark|XGCValues|XCharStruct|XSizeHints|XWMHints|XClassHint|XFontSet'

# An Xlib function call, e.g. XDrawLine( or XOpenDisplay(.
X_CALL_RE='X[A-Z][A-Za-z0-9_]*[[:space:]]*\('

mode="${1:---headers}"

case "$mode" in
	-h|--help)
		sed -n '2,18p' "$0" | sed 's/^# \{0,1\}//'
		exit 0
		;;
	--headers)
		;;
	--sources)
		;;
	--build)
		;;
	*)
		echo "check-x-boundary.sh: unknown mode '$mode' (try --headers, --sources or --build)" >&2
		exit 2
		;;
esac

if ! command -v "$CC" >/dev/null 2>&1; then
	echo "check-x-boundary.sh: C compiler '$CC' not found" >&2
	exit 2
fi

fail=0
scanned=0

# ---------------------------------------------------------------------------
# --headers: preprocess representative physics/bot headers.
# ---------------------------------------------------------------------------
check_headers() {
	local h hdr out x11_lines x_tokens
	for h in "${HEADERS[@]}"; do
		hdr="$INC/$h"
		if [ ! -f "$hdr" ]; then
			echo "check-x-boundary: MISSING representative header $hdr" >&2
			fail=1
			continue
		fi
		scanned=$((scanned + 1))

		out="$("$CC" -E "${DEFS[@]}" -I"$SRC" -I"$INC" "$hdr" 2>/dev/null)"
		if [ $? -ne 0 ]; then
			echo "check-x-boundary: FAIL $h (preprocessor error)" >&2
			fail=1
			continue
		fi

		# 1) Transitive <X11/...> includes appear as preprocessor line markers.
		x11_lines="$(printf '%s\n' "$out" | grep -nE '(^|[ "])(<)?X11/' || true)"
		# 2) X resource types in the preprocessed token stream.  Drop string
		#    literals first so user-visible strings cannot cause false positives.
		x_tokens="$(printf '%s\n' "$out" | sed 's/"[^"]*"//g' \
			| grep -owE "$X_TOKEN_RE" | sort -u | tr '\n' ' ')"

		if [ -n "$x11_lines" ] || [ -n "$x_tokens" ]; then
			fail=1
			echo "check-x-boundary: FAIL $h" >&2
			if [ -n "$x11_lines" ]; then
				echo "  transitive X11 include(s):" >&2
				printf '%s\n' "$x11_lines" | head -5 | sed 's/^/    /' >&2
			fi
			if [ -n "$x_tokens" ]; then
				echo "  X resource type(s): $x_tokens" >&2
			fi
		else
			echo "check-x-boundary: ok   $h"
		fi
	done

	if [ "$fail" -ne 0 ]; then
		echo "check-x-boundary: FAILED (X leaked into simulation/bot headers)" >&2
		exit 1
	fi
	echo "check-x-boundary: PASS ($scanned headers X-free)"
}

# ---------------------------------------------------------------------------
# --sources: every non-backend-private Src .c/.h must be free of Xlib.
# ---------------------------------------------------------------------------
# The display backend translation unit (graphics.c aggregates x11.c) and the
# backend-private files are the only places where Xlib calls/types may appear.
is_backend_private() {
	case "$1" in
	  "$SRC/Display/graphics.c" \
	| "$SRC/Display/x11.c" \
	| "$SRC/Display/x11.h" \
	| "$SRC/Display/XMultiSync.c" \
	| "$SRC/Display/x10.c" \
	| "$SRC/Display/amigagfx.c" \
	| "$SRC/Include/x11.h")
		return 0 ;;
	esac
	return 1
}

check_sources() {
	local f base out clean x11 hits calls types
	# Enumerate the source set deterministically; skip generated flex/version
	# files that may be present from a local build.
	while IFS= read -r f; do
		base="$(basename "$f")"
		case "$base" in
		  setups.c|lex.zz.c|version.c) continue ;;
		esac
		if is_backend_private "$f"; then
			continue
		fi
		scanned=$((scanned + 1))

		out="$("$CC" -E "${DEFS[@]}" -I"$SRC" -I"$INC" "$f" 2>/dev/null)"
		if [ $? -ne 0 ]; then
			fail=1
			echo "check-x-boundary: FAIL $f (preprocessor error)" >&2
			continue
		fi
		# Drop string literals so user-visible text cannot mask/trigger.
		clean="$(printf '%s\n' "$out" | sed 's/"[^"]*"//g')"

		x11="$(printf '%s\n' "$clean" | grep -nE 'X11/' | head -3 || true)"
		calls="$(printf '%s\n' "$clean" | grep -nE "$X_CALL_RE" | head -3 || true)"
		types="$(printf '%s\n' "$clean" | grep -nwE "$X_TOKEN_RE" | head -3 || true)"

		if [ -n "$x11$calls$types" ]; then
			fail=1
			echo "check-x-boundary: FAIL $f" >&2
			[ -n "$x11" ]   && { echo "  X11 header(s):" >&2; printf '%s\n' "$x11"   | sed 's/^/    /' >&2; }
			[ -n "$calls" ] && { echo "  Xlib call(s):" >&2;  printf '%s\n' "$calls" | sed 's/^/    /' >&2; }
			[ -n "$types" ] && { echo "  X resource type(s):" >&2; printf '%s\n' "$types" | sed 's/^/    /' >&2; }
		fi
	done < <(find "$SRC" -type f \( -name '*.c' -o -name '*.h' \) | LC_ALL=C sort)
	if [ "$fail" -ne 0 ]; then
		echo "check-x-boundary: FAILED (Xlib outside backend-private files)" >&2
		exit 1
	fi
	echo "check-x-boundary: PASS ($scanned non-backend sources X-free)"
}

# ---------------------------------------------------------------------------
# --build: no X header in any server translation unit's compiler deps.
# ---------------------------------------------------------------------------
# Fallback server source list (basenames) used if the build system cannot be
# queried; kept in sync with Src/Makefile.inc1:SERVER_SRCS.
FALLBACK_SERVER_SRCS="actions.c animate.c box.c camo.c collision.c cosell.c \
disc.c display.c escher.c explosion.c file.c game.c gr.c highlib.c hit.c \
hud.c icounter.c init.c input.c interface.c intersect.c lowlib.c main.c \
mapper.c maze.c mdesign.c menu.c message.c newconsole.c newfile.c newradar.c \
players.c program.c radar.c repair.c scroll.c setup.c setups.c status.c \
sysdep.c unix.c update.c util.c vdesign.c vehicle.c version.c lex.zz.c \
graphics.c objects.c three-d.c threadglue.c thread.c Bootlegger.c Dio.c \
Diophantine.c Flipper.c drone.c eliza.c gnat.c mmtf.c rdfbot.c spot.c tagman.c"

resolve_src() {   # basename -> path under Src, returning nonzero if absent
	local f="$1"
	local d
	for d in "$SRC" "$SRC/Display" "$SRC/Programs" "$SRC/Threads"; do
		if [ -f "$d/$f" ]; then
			printf '%s\n' "$d/$f"
			return 0
		fi
	done
	return 1
}

check_build() {
	local list depsdir
	list=""
	if command -v make >/dev/null 2>&1; then
		list="$(make -C "$SRC" -s print-server-srcs 2>/dev/null || true)"
	fi
	if [ -z "$list" ]; then
		echo "check-x-boundary: note: using fallback server source list" >&2
		list="$FALLBACK_SERVER_SRCS"
	fi

	local f path dep hits

	# Write dependency files to a private scratch dir: never clobber the
	# build's own nobj/*.d (which drive make's incremental rebuilds).
	depsdir="$(mktemp -d "${TMPDIR:-/tmp}/xtank-xbound.XXXXXX")"
	trap "rm -rf '$depsdir'" EXIT
	for f in $list; do
		if ! path="$(resolve_src "$f")"; then
			fail=1
			echo "check-x-boundary: FAIL missing server source $f" >&2
			continue
		fi
		scanned=$((scanned + 1))
		dep="$depsdir/${f%.c}.d"

		# Generate the compiler dependency file (system headers included, so
		# <X11/...> is visible), with the exact server flags.
		if ! "$CC" "${NULL_DEFS[@]}" -I"$SRC" -I"$INC" \
				-M -MF "$dep" -MT "${f%.c}.o" "$path" 2>/dev/null; then
			fail=1
			echo "check-x-boundary: FAIL $f (dependency generation error)" >&2
			continue
		fi

		hits="$(grep -nE '(^|[ /])X11/|/x11\.h([ :]|$)' "$dep" | head -3 || true)"
		if [ -n "$hits" ]; then
			fail=1
			echo "check-x-boundary: FAIL $f (X header in server TU deps)" >&2
			printf '%s\n' "$hits" | sed 's/^/    /' >&2
		fi
	done

	if [ "$fail" -ne 0 ]; then
		echo "check-x-boundary: FAILED (server TUs depend on X headers)" >&2
		exit 1
	fi
	echo "check-x-boundary: PASS ($scanned server translation units X-free)"
}

case "$mode" in
  --headers) check_headers ;;
  --sources) check_sources ;;
  --build)   check_build ;;
esac

exit 0
