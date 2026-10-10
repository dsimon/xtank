#!/bin/bash
#
# check-x-boundary.sh -- guard the simulation/bot <-> presentation boundary.
#
# P1.3 implements the --headers mode: preprocess representative physics and
# bot headers (plus the neutral boundary headers they transitively include)
# and fail if any of them pull in <X11/...> or expose an X resource type.
#
# Later tasks add modes that inspect the whole source set and the compiler
# dependency files:
#   --sources   X calls/types only in backend-private translation units (P1.4)
#   --build     no X header in any server translation unit's deps        (P1.5)
#
# Usage: bash Tests/phase1/check-x-boundary.sh --headers
# Exit:  0 clean, 1 boundary violation, 2 usage/unsupported mode.

set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SRC="$ROOT/Src"
INC="$SRC/Include"
CC="${CC:-gcc}"

# Same feature defines as the supported build (Src/Makefile.inc1).
DEFS=(
	-DX11 -DUNIX -DS1024x864 -DKEYPAD_DETECT -DSTINGY_REDRAW
	-DBATCH_LINES -DBATCH_POINTS -DBATCH_COLOR_LINES
	-DCODE_SIZE=0x20000 -DSTACK_SIZE=0x8000
	-DNEW_LIMITS -DSLOWER
	-DDYNALOAD -DMULTI_SYNC -DTHREAD_SWAPCONTEXT
)

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

# X resource types that must never leak into simulation/bot headers.
X_TOKEN_RE='Display|Window|Pixmap|GC|XFontStruct|XPoint|XSegment|Cursor|XErrorEvent|KeySym|XImage|XEvent|Colormap|XColor|XrmName|XrmClass|XrmRepresentation|XrmValue|XrmDatabase|XrmQuark|XGCValues|XCharStruct|XSizeHints|XWMHints|XClassHint|XFontSet'

mode="${1:---headers}"

case "$mode" in
	-h|--help)
		sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'
		exit 0
		;;
	--headers)
		;;
	--sources|--build)
		echo "check-x-boundary.sh: mode '$mode' is not implemented yet (P1.4/P1.5)" >&2
		exit 2
		;;
	*)
		echo "check-x-boundary.sh: unknown mode '$mode' (try --headers)" >&2
		exit 2
		;;
esac

if ! command -v "$CC" >/dev/null 2>&1; then
	echo "check-x-boundary.sh: C compiler '$CC' not found" >&2
	exit 2
fi

fail=0
scanned=0

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
exit 0
