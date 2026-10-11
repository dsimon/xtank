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
 * events_fixture.c -- P2.6 entity-lifetime / event / results acceptance.
 *
 * Links the shipped display-free server core (Src/sobj, without server_main.o)
 * plus the frozen P2.3 wire codec and drives one terminal-free human plus one
 * drone through two complete match epochs, asserting:
 *
 *   - generation-safe identities: the export-side lifetime counter advances on
 *     respawn and on forced bullet/explosion slot reuse, and the view's
 *     generation comes from that counter (never a pointer);
 *   - death/respawn DEATH and RESPAWN events carry the right (kind,id,generation);
 *   - two epochs restart the event sequence at 0 and keep the epoch monotonic;
 *   - an early win exit captures the immutable RESULTS before the score reset;
 *   - duplicate and reordered event delivery does not duplicate effects/score;
 *   - 0/1/5 observations leave the simulation state byte-identical.
 *
 * The fixture may manipulate simulation state directly (it is the harness); the
 * events exporter itself is the read-only unit under test.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xtank.h"
#include "bullet.h"
#include "clfkr.h"
#include "object.h"
#include "thread.h"
#include "vehicle.h"
#include "server.h"
#include "server_view.h"
#include "server_events.h"
#include "proto.h"
#include "gr.h"

/* Globals normally owned by server_main.c / main.c (both excluded here). */
char team_char[] = "NROYGBV";

Eset actual_eset, *eset;
Bset actual_bset, *bset;

Maze maze;
Map real_map;
Settings settings;
Boolean game_paused;
char *executable_name;

int num_veh;
int num_veh_alive;
int num_veh_dead;
Vehicle actual_vehicles[MAX_VEHICLES];
Vehicle *live_vehicles[MAX_VEHICLES];
Vehicle *dead_vehicles[MAX_VEHICLES];
CLFkr command_options;

extern char username[MAX_STRING];
extern Mdesc *mdesc;
extern Vdesc *vdesc;
extern int num_combatants;
extern Combatant combatant[];
extern int frame;

int
InitConfigStruct(CLFkr *ConfigRunning)
{
	ConfigRunning->AutoExit = FALSE;
	ConfigRunning->AutoStart = FALSE;
	ConfigRunning->UseSetting = FALSE;
	ConfigRunning->PrintScores = FALSE;
	ConfigRunning->Settings = NULL;
	ConfigRunning->NoDelay = FALSE;
	ConfigRunning->NoIO = FALSE;
	return 0;
}

/* ---- acceptance bookkeeping ------------------------------------------- */

static int failures;

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

/* ---- simulation-state hash (proves read-only export) ------------------ */

static void
hbytes(unsigned long *h, const void *p, size_t n)
{
	const unsigned char *b = (const unsigned char *) p;
	size_t i;

	for (i = 0; i < n; i++) {
		*h ^= b[i];
		*h *= 16777619UL;
	}
}

static unsigned long
sim_hash(void)
{
	unsigned long h = 2166136261UL;
	int i;

	hbytes(&h, &frame, sizeof(frame));
	hbytes(&h, &num_veh, sizeof(num_veh));
	hbytes(&h, &num_veh_alive, sizeof(num_veh_alive));
	hbytes(&h, &num_veh_dead, sizeof(num_veh_dead));
	hbytes(&h, actual_vehicles, (size_t) num_veh * sizeof(Vehicle));
	hbytes(&h, combatant, sizeof(Combatant) * (size_t) num_combatants);
	hbytes(&h, real_map, sizeof(real_map));
	hbytes(&h, bset->array, sizeof(actual_bset.array));
	hbytes(&h, &bset->number, sizeof(bset->number));
	hbytes(&h, eset->array, sizeof(actual_eset.array));
	hbytes(&h, &eset->number, sizeof(eset->number));
	for (i = 0; i < num_combatants; i++)
		hbytes(&h, &combatant[i].score, sizeof(combatant[i].score));
	return h;
}

/* ---- event stream helpers --------------------------------------------- */

static int
count_type(int type)
{
	int i, n = 0;

	for (i = 0; i < server_events_count(); i++)
		if (server_events_event(i)->event_type == (uint8_t) type)
			n++;
	return n;
}

