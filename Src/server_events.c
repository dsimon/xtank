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
** server_events.c -- generation-safe entity lifetimes, ticked events and
** immutable match results (P2.6).  See Src/Include/server_events.h.
**
** Strictly read-only with respect to the simulation except for the (read-only)
** transient hooks, which append to this module's own bounded buffers.  It never
** calls a draw callback, a special model action, the RNG or a control function,
** and its observation step is idempotent within a frame, so exporting zero,
** once or many times leaves the simulation byte-identical.
*/

#include <string.h>

#include "xtank.h"
#include "team.h"
#include "vehicle.h"
#include "bullet.h"
#include "object.h"
#include "globals.h"
#include "server_events.h"

/* Simulation globals read (never written) by this module. */
extern int frame;
extern int num_combatants;
extern Combatant combatant[];
extern Bset *bset;
extern Eset *eset;

/* ---- export-side state ------------------------------------------------ */

static uint32_t cur_epoch;

/* Monotonic entity-lifetime generations (never reset across epochs). */
static uint32_t veh_gen[MAX_VEHICLES];
static uint32_t bullet_gen[MAX_BULLETS];
static uint32_t exp_gen[MAX_EXPS];

/* Per-epoch observation baselines. */
static uint8_t veh_seen[MAX_VEHICLES];
static uint8_t bullet_present[MAX_BULLETS];
static unsigned long bullet_sig[MAX_BULLETS];
static uint8_t exp_present[MAX_EXPS];
static unsigned long exp_sig[MAX_EXPS];
static int score_seen[MAX_VEHICLES];
static uint8_t score_valid[MAX_VEHICLES];

static int have_frame;
static int last_frame;

static XtkEvent events[SERVER_EVENTS_MAX];
static int event_count;
static uint32_t next_seq;

static XtkResults results;
static int results_valid;

/* ---- helpers ---------------------------------------------------------- */

static void
emit(uint8_t type, uint8_t kind, uint16_t id, uint32_t gen,
     uint32_t aux0, uint32_t aux1, const char *text)
{
	XtkEvent *e;

	if (event_count >= SERVER_EVENTS_MAX)
		return;			/* bounded: drop beyond the ring */

	e = &events[event_count++];
	(void) memset(e, 0, sizeof(*e));
	e->event_sequence = next_seq++;
	e->event_type = type;
	e->entity_kind = kind;
	e->entity_id = id;
	e->entity_generation = gen;
	e->aux0 = aux0;
	e->aux1 = aux1;
	if (text != NULL) {
		(void) strncpy(e->text, text, XTK_MAX_NAME_BYTES);
		e->text[XTK_MAX_NAME_BYTES] = '\0';
	}
}

static uint8_t
explosion_kind(const Exp *e)
{
	int i;

	for (i = 0; i < num_exp_objs; i++)
		if (exp_obj[i] == e->obj)
			return (uint8_t) i;
	return 0xFF;
}

/* ---- lifecycle -------------------------------------------------------- */

void
server_events_reset(uint32_t epoch, uint32_t first_tick)
{
	cur_epoch = epoch;
	(void) first_tick;

	(void) memset(veh_seen, 0, sizeof(veh_seen));
	(void) memset(bullet_present, 0, sizeof(bullet_present));
	(void) memset(bullet_sig, 0, sizeof(bullet_sig));
	(void) memset(exp_present, 0, sizeof(exp_present));
	(void) memset(exp_sig, 0, sizeof(exp_sig));
	(void) memset(score_valid, 0, sizeof(score_valid));

	have_frame = 0;
	last_frame = 0;
	event_count = 0;
	next_seq = 0;
	results_valid = 0;
	(void) memset(&results, 0, sizeof(results));
}

uint32_t
server_events_epoch(void)
{
	return cur_epoch;
}

/* ---- reviewed read-only transient hooks ------------------------------- */

void
server_events_note_death(int victim_slot, int killer_slot)
{
	if (victim_slot < 0 || victim_slot >= MAX_VEHICLES)
		return;

	emit(XTK_EV_DEATH, XTK_ENTITY_VEHICLE, (uint16_t) victim_slot,
	     veh_gen[victim_slot],
	     (killer_slot >= 0) ? (uint32_t) killer_slot : UINT32_MAX, 0, NULL);
}

void
server_events_note_spawn(int slot, double x, double y)
{
	if (slot < 0 || slot >= MAX_VEHICLES)
		return;

	/* Dedicated export-side lifetime counter: every new life advances it. */
	veh_gen[slot]++;

	if (veh_seen[slot])
		emit(XTK_EV_RESPAWN, XTK_ENTITY_VEHICLE, (uint16_t) slot,
		     veh_gen[slot], (uint32_t) x, (uint32_t) y, NULL);
	veh_seen[slot] = 1;
}

