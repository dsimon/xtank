#!/usr/bin/env bash
#
# protocol.sh -- P3.3 acceptance harness for the frozen protocol-v1.1 schema
# and content identity.
#
# Contract (docs/PHASE3-PLAN.md, P3.3):
#   bash Tests/phase3/protocol.sh
#
# It proves, without touching simulation code:
#   * Protocol/ builds standalone with no engine/SDL/ENet/X11 header;
#   * the wire version is exact 1.1 and the C header agrees;
#   * the committed v1.1 golden fixtures decode, by an independent decoder, to
#     the hand-authored expected fields;
#   * the v1.1 generator reproduces the committed bytes;
#   * the retained v1.0 fixtures are byte-identical and still pass their own
#     independent decoder (no backward-compat claim without proof);
#   * the supplied source assets / geometry / content-set digest are unchanged;
#   * the calculated worst-case v1.1 message sizes fit XTK_MAX_MESSAGE_BYTES.
#
# The exact-simulation guardrail G3 is separate:
#   export LC_ALL=C XTANK_SEED=114336
#   bash Tests/phase1/regression.sh --suite full --stage after --subject auto
#
# Evidence lives OUTSIDE the checkout under
#   ${XTANK_P3_EVIDENCE:-/tmp/opencode/xtank-phase3/p3.3}/protocol
#
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PROTO="$REPO/Protocol"
FIX="$REPO/Tests/phase3/fixtures/protocol"
FIX10="$REPO/Tests/phase2/fixtures/protocol"
P1="$REPO/Tests/phase1"
EVIDENCE="${XTANK_P3_EVIDENCE:-/tmp/opencode/xtank-phase3/p3.3}"
WORK="$EVIDENCE/protocol"

mkdir -p "$WORK"

