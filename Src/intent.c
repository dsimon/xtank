/*-
 * Copyright (c) 1988 Terry Donahue
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
** intent.c -- bounded intent queue and the candidate-side legacy adapter
** (P1.7).
**
** The queue applies each intent exactly once, at the legacy input phase:
** held aim/chase/standoff first, then the tick's queued commands in stable
** (player, sequence) order, then held fire.  That matches the order the
** frozen reference's independent replay (Tests/phase1/reference_input.c)
** drives the legacy controls, so the candidate moves identically without
** sharing that implementation.
**
** When built for the oracle (-DSTATE_DUMP) this module also supplies the
** XTANK_TRACE adapter that turns the frozen bot-free traces into tagged
** intents.  The reference compiles reference_input.c instead; the candidate
** never links it.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "xtank.h"
#include "vehicle.h"
#include "globals.h"
#include "lowlib.h"
#include "proto.h"
#include "server.h"
#include "intent.h"

extern Vehicle actual_vehicles[];
extern int num_veh;
extern int num_veh_alive;

/* ------------------------------------------------------------------ */
/* Bounded queue                                                       */
/* ------------------------------------------------------------------ */
static Intent queue[INTENT_QUEUE_MAX];
static int qcount;
static int cur_tick;

/* Held controls, one entry per controller/tank. */
static int held_aim[INTENT_MAX_PLAYERS];
static int held_chase[INTENT_MAX_PLAYERS];
static double held_chase_drive[INTENT_MAX_PLAYERS];
static int held_standoff[INTENT_MAX_PLAYERS];
static double held_standoff_dist[INTENT_MAX_PLAYERS];
static int held_fire[INTENT_MAX_PLAYERS];

/* Anti-wedge bookkeeping for chase/standoff, indexed by vehicle number. */
static int dw_valid[MAX_VEHICLES];
static double dw_px[MAX_VEHICLES];
static double dw_py[MAX_VEHICLES];
static int dw_stuck[MAX_VEHICLES];

void
intent_reset(void)
{
	int i;

	qcount = 0;
	cur_tick = 0;
	for (i = 0; i < INTENT_MAX_PLAYERS; i++) {
		held_aim[i] = -1;
		held_chase[i] = -1;
		held_chase_drive[i] = 9.0;
		held_standoff[i] = -1;
		held_standoff_dist[i] = 200.0;
		held_fire[i] = 0;
	}
	for (i = 0; i < MAX_VEHICLES; i++) {
		dw_valid[i] = 0;
		dw_stuck[i] = 0;
		dw_px[i] = 0.0;
		dw_py[i] = 0.0;
	}
}

void
intent_init(void)
{
	intent_reset();
}

/*
** Reject an intent that is malformed, belongs to no controller, or names a
** target that is not a vehicle in this world.  Held-release values (-1) are
** valid and mean "stop applying".
*/
static int
intent_check(const Intent *in)
{
	if (in->player < 0 || in->player >= num_veh)
		return INTENT_REJECT_BADPLAYER;

	switch (in->op) {
	  case INTENT_DRIVE_REL:
	  case INTENT_DRIVE_ABS:
	  case INTENT_HEADING:
	  case INTENT_AIM_WORLD:
	  case INTENT_FIRE:
	  case INTENT_FIRE_HOLD:
		return INTENT_OK;
	  case INTENT_WEAPON:
		if ((int) in->a < 0 || (int) in->a >= MAX_WEAPONS)
			return INTENT_REJECT_BADOP;
		if ((int) in->b < INTENT_WEAPON_ON ||
		    (int) in->b > INTENT_WEAPON_FIRE)
			return INTENT_REJECT_BADOP;
		return INTENT_OK;
	  case INTENT_AIM_TARGET:
	  case INTENT_CHASE:
	  case INTENT_STANDOFF:
		if ((int) in->a < -1 || (int) in->a >= num_veh)
			return INTENT_REJECT_FOREIGN;
		return INTENT_OK;
	  default:
		return INTENT_REJECT_BADOP;
	}
}

int
intent_push(const Intent *in)
{
	int i, rc;

	if (in == (const Intent *) NULL)
		return INTENT_REJECT_BADOP;

	rc = intent_check(in);
	if (rc != INTENT_OK)
		return rc;

	if (in->tick < cur_tick)
		return INTENT_REJECT_STALE;

	for (i = 0; i < qcount; i++)
		if (queue[i].tick == in->tick &&
		    queue[i].player == in->player &&
		    queue[i].sequence == in->sequence)
			return INTENT_REJECT_DUPLICATE;

	if (qcount >= INTENT_QUEUE_MAX)
		return INTENT_REJECT_FULL;

	queue[qcount++] = *in;
	return INTENT_OK;
}

