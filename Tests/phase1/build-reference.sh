#!/usr/bin/env bash
#
# build-reference.sh -- build the frozen X reference and the 15 Hz scratch
# candidate for the Phase 1 regression guardrail (P1.1).
#
# The pinned reference source tree, its object tree, the binaries and all
# evidence live OUTSIDE the checkout (default /tmp/opencode/xtank-phase1/p1.1)
# so the branch stays clean.  The reference is the pinned baseline overlaid
# with the reviewed read-only STATE_DUMP hook only; the candidate is the
# current working tree.  No gameplay rule is changed.
#
# Two subject flavors are supported for the candidate:
#   x         real Xlib, run under Xvfb (the P1.1 subject)
#   headless  scratch shadow-Xlib build (TEMPORARY bootstrap, P1.3/P1.5 replace)
#
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HARNESS="$REPO/Tests/phase1"
SCRATCH="$HARNESS/scratch"
EVIDENCE="${XTANK_P1_EVIDENCE:-/tmp/opencode/xtank-phase1/p1.1}"

MODE=""
SUBJECT="x"
STAGE="after"
SRC_OVERRIDE=""

usage() {
	echo "usage: $0 --mode reference|candidate [--subject x|headless]" \
	     "[--stage before|after] [--evidence DIR] [--src DIR]" >&2
	exit 2
}

while [ $# -gt 0 ]; do
	case "$1" in
	  --mode) MODE="$2"; shift 2 ;;
	  --subject) SUBJECT="$2"; shift 2 ;;
	  --stage) STAGE="$2"; shift 2 ;;
	  --evidence) EVIDENCE="$2"; shift 2 ;;
	  --src) SRC_OVERRIDE="$2"; shift 2 ;;
	  *) usage ;;
	esac
done
[ -n "$MODE" ] || usage

mkdir -p "$EVIDENCE"

# ---------------------------------------------------------------------------
# Source snapshots (outside the checkout)
# ---------------------------------------------------------------------------
snapshot_reference() {          # pin + reviewed STATE_DUMP hook
	local dest="$1"
	rm -rf "$dest"; mkdir -p "$dest"
	git -C "$REPO" archive HEAD | tar -x -C "$dest"
	# Overlay the reviewed read-only instrumentation from the working tree.
	cp "$REPO/Src/animate.c" "$dest/Src/animate.c"
}

snapshot_candidate() {          # current working-tree content, incl. new sources
	local dest="$1"
	rm -rf "$dest"; mkdir -p "$dest"
	git -C "$REPO" ls-files --cached --others --exclude-standard -z \
	    | tar -C "$REPO" --null -T - -cf - | tar -x -C "$dest"
}

# ---------------------------------------------------------------------------
# Generated sources
# ---------------------------------------------------------------------------
gen_aux() {
	local src="$1/Src"
	[ -f "$src/setups.c" ] || (cd "$src" && flex -t setups.l > setups.c)
	[ -f "$src/lex.zz.c" ] || (cd "$src" && flex -Pzz vload.l)
	[ -f "$src/version.c" ] || (cd "$src" && sh ../Bin/newversion.sh)
}

# ---------------------------------------------------------------------------
# Canonical flag set (mirrors Src/Makefile.inc1) plus the oracle define.
# ---------------------------------------------------------------------------
DEFS_BASE="-DX11 -DUNIX -DS1024x864 -DKEYPAD_DETECT -DSTINGY_REDRAW \
-DBATCH_LINES -DBATCH_POINTS -DBATCH_COLOR_LINES \
-DCODE_SIZE=0x20000 -DSTACK_SIZE=0x8000 -DNEW_LIMITS -DSLOWER \
-DDYNALOAD -DMULTI_SYNC -DTHREAD_SWAPCONTEXT"

SRCS_CORE="actions animate box camo collision cosell disc display escher \
explosion file game gr highlib hit hud icounter init input interface \
intersect lowlib main mapper maze mdesign menu message newconsole newfile \
newradar players program radar repair scroll setup setups status sysdep unix \
update util vdesign vehicle version lex.zz"
SRCS_DISP="XMultiSync graphics objects three-d"
SRCS_THREADS="threadglue thread"
SRCS_PROGS="Bootlegger Dio Diophantine Flipper drone eliza gnat mmtf rdfbot spot tagman"

