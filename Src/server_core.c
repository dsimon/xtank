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

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "xtank.h"
#include "server.h"
#include "intent.h"
#include "vehicle.h"
#include "terminal.h"
#include "object.h"
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
static int seed_captured = 0;

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
** Explicit seed source (P1.8).  XTANK_SEED, when present, is the configured
** server seed; it is parsed strictly so a malformed value cannot silently
** fall back to the non-deterministic time()-based default.  Returns 0 when a
** seed was configured, 1 when absent, -1 when malformed.
*/
int
server_config_seed_env(ServerConfig *cfg)
{
	const char *s = getenv("XTANK_SEED");
	char *end;
	unsigned long v;

	if (s == NULL)
		return 1;
	if (*s == '\0')
		return -1;
	errno = 0;
	v = strtoul(s, &end, 10);
	if (errno != 0 || end == s || *end != '\0' || v > 0xffffffffUL)
		return -1;

	cfg->seed = (unsigned int) v;
	cfg->seed_set = 1;
	return 0;
}

/*
** Replay mode: the STATE_DUMP oracle is producing a comparable dump, so the
** run must be deterministic.  Only then does a missing configured seed become
** fatal; a normal front-end run keeps the pin's time()-based default.
*/
static int
server_replay_mode(void)
{
#ifdef STATE_DUMP
	return getenv("XTANK_DUMP") != NULL || getenv("XTANK_TRACE") != NULL;
#else
	return 0;
#endif
}

/*
** Seed the server-owned RNG stream.  This is the single seed source for the
** server: the pin's RNG implementation (init_random_seed -> SEEDRANDOM) and
** its call order are unchanged -- only the seed value differs.
*/
void
server_seed_random(unsigned int seed)
{
	init_random_seed(seed);
}

/*
** Explicit simulation rate (P1.9).  XTANK_TICK_HZ overrides the config; the
** display-free server defaults to SERVER_DEFAULT_TICK_HZ so the monotonic loop
** owns pacing, while the X front-end keeps tick_hz == 0 and the legacy pacer.
** A malformed value falls back to the default rather than aborting startup.
*/
static void
server_config_tick_env(ServerConfig *cfg)
{
	const char *s = getenv("XTANK_TICK_HZ");
	char *end;
	long v;

	if (s != NULL && *s != '\0') {
		errno = 0;
		v = strtol(s, &end, 10);
		if (errno == 0 && end != s && *end == '\0' && v >= 0 && v <= 1000000)
			cfg->tick_hz = (int) v;
	}
#ifdef NULL_BACKEND
	if (cfg->tick_hz == 0)
		cfg->tick_hz = SERVER_DEFAULT_TICK_HZ;
#endif
}

void
server_init(ServerConfig *cfg)
{
	intent_init();

	if (cfg != NULL && cfg != &the_config)
		the_config = *cfg;

	server_config_tick_env(&the_config);
	server_loop_configure(the_config.tick_hz);

	if (the_config.seed_set) {
		the_seed = the_config.seed;
		server_seed_random(the_seed);
	} else if (server_replay_mode()) {
		/*
		 * Deterministic replay without a configured seed is a hard error:
		 * failing predictably is better than silently dumping a different
		 * time()-seeded stream.
		 */
		fprintf(stderr,
				"xtank: replay mode requires an explicit seed "
				"(set XTANK_SEED to a decimal integer)\n");
		exit(2);
	} else {
		init_random();
		/* Capture the legacy time()-based seed so the lifecycle oracle can
		   replay each epoch from the same stream. */
		the_seed = (unsigned int) time((long *) NULL);
	}
	seed_captured = 1;
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
** P1.12 presentation-pass gate.
**
** This is deliberately a different question from the P1.11 specials draw gate
** (special_presentation_active): it decides whether animate() runs the
** per-terminal input poll, the per-terminal display pass and terminal
** synchronization at all.  The X front-end always presents, so the legacy
** front-end path in animate() is unchanged.  The display-free server never
** presents: it omits the terminal pass entirely and reproduces the small set
** of simulation-visible side effects that pass used to have (the end-of-tick
** disc thrower reset and the camouflage shadow advancement) as server-owned
** bookkeeping.
**
** XTANK_SERVER_PRESENT is a test-only control (used by
** Tests/phase1/check-x-boundary.sh --runtime to show the runtime
** instrumentation observes the calls when the gate is forced on); a real run
** never sets it.
*/
int
server_presentation_active(void)
{
#ifdef NULL_BACKEND
	static int cached = -1;

	if (cached < 0) {
		const char *e = getenv("XTANK_SERVER_PRESENT");

		cached = (e != NULL && atoi(e) != 0) ? 1 : 0;
	}
	return cached;
#else
	return 1;
#endif
}

/*
** Explicit, backend-independent tick pacing (P1.9).  When the monotonic loop
** is active this is the absolute-deadline pacer; otherwise it falls back to
** the legacy game-speed pacer so the X front-end is unchanged.  The production
** loop calls server_loop_pace() directly around server_step().
*/
void
server_tick_pace(void)
{
	if (server_loop_active())
		server_loop_pace(server_frame() + 1);
	else
		check_game_speed();
}

/*
** Explicit match-reset seed policy (P1.8).
**
** The pinned single-match behaviour is to seed once at process start and let
** the stream continue across resets; the normal server path does exactly
** that, so no frozen value changes.  The lifecycle replay oracle instead
** restarts every epoch from the captured configured seed so each epoch is
** self-contained and byte-comparable to the frozen X reference.
*/
static void
server_match_seed(void)
{
	if (server_lifecycle_active() && seed_captured)
		server_seed_random(the_seed);
}

/*
** Headless geometry (P1.10): make sure the body ordering and the 16 vehicle
** rotations exist before a match can advance, with no display, window or
** pixmap involvement.  In the X front-end main() has already ordered and
** rotated the bodies (and the null backend performed the no-op upload), so
** both calls are idempotent there; a future display-free entry that builds no
** terminal can rely on this to initialize its own geometry.
*/
static void
server_geometry_init(void)
{
	geometry_order_objects();
	geometry_rotate_objects();
}

/*
** Lifecycle: one match over the retained single-world globals.
*/
int
server_match_init(void)
{
	extern int num_combatants;
	extern Boolean game_running;
	extern char force_states[MAX_SPECIALS];

	if (num_combatants == 0)
		return GAME_FAILED;

	/*
	 * P1.11 release/MVP special preset.  The shipped local server selects it
	 * unconditionally (P1.15).  The oracle control scenarios rely on the
	 * environment gate to keep New Radar, Tac Link, Camo and Rdf, so those
	 * builds (which do not define SERVER_ONLY) keep the XTANK_MVP_PRESET test
	 * seam.
	 */
#ifdef SERVER_ONLY
	special_apply_mvp_preset(force_states);
#else
	if (getenv("XTANK_MVP_PRESET") != NULL)
		special_apply_mvp_preset(force_states);
#endif

	server_geometry_init();

	server_match_seed();

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