void
server_events_capture_results(int status, int winner_slot)
{
	int i;

	(void) memset(&results, 0, sizeof(results));
	results.end_tick = (uint32_t) frame;
	results.result_kind = (status == GAME_RESET && winner_slot >= 0) ? 0 : 1;
	results.winner_slot = (winner_slot >= 0 && winner_slot <= 0xFF) ?
		(uint8_t) winner_slot : 0xFF;

	for (i = 0; i < num_combatants && i < (int) XTK_MAX_PLAYERS; i++) {
		Combatant *c = &combatant[i];
		XtkResultEntry *re = &results.entry[results.vehicle_count];
		size_t len = strnlen(c->name, MAX_STRING);

		if (len > XTK_MAX_NAME_BYTES)
			len = XTK_MAX_NAME_BYTES;
		re->slot = (uint8_t) c->number;
		re->generation = (uint8_t) veh_gen[c->number];
		re->score = (uint32_t) c->score;
		(void) memset(re->name, 0, sizeof(re->name));
		(void) memcpy(re->name, c->name, len);
		results.vehicle_count++;
	}
	results_valid = 1;

	emit(XTK_EV_MATCH_END, XTK_ENTITY_VEHICLE,
	     (uint16_t) ((winner_slot >= 0) ? winner_slot : 0),
	     (winner_slot >= 0) ? veh_gen[winner_slot] : 0,
	     (winner_slot >= 0) ? (uint32_t) winner_slot : UINT32_MAX, 0, NULL);
}

/* ---- observation ------------------------------------------------------ */

static void
observe_scores(void)
{
	int i;

	for (i = 0; i < num_combatants && i < MAX_VEHICLES; i++) {
		int slot = combatant[i].number;

		if (slot < 0 || slot >= MAX_VEHICLES)
			continue;
		if (score_valid[slot] && combatant[i].score != score_seen[slot]) {
			int delta = combatant[i].score - score_seen[slot];

			emit(XTK_EV_SCORE, XTK_ENTITY_VEHICLE, (uint16_t) slot,
			     veh_gen[slot], (uint32_t) delta,
			     (uint32_t) combatant[i].score, NULL);
		}
		score_seen[slot] = combatant[i].score;
		score_valid[slot] = 1;
	}
}

static void
observe_bullets(void)
{
	unsigned char seen[MAX_BULLETS];
	int i;

	(void) memset(seen, 0, sizeof(seen));
	for (i = 0; i < bset->number; i++) {
		Bullet *b = bset->list[i];
		unsigned int idx = (unsigned int) (b - bset->array);
		unsigned long sig;

		if (idx >= MAX_BULLETS)
			continue;
		sig = (unsigned long) (unsigned) b->type ^
		    ((unsigned long) (unsigned)
		     (b->owner != NULL ? b->owner->number + 1 : 0) << 8);
		seen[idx] = 1;
		if (!bullet_present[idx] || bullet_sig[idx] != sig)
			bullet_gen[idx]++;	/* new life in a reused slot */
		bullet_present[idx] = 1;
		bullet_sig[idx] = sig;
	}
	for (i = 0; i < MAX_BULLETS; i++)
		if (!seen[i])
			bullet_present[i] = 0;
}

static void
observe_explosions(void)
{
	unsigned char seen[MAX_EXPS];
	int i;

	(void) memset(seen, 0, sizeof(seen));
	for (i = 0; i < eset->number; i++) {
		Exp *e = eset->list[i];
		unsigned int idx = (unsigned int) (e - eset->array);
		unsigned long sig;

		if (idx >= MAX_EXPS)
			continue;
		sig = (unsigned long) (unsigned) (e->obj != NULL ?
		    e->obj->num_pics : 0) ^
		    ((unsigned long) (unsigned) e->color << 8);
		seen[idx] = 1;
		if (!exp_present[idx] || exp_sig[idx] != sig) {
			exp_gen[idx]++;		/* new transient */
			emit(XTK_EV_EXPLOSION, XTK_ENTITY_EXPLOSION,
			     (uint16_t) idx, exp_gen[idx],
			     (uint32_t) explosion_kind(e), 0, NULL);
		}
		exp_present[idx] = 1;
		exp_sig[idx] = sig;
	}
	for (i = 0; i < MAX_EXPS; i++)
		if (!seen[i])
			exp_present[i] = 0;
}

void
server_events_observe(void)
{
	if (have_frame && last_frame == frame)
		return;			/* idempotent within one tick */
	have_frame = 1;
	last_frame = frame;

	observe_scores();
	observe_bullets();
	observe_explosions();
}

/* ---- accessors -------------------------------------------------------- */

uint32_t
server_events_generation(uint8_t kind, uint16_t id)
{
	switch (kind) {
	  case XTK_ENTITY_VEHICLE:
		return (id < MAX_VEHICLES) ? veh_gen[id] : 0u;
	  case XTK_ENTITY_BULLET:
		return (id < MAX_BULLETS) ? bullet_gen[id] : 0u;
	  case XTK_ENTITY_EXPLOSION:
		return (id < MAX_EXPS) ? exp_gen[id] : 0u;
	  default:
		return 0u;
	}
}

int
server_events_count(void)
{
	return event_count;
}

const XtkEvent *
server_events_event(int index)
{
	if (index < 0 || index >= event_count)
		return (const XtkEvent *) NULL;
	return &events[index];
}

int
server_events_results_valid(void)
{
	return results_valid;
}

int
server_events_results(XtkResults *out)
{
	if (!results_valid)
		return SERVER_EVENTS_ERR_NO_RESULTS;
	if (out == NULL)
		return SERVER_EVENTS_ERR_RANGE;
	*out = results;
	return SERVER_EVENTS_OK;
}
