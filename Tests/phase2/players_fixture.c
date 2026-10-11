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
 * players_fixture.c -- P2.4 terminal-free human lifecycle fixture.
 *
 * Links the shipped display-free server core (Src/sobj, without server_main.o)
 * and drives one explicit terminal-free human (controller -> name -> slot) plus
 * one drone through two complete match epochs, asserting:
 *
 *   - the human's terminal-free binding carries the human defaults
 *     (Combatant.num_players == 1, Vehicle.teleport == TRUE) exactly as a
 *     legacy X human, while the drone keeps the bot default (teleport FALSE);
 *   - init / death / respawn / win / reset all work with ZERO terminals;
 *   - the binding survives respawn and generation-checks stale controllers;
 *   - no terminal allocation, render-sync or event-input call is made during
 *     any epoch (link-time --wrap counters, all required to be zero).
 *
 * It never builds a Terminal and never labels the human a bot.
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
#include "proto.h"
#include "gr.h"
#include "graphics.h"

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

/* ---- terminal-path instrumentation (link-time --wrap) ------------------- */

static unsigned long n_make_terminal, n_set_terminal, n_display_terminal;
static unsigned long n_get_events, n_sync_terminals;

int __real_make_terminal(char *);
void __real_set_terminal(int);
void __real_display_terminal(unsigned int, int);
void __real_get_events(int *, Event *);
void __real_sync_terminals(Boolean);

int
__wrap_make_terminal(char *display_name)
{
	n_make_terminal++;
	return __real_make_terminal(display_name);
}

void
__wrap_set_terminal(int num)
{
	n_set_terminal++;
	__real_set_terminal(num);
}

void
__wrap_display_terminal(unsigned int status, int lastterm)
{
	n_display_terminal++;
	__real_display_terminal(status, lastterm);
}

void
__wrap_get_events(int *num, Event *ev)
{
	n_get_events++;
	__real_get_events(num, ev);
}

void
__wrap_sync_terminals(Boolean discard)
{
	n_sync_terminals++;
	__real_sync_terminals(discard);
}

/* ---- acceptance bookkeeping -------------------------------------------- */

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

int
main(int argc, char *argv[])
{
	const char *assets = (argc > 1) ? argv[1] : ".";
	ServerConfig *cfg;
	Vdesc *dummy_v = NULL;
	Mdesc *dummy_m = NULL;
	int vdesc_index, mdesc_index;
	int hb, epoch, t;
	unsigned int gen;

	(void) dummy_v;
	(void) dummy_m;

	bset = &actual_bset;
	eset = &actual_eset;
	InitConfigStruct(&command_options);
	command_options.NoDelay = TRUE;
	command_options.PrintScores = FALSE;
	command_options.AutoExit = TRUE;

	(void) strncpy(username, "p2.4", MAX_STRING - 1);
	username[MAX_STRING - 1] = '\0';

	if (content_set_root(assets) != 0) {
		fprintf(stderr, "players_fixture: %s\n", content_last_error());
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
		fprintf(stderr, "players_fixture: cannot load Vanguard\n");
		return 2;
	}
	if (make_mdesc((char *) "combat1", &mdesc_index) != DESC_LOADED) {
		fprintf(stderr, "players_fixture: cannot load combat1\n");
		return 2;
	}
	settings.mdesc = &mdesc[mdesc_index];
	settings.si.game = settings.mdesc->type;

	/* One terminal-free human (controller 7 -> slot 0) + one drone. */
	server_players_init();
	hb = server_players_add("humanA", 7);
	if (hb < 0) {
		fprintf(stderr, "players_fixture: cannot bind human\n");
		return 2;
	}
	if (server_players_bind_slot(hb, 0) != 0) {
		fprintf(stderr, "players_fixture: cannot bind slot\n");
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

	for (epoch = 0; epoch < 2; epoch++) {
		Vehicle *hv;
		Vehicle *dv;
		int died = 0, respawned = 0, won = 0;

		fprintf(stderr, "== epoch %d\n", epoch);
		if (server_match_init() != GAME_RUNNING) {
			check("match initializes", 0);
			break;
		}
		check("match initializes", 1);
		check("zero terminals", num_terminals == 0);

		hv = combatant[0].vehicle;
		dv = combatant[1].vehicle;
		check("human combatant has num_players == 1",
			  combatant[0].num_players == 1);
		check("human Vehicle.teleport == TRUE (X human default)",
			  hv != NULL && hv->teleport == TRUE);
		check("drone Vehicle.teleport == FALSE (bot default)",
			  dv != NULL && dv->teleport == FALSE);

		/* Binding ownership + generation validation. */
		gen = server_players_binding(hb)->generation;
		check("controller owns slot 0", server_players_validate(7, 0, gen) == 1);
		check("stale generation is rejected",
			  server_players_validate(7, 0, gen + 1) == 0);
		check("wrong slot is rejected", server_players_validate(7, 1, gen) == 0);
		check("unknown controller is rejected",
			  server_players_validate(6, 0, gen) == 0);

		/* Init -> death (no killer: no score) -> respawn. */
		kill_vehicle(hv, (Vehicle *) NULL);
		for (t = 0; t < 100000; t++) {
			int st = server_step();

			if (!tstflag(hv->status, VS_is_alive))
				died = 1;
			if (died && tstflag(hv->status, VS_is_alive)) {
				respawned = 1;
				break;
			}
			if (st != GAME_RUNNING)
				break;
		}
		check("human dies", died);
		check("human respawns", respawned);
		check("binding survives respawn (same controller/generation)",
			  server_players_validate(7, 0, gen) == 1);
		check("human is the same alive vehicle after respawn",
			  tstflag(hv->status, VS_is_alive));

		/* Win: award the human the winning score; rules fire GAME_RESET. */
		combatant[0].score = settings.si.winning_score;
		for (t = 0; t < 100000; t++) {
			int st = server_step();

			if (st == GAME_RESET) {
				won = 1;
				break;
			}
			if (st != GAME_RUNNING)
				break;
		}
		check("human reaches the winning score (GAME_RESET)", won);

		server_match_reset();
		check("reset leaves no live/dead vehicles",
			  num_veh_alive == 0 && num_veh_dead == 0);
		check("reset leaves zero terminals", num_terminals == 0);
	}

	check("no Terminal was ever allocated", n_make_terminal == 0);
	check("no set_terminal call", n_set_terminal == 0);
	check("no display_terminal render call", n_display_terminal == 0);
	check("no get_events input call", n_get_events == 0);
	check("no sync_terminals call", n_sync_terminals == 0);

	if (failures != 0) {
		printf("PLAYERS FIXTURE FAIL (%d checks)\n", failures);
		return 1;
	}
	printf("PLAYERS FIXTURE PASS (2 terminal-free human epochs; "
		   "make_terminal=%lu display_terminal=%lu get_events=%lu)\n",
		   n_make_terminal, n_display_terminal, n_get_events);
	return 0;
}
