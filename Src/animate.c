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

#include "xtank.h"
#include "bullet.h"
#include "vehicle.h"
#include "terminal.h"
#include "globals.h"
#include "clfkr.h"
#include "graphics.h"
#include "special.h"
#include "proto.h"

#ifdef STATE_DUMP
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
** Read-only per-frame state oracle for the Phase 1 regression guardrail.
**
** Compiled in only with -DSTATE_DUMP.  The output path and cadence come from
** XTANK_DUMP and XTANK_DUMP_PERIOD; the dump is emitted at each period
** boundary and unconditionally whenever the game leaves the GAME_RUNNING
** state, so the terminal frame/status/hash is always captured.  XTANK_DUMP_FORMAT
** selects the record format:
**
**   1 (default) -- the legacy P1.1 line format (retained permanently)
**   2           -- explicit-field, pointer/padding/screen-coordinate-free dumps
**
** XTANK_TICK_LIMIT (optional) stops the run after the boundary dump at that
** frame, giving a deterministic finite replay bound.  It only reads
** simulation state and cannot perturb a match.
**
** The v2 format additionally records all tick exits (including early returns
** before game_rules) and a reset-epoch counter, plus maze mutations, full
** vehicle/bullet/explosion state, mapper exploration and radar records.
*/

extern int frame;
extern int num_veh;
extern Vehicle actual_vehicles[];
extern Combatant combatant[];
extern Bset *bset;
extern Eset *eset;
extern Map real_map;

/* Deterministic replay hook, supplied by Tests/phase1/reference_input.c.
   Declared weak so a build without that source links and is a no-op. */
extern void xtank_replay_run(int) __attribute__((weak));

/* ------------------------------------------------------------------ */
/* Frozen legacy v1 oracle (values must not change)                    */
/* ------------------------------------------------------------------ */
static void
state_dump_fnv(unsigned long *h, void *p, int n)
{
	unsigned char *b = (unsigned char *) p;
	int i;

	for (i = 0; i < n; i++) {
		*h ^= b[i];
		*h *= 16777619UL;
	}
}

/* Environment / output shared by both formats, initialised exactly once. */
static FILE *dump_fp = NULL;
static int dump_fmt = -1;
static long dump_period = 100;
static long dump_limit = -1;
static long dump_total = 0;
static int dump_epoch = 0;
static int dump_last_frame = -1;

static int
state_dump_env(void)
{
	if (dump_fmt < 0) {
		char *path = getenv("XTANK_DUMP");
		char *per = getenv("XTANK_DUMP_PERIOD");
		char *lim = getenv("XTANK_TICK_LIMIT");
		char *fmt = getenv("XTANK_DUMP_FORMAT");

		dump_fmt = fmt ? atoi(fmt) : 1;
		dump_period = per ? atoi(per) : (dump_fmt == 2 ? 1 : 100);
		dump_limit = lim ? atol(lim) : -1;
		if (path)
			dump_fp = fopen(path, "w");
	}
	return dump_fmt;
}

/* ------------------------------------------------------------------ */
/* v2 explicit-field format                                            */
/* ------------------------------------------------------------------ */

typedef struct {
	int present;
	int flags;
	int type;
	int team;
	int strength;
} V2Box;

static V2Box v2_prev_maze[GRID_WIDTH][GRID_HEIGHT];
static V2Box v2_prev_mapper[MAX_VEHICLES][GRID_WIDTH][GRID_HEIGHT];
static int v2_first = 1;
static int v2_mapper_prev_valid[MAX_VEHICLES];

static void
v2_box_fields(int *present, int *flags, int *type, int *team, int *strength,
	      Box *b)
{
	*present = (b->flags || b->type || b->team || b->strength);
	*flags = (int) b->flags;
	*type = (int) b->type;
	*team = (int) b->team;
	*strength = (int) b->strength;
}