static const XtkEvent *
find_event(int type, int id, uint32_t gen)
{
	int i;

	for (i = 0; i < server_events_count(); i++) {
		const XtkEvent *e = server_events_event(i);

		if (e->event_type == (uint8_t) type &&
		    e->entity_id == (uint16_t) id &&
		    e->entity_generation == gen)
			return e;
	}
	return (const XtkEvent *) NULL;
}

/*
** A tiny consumer model: apply SCORE events to a per-slot scoreboard, dedup by
** (epoch, event_sequence) and ignore delivery order.  Applying the stream once,
** twice or in reverse must give the same board, and events never score twice.
*/
static void
apply_events(long applied[MAX_VEHICLES], int passes, int reverse)
{
	unsigned char seen[SERVER_EVENTS_MAX];
	int pass, i;

	(void) memset(seen, 0, sizeof(seen));
	(void) memset(applied, 0, sizeof(long) * MAX_VEHICLES);

	for (pass = 0; pass < passes; pass++) {
		for (i = 0; i < server_events_count(); i++) {
			int k = reverse ? (server_events_count() - 1 - i) : i;
			const XtkEvent *e = server_events_event(k);

			if (e->event_type != XTK_EV_SCORE)
				continue;
			if (seen[e->event_sequence])
				continue;	/* duplicate delivery */
			seen[e->event_sequence] = 1;
			applied[e->entity_id] += (int) e->aux0;
		}
	}
}

