#!/usr/bin/env bash
#
# intent.sh -- P1.7 acceptance fixture for the bounded intent queue.
#
# Compiles Src/intent.c against a tiny recording stub of the legacy controls
# and asserts the queue's contract directly:
#
#   * tick/player/sequence ordering (stable player order, then sequence);
#   * drive, heading, world-coordinate aim, fire and weapon ops dispatch;
#   * held fire / held aim persist and are released;
#   * rejected commands (invalid, bad player, foreign target, stale, duplicate,
#     overflow) have no state effect;
#   * future-tick commands stay queued until their tick is consumed;
#   * intent_reset() clears both queued and held input.
#
# Generated objects live outside the checkout (default
# /tmp/opencode/xtank-phase1/p1.7).
#
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HARNESS="$REPO/Tests/phase1"
EVIDENCE="${XTANK_P1_EVIDENCE:-/tmp/opencode/xtank-phase1/p1.7}"
WORK="$EVIDENCE/intent-fixture"

mkdir -p "$WORK"

DEFS="-DUNIX -DS1024x864 -DKEYPAD_DETECT -DSTINGY_REDRAW -DBATCH_LINES \
-DBATCH_POINTS -DBATCH_COLOR_LINES -DCODE_SIZE=0x20000 -DSTACK_SIZE=0x8000 \
-DNEW_LIMITS -DSLOWER -DDYNALOAD -DMULTI_SYNC -DTHREAD_SWAPCONTEXT -DNULL_BACKEND"

log()  { printf '%s\n' "$*"; }
fail() { printf 'INTENT FAIL: %s\n' "$*" >&2; exit 1; }

