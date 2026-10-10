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
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 */

/*
** server_core.c -- explicit match/player context and simulation ownership
** (P1.6).
**
** This file owns the pieces of the simulation that the display front-end
** used to hold implicitly:
**
**   - the game clock (frame) is defined here, not in main();
**   - the RNG seed lifecycle (server_init/server_seed_random) is owned here;
**   - the explicit player-to-terminal binding (ServerConfig) lives here and
**     replaces the temporary XTANK_NULL_PLAYERS test hook and the X resource
**     database lookup;
**   - a backend-independent tick-pacing seam (server_tick_pace);
**   - initialize/step/reset/destroy for one match over the retained
**     single-world globals (server_match_init/step/reset, server_destroy).
**
** Behaviour is unchanged: every function delegates to the exact legacy
** routine at the exact legacy phase.  Multi-match conversion is deliberately
** not attempted here.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "xtank.h"
#include "server.h"
#include "intent.h"
#include "vehicle.h"
#include "terminal.h"
#include "proto.h"

/*
** The game clock.  Moved out of main.c so the server core owns the frame
** counter; every simulation file already refers to it via `extern int frame`.
*/
int frame;

/* The single configuration for this process. */
static ServerConfig the_config;

/* The seed actually used for the server-owned RNG stream. */
static unsigned int the_seed = 0;
static int have_seed = 0;

/*
** Lifecycle oracle bookkeeping (P1.6 lifecycle.sh only).  Active only in the
** STATE_DUMP oracle build, so the shipped front-end never responds to
** XTANK_LIFECYCLE.
*/
static int lifecycle_extra = -1;
static int lifecycle_done = 0;
static int lifecycle_epochs = 0;

static void
lifecycle_probe(void)
{
	if (lifecycle_extra < 0) {
		char *e = getenv("XTANK_LIFECYCLE");

		lifecycle_extra = e ? atoi(e) : 0;
	}
}

ServerConfig *
server_config(void)
{
	return &the_config;
}

void
server_config_init(ServerConfig *cfg)
{
	(void) memset((char *) cfg, 0, sizeof(ServerConfig));
}

int
server_config_add_player(ServerConfig *cfg, const char *name)
{
	int i;

	if (name == NULL || cfg->players.num_players >= SERVER_MAX_PLAYERS)
		return -1;

	i = cfg->players.num_players;
	(void) strncpy(cfg->players.player_name[i], name, MAX_STRING);
	cfg->players.player_name[i][MAX_STRING] = '\0';
	cfg->players.terminal[i] = i;
	cfg->players.num_players++;
	return i;
}

const char *
server_player_name(int terminal_index)
{
	int i;

	for (i = 0; i < the_config.players.num_players; i++)
		if (the_config.players.terminal[i] == terminal_index)
			return the_config.players.player_name[i];
	return "";
}

/*
** Seed the server-owned RNG stream.  The default (seed == 0) keeps the pin's
** time()-based init_random() so the oracle reference stays comparable until
** P1.8 supplies an explicit configured seed.
*/
void
server_seed_random(unsigned int seed)
{
	srandom(seed);
}

void
server_init(ServerConfig *cfg)
{
	intent_init();

	if (cfg != NULL && cfg != &the_config)
		the_config = *cfg;

	if (the_config.seed != 0) {
		the_seed = the_config.seed;
		server_seed_random(the_seed);
	} else {
		init_random();
		/* Capture the legacy time()-based seed so the lifecycle oracle can
		   replay each epoch from the same stream.  (Under the reference
		   shim this is the configured XTANK_SEED.) */
		the_seed = (unsigned int) time((long *) NULL);
	}
	have_seed = 1;
}

int
server_frame(void)
{
	return frame;
}

void
server_frame_reset(void)
{
	frame = 0;
}

/*
** Explicit, backend-independent tick pacing.  Delegates to the legacy
** game-speed pacer (which is a no-op under -D replay); P1.9 replaces the
** signal-based implementation behind this same seam.
*/
void
server_tick_pace(void)
{
	check_game_speed();
}

/*
** Lifecycle: one match over the retained single-world globals.
*/
int
server_match_init(void)
{
	extern int num_combatants;
	extern Boolean game_running;

	if (num_combatants == 0)
		return GAME_FAILED;

	/*
	 * Lifecycle oracle: replay every epoch from the same captured seed so
	 * each epoch is comparable to the frozen X reference.  Normal runs never
	 * take this path (server_lifecycle_active() is 0), so the shared RNG
	 * stream is unchanged outside the oracle.
	 */
	if (server_lifecycle_active() && have_seed)
		server_seed_random(the_seed);

	/* A fresh match starts with no queued or held player input. */
	intent_reset();

	init_combatants();
	if (setup_game(TRUE) == GAME_FAILED)
		return GAME_FAILED;

	game_running = True;
	return GAME_RUNNING;
}

int
server_step(void)
{
	return animate();
}

/*
** Consume the player-intent queue for one tick.  Called from the animate()
** seam after bot programs and before movement; every other phase is the
** legacy one.  When no controller has submitted input this is a no-op.
*/
int
server_consume_intents(int tick)
{
	return intent_consume(tick);
}

void
server_match_reset(void)
{
	extern int num_veh_alive, num_veh_dead;
	extern int num_terminals;
	extern Terminal *terminal[];
	int i;

	game_cleanup();

	/*
	 * The legacy game_cleanup() unmakes the vehicles but leaves the live/dead
	 * counts and each terminal's vehicle pointer dangling.  Clear them here
	 * so a reset really leaves no stale ownership behind.  This runs after
	 * every dump for the match, so it cannot perturb oracle output.
	 */
	num_veh_alive = 0;
	num_veh_dead = 0;
	for (i = 0; i < num_terminals; i++)
		if (terminal[i] != NULL)
			terminal[i]->vehicle = (Vehicle *) NULL;

	lowlib_forget_vehicle();

	/* Drop queued commands and every held control. */
	intent_reset();
}

void
server_destroy(void)
{
	extern Vehicle *cv;
	extern Terminal *term;
	extern int num_veh_alive, num_veh_dead;

	lowlib_forget_vehicle();
	fprintf(stderr,
			"SERVER-LIFECYCLE epochs=%d alive=%d dead=%d cv=%s term=%s\n",
			lifecycle_epochs, num_veh_alive, num_veh_dead,
			(cv == NULL) ? "null" : "set",
			(term != NULL && term->vehicle != NULL) ? "set" : "null");
}

int
server_lifecycle_active(void)
{
#ifdef STATE_DUMP
	lifecycle_probe();
	return lifecycle_extra > 0;
#else
	return 0;
#endif
}

int
server_lifecycle_continue(void)
{
#ifdef STATE_DUMP
	lifecycle_probe();
	lifecycle_epochs++;
	if (lifecycle_extra > lifecycle_done) {
		lifecycle_done++;
		return 1;
	}
#endif
	return 0;
}
