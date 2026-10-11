#!/usr/bin/env bash
#
# protocol.sh -- P2.2 acceptance harness for the frozen wire schema and
# content identity.
#
# Contract (docs/PHASE2-PLAN.md, P2.2):
#   bash Tests/phase2/protocol.sh
#
# It proves, without touching simulation code:
#   * Protocol/ builds standalone with no engine/SDL/ENet/X11 header;
#   * the committed golden fixtures decode, by an independent decoder, to the
#     hand-authored expected fields (including cross-endian values);
#   * the generated fixtures reproduce the committed bytes;
#   * the supplied source assets and geometry table still match the Phase 1
#     frozen goldens and the pinned content manifest;
#   * the recomputed content-set digest equals the manifest's;
#   * the calculated worst-case message sizes fit XTK_MAX_MESSAGE_BYTES.
#
# The exact-simulation guardrail G is separate:
#   export LC_ALL=C XTANK_SEED=114336
#   bash Tests/phase1/regression.sh --suite full --stage after --subject auto
#
# Evidence lives OUTSIDE the checkout under
#   ${XTANK_P2_EVIDENCE:-/tmp/opencode/xtank-phase2/p2.2}/protocol
#
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PROTO="$REPO/Protocol"
FIX="$REPO/Tests/phase2/fixtures/protocol"
P1="$REPO/Tests/phase1"
EVIDENCE="${XTANK_P2_EVIDENCE:-/tmp/opencode/xtank-phase2/p2.2}"
WORK="$EVIDENCE/protocol"

mkdir -p "$WORK"

log()  { printf '%s\n' "$*"; }
fail() { printf 'PROTOCOL FAIL: %s\n' "$*" >&2; exit 1; }

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
# 2. Standalone build (no engine tree on the include path).
# ---------------------------------------------------------------------------
log "== standalone compile of Protocol/"
CC="${CC:-gcc}"
"$CC" -std=c11 -Wall -Wextra -Werror -I "$PROTO" \
	"$FIX/header_check.c" "$PROTO/content-manifest.c" \
	-o "$WORK/header_check" > "$WORK/cc.log" 2>&1 || \
	fail "standalone compile failed (see $WORK/cc.log)"

"$WORK/header_check" > "$WORK/header_check.out" 2>&1 || \
	fail "header_check run failed"
grep -q '^HEADER-OK 3$' "$WORK/header_check.out" || \
	fail "header_check did not report HEADER-OK 3"
log "   $("$WORK/header_check" | head -1)"

manifest_val() { awk -v k="$1" '$1==k{print $2}' "$WORK/header_check.out"; }

# ---------------------------------------------------------------------------
# 3. Independent fixture verification + regeneration consistency.
# ---------------------------------------------------------------------------
log "== fixtures: independent decode"
python3 "$FIX/verify.py" > "$WORK/verify.out" 2>&1 || {
	cat "$WORK/verify.out" >&2
	fail "fixture verification failed"
}
log "   $(tail -1 "$WORK/verify.out")"

log "== fixtures: generator reproduces committed bytes"
regen="$WORK/regen"
rm -rf "$regen"; mkdir -p "$regen"
cp "$FIX"/*.hex "$regen/"
cp "$FIX/gen_fixtures.py" "$regen/gen_fixtures.py"
( cd "$regen" && python3 gen_fixtures.py > "$WORK/regen.out" 2>&1 ) || \
	fail "fixture regeneration failed (see $WORK/regen.out)"
for f in "$FIX"/*.hex; do
	cmp "$f" "$regen/$(basename "$f")" >/dev/null || \
	    fail "regenerated $(basename "$f") differs from committed"
done
log "   all committed .hex reproduce byte-for-byte"

# ---------------------------------------------------------------------------
# 4. Supplied source assets unchanged (Phase 1 frozen content golden).
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

# Manifest C digests must equal the tree's actual bytes.
declare -A SH
SH[combat1]=$(sha256sum "$REPO/Mazes/combat1.m" | awk '{print $1}')
SH[arena]=$(sha256sum "$REPO/Mazes/Arena.m" | awk '{print $1}')
SH[vanguard]=$(sha256sum "$REPO/Vehicles/Vanguard.V" | awk '{print $1}')
[ "$(manifest_val COMBAT1)" = "${SH[combat1]}" ] || fail "manifest COMBAT1 digest drift"
[ "$(manifest_val ARENA)"   = "${SH[arena]}"   ] || fail "manifest ARENA digest drift"
[ "$(manifest_val VANGUARD)" = "${SH[vanguard]}" ] || fail "manifest VANGUARD digest drift"
log "   manifest asset digests match the tree"

# ---------------------------------------------------------------------------
# 5. Geometry table + release preset + content-set digest.
# ---------------------------------------------------------------------------
geom_want=$(sed -n 's/^table //p' "$P1/geometry.golden.sha256")
[ "$(manifest_val GEOM)" = "$geom_want" ] || \
    fail "manifest geometry digest drift"
preset_got=$(sha256sum "$FIX/release-preset.txt" | awk '{print $1}')
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
log "   content-set digest $computed matches the manifest"

# ---------------------------------------------------------------------------
# 6. Worst-case message sizes fit the frozen bound.
# ---------------------------------------------------------------------------
max_msg=$(awk '/#define XTK_MAX_MESSAGE_BYTES/{gsub(/[^0-9]/,"",$3);print $3}' "$PROTO/limits.h")
[ -n "$max_msg" ] || fail "could not read XTK_MAX_MESSAGE_BYTES"
python3 - "$max_msg" > "$WORK/sizes.out" <<'PY'
import sys
limit = int(sys.argv[1])
sizes = {
    "HELLO":           32 + 41 + 32,
    "CONTENT_CATALOG": 32 + 34 + 8 * 68,
    "RESULTS":         32 + 8 + 4 * 41,
    "EVENT":           32 + 20 + 32,
    "CMD_ACK":         32 + 4 + 32 * 12,
    "RELIABLE_CMD":    32 + 8 + 32 * 28,
    "INPUT_BATCH":     32 + 12 + 32 * 28,
}
snap = (28 + 48 + 16 * 44 + 511 * 32 + 255 * 24 + 900 * 4 + 50 * 24 + 22 * 20)
sizes["VIEW_SNAPSHOT"] = 32 + snap
over = [k for k, v in sizes.items() if v > limit]
for k in sorted(sizes):
    print("%-16s %6d" % (k, sizes[k]))
print("VIEW_SNAPSHOT payload %d" % snap)
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
log "   worst-case VIEW_SNAPSHOT $(awk '/VIEW_SNAPSHOT payload/{print $3}' "$WORK/sizes.out") bytes <= $max_msg"

log ""
log "P2.2 PROTOCOL PASS (headers standalone, fixtures verified, assets unchanged, sizes bounded)"