static void
state_dump_v2(int frame_no, unsigned int status)
{
	int i, j, w, x, y;
	static int warned = 0;

	(void) warned;

	if (dump_last_frame >= 0 && frame_no < dump_last_frame) {
		/*
		 * New reset epoch (P1.6 lifecycle oracle): the per-tick diff
		 * baselines are process-global, so re-arm them at the epoch
		 * boundary to keep each epoch's mutation/exploration records
		 * self-contained and directly comparable.  Single-epoch runs
		 * never take this path, so the frozen v2 output is unchanged.
		 */
		dump_epoch++;
		v2_first = 1;
		for (i = 0; i < MAX_VEHICLES; i++)
			v2_mapper_prev_valid[i] = 0;
	}
	dump_last_frame = frame_no;
	dump_total++;

	if (!dump_fp)
		return;

	fprintf(dump_fp,
		"F2 %d epoch %d status %u alive %d dead %d nb %d ne %d nv %d\n",
		frame_no, dump_epoch, status, num_veh_alive, num_veh_dead,
		bset->number, eset->number, num_veh);

	for (i = 0; i < num_veh; i++) {
		Vehicle *v = &actual_vehicles[i];
		Combatant *o = v->owner;

		fprintf(dump_fp,
			"V %d team %d status %x grid %d %d box %d %d pos %.4f %.4f "
			"z %.4f heading %.6f oldheading %.6f desired %.6f drive %.4f "
			"speed %.4f xspeed %.4f yspeed %.4f rot %d oldrot %d fuel %.4f "
			"heat %d armor %d %d %d %d %d %d discs %d deathtimer %d "
			"nweapons %d nturrets %d score %d kills %d deaths %d\n",
			v->number, v->team, (unsigned) v->status,
			v->loc->grid_x, v->loc->grid_y,
			(int) v->loc->box_x, (int) v->loc->box_y,
			v->loc->x, v->loc->y, v->loc->z,
			v->vector.heading, v->vector.old_heading,
			v->vector.desired_heading, v->vector.drive,
			v->vector.speed, v->vector.xspeed, v->vector.yspeed,
			v->vector.rot, v->vector.old_rot, v->fuel, v->heat,
			v->armor.side[0], v->armor.side[1], v->armor.side[2],
			v->armor.side[3], v->armor.side[4], v->armor.side[5],
			v->num_discs, v->death_timer,
			v->num_weapons, v->num_turrets,
			o ? o->score : 0, o ? o->kills : 0, o ? o->deaths : 0);

		for (w = 0; w < v->num_weapons; w++)
			fprintf(dump_fp,
				"VW %d %d type %d status %x ammo %d reload %d hits %d\n",
				v->number, w, (int) v->weapon[w].type,
				(unsigned) v->weapon[w].status, v->weapon[w].ammo,
				v->weapon[w].reload_counter, v->weapon[w].hits);

		for (w = 0; w < v->num_turrets; w++)
			fprintf(dump_fp,
				"VT %d %d angle %.6f desired %.6f flag %d\n",
				v->number, w, v->turret[w].angle,
				v->turret[w].desired_angle,
				(int) v->turret[w].angle_flag);

		fprintf(dump_fp,
			"VC %d timetocamo %d camod %d oldcamod %d rcs %.4f "
			"stealthy %.4f normal %.4f\n",
			v->number, v->time_to_camo, v->camod, v->old_camod,
			v->rcs, v->stealthy_rcs, v->normal_rcs);
	}

	for (i = 0; i < bset->number; i++) {
		Bullet *b = bset->list[i];
		int own = b->owner ? (int) b->owner->number : -1;

		fprintf(dump_fp,
			"B %d owner %d type %d life %d thrower %d safety %d hurt %d "
			"state %d rot %d grid %d %d box %d %d pos %.4f %.4f z %.4f "
			"xspeed %.4f yspeed %.4f\n",
			i, own, (int) b->type, b->life, b->thrower, b->safety,
			b->hurt_owner, b->state, b->rot,
			b->loc->grid_x, b->loc->grid_y,
			(int) b->loc->box_x, (int) b->loc->box_y,
			b->loc->x, b->loc->y, b->loc->z, b->xspeed, b->yspeed);
	}

	for (i = 0; i < eset->number; i++) {
		Exp *e = eset->list[i];

		fprintf(dump_fp, "E %d x %d y %d z %d life %d color %d\n",
			i, e->x, e->y, e->z, e->life, e->color);
	}

	/* Maze mutations: emit inside-maze boxes whose persisted fields changed
	   since the previous tick (all inside-maze boxes on the first tick). */
	for (x = 0; x < GRID_WIDTH; x++)
		for (y = 0; y < GRID_HEIGHT; y++) {
			Box *b = &real_map[x][y];
			V2Box *p = &v2_prev_maze[x][y];
			int present, flags, type, team, strength;

			v2_box_fields(&present, &flags, &type, &team, &strength, b);
			if (v2_first || p->present != present || p->flags != flags ||
			    p->type != type || p->team != team ||
			    p->strength != strength) {
				if (present)
					fprintf(dump_fp,
						"MX %d %d flags %x type %d team %d "
						"code %d strength %d\n",
						x, y, (unsigned) b->flags, (int) b->type,
						(int) b->team, (int) b->teleport_code,
						(int) b->strength);
				p->present = present; p->flags = flags;
				p->type = type; p->team = team;
				p->strength = strength;
			}
		}

	/* Mapper exploration and radar records, per vehicle. */
	for (i = 0; i < num_veh; i++) {
		Vehicle *v = &actual_vehicles[i];
		Special *s;

		s = &v->special[(int) MAPPER];
		if (s->status != SP_nonexistent && s->record) {
			Mapper *m = (Mapper *) s->record;

			fprintf(dump_fp,
				"MPH %d need_redisplay %d map_invalid %d "
				"num_symbols %d num_landmarks %d\n",
				v->number, m->need_redisplay, m->map_invalid,
				m->num_symbols, m->num_landmarks);

			for (x = 0; x < GRID_WIDTH; x++)
				for (y = 0; y < GRID_HEIGHT; y++) {
					Box *b = &m->map[x][y];
					V2Box *p = &v2_prev_mapper[i][x][y];
					int present, flags, type, team, strength;

					v2_box_fields(&present, &flags, &type, &team,
						      &strength, b);
					if (!v2_mapper_prev_valid[i] ||
					    p->present != present || p->flags != flags ||
					    p->type != type || p->team != team ||
					    p->strength != strength) {
						if (present)
							fprintf(dump_fp,
								"MP %d %d %d flags %x type %d "
								"team %d strength %d\n",
								v->number, x, y,
								(unsigned) b->flags,
								(int) b->type,
								(int) b->team,
								(int) b->strength);
						p->present = present; p->flags = flags;
						p->type = type; p->team = team;
						p->strength = strength;
					}
				}
			v2_mapper_prev_valid[i] = 1;

			for (j = 0; j < m->num_landmarks; j++)
				fprintf(dump_fp,
					"ML %d %d %d type %d team %d\n",
					v->number, m->landmark[j].x, m->landmark[j].y,
					(int) m->landmark[j].type,
					(int) m->landmark[j].team);
		}

		s = &v->special[(int) RADAR];
		if (s->status != SP_nonexistent && s->record) {
			Radar *r = (Radar *) s->record;

			fprintf(dump_fp,
				"RNH %d num_blips %d pos %d start %d %d end %d %d\n",
				v->number, r->num_blips, r->pos, r->start_x,
				r->start_y, r->end_x, r->end_y);
			for (j = 0; j < r->num_blips; j++)
				fprintf(dump_fp,
					"RB %d %d x %d y %d life %d view %d "
					"oldview %d flags %d\n",
					v->number, j, r->blip[j].x, r->blip[j].y,
					r->blip[j].life, r->blip[j].view,
					r->blip[j].old_view, r->blip[j].flags);
		}

		s = &v->special[(int) NEW_RADAR];
		if (s->status != SP_nonexistent && s->record) {
			newRadar *nr = (newRadar *) s->record;

			fprintf(dump_fp, "NRH %d frame_updated %d need_redisplay %d\n",
				v->number, nr->frame_updated, nr->need_redisplay);
			for (j = 0; j < num_veh; j++)
				fprintf(dump_fp,
					"NRB %d %d draw %d %d %d %d %d\n",
					v->number, j, nr->blip[j].draw_loc.x,
					nr->blip[j].draw_loc.y,
					nr->blip[j].draw_radar,
					nr->blip[j].draw_tactical,
					nr->blip[j].draw_friend);
		}
	}

	v2_first = 0;
	fflush(dump_fp);
}