int
main(int argc, char *argv[])
{
	const char *assets = (argc > 1) ? argv[1] : ".";
	ServerConfig *cfg;
	int vdesc_index, mdesc_index;
	int hb, epoch, t;
	unsigned int first_tick = 0;

	bset = &actual_bset;
	eset = &actual_eset;
	InitConfigStruct(&command_options);
	command_options.NoDelay = TRUE;
	command_options.PrintScores = FALSE;
	command_options.AutoExit = TRUE;

	(void) strncpy(username, "p2.6", MAX_STRING - 1);
	username[MAX_STRING - 1] = '\0';

	if (content_set_root(assets) != 0) {
		fprintf(stderr, "events_fixture: %s\n", content_last_error());
		return 2;
	}

	cfg = server_config();
	server_config_init(cfg);
	server_config_release(cfg);
	cfg->seed = 114336;
	cfg->seed_set = 1;
	server_init(cfg);

	init_prog_descs();
	init_settings();
	server_release_settings(&settings);
	init_threader();
	init_msg_sys();
	init_box_names();
	geometry_order_objects();
	geometry_rotate_objects();

	if (make_vdesc((char *) "Vanguard", &vdesc_index) != DESC_LOADED) {
		fprintf(stderr, "events_fixture: cannot load Vanguard\n");
		return 2;
	}
	if (make_mdesc((char *) "combat1", &mdesc_index) != DESC_LOADED) {
		fprintf(stderr, "events_fixture: cannot load combat1\n");
		return 2;
	}
	settings.mdesc = &mdesc[mdesc_index];
	settings.si.game = settings.mdesc->type;

	server_players_init();
	hb = server_players_add("humanA", 7);
	if (hb < 0 || server_players_bind_slot(hb, 0) != 0) {
		fprintf(stderr, "events_fixture: cannot bind human\n");
		return 2;
	}

	num_combatants = 2;
	combatant[0].num_players = 1;
	combatant[0].player[0] = hb;
	combatant[0].num_programs = 0;
	combatant[0].team = 1 % MAX_TEAMS;
	combatant[0].vdesc = vdesc_index;
	(void) strncpy(combatant[0].name, "humanA", MAX_STRING - 1);
	combatant[0].name[MAX_STRING - 1] = '\0';

	combatant[1].num_players = 0;
	combatant[1].num_programs = 1;
	combatant[1].program[0] = 0;
	combatant[1].team = 2 % MAX_TEAMS;
	combatant[1].vdesc = vdesc_index;
	(void) strncpy(combatant[1].name, "drone", MAX_STRING - 1);
	combatant[1].name[MAX_STRING - 1] = '\0';

	for (epoch = 0; epoch < 2; epoch++) {
		Vehicle *hv;
		uint32_t gen0, gen_before;
		int died = 0, respawned = 0, won = 0;
		XtkResults res;
		XtkViewSnapshot snap;

		fprintf(stderr, "== epoch %d\n", epoch);
		server_events_reset((uint32_t) (epoch + 1), first_tick);

		if (server_match_init() != GAME_RUNNING) {
			check("match initializes", 0);
			break;
		}
		hb = server_players_index_for_player(combatant[0].player[0]);
		hv = combatant[0].vehicle;

		check("match initializes", 1);
		check("epoch accessor", server_events_epoch() ==
		      (uint32_t) (epoch + 1));

		server_events_observe();

		/* Initial life: dedicated lifetime counter is non-zero and monotonic
		   across epochs (the epoch is part of the stable key). */
		gen0 = server_events_generation(XTK_ENTITY_VEHICLE, 0);
		check("initial vehicle generation is non-zero", gen0 >= 1u);
		check("view generation comes from the lifetime counter",
		      server_view_export(0, 0, 1, XTK_FLAG_KEYFRAME, &snap) ==
		      SERVER_VIEW_OK && snap.self.generation == (uint8_t) gen0);
		check("initial spawn is not a RESPAWN event",
		      find_event(XTK_EV_RESPAWN, 0, gen0) == NULL);

		/* ---- death -> respawn advances the generation ---------------- */
		fprintf(stderr, "  -- death/respawn\n");
		gen_before = server_events_generation(XTK_ENTITY_VEHICLE, 0);
		kill_vehicle(hv, (Vehicle *) NULL);
		server_events_observe();
		check("DEATH event carries the dying life's generation",
		      find_event(XTK_EV_DEATH, 0, gen_before) != NULL);
		check("event sequence restarts at 0 each epoch",
		      server_events_count() >= 1 &&
		      server_events_event(0)->event_sequence == 0);

		for (t = 0; t < 100000; t++) {
			int st = server_step();

			server_events_observe();
			if (!tstflag(hv->status, VS_is_alive))
				died = 1;
			if (died && tstflag(hv->status, VS_is_alive)) {
				respawned = 1;
				break;
			}
			if (st != GAME_RUNNING)
				break;
		}
		check("human dies and respawns", died && respawned);
		check("respawn advances the lifetime counter by one",
		      server_events_generation(XTK_ENTITY_VEHICLE, 0) ==
		      gen_before + 1u);
		check("RESPAWN event carries the new generation",
		      find_event(XTK_EV_RESPAWN, 0, gen_before + 1u) != NULL);
		check("view generation tracks the respawn",
		      server_view_export(0, 0, 2, 0, &snap) == SERVER_VIEW_OK &&
		      snap.self.generation == (uint8_t) (gen_before + 1u));

		/* ---- forced transient slot reuse ----------------------------- */
		fprintf(stderr, "  -- forced slot reuse (bullet/explosion)\n");
		{
			Bullet *b0 = &bset->array[3];
			uint32_t bg0, bg1, eg0, eg1;
			int ne0;

			/* Force a bullet life into array slot 3 and observe it. */
			(void) memset(&actual_bset, 0, sizeof(actual_bset));
			bg0 = server_events_generation(XTK_ENTITY_BULLET, 3);
			bset->number = 1;
			bset->list[0] = b0;
			b0->type = CANNON;
			b0->owner = hv;
			b0->loc = &b0->loc1;
			b0->loc1.x = hv->loc->x;
			b0->loc1.y = hv->loc->y;
			frame++;
			server_events_observe();
			check("bullet appearance advances the generation",
			      server_events_generation(XTK_ENTITY_BULLET, 3) ==
			      bg0 + 1u);

			/* Remove it, observe the absence, then reuse the slot with a
			   different life; the generation must advance again. */
			bset->number = 0;
			frame++;
			server_events_observe();
			bset->number = 1;
			bset->list[0] = b0;
			b0->type = FLAME;	/* different life in the same slot */
			frame++;
			server_events_observe();
			bg1 = server_events_generation(XTK_ENTITY_BULLET, 3);
			check("bullet slot reuse advances the generation",
			      bg1 == bg0 + 2u);

			/* Explosion slot reuse emits a new ticked EXPLOSION event. */
			(void) memset(&actual_eset, 0, sizeof(actual_eset));
			eg0 = server_events_generation(XTK_ENTITY_EXPLOSION, 9);
			ne0 = count_type(XTK_EV_EXPLOSION);
			eset->number = 1;
			eset->list[0] = &eset->array[9];
			eset->array[9].obj = NULL;
			eset->array[9].color = 1;
			frame++;
			server_events_observe();
			check("explosion appearance advances the generation",
			      server_events_generation(XTK_ENTITY_EXPLOSION, 9) ==
			      eg0 + 1u);
			check("EXPLOSION event emitted",
			      count_type(XTK_EV_EXPLOSION) == ne0 + 1);

			eset->number = 0;
			frame++;
			server_events_observe();
			eset->number = 1;
			eset->list[0] = &eset->array[9];
			eset->array[9].obj = NULL;
			eset->array[9].color = 2;
			frame++;
			server_events_observe();
			eg1 = server_events_generation(XTK_ENTITY_EXPLOSION, 9);
			check("explosion slot reuse advances the generation",
			      eg1 == eg0 + 2u);
			check("reused explosion emits a second event",
			      count_type(XTK_EV_EXPLOSION) == ne0 + 2);

			/* Clear the harness-forced transients before stepping on. */
			(void) memset(&actual_bset, 0, sizeof(actual_bset));
			(void) memset(&actual_eset, 0, sizeof(actual_eset));
		}

		/* ---- read-only: 0/1/5 observations leave state identical ----- */
		fprintf(stderr, "  -- read-only parity (0/1/5 observations)\n");
		{
			unsigned long h0, h1, h5;
			int i;

			frame++;	/* ensure the first observation really runs */
			h0 = sim_hash();
			for (i = 0; i < 1; i++)
				server_events_observe();
			h1 = sim_hash();
			for (i = 0; i < 5; i++)
				server_events_observe();
			h5 = sim_hash();
			check("simulation state unchanged after 1 observation",
			      h0 == h1);
			check("simulation state unchanged after 5 observations",
			      h0 == h5);
		}

		/* ---- early win exits and immutable results ------------------- */
		fprintf(stderr, "  -- early win exit / results\n");
		combatant[0].score = settings.si.winning_score;
		for (t = 0; t < 100000; t++) {
			int st = server_step();

			server_events_observe();
			if (st == GAME_RESET) {
				won = 1;
				break;
			}
			if (st != GAME_RUNNING)
				break;
		}
		check("human reaches the winning score (GAME_RESET)", won);

		/* Settle the result exactly as play_game() does (before the reset). */
		(void) game_results(GAME_RESET);

		(void) memset(&res, 0, sizeof(res));
		check("RESULTS captured before score reset",
		      server_events_results_valid() == 1 &&
		      server_events_results(&res) == SERVER_EVENTS_OK);
		check("result is a win for slot 0",
		      res.result_kind == 0 && res.winner_slot == 0);
		check("result score is the winning score (not the reset 0)",
		      res.entry[0].score ==
		      (uint32_t) settings.si.winning_score);
		check("result entry carries the lifetime generation",
		      res.entry[0].generation ==
		      (uint8_t) server_events_generation(XTK_ENTITY_VEHICLE, 0));
		check("MATCH_END event emitted",
		      count_type(XTK_EV_MATCH_END) == 1);

		/* The consumer model of the score stream. */
		{
			long once[MAX_VEHICLES], twice[MAX_VEHICLES];
			int nscore = count_type(XTK_EV_SCORE);

			apply_events(once, 1, 0);
			apply_events(twice, 2, 1);	/* duplicate + reversed */
			check("score stream is non-empty", nscore >= 1);
			check("duplicate/reordered events do not double score",
			      memcmp(once, twice, sizeof(once)) == 0);
			check("event-derived score matches the award",
			      once[0] == (long) settings.si.winning_score);
		}

		/* ---- results survive until reset; scores now zeroed ---------- */
		server_match_reset();
		check("reset zeroes the live score",
		      combatant[0].score == 0);
		check("reset leaves no live/dead vehicles",
		      num_veh_alive == 0 && num_veh_dead == 0);
	}

	if (failures != 0) {
		printf("EVENTS FIXTURE FAIL (%d checks)\n", failures);
		return 1;
	}
	printf("EVENTS FIXTURE PASS (2 epochs; death=%d respawn=%d score=%d "
	       "explosion=%d match_end=%d; results before reset; dedup ok; "
	       "read-only 0/1/5)\n",
	       count_type(XTK_EV_DEATH), count_type(XTK_EV_RESPAWN),
	       count_type(XTK_EV_SCORE), count_type(XTK_EV_EXPLOSION),
	       count_type(XTK_EV_MATCH_END));
	return 0;
}