compile_tree() {
	local tree="$1" obj="$2" out="$3"; shift 3
	local extra_defs="$1"; shift
	local is_headless="$1"; shift
	local src="$tree/Src"
	local root="$tree"

	local -a inc=(-I"$src" -I"$src/Include")
	local -a defs=(-DXTANK_DIR="\"$root\"" -DALLDEFINES="\"p1.1-$MODE\"")
	if [ "$is_headless" = "yes" ]; then
		inc=(-I"$SCRATCH/include" "${inc[@]}")
	fi

	mkdir -p "$obj"
	local -a objs=()
	local f o
	cc_one() {
		local cfile="$1"
		o="$obj/$(basename "$cfile" .c).o"
		echo "  CC $cfile"
		gcc -O0 -g -w $DEFS_BASE $extra_defs "${inc[@]}" "${defs[@]}" \
		    -c "$cfile" -o "$o"
		objs+=("$o")
	}
	for f in $SRCS_CORE; do cc_one "$src/$f.c"; done
	for f in $SRCS_DISP;  do
		# XMultiSync.c uses private Xlib internals; the scratch headless
		# build ships a no-op XMultiSync in stubs.c instead.
		if [ "$is_headless" = "yes" ] && [ "$f" = "XMultiSync" ]; then
			continue
		fi
		cc_one "$src/Display/$f.c"
	done
	for f in $SRCS_THREADS; do cc_one "$src/Threads/$f.c"; done
	for f in $SRCS_PROGS; do cc_one "$src/Programs/$f.c"; done
	if [ "$is_headless" = "yes" ]; then
		cc_one "$SCRATCH/stubs.c"
		echo "  LINK $out (headless)"
		gcc "${objs[@]}" -ldl -lm -o "$out"
	else
		echo "  LINK $out (x11)"
		gcc "${objs[@]}" -ldl -lX11 -lm -o "$out"
	fi
}

# ---------------------------------------------------------------------------
# Reference build
# ---------------------------------------------------------------------------
build_reference() {
	local root="$EVIDENCE/reference"
	local src="$root/src"
	echo "== reference: $src"
	if [ ! -d "$src" ]; then
		mkdir -p "$root"
		snapshot_reference "$src"
	fi
	gen_aux "$src"
	local bin="$src/Src/xtank-refdump"
	local build=no
	if [ ! -x "$bin" ] || [ "${XTANK_P1_REBUILD:-0}" = "1" ]; then
		build=yes
	fi

	write_manifest() {
		{
			echo "# P1.1 frozen X reference manifest"
			echo "pin_source=working-tree-overlay:""$(git -C "$REPO" rev-parse HEAD)"
			echo "flags=$DEFS_BASE -DSTATE_DUMP"
			echo "binary_sha256=$(sha256sum "$bin" | awk '{print $1}')"
			echo "animate_sha256=$(sha256sum "$src/Src/animate.c" | awk '{print $1}')"
			sha256sum "$HARNESS/fixseed.c" "$HARNESS/scenarios/"*.settings
		} > "$root/MANIFEST"
	}

	if [ "$build" = "yes" ]; then
		compile_tree "$src" "$root/obj" "$bin" "-DSTATE_DUMP" "no"
		write_manifest
	elif [ ! -f "$root/MANIFEST" ]; then
		write_manifest
	else
		echo "  (frozen reference binary already present)"
		# The manifest is authoritative: refuse a reference binary that does
		# not match the frozen hash, so the oracle can never drift.
		want=$(sed -n 's/^binary_sha256=//p' "$root/MANIFEST")
		got=$(sha256sum "$bin" | awk '{print $1}')
		if [ "$want" != "$got" ]; then
			echo "ERROR: reference binary $bin does not match frozen manifest" >&2
			echo "       manifest=$want actual=$got" >&2
			echo "       rebuild deliberately with XTANK_P1_REBUILD=1" >&2
			exit 1
		fi
	fi
	cat "$root/MANIFEST"
}

# ---------------------------------------------------------------------------
# Candidate build
# ---------------------------------------------------------------------------
build_candidate() {
	local root="$EVIDENCE/candidate-$STAGE-$SUBJECT"
	local src="$root/src"
	echo "== candidate: subject=$SUBJECT stage=$STAGE"
	if [ -n "$SRC_OVERRIDE" ]; then
		src="$SRC_OVERRIDE"
	elif [ "$STAGE" = "before" ]; then
		# Pre-change evidence: pinned baseline plus the read-only oracle.
		# Always re-snapshot so a re-run reflects the pinned tree afresh.
		mkdir -p "$root"; snapshot_reference "$src"
	else
		# Always re-snapshot so the candidate is the current working tree.
		mkdir -p "$root"; snapshot_candidate "$src"
	fi

	if [ "$SUBJECT" = "headless" ]; then
		# Apply the temporary HEADLESS instrumentation to the candidate copy
		# only; the checkout's Src/animate.c is the sole branch edit.
		if ! grep -q "#ifdef HEADLESS" "$src/Src/Display/x11.c" 2>/dev/null; then
			patch -p1 -d "$src" < "$SCRATCH/headless-x11.patch"
		fi
		gen_aux "$src"
		compile_tree "$src" "$root/obj" "$src/Src/xtank-headless" \
		    "-DHEADLESS -DSTATE_DUMP" "yes"
	else
		gen_aux "$src"
		compile_tree "$src" "$root/obj" "$src/Src/xtank-xcand" \
		    "-DSTATE_DUMP" "no"
	fi
}

case "$MODE" in
  reference) build_reference ;;
  candidate) build_candidate ;;
  *) usage ;;
esac