log()  { printf '%s\n' "$*"; }
fail() { printf 'P3.3 PROTOCOL FAIL: %s\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------------------
# 1. Protocol/ is self-contained: no engine/SDL/ENet/X11 headers.
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

# ---------------------------------------------------------------------------
# 2. Exact version 1.1 declared by the C header.
# ---------------------------------------------------------------------------
log "== wire version is exact 1.1"
grep -qE '^#define XTK_PROTOCOL_MAJOR[[:space:]]+1u' "$PROTO/wire.h" || \
	fail "XTK_PROTOCOL_MAJOR is not 1"
grep -qE '^#define XTK_PROTOCOL_MINOR[[:space:]]+1u' "$PROTO/wire.h" || \
	fail "XTK_PROTOCOL_MINOR is not 1 (no dual-version stack, D3-A)"
log "   XTK_PROTOCOL_MAJOR=1 XTK_PROTOCOL_MINOR=1"

# ---------------------------------------------------------------------------
# 3. Standalone build (no engine tree on the include path).
# ---------------------------------------------------------------------------
log "== standalone compile of Protocol/"
CC="${CC:-gcc}"
"$CC" -std=c11 -Wall -Wextra -Werror -I "$PROTO" \
	"$FIX/header_check.c" "$PROTO/content-manifest.c" \
	-o "$WORK/header_check" > "$WORK/cc.log" 2>&1 || \
	fail "standalone compile failed (see $WORK/cc.log)"

"$WORK/header_check" > "$WORK/header_check.out" 2>&1 || \
	fail "header_check run failed"
grep -q '^HEADER-OK 4$' "$WORK/header_check.out" || \
	fail "header_check did not report HEADER-OK 4"
log "   $("$WORK/header_check" | head -1)"

manifest_val() { awk -v k="$1" '$1==k{print $2}' "$WORK/header_check.out"; }

# ---------------------------------------------------------------------------
# 4. Independent v1.1 fixture verification + regeneration consistency.
# ---------------------------------------------------------------------------
log "== v1.1 fixtures: independent decode"
python3 "$FIX/verify.py" > "$WORK/verify.out" 2>&1 || {
	cat "$WORK/verify.out" >&2
	fail "v1.1 fixture verification failed"
}
log "   $(tail -1 "$WORK/verify.out")"

log "== v1.1 fixtures: generator reproduces committed bytes"
regen="$WORK/regen"
rm -rf "$regen"; mkdir -p "$regen"
cp "$FIX"/*.hex "$regen/"
cp "$FIX/gen_fixtures.py" "$regen/gen_fixtures.py"
( cd "$regen" && python3 gen_fixtures.py > "$WORK/regen.out" 2>&1 ) || \
	fail "v1.1 fixture regeneration failed (see $WORK/regen.out)"
for f in "$FIX"/*.hex; do
	cmp "$f" "$regen/$(basename "$f")" >/dev/null || \
	    fail "regenerated $(basename "$f") differs from committed"
done
log "   all committed v1.1 .hex reproduce byte-for-byte"

# ---------------------------------------------------------------------------
# 5. Retained v1.0 fixtures: byte-identical + independent decode.
# ---------------------------------------------------------------------------
log "== retained v1.0 fixtures unchanged and independently verified"
V10="$FIX/v1.0-fixtures.sha256"
[ -f "$V10" ] || fail "missing $V10"
n10=0
while read -r want path; do
	case "$want" in \#*|'') continue ;; esac
	[ -f "$REPO/$path" ] || fail "missing retained v1.0 fixture $path"
	got=$(sha256sum "$REPO/$path" | awk '{print $1}')
	[ "$got" = "$want" ] || fail "v1.0 fixture $path sha256 drift ($got != $want)"
	n10=$((n10+1))
done < "$V10"
[ "$n10" = "16" ] || fail "expected 16 retained v1.0 fixtures, found $n10"
python3 "$FIX10/verify.py" > "$WORK/verify-v1.0.out" 2>&1 || {
	cat "$WORK/verify-v1.0.out" >&2
	fail "retained v1.0 fixture independent verification failed"
}
log "   16 v1.0 fixtures byte-identical; $(tail -1 "$WORK/verify-v1.0.out")"

# ---------------------------------------------------------------------------
# 6. Supplied source assets unchanged (Phase 1 frozen content golden).
# ---------------------------------------------------------------------------
log "== supplied asset hashes unchanged"
CONTENT_GOLDEN="$P1/content.golden.sha256"
[ -f "$CONTENT_GOLDEN" ] || fail "missing $CONTENT_GOLDEN"
n=0
while read -r want path; do
	case "$want" in \#*|'') continue ;; esac
	if [ "$want" = "pin" ]; then continue; fi
	[ -f "$REPO/$path" ] || fail "missing asset $path"
	got=$(sha256sum "$REPO/$path" | awk '{print $1}')
	[ "$got" = "$want" ] || fail "asset $path sha256 $got != frozen $want"
	n=$((n+1))
done < "$CONTENT_GOLDEN"
[ "$n" = "3" ] || fail "content golden must pin exactly 3 assets (found $n)"
log "   3 source assets match content.golden.sha256"

declare -A SH
SH[combat1]=$(sha256sum "$REPO/Mazes/combat1.m" | awk '{print $1}')
SH[arena]=$(sha256sum "$REPO/Mazes/Arena.m" | awk '{print $1}')
SH[vanguard]=$(sha256sum "$REPO/Vehicles/Vanguard.V" | awk '{print $1}')
[ "$(manifest_val COMBAT1)" = "${SH[combat1]}" ] || fail "manifest COMBAT1 digest drift"
[ "$(manifest_val ARENA)"   = "${SH[arena]}"   ] || fail "manifest ARENA digest drift"
[ "$(manifest_val VANGUARD)" = "${SH[vanguard]}" ] || fail "manifest VANGUARD digest drift"
log "   manifest asset digests match the tree"

geom_want=$(sed -n 's/^table //p' "$P1/geometry.golden.sha256")
[ "$(manifest_val GEOM)" = "$geom_want" ] || fail "manifest geometry digest drift"
preset_got=$(sha256sum "$FIX10/release-preset.txt" | awk '{print $1}')
[ "$(manifest_val PRESET)" = "$preset_got" ] || \
	fail "manifest release-preset digest drift"

computed=$(python3 - "${SH[combat1]}" "${SH[arena]}" "${SH[vanguard]}" "$preset_got" "$geom_want" <<'PY'
import hashlib, sys
combat1, arena, vanguard, preset, geom = sys.argv[1:6]
text = (
    "XTK-CONTENT-SET-v1\n"
    "maze combat1 %s\n"
    "maze Arena %s\n"
    "vehicle Vanguard %s\n"
    "config release %s\n"
    "geometry table %s\n" % (combat1, arena, vanguard, preset, geom))
sys.stdout.write(hashlib.sha256(text.encode()).hexdigest())
PY
)
[ "$(manifest_val CONTENT-SET)" = "$computed" ] || \
    fail "content-set digest $computed != manifest $(manifest_val CONTENT-SET)"
log "   content-set digest $computed matches the manifest (unchanged)"

# ---------------------------------------------------------------------------
# 7. Worst-case v1.1 message sizes fit the frozen bound.
# ---------------------------------------------------------------------------
max_msg=$(awk '/#define XTK_MAX_MESSAGE_BYTES/{gsub(/[^0-9]/,"",$3);print $3}' "$PROTO/limits.h")
[ -n "$max_msg" ] || fail "could not read XTK_MAX_MESSAGE_BYTES"
python3 - "$max_msg" > "$WORK/sizes.out" <<'PY'
import sys
limit = int(sys.argv[1])
env = 32
sizes = {
    "HELLO":           env + 41 + 32,
    "WELCOME":         env + 9,
    "CONFIG_SELECT":   env + 12,
    "CONFIG_STATUS":   env + 16,
    "READY":           env + 8,
    "START":           env + 16,
    "START_REQUEST":   env + 8,
    "LOBBY_STATE":     env + 16 + 2 * (13 + 32),
    "RESULTS":         env + 8 + 4 * (13 + 32),
    "EVENT":           env + 25 + 32,
    "CMD_ACK":         env + 4 + 32 * 12,
    "RELIABLE_CMD":    env + 8 + 32 * 32,
    "INPUT_BATCH":     env + 12 + 32 * 32,
    "PROTOCOL_ERROR":  env + 12,
}
snap = (32 + 52 + 16 * 48 + 511 * 40 + 255 * 28 + 900 * 4 +
        50 * 28 + 22 * 20)
sizes["VIEW_SNAPSHOT"] = env + snap
sizes["VIEW_KEYFRAME"] = env + snap
over = [k for k, v in sizes.items() if v > limit]
for k in sorted(sizes):
    print("%-16s %6d" % (k, sizes[k]))
print("VIEW_* payload %d" % snap)
print("limit %d" % limit)
if over:
    print("OVER %s" % " ".join(over))
    sys.exit(1)
print("SIZES-OK")
PY
grep -q '^SIZES-OK$' "$WORK/sizes.out" || {
	cat "$WORK/sizes.out" >&2
	fail "worst-case message size exceeds the approved bound"
}
log "   worst-case VIEW payload $(awk '/VIEW_\* payload/{print $3}' "$WORK/sizes.out") bytes; message <= $max_msg"

log ""
log "P3.3 PROTOCOL PASS (standalone headers, exact 1.1, v1.1 fixtures verified, v1.0 retained, sizes bounded)"