/* ------------------------------------------------------------------ */
/* Dispatcher                                                          */
/* ------------------------------------------------------------------ */
static void
state_dump_v1(int frame_no, unsigned int status)
{
	unsigned long h = 2166136261UL;
	int i, w;

	if (!dump_fp)
		return;

	state_dump_fnv(&h, &frame_no, sizeof(frame_no));
	for (i = 0; i < num_veh; i++) {
		Vehicle *v = &actual_vehicles[i];

		state_dump_fnv(&h, &v->number, sizeof(v->number));
		state_dump_fnv(&h, &v->status, sizeof(v->status));
		state_dump_fnv(&h, &v->team, sizeof(v->team));
		state_dump_fnv(&h, &v->loc->grid_x, sizeof(v->loc->grid_x));
		state_dump_fnv(&h, &v->loc->grid_y, sizeof(v->loc->grid_y));
		state_dump_fnv(&h, &v->loc->x, sizeof(v->loc->x));
		state_dump_fnv(&h, &v->loc->y, sizeof(v->loc->y));
		state_dump_fnv(&h, &v->vector.rot, sizeof(v->vector.rot));
		state_dump_fnv(&h, &v->fuel, sizeof(v->fuel));
		state_dump_fnv(&h, &v->heat, sizeof(v->heat));
		state_dump_fnv(&h, &v->armor, sizeof(v->armor));
		state_dump_fnv(&h, &v->num_discs, sizeof(v->num_discs));
		for (w = 0; w < v->num_weapons; w++)
			state_dump_fnv(&h, &v->weapon[w].ammo, sizeof(v->weapon[w].ammo));
		state_dump_fnv(&h, &v->owner->score, sizeof(v->owner->score));
		state_dump_fnv(&h, &v->owner->kills, sizeof(v->owner->kills));
		state_dump_fnv(&h, &v->owner->deaths, sizeof(v->owner->deaths));
	}
	state_dump_fnv(&h, &bset->number, sizeof(bset->number));
	state_dump_fnv(&h, &eset->number, sizeof(eset->number));
	for (i = 0; i < bset->number; i++) {
		Bullet *b = bset->list[i];
		int own = b->owner ? (int) b->owner->number : -1;

		state_dump_fnv(&h, &own, sizeof(own));
		state_dump_fnv(&h, &b->type, sizeof(b->type));
		state_dump_fnv(&h, &b->life, sizeof(b->life));
		state_dump_fnv(&h, &b->thrower, sizeof(b->thrower));
		state_dump_fnv(&h, &b->loc->x, sizeof(b->loc->x));
		state_dump_fnv(&h, &b->loc->y, sizeof(b->loc->y));
		state_dump_fnv(&h, &b->xspeed, sizeof(b->xspeed));
		state_dump_fnv(&h, &b->yspeed, sizeof(b->yspeed));
	}

	if ((frame_no % dump_period) == 0 || status != GAME_RUNNING ||
	    (dump_limit > 0 && frame_no >= dump_limit)) {
		fprintf(dump_fp,
			"F %d status %u alive %d dead %d bullets %d exp %d hash %08lx\n",
			frame_no, status, num_veh_alive, num_veh_dead,
			bset->number, eset->number, h);
		for (i = 0; i < num_veh; i++) {
			Vehicle *v = &actual_vehicles[i];

			fprintf(dump_fp,
				"  V%d team %d grid %d,%d pos %.6f,%.6f rot %d fuel %.4f heat %d "
				"armor %d,%d,%d,%d discs %d",
				v->number, v->team, v->loc->grid_x, v->loc->grid_y,
				v->loc->x, v->loc->y, v->vector.rot, v->fuel, v->heat,
				v->armor.side[0], v->armor.side[1],
				v->armor.side[2], v->armor.side[3], v->num_discs);
			for (w = 0; w < v->num_weapons; w++)
				fprintf(dump_fp, " ammo%d", v->weapon[w].ammo);
			fprintf(dump_fp, " score %d kills %d deaths %d status %lx\n",
				v->owner->score, v->owner->kills, v->owner->deaths,
				(unsigned long) v->status);
		}
		fflush(dump_fp);
	}

	if (dump_limit > 0 && frame_no >= dump_limit)
		exit(0);
}

