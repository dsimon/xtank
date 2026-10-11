#!/usr/bin/env bash
#
# content.sh -- P1.14 acceptance harness for display-free content/config
# loading.
#
# Contract (PHASE1-PLAN.md, P1.14):
#   bash Tests/phase1/content.sh
#
# It builds the display-free (null) server candidate and proves, with DISPLAY,
# XTANK_DIR and HOME unset:
#
#   * the two supplied mazes (Mazes/combat1.m, Mazes/Arena.m) and the supplied
#     vehicle (Vehicles/Vanguard.V) load through the explicit asset root;
#   * combat1 loads even where there is no Mazes/list at all (list-independent,
#     no random-maze fallback);
#   * the supplied assets are byte-identical to the pinned manifest;
#   * a missing asset tree fails nonzero with a precise error;
#   * the approved release config verifies as Combat free-for-all, at most four
#     vehicles, respawn on, bots may win, individual winning score 1000, 15 Hz,
#     with every other init_settings() field left at its default;
#   * the approved special preset disables New Radar/Tac Link/Camo/Rdf while
#     leaving Console/Mapper/Ordinary Radar/HUD permitted.
#
# The mandatory byte-level simulation guardrail (G) is separate:
#   bash Tests/phase1/regression.sh --suite full --stage after --subject null
#
# Evidence lives OUTSIDE the checkout under
#   ${XTANK_P1_EVIDENCE:-/tmp/opencode/xtank-phase1/p1.14}/content
#
set -euo pipefail

# The build snapshots the tree with tar; in a container without CAP_CHOWN the
# extracting tar must not try to restore the source ownership.
export TAR_OPTIONS="${TAR_OPTIONS:---no-same-owner}"

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HARNESS="$REPO/Tests/phase1"
EVIDENCE="${XTANK_P1_EVIDENCE:-/tmp/opencode/xtank-phase1/p1.14}"
WORK="$EVIDENCE/content"
GOLDEN="$HARNESS/content.golden.sha256"
REGEN="${XTANK_P1_REGEN_CONTENT_GOLDEN:-0}"

while [ $# -gt 0 ]; do
	case "$1" in
	  --evidence) EVIDENCE="$2"; shift 2 ;;
	  *) echo "usage: content.sh [--evidence DIR]" >&2; exit 2 ;;
	esac
done

mkdir -p "$WORK"