int
intent_pending(int tick)
{
	int i, n = 0;

	for (i = 0; i < qcount; i++)
		if (queue[i].tick == tick)
			n++;
	return n;
}

int
intent_queued(void)
{
	return qcount;
}

int
intent_held_fire(int player)
{
	if (player < 0 || player >= INTENT_MAX_PLAYERS)
		return 0;
	return held_fire[player];
}

int
intent_held_aim(int player)
{
	if (player < 0 || player >= INTENT_MAX_PLAYERS)
		return -1;
	return held_aim[player];
}

/* ------------------------------------------------------------------ */
/* Applying intents through the legacy controls                        */
/* ------------------------------------------------------------------ */
static Vehicle *
intent_vehicle(int player)
{
	return lowlib_actual_vehicle(player);
}

/* Chase/standoff get "unstuck": if the tank has barely moved for a while, add
** a small sideways bias so two tanks pressed together do not deadlock.  This
** mirrors the reference replay's arithmetic exactly. */
static void
intent_dewedge(Vehicle *v, double *ang)
{
	int num = v->number;
	double moved;

	if (!dw_valid[num]) {
		dw_px[num] = v->loc->x;
		dw_py[num] = v->loc->y;
		dw_valid[num] = 1;
		moved = 999.0;
	} else {
		double ddx = v->loc->x - dw_px[num];
		double ddy = v->loc->y - dw_py[num];

		moved = sqrt(ddx * ddx + ddy * ddy);
		dw_px[num] = v->loc->x;
		dw_py[num] = v->loc->y;
	}
	if (moved < 0.5)
		dw_stuck[num]++;
	else
		dw_stuck[num] = 0;
	if (dw_stuck[num] > 12)
		*ang += (num & 1) ? 1.0 : -1.0;
}

static void
intent_aim_target(int player, int target)
{
	Vehicle *v = intent_vehicle(player);
	Vehicle *t = intent_vehicle(target);

	if (v == (Vehicle *) NULL || t == (Vehicle *) NULL)
		return;
	action_aim_world(v, t->loc->x, t->loc->y);
}

static void
intent_chase(int player, int target, double drive)
{
	Vehicle *v = intent_vehicle(player);
	Vehicle *t = intent_vehicle(target);
	double dx, dy, ang;

	if (v == (Vehicle *) NULL || t == (Vehicle *) NULL)
		return;
	set_current_vehicle(v);
	dx = t->loc->x - v->loc->x;
	dy = t->loc->y - v->loc->y;
	ang = atan2(dy, dx);
	if (drive > 0.0)
		intent_dewedge(v, &ang);
	turn_vehicle_human(ang);
	set_rel_drive(drive);
}

static void
intent_standoff(int player, int target, double want)
{
	Vehicle *v = intent_vehicle(player);
	Vehicle *t = intent_vehicle(target);
	double dx, dy, d, ang, desired;

	if (v == (Vehicle *) NULL || t == (Vehicle *) NULL)
		return;
	set_current_vehicle(v);
	dx = t->loc->x - v->loc->x;
	dy = t->loc->y - v->loc->y;
	d = sqrt(dx * dx + dy * dy);
	ang = atan2(dy, dx);
	if (d > want + 40.0)
		desired = 9.0;
	else if (d < want - 40.0)
		desired = -9.0;
	else
		desired = 0.0;

	if (desired > 0.0)
		intent_dewedge(v, &ang);
	else
		dw_stuck[v->number] = 0;
	turn_vehicle_human(ang);
	set_rel_drive(desired);
}

