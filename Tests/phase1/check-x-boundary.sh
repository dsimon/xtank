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
# P1.12 adds:
#   --runtime   instrument the display-free server at link time (--wrap around
#               display_terminal, update_screen_locs, sync_terminals,
#               get_events and multi_sync, counted only while server_step() is
#               on the stack) and prove that advancing a whole match makes
#               ZERO render/event/sync calls, while the produced legacy dump is
#               byte-identical to the frozen X reference checksum.  A forced
#               presentation control run must show the counters firing, so the
#               instrumentation itself is proven live.
#
# Usage: bash Tests/phase1/check-x-boundary.sh --headers
#        bash Tests/phase1/check-x-boundary.sh --sources
#        bash Tests/phase1/check-x-boundary.sh --build
#        bash Tests/phase1/check-x-boundary.sh --runtime
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
	--runtime)
		;;
	*)
		echo "check-x-boundary.sh: unknown mode '$mode' (try --headers, --sources, --build or --runtime)" >&2
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

# ---------------------------------------------------------------------------
# --runtime: the display-free server must make no render/event/sync call while
# advancing a match, and must still produce the frozen legacy checksum.
# ---------------------------------------------------------------------------
EVIDENCE="${XTANK_P1_EVIDENCE:-/tmp/opencode/xtank-phase1/p1.12}"
RUNTIME_WORK="$EVIDENCE/runtime"
# Canonical frozen legacy v1 combat-2drone dump (PHASE1-PLAN.md, regression
# strategy).
RUNTIME_EXPECT_SHA="76f23a333a0907f9bb40fa4d9027feac39f3eccdc11aa85aa34fa33109688c6a"
RUNTIME_EXPECT_LINES=798
RUNTIME_SCEN="$ROOT/Tests/phase1/scenarios/combat-2drone.settings"

rt_fail() { echo "check-x-boundary: FAIL $*" >&2; exit 1; }

# Link-time instrumentation.  It counts a presentation call only while
# server_step() is on the stack, so terminal construction at startup (which
# legitimately polls events) cannot inflate the tick-path counters.
runtime_wrapper_src() {
	cat <<'WRAPPER'
#include <stdio.h>
#include <stdlib.h>
#include "xtank.h"
#include "graphics.h"
#include "server.h"
#include "proto.h"

static int in_tick;
static unsigned long n_display, n_locs, n_sync, n_events, n_multi, n_ticks;

void __real_display_terminal(unsigned int, int);
void __real_update_screen_locs(void);
void __real_sync_terminals(Boolean);
void __real_get_events(int *, Event *);
void __real_multi_sync(Video **, int, Boolean);
int  __real_server_step(void);

void __wrap_display_terminal(unsigned int status, int lastterm)
{
	if (in_tick) n_display++;
	__real_display_terminal(status, lastterm);
}

void __wrap_update_screen_locs(void)
{
	if (in_tick) n_locs++;
	__real_update_screen_locs();
}

void __wrap_sync_terminals(Boolean discard)
{
	if (in_tick) n_sync++;
	__real_sync_terminals(discard);
}

void __wrap_get_events(int *num, Event *ev)
{
	if (in_tick) n_events++;
	__real_get_events(num, ev);
}

void __wrap_multi_sync(Video **v, int n, Boolean discard)
{
	if (in_tick) n_multi++;
	__real_multi_sync(v, n, discard);
}

int __wrap_server_step(void)
{
	int r;

	in_tick = 1;
	n_ticks++;
	r = __real_server_step();
	in_tick = 0;
	return r;
}

__attribute__((destructor))
static void runtime_report(void)
{
	const char *p = getenv("XTANK_RUNTIME_REPORT");
	FILE *fp = stderr;

	if (p != NULL) {
		FILE *f = fopen(p, "w");

		if (f != NULL)
			fp = f;
	}
	fprintf(fp,
		"RUNTIME ticks=%lu display_terminal=%lu update_screen_locs=%lu "
		"sync_terminals=%lu get_events=%lu multi_sync=%lu\n",
		n_ticks, n_display, n_locs, n_sync, n_events, n_multi);
	fflush(fp);
	if (fp != stderr)
		fclose(fp);
}
WRAPPER
}

rt_counter() {   # report-file  key
	grep -o "$2=[0-9]*" "$1" | head -1 | cut -d= -f2
}