/*
** Emit one tick of oracle state.  format 1 preserves the frozen legacy
** output; format 2 adds explicit-field records at every tick exit.
*/
static void
state_dump(int frame_no, unsigned int status)
{
	if (state_dump_env() == 2) {
		state_dump_v2(frame_no, status);
		if (dump_limit > 0 && dump_total >= dump_limit)
			exit(0);
		return;
	}
	state_dump_v1(frame_no, status);
}

/*
** Called at animate() exits that occur before the normal end-of-tick dump
** (early GAME_OVER / GAME_QUIT).  A no-op unless v2 is selected, so the
** frozen v1 output is untouched.
*/
static void
state_dump_exit(int frame_no, unsigned int status)
{
	if (state_dump_env() == 2)
		state_dump(frame_no, status);
}
#endif /* STATE_DUMP */

extern int frame;
extern CLFkr command_options;
extern int num_combatants;
extern Combatant combatant[MAX_VEHICLES];

/* # frames between display synchronizations */
#ifndef LOCK_GAME_CONTROLS
int sync_rate = 1;

#else
int sync_rate = 16;

#endif

/*
** Removes dead vehicles, updates everything, checks for collisions,
** and displays all terminals, for one frame of animation.
**
** Returns one of GAME_RUNNING, GAME_QUIT, GAME_OVER, or GAME_RESET.
*/
int
animate(void)
{
	extern int num_terminals;
	extern Terminal *terminal[];
	Vehicle *v;
	unsigned int retval;
	int i;
	static int quit_frame;		/* frame that game ends */

	/* Check for paused or slowed game */
	check_game_speed();

	/* Increment frame counter */
	++frame;

	/* Reset quit frame if we are starting a new game */
	if (frame <= 1)
		quit_frame = frame - 1;

	/* Check for internal end-of-game */
	if (frame == quit_frame) {
#ifdef STATE_DUMP
		state_dump_exit(frame, GAME_OVER);
#endif
		return GAME_OVER;
	}

	/* check living vehicles */
	for (i = 0; i < num_veh_alive; ++i) {
		v = live_vehicles[i];

		if (tstflag(v->status, VS_is_alive)) {
			setflag(v->status, VS_was_alive);
		} else {
			if (tstflag(v->status, VS_permanently_dead)) {
				if (--(teamdata[v->team].vehicle_count) == 0) {
					--num_teams;
				}
				/* everybody left on the same side? */
				if (num_teams < 2 && teamdata[NEUTRAL].vehicle_count < 2) {
					quit_frame = frame + QUIT_DELAY;
				}
				unmake_vehicle(v);
			} else {
				v->death_timer = DEATH_DELAY;
				/* put vehicle on dead list */
				dead_vehicles[num_veh_dead++] = v;
			}
			/* remove vehicle from live list */
			live_vehicles[i] = live_vehicles[--num_veh_alive];
			--i;				/* new vehicle fell into this slot */
		}
	}

	/* check dead vehicles */
	for (i = 0; i < num_veh_dead; ++i) {
		v = dead_vehicles[i];

		/* time to resurrect yet? */
		if (--(v->death_timer) <= 0) {
			if (activate_vehicle(v) == GAME_OVER) {
				printf("animate(): couldn't resurrect_vehicle()\n");
#ifdef STATE_DUMP
				state_dump_exit(frame, GAME_OVER);
#endif
				return GAME_OVER;
			}
			/* remove vehicle from dead list */
			dead_vehicles[i] = dead_vehicles[--num_veh_dead];
			--i;				/* new dead vehicle fell into this slot */
		}
	}

	/* Initialize the changed boxes in the maze */
	init_changed_boxes();

	/* Update the old screen locations for all the terminals */
	for (i = 0; i < num_terminals; i++)
		terminal[i]->old_loc = terminal[i]->loc;

	/* Clear the number of new messages for all the vehicles */
	for (i = 0; i < num_veh_alive; i++)
		live_vehicles[i]->new_messages = 0;

	/* Process input from all the programs */
	run_all_programs();

#ifdef STATE_DUMP
	/*
	** Deterministic bot-free replay instrumentation (P1.2).  Present only
	** in the oracle build; a no-op unless XTANK_TRACE names a trace file.
	** It drives the legacy controls at the pre-terminal input seam.
	*/
	if (xtank_replay_run)
		xtank_replay_run(frame);
#endif

	/* Process input from all the terminals */
	for (i = 0; i < num_terminals; i++) {
		set_terminal(i);
		if (get_input() == GAME_QUIT) {
			if (i == 0) {
#ifdef STATE_DUMP
				state_dump_exit(frame, GAME_QUIT);
#endif
				return GAME_QUIT;	/* game god quit? */
			}

			/* %%% I don't think this is complete enough -RDP */

			if (--terminal[i]->vehicle->owner->num_players <= 0) {
				int j;
				int flags = 0;
				int need = PLAYS_COMBAT | PLAYS_WAR | PLAYS_ULTIMATE |
				PLAYS_CAPTURE | PLAYS_RACE;

				for (j = 0; j < terminal[i]->vehicle->num_programs; j++) {
					flags |= terminal[i]->vehicle->program[j].desc->abilities;
					if (need & flags)
						break;
				}

				if (!(need & flags)) {   /* HAK 2/93 */
					Vehicle *temp;

					/* save the vehicle addr, since kill_vehicle */
					/* will set terminal[i]->vehicle to NULL */
					temp = terminal[i]->vehicle;

					kill_vehicle(terminal[i]->vehicle, (Vehicle *) NULL);

					/* he ain't comin' back */
					setflag(temp->status, VS_permanently_dead);
					if (i != num_combatants - 1)
						combatant[i] = combatant[num_combatants - 1];
				}
			}
			/*
			terminal[i]->vehicle->owner->num_players--;
			*/
			remove_player(i);	/* close the terminal */
			i--;		/* new terminal has been moved in */
		}
	}

	/* Update locations and vectors of all vehicles, and bullets */
	for (i = 0; i < num_veh_alive; i++)
		update_vehicle(live_vehicles[i]);
	update_bullets();

	/* Check for collisions between all vehicles */
	coll_vehicles_vehicles();

	/* Check for collisions between each vehicle and the walls */
	for (i = 0; i < num_veh_alive; i++)
		coll_vehicle_walls(live_vehicles[i]);

	/* Check for bullet collisions against the maze and vehicles */
	coll_bullets_maze();
	coll_bullets_vehicles();

	/* Update vehicle rotations after collisions */
	for (i = 0; i < num_veh_alive; i++)
		update_rotation(live_vehicles[i]);

	/* Update explosions after bullet colls, since they might create some */
	update_explosions();

	/* Update maze flags after vehicle colls, to get new vehicle positions */
	update_maze_flags();

	/* Update specials after maze flags, since some specials use them */
	update_specials();

	/* Apply rules of the current game */
	retval = game_rules(FALSE);
#ifdef STATE_DUMP
	state_dump(frame, retval);
#endif
	if (retval != GAME_RUNNING)
		return retval;

	if (!command_options.NoIO) {
		/* Update screen locations, and display everything on each terminal */
		for (i = 0; i < num_terminals; i++) {
			set_terminal(i);
			update_screen_locs();
			display_terminal(REDISPLAY, i == num_terminals - 1);
		}
	}
#ifndef NO_CAMO
/*
 * kind of a funny place, eh?  It's because old_camod actually
 * refers to if it has been drawn as a "camod" vehicle yet by
 * display.c; I must have been up too late when I thought of
 * this scheme.
 *
 * This could be moved to update_vehicle(), but it would require that
 * the update_vehicle be called after display_terminal, not before.
 *
 * Seemed a little risky!
 *
 * The reson for updating everyone, instead of just the "live vehicles"
 * is that I'm not sure how the "death-delay" thing works, I didn't want
 * to risk a screwup if you blew up a camo'd tank.  Feel free to try
 * it with live_vehicles.
 *
 * I'm welcome to suggestions for alternate hacks, several occured
 * to me, but this was by far the simplest, though it is a bit
 * "ugly" on the surface.   --ane
 *
 */
	for (i = 0; i < MAX_VEHICLES; i++)
		actual_vehicles[i].old_camod = actual_vehicles[i].camod;

#endif /* !NO_CAMO */

	/* Synchronize all terminals every sync_rate frames */
	if ((frame % sync_rate) == 0)
		sync_terminals(FALSE);

	return GAME_RUNNING;
}