static void
intent_apply(const Intent *in)
{
	int p = in->player;
	Vehicle *v = intent_vehicle(p);

	if (v == (Vehicle *) NULL)
		return;

	switch (in->op) {
	  case INTENT_DRIVE_REL:
		set_current_vehicle(v);
		set_rel_drive(in->a);
		break;
	  case INTENT_DRIVE_ABS:
		set_current_vehicle(v);
		set_abs_drive(in->a);
		break;
	  case INTENT_HEADING:
		set_current_vehicle(v);
		turn_vehicle_human(in->a);
		break;
	  case INTENT_AIM_WORLD:
		action_aim_world(v, in->a, in->b);
		break;
	  case INTENT_AIM_TARGET:
		held_aim[p] = (int) in->a;
		if (held_aim[p] >= 0 && held_aim[p] < num_veh)
			intent_aim_target(p, held_aim[p]);
		break;
	  case INTENT_FIRE:
		action_fire_all(v);
		break;
	  case INTENT_FIRE_HOLD:
		held_fire[p] = (in->a != 0.0);
		break;
	  case INTENT_WEAPON:
		set_current_vehicle(v);
		if ((int) in->b == INTENT_WEAPON_ON)
			turn_on_weapon((int) in->a);
		else if ((int) in->b == INTENT_WEAPON_OFF)
			turn_off_weapon((int) in->a);
		else
			anim_fire_weapon((int) in->a);
		break;
	  case INTENT_CHASE:
		held_chase[p] = (int) in->a;
		held_chase_drive[p] = in->b;
		if (held_chase[p] >= 0 && held_chase[p] < num_veh)
			intent_chase(p, held_chase[p], held_chase_drive[p]);
		break;
	  case INTENT_STANDOFF:
		held_standoff[p] = (int) in->a;
		held_standoff_dist[p] = in->b;
		if (held_standoff[p] >= 0 && held_standoff[p] < num_veh)
			intent_standoff(p, held_standoff[p],
					held_standoff_dist[p]);
		break;
	  default:
		break;
	}
}

int
intent_consume(int tick)
{
	int idx[INTENT_QUEUE_MAX];
	int n = 0;
	int applied = 0;
	int i, j;

	/* Collect this tick's intents (queue order). */
	for (i = 0; i < qcount; i++)
		if (queue[i].tick == tick)
			idx[n++] = i;

	/* Stable insertion sort into (player, sequence) order. */
	for (i = 1; i < n; i++) {
		int key = idx[i];

		j = i - 1;
		while (j >= 0 &&
		       (queue[idx[j]].player > queue[key].player ||
			(queue[idx[j]].player == queue[key].player &&
			 queue[idx[j]].sequence > queue[key].sequence))) {
			idx[j + 1] = idx[j];
			j--;
		}
		idx[j + 1] = key;
	}

	/* Held aim, then held chase, then held standoff. */
	for (i = 0; i < num_veh; i++)
		if (held_aim[i] >= 0 && held_aim[i] < num_veh)
			intent_aim_target(i, held_aim[i]);
	for (i = 0; i < num_veh; i++)
		if (held_chase[i] >= 0 && held_chase[i] < num_veh)
			intent_chase(i, held_chase[i], held_chase_drive[i]);
	for (i = 0; i < num_veh; i++)
		if (held_standoff[i] >= 0 && held_standoff[i] < num_veh)
			intent_standoff(i, held_standoff[i],
					held_standoff_dist[i]);

	/* This tick's queued commands. */
	for (i = 0; i < n; i++) {
		intent_apply(&queue[idx[i]]);
		applied++;
	}

	/* Held fire last, in player order, for live tanks only.  Held fire is
	   not a queued intent, so it does not contribute to the return value. */
	for (i = 0; i < num_veh; i++)
		if (held_fire[i] && i < num_veh_alive) {
			Vehicle *v = intent_vehicle(i);

			if (v != (Vehicle *) NULL)
				action_fire_all(v);
		}

	/* Drop the consumed intents; later ticks stay queued. */
	{
		int w = 0;

		for (i = 0; i < qcount; i++)
			if (queue[i].tick != tick)
				queue[w++] = queue[i];
		qcount = w;
	}

	if (tick > cur_tick)
		cur_tick = tick;
	return applied;
}

/* ------------------------------------------------------------------ */
/* Candidate-side legacy adapter (oracle builds only)                  */
/* ------------------------------------------------------------------ */
#ifdef STATE_DUMP

#define AD_MAX_LINES	8192
#define AD_LINE_LEN	256

static char *ad_lines[AD_MAX_LINES];
static int ad_nlines = -1;
static int ad_loaded;

static void
ad_load(void)
{
	char *path = getenv("XTANK_TRACE");
	FILE *fp;
	char buf[AD_LINE_LEN];
	int n = 0;

	ad_loaded = 1;
	ad_nlines = 0;
	if (path == (char *) NULL)
		return;
	fp = fopen(path, "r");
	if (fp == (FILE *) NULL)
		return;
	while (n < AD_MAX_LINES && fgets(buf, sizeof(buf), fp) != NULL) {
		char *p = buf;
		char *end;

		while (*p == ' ' || *p == '\t')
			p++;
		if (*p == '#' || *p == '\n' || *p == '\0')
			continue;
		end = p + strlen(p);
		while (end > p && (end[-1] == '\n' || end[-1] == '\r'))
			*--end = '\0';
		ad_lines[n] = strdup(p);
		n++;
	}
	fclose(fp);
	ad_nlines = n;
}

