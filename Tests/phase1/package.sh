#!/usr/bin/env bash
#
# package.sh -- P1.15 acceptance harness for the local server and its
# relocatable bundle.
#
# Contract (PHASE1-PLAN.md, P1.15):
#   make phase1-server && bash Tests/phase1/package.sh
#
# It builds the shipped terminal-free server, assembles the relocatable bundle
# (Bin/package-server.sh), relocates it to a path with spaces, and then proves:
#
#   * the bundle resolves its assets relative to the executable and runs both
#     supplied mazes with DISPLAY, XTANK_DIR and HOME unset, playing a match,
#     reporting results and resetting for a second match;
#   * the binary has no X library dependency, no undefined Xlib symbol and no
#     <X11/...> header in any server translation unit's dependency list;
#   * the process opens no X connection and makes no render/event/sync call,
#     and constructs no Terminal at all (link-time instrumentation);
#   * the server links no DYNALOAD (-ldl).
#
# The byte-level simulation guardrail (G) is separate and mandatory:
#   bash Tests/phase1/regression.sh --suite full --stage after --subject null
#
# Evidence lives OUTSIDE the checkout under
#   ${XTANK_P1_EVIDENCE:-/tmp/opencode/xtank-phase1/p1.15}/package
#
set -euo pipefail

export TAR_OPTIONS="${TAR_OPTIONS:---no-same-owner}"

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HARNESS="$REPO/Tests/phase1"
EVIDENCE="${XTANK_P1_EVIDENCE:-/tmp/opencode/xtank-phase1/p1.15}"
WORK="$EVIDENCE/package"
CC="${CC:-gcc}"
MAX_TICKS=100000

while [ $# -gt 0 ]; do
	case "$1" in
	  --evidence) EVIDENCE="$2"; WORK="$EVIDENCE/package"; shift 2 ;;
	  --max-ticks) MAX_TICKS="$2"; shift 2 ;;
	  *) echo "usage: package.sh [--evidence DIR] [--max-ticks N]" >&2; exit 2 ;;
	esac
done

rm -rf "$WORK"
mkdir -p "$WORK"

