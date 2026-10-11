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
** server_view.c -- authorized read-only per-player view exporter (P2.5).
**
** Implements docs/VIEW-v1.md.  It derives the reference animation viewport and
** copies the authorized fields of the authoritative simulation records into the
** frozen protocol-v1 XtkViewSnapshot.  It is strictly read-only: no draw
** callback, special model action, RNG call or control/game-rule function is
** ever invoked, and no simulation value is written.  Call it after
** server_step() completes.
*/

#include <string.h>
#include <math.h>

#include "xtank.h"
#include "team.h"
#include "vehicle.h"
#include "bullet.h"
#include "object.h"
#include "special.h"
#include "globals.h"
#include "server_view.h"
#include "server_events.h"

/* Simulation globals read (never written) by the exporter. */
extern int frame;
extern int num_combatants;
extern Combatant combatant[];
extern Bset *bset;
extern Eset *eset;
extern Map real_map;

/* The vehicle-status bits that make up lifecycle_flags (bits 0..6 only). */
#define SERVER_LIFECYCLE_MASK \
	(VS_functioning | VS_is_alive | VS_was_alive | VS_disc_spin | \
	 VS_rel_turret | VS_sliding | VS_permanently_dead)

static uint16_t
clamp_u16_double(double v)
{
	if (v < 0.0)
		return 0;
	if (v > 65535.0)
		return 65535;
	return (uint16_t) v;
}

static uint16_t
clamp_u16_int(int v)
{
	if (v < 0)
		return 0;
	if (v > 65535)
		return 65535;
	return (uint16_t) v;
}

static uint16_t
lifecycle_flags(const Vehicle *v)
{
	return (uint16_t) (v->status & SERVER_LIFECYCLE_MASK);
}

int
server_view_half_extent(void)
{
	return ANIM_WIN_WIDTH / 2;
}