check_runtime() {
	local work="$RUNTIME_WORK"
	local src="$EVIDENCE/candidate-after-null/src"
	local obj="$EVIDENCE/candidate-after-null/obj"
	local wrap_c="$work/runtime_wrap.c"
	local wrap_o="$work/runtime_wrap.o"
	local probe="$work/xtank-runtime-probe"

	mkdir -p "$work"
	[ -f "$RUNTIME_SCEN" ] || rt_fail "missing scenario $RUNTIME_SCEN"

	# 1. Build the display-free (null) candidate from the working tree.
	bash "$ROOT/Tests/phase1/build-reference.sh" --mode candidate --subject null \
	    --stage after --evidence "$EVIDENCE" \
	    >"$work/build-candidate.out" 2>&1 || \
	    rt_fail "server candidate build failed (see $work/build-candidate.out)"
	[ -d "$obj" ] || rt_fail "missing candidate object dir $obj"

	# 2. Compile the instrumentation shim and relink the server objects with
	#    the presentation entry points wrapped.
	runtime_wrapper_src > "$wrap_c"
	"$CC" -O0 -g -w "${NULL_DEFS[@]}" -I"$SRC" -I"$INC" \
	    -c "$wrap_c" -o "$wrap_o" 2>"$work/wrap.err" || \
	    rt_fail "instrumentation shim failed to compile (see $work/wrap.err)"
	# shellcheck disable=SC2046
	gcc "$obj"/*.o "$wrap_o" -ldl -lm \
	    -Wl,--wrap=display_terminal,--wrap=update_screen_locs \
	    -Wl,--wrap=sync_terminals,--wrap=get_events,--wrap=multi_sync \
	    -Wl,--wrap=server_step \
	    -o "$probe" 2>"$work/link.err" || \
	    rt_fail "instrumented server failed to link (see $work/link.err)"
	[ -x "$probe" ] || rt_fail "instrumented server binary missing"

	# 3. Normal run: presentation natively off, a whole match.
	( cd "$src/Src" && \
	  env -u DISPLAY -u LD_PRELOAD LC_ALL=C XTANK_DIR="$src" \
	      XTANK_SEED=114336 XTANK_DUMP="$work/normal.state" \
	      XTANK_DUMP_PERIOD=100 XTANK_RUNTIME_REPORT="$work/normal.report" \
	      timeout 300 "$probe" -S -D -P -X -F "$RUNTIME_SCEN" ) \
	    >"$work/normal.stdout" 2>&1 || \
	    rt_fail "instrumented server run failed (see $work/normal.stdout)"

	[ -s "$work/normal.report" ] || rt_fail "no runtime report"
	local ticks k v
	ticks=$(rt_counter "$work/normal.report" ticks)
	[ -n "$ticks" ] && [ "$ticks" -gt 0 ] || rt_fail "no simulation ticks observed"
	for k in display_terminal update_screen_locs sync_terminals get_events multi_sync; do
		v=$(rt_counter "$work/normal.report" "$k")
		[ "$v" = "0" ] || \
		    rt_fail "server made $k calls during a match (expected 0; report: $(cat "$work/normal.report"))"
	done

	# 4. Identical checksum with no terminal pass.
	[ -s "$work/normal.state" ] || rt_fail "instrumented server produced no dump"
	local got_sha got_lines
	got_sha=$(sha256sum "$work/normal.state" | awk '{print $1}')
	got_lines=$(wc -l < "$work/normal.state")
	[ "$got_sha" = "$RUNTIME_EXPECT_SHA" ] || \
	    rt_fail "server dump sha256 $got_sha != frozen $RUNTIME_EXPECT_SHA"
	[ "$got_lines" = "$RUNTIME_EXPECT_LINES" ] || \
	    rt_fail "server dump lines $got_lines != frozen $RUNTIME_EXPECT_LINES"

	# 5. Control: force the presentation pass on and prove the counters fire,
	#    so the zero above is the gate at work, not dead instrumentation.
	( cd "$src/Src" && \
	  env -u DISPLAY -u LD_PRELOAD LC_ALL=C XTANK_DIR="$src" \
	      XTANK_SEED=114336 XTANK_SERVER_PRESENT=1 XTANK_TICK_LIMIT=50 \
	      XTANK_DUMP="$work/control.state" XTANK_DUMP_PERIOD=50 \
	      XTANK_RUNTIME_REPORT="$work/control.report" \
	      timeout 300 "$probe" -S -D -P -X -F "$RUNTIME_SCEN" ) \
	    >"$work/control.stdout" 2>&1 || \
	    rt_fail "control run failed (see $work/control.stdout)"
	local cdisp
	cdisp=$(rt_counter "$work/control.report" display_terminal)
	[ -n "$cdisp" ] && [ "$cdisp" -gt 0 ] || \
	    rt_fail "control run made no display_terminal calls; instrumentation is dead"

	echo "check-x-boundary: PASS (server made 0 render/event/sync calls over $ticks ticks;"
	echo "                    dump sha $got_sha == frozen; control display_terminal=$cdisp)"
}

case "$mode" in
  --headers) check_headers ;;
  --sources) check_sources ;;
  --build)   check_build ;;
  --runtime) check_runtime ;;
esac

exit 0
