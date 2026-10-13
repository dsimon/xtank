#!/usr/bin/env bash
#
# codec.sh -- P3.3 acceptance harness for the portable protocol-v1.1 codec.
#
# Contract (docs/PHASE3-PLAN.md, P3.3):
#   bash Tests/phase3/codec.sh
#
# It proves, without touching simulation code:
#   * Protocol/ still includes only stdint/stddef and protocol headers, and the
#     P3.3 schema/rejection gate (Tests/phase3/protocol.sh) passes;
#   * the codec reproduces every committed v1.1 golden fixture byte-for-byte on
#     Linux and, cross-built with MinGW-w64, on the Windows target;
#   * truncation at every field, oversized counts, unknown types, wrong
#     versions (explicit 1.0 rejection), wrong channels, nonzero reserved (every
#     new field), NaN/Inf, malformed strings, length wrap, trailing octets and
#     cross-endian values are rejected;
#   * a deterministic fuzz run built with ASan+UBSan has no invalid access, and
#     decode/encode objects reference no allocator symbol.
#
# The frozen-X simulation guardrail G3 is separate and unchanged:
#   export LC_ALL=C XTANK_SEED=114336
#   bash Tests/phase1/regression.sh --suite full --stage after --subject auto
#
# Evidence lives OUTSIDE the checkout under
#   ${XTANK_P3_EVIDENCE:-/tmp/opencode/xtank-phase3/p3.3}/codec
#
# Env overrides: CC, MINGW_CC, XTK_WINE, XTK_WINEPREFIX, XTK_FUZZ_SEED,
# XTK_FUZZ_SECONDS, XTK_ALLOW_NO_WINE.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PROTO="$REPO/Protocol"
FIX="$REPO/Tests/phase3/fixtures/protocol"
TEST="$REPO/Tests/phase3"
EVIDENCE="${XTANK_P3_EVIDENCE:-/tmp/opencode/xtank-phase3/p3.3}"
WORK="$EVIDENCE/codec"
LIN="$WORK/linux"
MINGW="$WORK/mingw"

CC="${CC:-gcc}"
MINGW_CC="${MINGW_CC:-x86_64-w64-mingw32-gcc}"
WINE="${XTK_WINE:-wine}"
FUZZ_CC="${FUZZ_CC:-gcc}"
FUZZ_SEED="${XTK_FUZZ_SEED:-12648430}"
FUZZ_SECONDS="${XTK_FUZZ_SECONDS:-20}"

mkdir -p "$LIN" "$MINGW"

log()  { printf '%s\n' "$*"; }
fail() { printf 'P3.3 CODEC FAIL: %s\n' "$*" >&2; exit 1; }

CFLAGS="-std=c11 -Wall -Wextra -Werror -I $PROTO"

# ---------------------------------------------------------------------------
# 1. Wire types stay independent and the P3.3 schema gate still passes.
# ---------------------------------------------------------------------------
log "== protocol header independence"
bad=$(
	grep -RhE '^[[:space:]]*#[[:space:]]*include' "$PROTO" \
	| sed -E 's/.*[<"]([^">]+)[">].*/\1/' \
	| grep -Ev '^(stdint\.h|stddef\.h|wire\.h|limits\.h|content-manifest\.h)$' \
	| sort -u || true
)
[ -z "$bad" ] || fail "Protocol/ includes non-protocol headers: $(tr '\n' ' ' <<<"$bad")"
log "   Protocol/ includes only stdint/stddef and protocol headers"

log "== P3.3 schema/fixture gate"
XTANK_P3_EVIDENCE="$WORK/p3.3" bash "$TEST/protocol.sh" > "$WORK/protocol.log" 2>&1 || {
	cat "$WORK/protocol.log" >&2
	fail "P3.3 protocol.sh no longer passes"
}
log "   $(tail -1 "$WORK/protocol.log")"

# ---------------------------------------------------------------------------
# 2. Linux golden-byte + rejection driver.
# ---------------------------------------------------------------------------
log "== codec golden/rejection driver (Linux)"
# shellcheck disable=SC2086
$CC $CFLAGS "$TEST/codec_fixture.c" "$PROTO/encode.c" "$PROTO/decode.c" \
	"$PROTO/content-manifest.c" -o "$LIN/codec" > "$LIN/cc.log" 2>&1 || {
	cat "$LIN/cc.log" >&2
	fail "Linux codec build failed"
}
"$LIN/codec" "$FIX" > "$LIN/run.out" 2>&1 || {
	cat "$LIN/run.out" >&2
	fail "Linux codec driver reported failures"
}
grep -q '^CODEC PASS' "$LIN/run.out" || {
	cat "$LIN/run.out" >&2
	fail "Linux codec did not PASS"
}
log "   $(tail -1 "$LIN/run.out")"