cat > "$WORK/intent_fixture.c" <<'FIXTURE'
/*
 * P1.7 intent-queue acceptance fixture: records legacy control calls so the
 * queue's ordering, rejection and held-control semantics can be asserted
 * without a full match.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xtank.h"
#include "vehicle.h"
#include "globals.h"
#include "lowlib.h"
#include "proto.h"
#include "intent.h"

Vehicle actual_vehicles[MAX_VEHICLES];
Vehicle *cv;
int num_veh;
int num_veh_alive;

static Loc locs[MAX_VEHICLES];

enum {
	O_DRIVE_REL, O_DRIVE_ABS, O_TURN, O_AIM,
	O_FIRE, O_WEAPON_ON, O_WEAPON_OFF, O_WEAPON_FIRE
};

#define LOGMAX 4096
static int o_op[LOGMAX];
static int o_player[LOGMAX];
static double o_a[LOGMAX];
static double o_b[LOGMAX];
static int o_n;

static int failures;

static void
rec(int op, double a, double b)
{
	if (o_n >= LOGMAX)
		return;
	o_op[o_n] = op;
	o_player[o_n] = (cv != NULL) ? (int) (cv - actual_vehicles) : -1;
	o_a[o_n] = a;
	o_b[o_n] = b;
	o_n++;
}

void set_current_vehicle(Vehicle *v) { cv = v; }
void set_rel_drive(double d) { rec(O_DRIVE_REL, d, 0); }
void set_abs_drive(double d) { rec(O_DRIVE_ABS, d, 0); }
void turn_vehicle_human(Angle a) { rec(O_TURN, a, 0); }
void aim_all_turrets(int dx, int dy) { rec(O_AIM, dx, dy); }
int fire_all_weapons(void) { rec(O_FIRE, 0, 0); return 0; }
void anim_fire_weapon(WeaponNum n) { rec(O_WEAPON_FIRE, n, 0); }
int turn_on_weapon(WeaponNum n) { rec(O_WEAPON_ON, n, 0); return 0; }
int turn_off_weapon(WeaponNum n) { rec(O_WEAPON_OFF, n, 0); return 0; }
void action_aim_world(Vehicle *v, double wx, double wy)
{
	cv = v;
	rec(O_AIM, wx, wy);
}
void action_fire_all(Vehicle *v)
{
	cv = v;
	rec(O_FIRE, 0, 0);
}
Vehicle *lowlib_actual_vehicle(int p)
{
	return (p >= 0 && p < num_veh) ? &actual_vehicles[p] : (Vehicle *) NULL;
}

static void
check(const char *what, int ok)
{
	if (!ok) {
		fprintf(stderr, "  [FAIL] %s\n", what);
		failures++;
	} else {
		fprintf(stderr, "  [ok]   %s\n", what);
	}
}

static Intent
mk(int tick, int player, unsigned int seq, IntentOp op, double a, double b)
{
	Intent in;

	memset(&in, 0, sizeof(in));
	in.tick = tick;
	in.player = player;
	in.sequence = seq;
	in.op = op;
	in.a = a;
	in.b = b;
	return in;
}

static void
reset_all(void)
{
	intent_reset();
	o_n = 0;
	cv = NULL;
}

int
main(int argc, char *argv[])
{
	Intent in;
	int i, st;
	int max = INTENT_QUEUE_MAX;

	(void) argc;
	(void) argv;

	for (i = 0; i < MAX_VEHICLES; i++) {
		actual_vehicles[i].number = (Byte) i;
		actual_vehicles[i].loc = &locs[i];
		locs[i].x = 100.0 * i;
		locs[i].y = 200.0 * i;
	}
	num_veh = 2;
	num_veh_alive = 2;
	intent_init();

	/* A. future-tick intents stay queued until their tick. */
	reset_all();
	in = mk(5, 0, 1, INTENT_DRIVE_REL, 3, 0);
	check("future: accepted", intent_push(&in) == INTENT_OK);
	check("future: not pending for tick 1", intent_pending(1) == 0);
	check("future: pending for tick 5", intent_pending(5) == 1);
	check("future: consume(1) applies nothing", intent_consume(1) == 0);
	check("future: no state effect at tick 1", o_n == 0);
	check("future: still queued after tick 1", intent_pending(5) == 1);
	check("future: consume(5) applies one", intent_consume(5) == 1);
	check("future: drive applied once", o_n == 1 && o_op[0] == O_DRIVE_REL &&
	      o_a[0] == 3.0);

	/* B. duplicate (player,tick,sequence) is rejected with no effect. */
	reset_all();
	in = mk(2, 0, 7, INTENT_DRIVE_REL, 4, 0);
	check("dup: first accepted", intent_push(&in) == INTENT_OK);
	check("dup: second rejected", intent_push(&in) == INTENT_REJECT_DUPLICATE);
	check("dup: one applied", intent_consume(2) == 1);
	check("dup: applied exactly once", o_n == 1 && o_a[0] == 4.0);

	/* C. invalid / bad player / foreign target rejected, no effect. */
	reset_all();
	in = mk(1, 0, 1, INTENT_NONE, 0, 0);
	check("invalid op rejected", intent_push(&in) == INTENT_REJECT_BADOP);
	in = mk(1, 5, 1, INTENT_DRIVE_REL, 1, 0);
	check("bad player rejected", intent_push(&in) == INTENT_REJECT_BADPLAYER);
	in = mk(1, 0, 2, INTENT_AIM_TARGET, 9, 0);
	check("foreign aim target rejected", intent_push(&in) ==
	      INTENT_REJECT_FOREIGN);
	in = mk(1, 0, 3, INTENT_CHASE, 5, 0);
	check("foreign chase target rejected", intent_push(&in) ==
	      INTENT_REJECT_FOREIGN);
	check("invalid batch: nothing queued", intent_queued() == 0);
	check("invalid batch: consume applies nothing", intent_consume(1) == 0);
	check("invalid batch: no state effect", o_n == 0);

	/* D. stale (already consumed) tick is rejected. */
	reset_all();
	(void) intent_consume(10);
	in = mk(9, 0, 1, INTENT_DRIVE_REL, 1, 0);
	check("stale tick rejected", intent_push(&in) == INTENT_REJECT_STALE);
	in = mk(10, 0, 2, INTENT_DRIVE_REL, 1, 0);
	check("current tick accepted", intent_push(&in) == INTENT_OK);
	check("current tick applied", intent_consume(10) == 1);

	/* E. explicit bounded overflow rejection. */
	reset_all();
	for (i = 0; i < max; i++) {
		in = mk(100, i % 2, (unsigned int) (i + 1), INTENT_DRIVE_REL, 0, 0);
		st = intent_push(&in);
		if (st != INTENT_OK) {
			check("overflow: fill rejected early", 0);
			break;
		}
	}
	check("overflow: queue full", intent_queued() == max);
	in = mk(100, 0, 99999, INTENT_DRIVE_REL, 0, 0);
	check("overflow: extra rejected", intent_push(&in) == INTENT_REJECT_FULL);
	check("overflow: full queue unchanged", intent_queued() == max);

	/* F. held fire persists until released. */
	reset_all();
	in = mk(1, 0, 1, INTENT_FIRE_HOLD, 1, 0);
	check("held fire: hold accepted", intent_push(&in) == INTENT_OK);
	check("held fire: fires on tick 1", intent_consume(1) == 1 && o_n == 1);
	check("held fire: fires on tick 2", intent_consume(2) == 0 && o_n == 2);
	in = mk(3, 0, 2, INTENT_FIRE_HOLD, 0, 0);
	check("held fire: release accepted", intent_push(&in) == INTENT_OK);
	(void) intent_consume(3);
	check("held fire: released without firing", o_n == 2);
	(void) intent_consume(4);
	check("held fire: stays released", o_n == 2);
	check("held fire: state cleared", intent_held_fire(0) == 0);

	/* G. held aim re-applies every tick until changed. */
	reset_all();
	in = mk(1, 0, 1, INTENT_AIM_TARGET, 1, 0);
	check("held aim: target accepted", intent_push(&in) == INTENT_OK);
	check("held aim: applied on issue tick", intent_consume(1) == 1 && o_n == 1);
	check("held aim: re-applied tick 2", intent_consume(2) == 0 && o_n == 2);
	check("held aim: re-applied tick 3", intent_consume(3) == 0 && o_n == 3);
	check("held aim: state recorded", intent_held_aim(0) == 1);

	/* H. reset clears queued and held input. */
	reset_all();
	in = mk(1, 0, 1, INTENT_FIRE_HOLD, 1, 0);
	(void) intent_push(&in);
	in = mk(5, 0, 2, INTENT_DRIVE_REL, 2, 0);
	(void) intent_push(&in);
	(void) intent_consume(1);
	intent_reset();
	check("reset: queue cleared", intent_queued() == 0);
	check("reset: held fire cleared", intent_held_fire(0) == 0);
	check("reset: held aim cleared", intent_held_aim(0) == -1);
	o_n = 0;
	check("reset: consume(5) empty", intent_consume(5) == 0 && o_n == 0);

	/* I. stable player order, then sequence. */
	reset_all();
	in = mk(1, 1, 2, INTENT_DRIVE_REL, 4, 0);
	(void) intent_push(&in);
	in = mk(1, 0, 2, INTENT_DRIVE_REL, 2, 0);
	(void) intent_push(&in);
	in = mk(1, 1, 1, INTENT_DRIVE_REL, 3, 0);
	(void) intent_push(&in);
	in = mk(1, 0, 1, INTENT_DRIVE_REL, 1, 0);
	(void) intent_push(&in);
	check("order: four applied", intent_consume(1) == 4);
	check("order: player0 seq1 first",
	      o_player[0] == 0 && o_a[0] == 1.0);
	check("order: player0 seq2 second",
	      o_player[1] == 0 && o_a[1] == 2.0);
	check("order: player1 seq1 third",
	      o_player[2] == 1 && o_a[2] == 3.0);
	check("order: player1 seq2 fourth",
	      o_player[3] == 1 && o_a[3] == 4.0);

	/* J. one-shot op dispatch. */
	reset_all();
	in = mk(1, 0, 1, INTENT_DRIVE_ABS, 2.5, 0);   (void) intent_push(&in);
	in = mk(1, 0, 2, INTENT_HEADING, 1.5, 0);     (void) intent_push(&in);
	in = mk(1, 0, 3, INTENT_AIM_WORLD, 100, 200); (void) intent_push(&in);
	in = mk(1, 0, 4, INTENT_WEAPON, 0, INTENT_WEAPON_ON);   (void) intent_push(&in);
	in = mk(1, 0, 5, INTENT_WEAPON, 1, INTENT_WEAPON_OFF);  (void) intent_push(&in);
	in = mk(1, 0, 6, INTENT_WEAPON, 2, INTENT_WEAPON_FIRE); (void) intent_push(&in);
	in = mk(1, 0, 7, INTENT_FIRE, 0, 0);          (void) intent_push(&in);
	check("dispatch: seven applied", intent_consume(1) == 7);
	check("dispatch: drive abs", o_op[0] == O_DRIVE_ABS && o_a[0] == 2.5);
	check("dispatch: heading", o_op[1] == O_TURN && o_a[1] == 1.5);
	check("dispatch: aim world", o_op[2] == O_AIM && o_a[2] == 100.0 &&
	      o_b[2] == 200.0);
	check("dispatch: weapon on", o_op[3] == O_WEAPON_ON && o_a[3] == 0.0);
	check("dispatch: weapon off", o_op[4] == O_WEAPON_OFF && o_a[4] == 1.0);
	check("dispatch: weapon fire", o_op[5] == O_WEAPON_FIRE && o_a[5] == 2.0);
	check("dispatch: fire", o_op[6] == O_FIRE);

	if (failures != 0) {
		printf("INTENT FIXTURE FAIL (%d checks)\n", failures);
		return 1;
	}
	printf("INTENT FIXTURE PASS\n");
	return 0;
}
FIXTURE

log "== compiling intent.c + fixture"
# shellcheck disable=SC2086
gcc -O0 -g -w $DEFS -I"$REPO/Src" -I"$REPO/Src/Include" \
    -o "$WORK/intent_fixture" "$REPO/Src/intent.c" \
    "$WORK/intent_fixture.c" -lm || fail "fixture did not build"

log "== running intent-queue assertions"
if ! "$WORK/intent_fixture" > "$WORK/intent_fixture.out" 2>&1; then
	cat "$WORK/intent_fixture.out" >&2
	fail "intent-queue assertions failed"
fi
cat "$WORK/intent_fixture.out"

log ""
log "P1.7 INTENT PASS"