log()  { printf '%s\n' "$*"; }
fail() { printf 'CONTENT FAIL: %s\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------------------
# 1. Frozen asset manifest (pin identity)
# ---------------------------------------------------------------------------
log "== asset manifest $GOLDEN"
[ -f "$GOLDEN" ] || fail "missing manifest $GOLDEN"

ASSETS="Mazes/combat1.m Mazes/Arena.m Vehicles/Vanguard.V"

if [ "$REGEN" = "1" ]; then
	{
		printf 'pin 14e6d42efdc0318fbf37717b21cc9f6027dc7e9f\n'
		( cd "$REPO" && sha256sum $ASSETS )
	} > "$WORK/content.generated.sha256"
	cp "$WORK/content.generated.sha256" "$GOLDEN"
	log "== regenerated $GOLDEN"
fi

want_entries=$(grep -c '^[0-9a-f]\{64\}  ' "$GOLDEN" || true)
[ "$want_entries" = "3" ] || fail "manifest must pin exactly 3 assets (found $want_entries)"

for a in $ASSETS; do
	want=$(awk -v p="$a" '$2==p {print $1}' "$GOLDEN")
	[ -n "$want" ] || fail "no manifest entry for $a"
	[ -f "$REPO/$a" ] || fail "missing asset $REPO/$a"
	got=$(sha256sum "$REPO/$a" | awk '{print $1}')
	[ "$want" = "$got" ] || \
	    fail "asset $a sha256 $got != pinned $want"
	log "   $a $got"
done

# ---------------------------------------------------------------------------
# 2. Build the display-free (null) candidate
# ---------------------------------------------------------------------------
log "== building null candidate"
bash "$HARNESS/build-reference.sh" --mode candidate --subject null \
    --stage after --evidence "$EVIDENCE" > "$WORK/build.out" 2>&1 || \
    fail "candidate build failed (see $WORK/build.out)"
BIN="$EVIDENCE/candidate-after-null/src/Src/xtank-headless"
[ -x "$BIN" ] || fail "candidate binary missing: $BIN"

# The asset root passed to the content loader is independent of the build
# layout; the checkout is only one source of the pinned asset tree.
SRC_ROOT="$EVIDENCE/candidate-after-null/src"

# ---------------------------------------------------------------------------
# 3. Controlled asset trees
# ---------------------------------------------------------------------------
mkdir -p "$WORK/empty"
mkdir -p "$WORK/listless/Mazes" "$WORK/listless/Vehicles"
cp "$REPO/Mazes/combat1.m" "$REPO/Mazes/Arena.m" "$WORK/listless/Mazes/"
cp "$REPO/Vehicles/Vanguard.V" "$WORK/listless/Vehicles/"
rm -f "$WORK/listless/Mazes/list" "$WORK/listless/Vehicles/list"
[ ! -e "$WORK/listless/Mazes/list" ] || fail "listless tree still has a maze list"

# ---------------------------------------------------------------------------
# 4. Run the explicit content loader with a controlled environment
# ---------------------------------------------------------------------------
run_check() {   # tag root; writes $WORK/$tag.{out,err}; returns the rc
	local tag="$1" root="$2"
	local rc=0
	env -u DISPLAY -u XTANK_DIR -u HOME \
	    -u XTANK_VEHICLES -u XTANK_MAZES -u XTANK_PROGRAMS \
	    -u XTANK_ASSET_ROOT -u XTANK_SEED -u LD_PRELOAD \
	    LC_ALL=C XTANK_ASSET_ROOT="$root" XTANK_CONTENT_CHECK=1 \
	    "$BIN" > "$WORK/$tag.out" 2> "$WORK/$tag.err" || rc=$?
	return $rc
}

log "== explicit root: shipped assets (DISPLAY/XTANK_DIR/HOME unset)"
if ! run_check shipped "$SRC_ROOT"; then
	fail "loading the supplied assets failed (see $WORK/shipped.err)"
fi
OUT="$WORK/shipped.out"
grep -q '^CONTENT-OK$' "$OUT" || fail "loader did not report CONTENT-OK"
if grep -q '^CONTENT-FAIL$' "$OUT"; then
	fail "loader reported CONTENT-FAIL"
fi

grep -q '^CONTENT-MAZE combat1 rc=0 name=combat1 type=0$' "$OUT" || \
    fail "combat1.m did not load as a Combat maze"
grep -q '^CONTENT-MAZE Arena rc=0 name=Arena type=0$' "$OUT" || \
    fail "Arena.m did not load as a Combat maze"
grep -q '^CONTENT-VEHICLE Vanguard rc=0 name=Vanguard body=6 weapons=2 ' "$OUT" || \
    fail "Vanguard.V did not load (want Marauder body 6, 2 weapons)"
log "   combat1.m / Arena.m / Vanguard.V loaded"

log "== listless root: no Mazes/list or Vehicles/list"
if ! run_check listless "$WORK/listless"; then
	fail "loading combat1 without a maze list failed (see $WORK/listless.err)"
fi
grep -q '^CONTENT-MAZE combat1 rc=0 ' "$WORK/listless.out" || \
    fail "combat1 did not load from a listless tree"
grep -q '^CONTENT-VEHICLE Vanguard rc=0 ' "$WORK/listless.out" || \
    fail "Vanguard did not load from a listless tree"
log "   list-independent loading confirmed"

log "== missing assets must fail nonzero"
if run_check empty "$WORK/empty"; then
	fail "loading from an empty asset root unexpectedly succeeded"
fi
grep -q '^CONTENT-FAIL$' "$WORK/empty.out" || \
    fail "empty root did not report CONTENT-FAIL"
grep -q '^CONTENT-ERROR ' "$WORK/empty.out" || \
    fail "empty root did not report a specific error"
log "   empty root failed nonzero with a precise error"
log "   $(head -1 "$WORK/empty.out")"

# ---------------------------------------------------------------------------
# 5. Release configuration verification
# ---------------------------------------------------------------------------
log "== release configuration"
cfg()   { awk -v k="$1" '/^CONTENT-CONFIG /{for(i=1;i<=NF;i++){split($i,a,"=");if(a[1]==k)print a[2]}}' "$OUT" | head -1; }
rel()   { sed -n "s/^SETREL $1=//p" "$OUT" | head -1; }

eq()    { [ "$2" = "$3" ] || fail "$1: expected '$3', got '$2'"; }

eq "tick_hz"            "$(cfg tick_hz)" 15
eq "max_vehicles"       "$(cfg max_vehicles)" 4
eq "release_max"        "$(cfg release_max)" 4
[ "$(cfg max_vehicles)" -le 4 ] || fail "release config exceeds four vehicles"

eq "game_speed"         "$(rel game_speed)" 15
eq "si_game (Combat)"   "$(rel si_game)" 0
eq "si_team_score (FFA)" "$(rel si_team_score)" 0
eq "si_restart (respawn)" "$(rel si_restart)" 1
eq "robots_dont_win"    "$(rel robots_dont_win)" 0
eq "si_winning_score"   "$(rel si_winning_score)" 1000

# Every other field must equal the init_settings() default: the only approved
# differences are the FFA, bots-may-win and winning-score overrides.
awk '
  /^SETDEF / { sub(/^SETDEF /,""); k=$0; sub(/=.*/,"",k); def[k]=$0 }
  /^SETREL / { sub(/^SETREL /,""); k=$0; sub(/=.*/,"",k); rel[k]=$0 }
  END { for (k in rel) if (rel[k] != def[k]) print k }
' "$OUT" | sort > "$WORK/diff.keys"
printf 'robots_dont_win\nsi_team_score\nsi_winning_score\n' > "$WORK/diff.want"
if ! cmp "$WORK/diff.keys" "$WORK/diff.want" >/dev/null; then
	fail "release config differs from init_settings() defaults in unexpected fields: $(tr '\n' ' ' < "$WORK/diff.keys")"
fi
log "   release overrides: $(tr '\n' ' ' < "$WORK/diff.keys")"

# ---------------------------------------------------------------------------
# 6. Approved special preset
# ---------------------------------------------------------------------------
log "== approved special preset"
npreset=$(grep -c '^PRESET ' "$OUT" || true)
[ "$npreset" = "12" ] || \
    fail "unexpected special count ($npreset); preset check is build-flag specific"
preset() { sed -n "s/^PRESET $1=//p" "$OUT" | head -1; }
# Order (no NO_HUD/NO_CAMO): 0 Console 1 Mapper 2 Radar 5 HUD
#                            8 New Radar 9 Tac Link 10 Camo 11 Rdf
for i in 0 1 2 5; do
	eq "preset index $i permitted" "$(preset "$i")" 0
done
for i in 8 9 10 11; do
	eq "preset index $i disabled" "$(preset "$i")" 2
done
log "   Console/Mapper/Radar/HUD permitted; New Radar/Tac Link/Camo/Rdf off"

log ""
log "P1.14 CONTENT PASS (explicit root, list-independent, missing fails, pin == manifest, release config verified)"
