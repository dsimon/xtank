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

/*
** P3.8 retirement ring.  `pub` is the wire DTO; the sidecar carries the
** export-only occurrence location used by the recipient authorization table
** (EXPLOSION / RESPAWN).  `next_ordinal` is the monotonic per-epoch ordinal
** of the next event; `event_count` is the number of retained entries, so the
** oldest retained ordinal is next_ordinal - event_count.  A full ring
** overwrites (retires) its oldest entry.
*/
typedef struct {
	XtkEvent pub;
	uint8_t has_loc;
	double lx, ly;
} ServerEventEntry;

static ServerEventEntry ring[SERVER_EVENTS_RING];
static int event_count;
static uint32_t next_ordinal;

static XtkResults results;
static int results_valid;

/* ---- helpers ---------------------------------------------------------- */

static ServerEventEntry *
emit(uint8_t type, uint8_t kind, uint16_t id, uint32_t gen,
     uint32_t aux0, uint32_t aux1, const char *text)
{
	ServerEventEntry *ent;
	XtkEvent *e;
	uint32_t ordinal = next_ordinal++;

	/* Ring slot for this ordinal; a full ring retires its oldest entry. */
	ent = &ring[ordinal % SERVER_EVENTS_RING];
	(void) memset(ent, 0, sizeof(*ent));
	if (event_count < SERVER_EVENTS_RING)
		event_count++;

	e = &ent->pub;
	e->event_sequence = ordinal;	/* per-epoch capture ordinal */
	e->capture_tick = (uint32_t) frame;	/* occurrence tick */
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
	return ent;
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
	(void) memset(ring, 0, sizeof(ring));
	event_count = 0;
	next_ordinal = 0;
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
	ServerEventEntry *ent;
	Vehicle *victim;
	double x = 0.0, y = 0.0;
	uint8_t has_loc = 0;

	if (victim_slot < 0 || victim_slot >= MAX_VEHICLES)
		return;

	/*
	 * Export-side occurrence location (read-only): the victim's death-point
	 * Loc while it is still valid.  Used only for recipient authorization.
	 */
	victim = combatant[victim_slot].vehicle;
	if (victim != (Vehicle *) NULL && victim->loc != (Loc *) NULL) {
		x = (double) victim->loc->x;
		y = (double) victim->loc->y;
		has_loc = 1;
	}

	ent = emit(XTK_EV_DEATH, XTK_ENTITY_VEHICLE, (uint16_t) victim_slot,
		   veh_gen[victim_slot],
		   (killer_slot >= 0) ? (uint32_t) killer_slot : UINT32_MAX, 0,
		   NULL);
	ent->has_loc = has_loc;
	ent->lx = x;
	ent->ly = y;
}

void
server_events_note_spawn(int slot, double x, double y)
{
	ServerEventEntry *ent;

	if (slot < 0 || slot >= MAX_VEHICLES)
		return;

	/* Dedicated export-side lifetime counter: every new life advances it. */
	veh_gen[slot]++;

	if (veh_seen[slot]) {
		ent = emit(XTK_EV_RESPAWN, XTK_ENTITY_VEHICLE, (uint16_t) slot,
			   veh_gen[slot], (uint32_t) x, (uint32_t) y, NULL);
		ent->has_loc = 1;
		ent->lx = x;
		ent->ly = y;
	}
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
		/* Full-width u32 entity-life generation (D3-C). */
		re->generation = veh_gen[c->number];
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
			ServerEventEntry *ent;

			exp_gen[idx]++;		/* new transient */
			ent = emit(XTK_EV_EXPLOSION, XTK_ENTITY_EXPLOSION,
				   (uint16_t) idx, exp_gen[idx],
				   (uint32_t) explosion_kind(e), 0, NULL);
			ent->has_loc = 1;
			ent->lx = (double) e->x;
			ent->ly = (double) e->y;
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
	uint32_t first;

	if (index < 0 || index >= event_count)
		return (const XtkEvent *) NULL;
	first = next_ordinal - (uint32_t) event_count;
	return &ring[(first + (uint32_t) index) % SERVER_EVENTS_RING].pub;
}

uint32_t
server_events_first_ordinal(void)
{
	return next_ordinal - (uint32_t) event_count;
}

uint32_t
server_events_next_ordinal(void)
{
	return next_ordinal;
}

const XtkEvent *
server_events_event_at(uint32_t ordinal)
{
	if (ordinal < server_events_first_ordinal() || ordinal >= next_ordinal)
		return (const XtkEvent *) NULL;
	return &ring[ordinal % SERVER_EVENTS_RING].pub;
}

/*
** The reference animation viewport (docs/VIEW-v1.md section 3), duplicated
** read-only from the exporter so recipient authorization uses the identical
** half-open world-space rule.
*/
static int
event_in_view(double own_x, double own_y, double x, double y)
{
	double half = (double) (ANIM_WIN_WIDTH / 2);
	double dx = x - own_x;
	double dy = y - own_y;

	return dx >= -half && dx < half && dy >= -half && dy < half;
}

int
server_events_authorized(uint32_t ordinal, int own_slot,
			 double own_x, double own_y)
{
	const ServerEventEntry *ent;
	const XtkEvent *e;

	if (ordinal < server_events_first_ordinal() || ordinal >= next_ordinal)
		return 0;
	ent = &ring[ordinal % SERVER_EVENTS_RING];
	e = &ent->pub;

	switch (e->event_type) {
	  case XTK_EV_SCORE:
		/* SELF: own score only, remote score is public only at RESULTS. */
		return own_slot >= 0 && (int) e->entity_id == own_slot;

	  case XTK_EV_DEATH:
		/* Participants: the victim and (when present) the killer. */
		if (own_slot >= 0 && (int) e->entity_id == own_slot)
			return 1;
		return own_slot >= 0 && e->aux0 != UINT32_MAX &&
		       (int) e->aux0 == own_slot;

	  case XTK_EV_RESPAWN:
		/* Own respawn always; a remote spawn only inside the viewport. */
		if (own_slot >= 0 && (int) e->entity_id == own_slot)
			return 1;
		return ent->has_loc &&
		       event_in_view(own_x, own_y, ent->lx, ent->ly);

	  case XTK_EV_EXPLOSION:
		/* A transient is visible only inside the reference viewport. */
		return ent->has_loc &&
		       event_in_view(own_x, own_y, ent->lx, ent->ly);

	  case XTK_EV_MATCH_END:
		/* Public winner/result marker. */
		return 1;

	  default:
		return 0;
	}
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