/*
** Translate the frozen trace vocabulary into tagged intents.  The reference
** keeps its own independent copy of this mapping; the two are compared only
** through the simulation dump.
*/
static void
ad_push_all(void)
{
	int i;

	for (i = 0; i < ad_nlines; i++) {
		char buf[AD_LINE_LEN];
		char *tok, *save;
		Intent in;

		(void) strncpy(buf, ad_lines[i], sizeof(buf) - 1);
		buf[sizeof(buf) - 1] = '\0';
		tok = strtok_r(buf, " \t", &save);
		if (tok == (char *) NULL)
			continue;
		(void) memset((char *) &in, 0, sizeof(in));
		in.tick = atoi(tok);
		in.sequence = (unsigned int) i;

		tok = strtok_r(NULL, " \t", &save);
		if (tok == (char *) NULL)
			continue;
		in.player = atoi(tok);

		tok = strtok_r(NULL, " \t", &save);
		if (tok == (char *) NULL)
			continue;
		if (strcmp(tok, "drive") == 0) {
			char *a = strtok_r(NULL, " \t", &save);

			in.op = INTENT_DRIVE_REL;
			in.a = a ? strtod(a, NULL) : 0.0;
		} else if (strcmp(tok, "driveabs") == 0) {
			char *a = strtok_r(NULL, " \t", &save);

			in.op = INTENT_DRIVE_ABS;
			in.a = a ? strtod(a, NULL) : 0.0;
		} else if (strcmp(tok, "turn") == 0) {
			char *a = strtok_r(NULL, " \t", &save);

			in.op = INTENT_HEADING;
			in.a = a ? strtod(a, NULL) : 0.0;
		} else if (strcmp(tok, "aimv") == 0) {
			char *a = strtok_r(NULL, " \t", &save);

			in.op = INTENT_AIM_TARGET;
			in.a = (a && strcmp(a, "none") != 0) ? atoi(a) : -1;
		} else if (strcmp(tok, "aim") == 0) {
			char *a = strtok_r(NULL, " \t", &save);
			char *b = strtok_r(NULL, " \t", &save);

			in.op = INTENT_AIM_WORLD;
			in.a = a ? strtod(a, NULL) : 0.0;
			in.b = b ? strtod(b, NULL) : 0.0;
		} else if (strcmp(tok, "fire") == 0) {
			in.op = INTENT_FIRE;
		} else if (strcmp(tok, "firehold") == 0) {
			char *a = strtok_r(NULL, " \t", &save);

			in.op = INTENT_FIRE_HOLD;
			in.a = (a && strcmp(a, "on") == 0) ? 1.0 : 0.0;
		} else if (strcmp(tok, "wep") == 0) {
			char *a = strtok_r(NULL, " \t", &save);
			char *b = strtok_r(NULL, " \t", &save);

			in.op = INTENT_WEAPON;
			in.a = a ? atoi(a) : -1;
			if (b && strcmp(b, "on") == 0)
				in.b = INTENT_WEAPON_ON;
			else if (b && strcmp(b, "off") == 0)
				in.b = INTENT_WEAPON_OFF;
			else
				in.b = INTENT_WEAPON_FIRE;
		} else if (strcmp(tok, "chase") == 0) {
			char *a = strtok_r(NULL, " \t", &save);
			char *b = strtok_r(NULL, " \t", &save);

			in.op = INTENT_CHASE;
			in.a = (a && strcmp(a, "none") != 0) ? atoi(a) : -1;
			in.b = b ? strtod(b, NULL) : 9.0;
		} else if (strcmp(tok, "standoff") == 0) {
			char *a = strtok_r(NULL, " \t", &save);
			char *b = strtok_r(NULL, " \t", &save);

			in.op = INTENT_STANDOFF;
			in.a = (a && strcmp(a, "none") != 0) ? atoi(a) : -1;
			in.b = b ? strtod(b, NULL) : 200.0;
		} else {
			continue;
		}
		(void) intent_push(&in);
	}
}

/*
** Oracle adapter: load the trace once, then at each epoch start (frame 1)
** reset the queue and enqueue every command tagged with its tick.  The
** separation from reference_input.c is deliberate -- the reference drives the
** legacy controls directly and never links this file.
*/
void
xtank_replay_run(int frame_no)
{
	if (!ad_loaded)
		ad_load();
	if (frame_no == 1) {
		intent_reset();
		ad_push_all();
	}
	/* Consume exactly this tick's intents at the pre-terminal input seam,
	   after bots and before movement. */
	(void) server_consume_intents(frame_no);
}

#endif /* STATE_DUMP */