/*
** The reference animation viewport (docs/VIEW-v1.md section 3): a half-open
** world-space square of half-extent ANIM_WIN_WIDTH/2 centred on the controlled
** vehicle's Loc.  Pure read.
*/
static int
in_view(const Vehicle *own, double x, double y)
{
	double half = (double) (ANIM_WIN_WIDTH / 2);
	double dx = x - (double) own->loc->x;
	double dy = y - (double) own->loc->y;

	return dx >= -half && dx < half && dy >= -half && dy < half;
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

static void
export_self(const Vehicle *own, XtkSelfState *s)
{
	int i;

	s->slot = (uint8_t) own->number;
	s->generation = (uint8_t) server_events_generation(XTK_ENTITY_VEHICLE,
							   (uint16_t) own->number);
	s->lifecycle_flags = lifecycle_flags(own);

	s->armor[0] = clamp_u16_int(own->armor.side[FRONT]);
	s->armor[1] = clamp_u16_int(own->armor.side[BACK]);
	s->armor[2] = clamp_u16_int(own->armor.side[LEFT]);
	s->armor[3] = clamp_u16_int(own->armor.side[RIGHT]);

	s->fuel = clamp_u16_double(own->fuel);
	s->heat = clamp_u16_int(own->heat);
	s->score = (uint32_t) (own->owner ? own->owner->score : 0);

	s->weapon_count = (own->num_weapons > XTK_MAX_WEAPONS) ?
		(uint8_t) XTK_MAX_WEAPONS : (uint8_t) own->num_weapons;

	for (i = 0; i < s->weapon_count; i++) {
		s->ammo[i] = clamp_u16_int(own->weapon[i].ammo);
		s->reload[i] = clamp_u16_int(own->weapon[i].reload_counter);
		if (own->weapon[i].status & WS_on)
			s->weapon_enabled_bitmap |= (uint8_t) (1u << i);
	}
}

int
server_view_export(int player_slot, unsigned int generation,
		   uint32_t snapshot_sequence, uint16_t flags,
		   XtkViewSnapshot *out)
{
	Vehicle *own;
	Combatant *c;
	uint32_t gen;
	uint16_t vcount = 0, bcount = 0, ecount = 0;
	uint16_t mcount = 0, lcount = 0;
	uint8_t rcount = 0;
	int i, x, y;

	if (out == NULL)
		return SERVER_VIEW_ERR_NO_SLOT;
	if (player_slot < 0 || player_slot >= num_combatants)
		return SERVER_VIEW_ERR_NO_SLOT;

	c = &combatant[player_slot];
	own = c->vehicle;
	if (own == NULL)
		return SERVER_VIEW_ERR_NO_VEHICLE;

	/*
	 * Bring the export-side entity-lifetime counters current before reading
	 * them (idempotent within a tick); the stable key is unchanged.
	 */
	server_events_observe();

	gen = server_events_generation(XTK_ENTITY_VEHICLE, (uint16_t) own->number);
	if (generation != 0 && gen != generation)
		return SERVER_VIEW_ERR_GENERATION;

	(void) memset(out, 0, sizeof(*out));

	out->snapshot_sequence = snapshot_sequence;
	out->server_tick = (uint32_t) frame;
	out->flags = flags;
	out->own_slot = (uint8_t) own->number;
	out->own_generation = (uint8_t) gen;
	out->map_revision = (uint32_t) special_stats.mapper_updates;

	export_self(own, &out->self);
	out->weapon_count = out->self.weapon_count;

	/* --- VISIBLE: vehicles in the reference viewport -------------------- */
	for (i = 0; i < num_veh_alive; i++) {
		Vehicle *v = live_vehicles[i];
		XtkVehicleView *vv;

		/* VIEW-v1.md §5: dead vehicles are not in VehicleView (kill_vehicle
		   clears VS_is_alive before the frame sweep drops the slot). */
		if (!tstflag(v->status, VS_is_alive))
			continue;
		if (!in_view(own, v->loc->x, v->loc->y))
			continue;
		if (vcount >= XTK_MAX_VEHICLES)
			return SERVER_VIEW_ERR_OVERFLOW;

		vv = &out->vehicle[vcount++];
		vv->slot = (uint8_t) v->number;
		vv->generation = (uint8_t) server_events_generation(
			XTK_ENTITY_VEHICLE, (uint16_t) v->number);
		vv->kind = (uint8_t) v->vdesc->body;
		vv->flags = (uint8_t) (lifecycle_flags(v) & 0xFF);
		vv->x = v->loc->x;
		vv->y = v->loc->y;
		vv->body_angle = v->vector.heading;
		vv->turret_angle = (v->num_turrets > 0) ?
			v->turret[0].angle : v->vector.heading;
		vv->body_picture = (uint16_t) v->vector.rot;
		vv->turret_picture = (v->num_turrets > 0) ?
			(uint16_t) v->turret[0].rot : 0;
	}

	/* --- VISIBLE: bullets in the reference viewport --------------------- */
	for (i = 0; i < bset->number; i++) {
		Bullet *b = bset->list[i];
		XtkBulletView *bv;
		double heading = 0.0;

		if (!in_view(own, b->loc->x, b->loc->y))
			continue;
		if (bcount >= XTK_MAX_BULLETS)
			return SERVER_VIEW_ERR_OVERFLOW;

		if (b->xspeed != 0.0 || b->yspeed != 0.0)
			heading = atan2((double) b->yspeed, (double) b->xspeed);

		bv = &out->bullet[bcount++];
		bv->id = (uint16_t) (b - bset->array);
		bv->flags = 0;
		bv->x = b->loc->x;
		bv->y = b->loc->y;
		bv->heading = heading;
		bv->owner_slot = (b->owner != NULL) ?
			(uint8_t) b->owner->number : 0xFF;
		bv->owner_generation = (b->owner != NULL) ?
			(uint8_t) server_events_generation(
				XTK_ENTITY_VEHICLE,
				(uint16_t) b->owner->number) : 0;
		bv->kind = (uint8_t) b->type;
	}

	/* --- VISIBLE: explosions in the reference viewport ------------------ */
	for (i = 0; i < eset->number; i++) {
		Exp *e = eset->list[i];
		XtkExplosionView *ev;

		if (!in_view(own, (double) e->x, (double) e->y))
			continue;
		if (ecount >= XTK_MAX_EXPLOSIONS)
			return SERVER_VIEW_ERR_OVERFLOW;

		ev = &out->explosion[ecount++];
		ev->id = (uint16_t) (e - eset->array);
		ev->kind = explosion_kind(e);
		ev->flags = 0;
		ev->x = (double) e->x;
		ev->y = (double) e->y;
	}

	/* --- MAP: the controlled player's mapper knowledge only ------------- */
	{
		Special *s = &own->special[(int) MAPPER];

		if (s->status != SP_nonexistent && s->record != NULL) {
			Mapper *m = (Mapper *) s->record;

			for (x = 0; x < GRID_WIDTH; x++)
				for (y = 0; y < GRID_HEIGHT; y++) {
					Box *b = &m->map[x][y];
					XtkMapCell *mc;
					uint8_t f = 0;

					if (!(b->flags || b->type || b->team ||
					      b->strength))
						continue;
					if (mcount >= XTK_MAX_MAP_CELLS)
						return SERVER_VIEW_ERR_OVERFLOW;

					if (b->flags & NORTH_WALL)
						f |= SERVER_MAPCELL_NORTH_WALL;
					if (b->flags & WEST_WALL)
						f |= SERVER_MAPCELL_WEST_WALL;
					if (b->flags & NORTH_DEST)
						f |= SERVER_MAPCELL_NORTH_DEST;
					if (b->flags & WEST_DEST)
						f |= SERVER_MAPCELL_WEST_DEST;
					if (b->team != NEUTRAL)
						f |= SERVER_MAPCELL_TEAM_KNOWN;

					mc = &out->cell[mcount++];
					mc->grid_x = (uint8_t) x;
					mc->grid_y = (uint8_t) y;
					mc->flags = (uint8_t) (f | SERVER_MAPCELL_KNOWN);
					mc->type = (uint8_t) b->type;
				}
		}
	}

	/* --- VISIBLE: currently visible landmarks (real_map, viewport only) - */
	for (x = 0; x < GRID_WIDTH; x++)
		for (y = 0; y < GRID_HEIGHT; y++) {
			Box *b = &real_map[x][y];
			XtkLandmarkView *lv;
			double wx = (double) x * BOX_WIDTH + BOX_WIDTH / 2.0;
			double wy = (double) y * BOX_HEIGHT + BOX_HEIGHT / 2.0;

			if (b->type == NORMAL)
				continue;
			if (!in_view(own, wx, wy))
				continue;
			if (lcount >= XTK_MAX_LANDMARKS)
				return SERVER_VIEW_ERR_OVERFLOW;

			lv = &out->landmark[lcount++];
			lv->kind = (uint8_t) b->type;
			lv->id = (uint8_t) ((x * GRID_HEIGHT + y) & 0xFF);
			lv->x = wx;
			lv->y = wy;
		}

	/* --- RADAR: ordinary radar blips, anonymous ------------------------- */
	{
		Special *s = &own->special[(int) RADAR];

		if (s->status == SP_on && s->record != NULL) {
			Radar *r = (Radar *) s->record;

			for (i = 0; i < r->num_blips; i++) {
				Blip *b = &r->blip[i];
				XtkRadarBlip *rb;
				int age;

				if (rcount >= XTK_MAX_RADAR_BLIPS)
					return SERVER_VIEW_ERR_OVERFLOW;

				age = SERVER_RADAR_BORN - (int) b->life;
				if (age < 0)
					age = 0;

				rb = &out->radar[rcount++];
				rb->x = (double) b->x;
				rb->y = (double) b->y;
				rb->age = (uint8_t) age;
				rb->classification = SERVER_RADAR_ANONYMOUS;
			}
		}
	}

	out->vehicle_count = vcount;
	out->bullet_count = bcount;
	out->explosion_count = ecount;
	out->map_cell_count = mcount;
	out->landmark_count = lcount;
	out->radar_blip_count = rcount;

	return SERVER_VIEW_OK;
}