# ---------------------------------------------------------------------------
# 3. MinGW cross target golden-byte + rejection driver.
# ---------------------------------------------------------------------------
log "== codec golden/rejection driver (MinGW-w64 target)"
command -v "$MINGW_CC" >/dev/null 2>&1 || fail "missing MinGW cross compiler $MINGW_CC"
# shellcheck disable=SC2086
$MINGW_CC $CFLAGS "$TEST/codec_fixture.c" "$PROTO/encode.c" "$PROTO/decode.c" \
	"$PROTO/content-manifest.c" -o "$MINGW/codec.exe" > "$MINGW/cc.log" 2>&1 || {
	cat "$MINGW/cc.log" >&2
	fail "MinGW codec build failed"
}
cp "$FIX"/*.hex "$MINGW/"

if command -v "$WINE" >/dev/null 2>&1 || [ -x "$WINE" ]; then
	export WINEARCH=win64 WINEDEBUG=-all
	export WINEPREFIX="${XTK_WINEPREFIX:-${XTK_WINEPREFIX:-$WORK/wineprefix}}"
	if [ ! -d "$WINEPREFIX" ]; then
		env -u TMPDIR "$WINE" wineboot -u > "$MINGW/wineboot.log" 2>&1 || true
	fi
	( cd "$MINGW" && env -u TMPDIR "$WINE" ./codec.exe . > run.out 2> run.err ) || {
		cat "$MINGW/run.out" 2>/dev/null
		cat "$MINGW/run.err" 2>/dev/null >&2
		fail "MinGW codec.exe did not exit 0"
	}
	grep -q '^CODEC PASS' "$MINGW/run.out" || {
		cat "$MINGW/run.out" >&2
		fail "MinGW codec.exe did not PASS"
	}
	log "   $(tail -1 "$MINGW/run.out")"
else
	if [ -n "${XTK_ALLOW_NO_WINE:-}" ]; then
		log "   SKIPPED: no Wine runner (XTK_ALLOW_NO_WINE set); cross-build verified only"
	else
		fail "no Wine runner found; set XTK_WINE, install wine64, or export XTK_ALLOW_NO_WINE=1 to record a deferral"
	fi
fi

# ---------------------------------------------------------------------------
# 4. Deterministic fuzz run with ASan + UBSan.
# ---------------------------------------------------------------------------
log "== decoder fuzz under ASan+UBSan (seed=$FUZZ_SEED, ${FUZZ_SECONDS}s)"
$FUZZ_CC $CFLAGS -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
	"$TEST/codec_fuzz.c" "$PROTO/encode.c" "$PROTO/decode.c" \
	-o "$LIN/codec_fuzz" > "$LIN/fuzz-cc.log" 2>&1 || {
	cat "$LIN/fuzz-cc.log" >&2
	fail "fuzz build failed"
}
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
	"$LIN/codec_fuzz" "$FUZZ_SEED" "$FUZZ_SECONDS" "$FIX" \
	> "$LIN/fuzz.out" 2>&1 || {
	cat "$LIN/fuzz.out" >&2
	fail "fuzz run reported a sanitizer error"
}
grep -q '^FUZZ PASS' "$LIN/fuzz.out" || {
	cat "$LIN/fuzz.out" >&2
	fail "fuzz run did not PASS"
}
log "   $(tail -1 "$LIN/fuzz.out")"

# ---------------------------------------------------------------------------
# 5. No allocator in the codec objects.
# ---------------------------------------------------------------------------
log "== codec objects reference no allocator"
$CC $CFLAGS -c "$PROTO/encode.c" -o "$LIN/encode.o" 2>/dev/null
$CC $CFLAGS -c "$PROTO/decode.c" -o "$LIN/decode.o" 2>/dev/null
alloc=$(
	{ nm "$LIN/encode.o" "$LIN/decode.o" 2>/dev/null || true; } \
	| grep -E '\b(malloc|calloc|realloc|free|aligned_alloc|posix_memalign)\b' || true
)
[ -z "$alloc" ] || fail "codec references an allocator: $alloc"
log "   no malloc/calloc/realloc/free in encode.o/decode.o"

# ---------------------------------------------------------------------------
# 6. Independent fixture decoder (not the codec, not the generator).
# ---------------------------------------------------------------------------
log "== independent fixture decoder"
python3 "$FIX/verify.py" > "$WORK/verify.out" 2>&1 || {
	cat "$WORK/verify.out" >&2
	fail "independent fixture verification failed"
}
log "   $(tail -1 "$WORK/verify.out")"

log ""
log "P3.3 CODEC PASS (golden bytes Linux+MinGW, rejections, fuzz/sanitizer, no-alloc)"