log()  { printf '%s\n' "$*"; }
fail() { printf 'PACKAGE FAIL: %s\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------------------
# 1. Build the shipped server
# ---------------------------------------------------------------------------
log "== make phase1-server"
( cd "$REPO" && make -s phase1-server ) > "$WORK/make.log" 2>&1 || \
    fail "make phase1-server failed (see $WORK/make.log)"
SERVER="$REPO/Src/xtank-server"
[ -x "$SERVER" ] || fail "server binary missing: $SERVER"

# ---------------------------------------------------------------------------
# 2. Assemble and relocate the bundle (path with spaces)
# ---------------------------------------------------------------------------
log "== assemble bundle"
bash "$REPO/Bin/package-server.sh" --src "$REPO" --binary "$SERVER" \
    --out "$WORK/bundle" --name xtank-server > "$WORK/package-server.log" 2>&1 || \
    fail "package-server.sh failed (see $WORK/package-server.log)"

SPACED="$WORK/a path with spaces"
mkdir -p "$SPACED"
cp -r "$WORK/bundle/xtank-server" "$SPACED/xtank-server"
BUNDLE="$SPACED/xtank-server"
QBIN="$BUNDLE/bin/xtank-server"
[ -x "$QBIN" ] || fail "relocated server binary missing: $QBIN"
[ -d "$BUNDLE/share/xtank/Mazes" ] || fail "relocated bundle has no Mazes"
log "   bundle=$BUNDLE"

# ---------------------------------------------------------------------------
# 3. Static audits: no X, no DYNALOAD
# ---------------------------------------------------------------------------
log "== link/dependency audit"
ldd "$QBIN" > "$WORK/ldd.txt" 2>&1 || true
if grep -qiE 'libX[0-9A-Za-z]*\.so|libxcb|libX11' "$WORK/ldd.txt"; then
	fail "server links an X library: $(grep -iE 'libX' "$WORK/ldd.txt")"
fi
if grep -qi 'libdl\.so' "$WORK/ldd.txt"; then
	fail "server links libdl (DYNALOAD)"
fi

if nm -u "$QBIN" 2>/dev/null | grep -qE 'X[A-Z][A-Za-z0-9_]*'; then
	fail "server has an undefined Xlib symbol: $(nm -u "$QBIN" | grep -E 'X[A-Z]' | head -1)"
fi
if nm "$QBIN" 2>/dev/null | grep -q ' U dlopen'; then
	fail "server references dlopen"
fi

# Every shipped server translation unit must be free of <X11/...> headers.
XHITS=$(grep -lE 'X11/|/x11\.h' "$REPO"/Src/sobj/*.d 2>/dev/null || true)
[ -z "$XHITS" ] || fail "server translation units depend on X headers: $XHITS"
NTU=$(ls "$REPO"/Src/sobj/*.d 2>/dev/null | wc -l)
log "   no libX, no libdl, no X symbol, no X header in $NTU server TUs"

# ---------------------------------------------------------------------------
# 4. Runtime audit: no X connection, no Terminal, no render/event/sync pass
# ---------------------------------------------------------------------------
log "== runtime audit"

# 4a. No X connection is opened (x11 socket or TCP display port).
if command -v strace >/dev/null 2>&1; then
	AUD="combat1"
	( cd "$BUNDLE/bin" && \
	  env -u DISPLAY -u XTANK_DIR -u HOME -u LD_PRELOAD LC_ALL=C \
	      strace -f -e trace=connect -o "$WORK/connect.trace" \
	      ./xtank-server --maze "$AUD" --seed 114336 --drones 2 --fast \
	      --matches 1 --max-ticks "$MAX_TICKS" ) > "$WORK/strace-run.out" 2>&1 || \
	    fail "strace server run failed (see $WORK/strace-run.out)"
	if grep -qE 'X11-unix|X99|:60[0-9][0-9]|/tmp/\.X' "$WORK/connect.trace"; then
		fail "server opened an X connection: $(grep -E 'X11-unix|/tmp/\.X|:60' "$WORK/connect.trace" | head -1)"
	fi
	log "   no X connection in connect(2) trace"
else
	log "   strace unavailable; skipping connect(2) trace"
fi

# 4b. Link-time instrumentation: run the real server objects with the
#     presentation entry points and server_step() wrapped, and require zero
#     display/event/sync calls and zero make_terminal() calls over a match.
WRAP_C="$WORK/runtime_wrap.c"
WRAP_O="$WORK/runtime_wrap.o"
PROBE="$WORK/xtank-server-probe"
cat > "$WRAP_C" <<'WRAPPER'
#include <stdio.h>
#include <stdlib.h>
#include "tanklimits.h"
#include "xtank.h"
#include "graphics.h"
#include "server.h"
#include "proto.h"

static int in_tick;
static unsigned long n_disp, n_locs, n_sync, n_events, n_multi, n_term, n_ticks;

void __real_display_terminal(unsigned int, int);
void __real_update_screen_locs(void);
void __real_sync_terminals(Boolean);
void __real_get_events(int *, Event *);
void __real_multi_sync(Video **, int, Boolean);
int  __real_make_terminal(char *);
int  __real_server_step(void);

void __wrap_display_terminal(unsigned int s, int l) { if (in_tick) n_disp++; __real_display_terminal(s, l); }
void __wrap_update_screen_locs(void) { if (in_tick) n_locs++; __real_update_screen_locs(); }
void __wrap_sync_terminals(Boolean d) { if (in_tick) n_sync++; __real_sync_terminals(d); }
void __wrap_get_events(int *n, Event *e) { if (in_tick) n_events++; __real_get_events(n, e); }
void __wrap_multi_sync(Video **v, int n, Boolean d) { if (in_tick) n_multi++; __real_multi_sync(v, n, d); }
int  __wrap_make_terminal(char *name) { n_term++; return __real_make_terminal(name); }

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
	fprintf(fp, "SERVER-RUNTIME ticks=%lu display_terminal=%lu "
		"update_screen_locs=%lu sync_terminals=%lu get_events=%lu "
		"multi_sync=%lu make_terminal=%lu\n",
		n_ticks, n_disp, n_locs, n_sync, n_events, n_multi, n_term);
	fflush(fp);
	if (fp != stderr)
		fclose(fp);
}
WRAPPER

SERVER_DEFS="-DUNIX -DS1024x864 -DKEYPAD_DETECT -DSTINGY_REDRAW \
-DBATCH_LINES -DBATCH_POINTS -DBATCH_COLOR_LINES \
-DCODE_SIZE=0x20000 -DSTACK_SIZE=0x8000 -DNEW_LIMITS -DSLOWER \
-DMULTI_SYNC -DTHREAD_SWAPCONTEXT -DNULL_BACKEND"
# shellcheck disable=SC2086
"$CC" -O0 -g -w $SERVER_DEFS -I"$REPO/Src" -I"$REPO/Src/Include" \
    -c "$WRAP_C" -o "$WRAP_O" \
    2> "$WORK/wrap.err" || fail "instrumentation shim failed to compile (see $WORK/wrap.err)"
# shellcheck disable=SC2046
"$CC" "$REPO"/Src/sobj/*.o "$WRAP_O" -lm \
    -Wl,--wrap=display_terminal,--wrap=update_screen_locs \
    -Wl,--wrap=sync_terminals,--wrap=get_events,--wrap=multi_sync \
    -Wl,--wrap=make_terminal,--wrap=server_step \
    -o "$PROBE" 2> "$WORK/probe-link.err" || \
    fail "instrumented server failed to link (see $WORK/probe-link.err)"

env -u DISPLAY -u XTANK_DIR -u HOME -u LD_PRELOAD LC_ALL=C \
    XTANK_RUNTIME_REPORT="$WORK/runtime.report" \
    "$PROBE" --assets "$BUNDLE/share/xtank" --maze combat1 --seed 114336 \
    --drones 2 --fast --matches 1 --max-ticks "$MAX_TICKS" \
    > "$WORK/probe-run.out" 2>&1 || \
    fail "instrumented server run failed (see $WORK/probe-run.out)"
[ -s "$WORK/runtime.report" ] || fail "no runtime report"
REPORT=$(cat "$WORK/runtime.report")
log "   $REPORT"
rval() { printf '%s\n' "$REPORT" | grep -o "$1=[0-9]*" | head -1 | cut -d= -f2; }
TICKS=$(rval ticks)
[ -n "$TICKS" ] && [ "$TICKS" -gt 0 ] || fail "no simulation ticks observed"
for k in display_terminal update_screen_locs sync_terminals get_events multi_sync make_terminal; do
	v=$(rval "$k")
	[ "$v" = "0" ] || fail "server made $k calls during a match (expected 0)"
done
log "   zero render/event/sync calls and zero Terminal constructions"

# ---------------------------------------------------------------------------
# 5. Relocated bundle: both mazes, match/results/reset, env unset
# ---------------------------------------------------------------------------
for maze in combat1 Arena; do
	log "== relocated run: $maze"
	OUT="$WORK/run-$maze.out"
	( cd "$BUNDLE/bin" && \
	  env -u DISPLAY -u XTANK_DIR -u HOME -u LD_PRELOAD \
	      -u XTANK_ASSET_ROOT -u XTANK_SERVER_ASSETS \
	      LC_ALL=C ./xtank-server --maze "$maze" --seed 114336 --drones 2 \
	      --fast --matches 2 --max-ticks "$MAX_TICKS" ) > "$OUT" 2>&1 || \
	    fail "$maze run failed (see $OUT)"
	grep -q '^SERVER maze=' "$OUT" || fail "$maze: no server banner"
	grep -q '^SERVER-MATCH 1 ' "$OUT" || fail "$maze: match 1 produced no result"
	grep -q '^SERVER-MATCH 2 ' "$OUT" || fail "$maze: no reset/second match result"
	grep -q '^SERVER-DONE matches=2$' "$OUT" || fail "$maze: no completion line"
	grep -q 'Current score:' "$OUT" || fail "$maze: no score results reported"
	grep -q '^SERVER-LOOP ' "$OUT" || fail "$maze: no pacing/work report"
	t=$(grep -o 'ticks=[0-9]*' "$OUT" | head -1 | cut -d= -f2)
	log "   $maze: matches=2 ticks=$t results reported"
done

log ""
log "P1.15 PACKAGE PASS (relocatable bundle, both mazes, match/results/reset, no X)"
