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
** server.h -- explicit match/player context and simulation-ownership seam
** (P1.6).
**
** The legacy engine keeps one world in file-scope globals and discovers its
** players from the window-system resource database.  This header names the
** server-owned pieces the headless/server path needs without a display:
**
**   - an explicit player-to-terminal binding (ServerConfig) that replaces the
**     temporary XTANK_NULL_PLAYERS test hook and the X resource lookup;
**   - ownership of the seed/RNG lifecycle and the game clock (frame);
**   - an explicit, backend-independent tick-pacing seam;
**   - initialize/step/reset/destroy for one match over the retained
**     single-world globals (no multi-match conversion).
*/

#ifndef _SERVER_H_
#define _SERVER_H_

#include "tanktypes.h"
#include "tanklimits.h"
#include "team.h"
#include "settings.h"
#include "maze.h"
#include "vdesc.h"

#define SERVER_MAX_PLAYERS	MAX_TERMINALS

/*
 * Approved release/MVP content and match limits (PHASE0-BRIEF section 3):
 * Combat, individual winning score 1000, at most four vehicles, all
 * players/bots in a Vanguard, and the approved special preset.
 */
#define SERVER_RELEASE_MAX_VEHICLES	4

/*
 * One explicit player binding: a player name attached to a terminal slot.
 * The name is what a settings file's "V#n: <name>, ..." line matches against.
 */
typedef struct {
	int num_players;
	char player_name[SERVER_MAX_PLAYERS][MAX_STRING + 1];
	int terminal[SERVER_MAX_PLAYERS];
} ServerPlayers;

/*
 * Server-owned match configuration.  seed_set == 0 selects the legacy
 * time()-based default; seed_set == 1 pins the RNG to seed before the first
 * random call (P1.8).  tick_hz == 0 selects the legacy game_speed pacing.
 */
typedef struct {
	ServerPlayers players;
	unsigned int seed;
	int seed_set;
	int tick_hz;
	int max_vehicles;	/* <= 0 selects the legacy MAX_VEHICLES limit */
} ServerConfig;

/* The single server configuration used by this process. */
ServerConfig *server_config(void);
void server_config_init(ServerConfig *cfg);
int server_config_add_player(ServerConfig *cfg, const char *name);
const char *server_player_name(int terminal_index);

/*
 * Explicit terminal-free human binding (P2.4).
 *
 * Legacy humans are Terminal objects and the engine binds a human to a tank
 * through terminal[c->player[i]].  A terminal-free (network/local server)
 * human is instead an explicit binding owned by the server:
 *
 *   controller  a stable controller/session id (NOT a terminal slot),
 *   slot        the combatant/vehicle index the controller drives,
 *   generation  bumped whenever the binding is (re)bound to a slot, so a
 *               respawned or reassigned slot cannot be driven by a stale
 *               controller without validation, and
 *   name        the player name shown and matched against settings.
 *
 * These functions never allocate a Terminal.  The legacy X front-end does not
 * use them; the display-free server does.  All functions are idempotent and
 * bounded (SERVER_MAX_HUMANS).
 */
#define SERVER_MAX_HUMANS	SERVER_RELEASE_MAX_VEHICLES

typedef struct {
	int used;
	int controller;			/* stable controller/connection id */
	unsigned int generation;	/* bumped on (re)bind */
	int slot;			/* combatant/vehicle index, -1 unbound */
	char name[MAX_STRING + 1];
} ServerHumanBinding;

/* Clear every binding (called once per process before a match is built). */
void server_players_init(void);

/*
 * Register a named human controlled by `controller` (>= 0).  Returns the
 * binding index, or -1 on a full table, a NULL/empty name, a duplicate name or
 * a duplicate controller.  The new binding starts unbound (slot -1),
 * generation 1.
 */
int server_players_add(const char *name, int controller);

/* Number of registered human bindings and read-only access to one. */
int server_players_count(void);
const ServerHumanBinding *server_players_binding(int index);

/*
 * Bind binding `index` to combatant/vehicle `slot`.  If another binding already
 * owns that slot, its generation is bumped and it is unbound first, so the
 * displaced controller can no longer drive the slot.  Returns 0 on success,
 * -1 on a bad index/slot.
 */
int server_players_bind_slot(int index, int slot);

/* Release whatever binding owns `slot` (bumping its generation). */
void server_players_unbind_slot(int slot);

/*
 * Resolve/claim a combatant slot for a terminal-free human named `name`.
 * If a binding with that name exists it is bound to `slot` (idempotently) and
 * its index returned; otherwise -1.  Used by init_combatants() to attach a
 * settings-defined human to its explicit controller.
 */
int server_players_claim_name(const char *name, int slot);

/* Binding index for a raw player[] value, or -1 if it is not a live binding. */
int server_players_index_for_player(int player);

/* Name of the binding bound to `slot`, or "" when none. */
const char *server_players_name_for_slot(int slot);

/*
 * Controller ownership check.  Returns 1 only when `controller` owns `slot`
 * with exactly `generation`; a stale generation or a different slot is 0.
 * A negative generation means "ignore generation".
 */
int server_players_validate(int controller, int slot, unsigned int generation);

/* 1 when at least one terminal-free human binding is registered. */
int server_players_active(void);

/*
 * Safe terminal accessor for the legacy terminal-facing setup paths.  Returns
 * terminal[player_index] when that legacy terminal exists, or NULL when the
 * run is terminal-free (num_terminals == 0) or the slot is out of range.  The
 * caller must treat NULL as "no terminal to configure" and skip the legacy
 * terminal work; the binding above carries the human identity instead.
 */
void *server_player_terminal(int player_index);

/*
 * Approved release/MVP configuration (P1.14, PHASE0-BRIEF section 3).
 * server_release_settings() starts from init_settings() and applies only the
 * approved overrides: Combat free-for-all (team scoring off), respawn on,
 * bots may win, individual winning score 1000 and 15 simulation ticks/s.
 * server_config_release() additionally pins the server config fields (15 Hz
 * and at most SERVER_RELEASE_MAX_VEHICLES vehicles).
 */
void server_release_settings(Settings *s);
void server_config_release(ServerConfig *cfg);

/*
 * Explicit seed source (P1.8).  Reads XTANK_SEED into cfg before the RNG is
 * seeded.  Returns 0 when a valid seed was configured, 1 when the variable is
 * absent (legacy time()-based default), and -1 when it is malformed (empty,
 * non-numeric, trailing junk or out of range).
 */
int server_config_seed_env(ServerConfig *cfg);

/*
 * One-time server initialisation: installs the configuration and seeds the
 * RNG before the first random call.  Replaces the bare init_random() call.
 */
void server_init(ServerConfig *cfg);

/* One match, over the retained single-world globals. */
int server_match_init(void);
int server_step(void);
void server_match_reset(void);
void server_destroy(void);

/*
 * Display-free match result (P1.14): finalize/report the result and reset
 * per-match scores with no graphical stats wait or event loop.
 */
int game_results(int status);

/* Server-owned simulation state. */
int server_frame(void);
void server_frame_reset(void);
void server_seed_random(unsigned int seed);
void server_tick_pace(void);

/*
 * Default simulation rate for the display-free server when no explicit
 * XTANK_TICK_HZ is configured (P1.9).  The X front-end leaves tick_hz == 0
 * and keeps the legacy pacer.
 */
#define SERVER_DEFAULT_TICK_HZ	15

/*
 * Monotonic fixed-step loop (P1.9).  server_loop_configure() installs the
 * configured rate (0 = legacy front-end pacing); server_loop_active() reports
 * whether the monotonic loop owns pacing.  server_loop_run() advances one
 * match with absolute 1/tick_hz deadlines and reports bounded per-tick work.
 */
void server_loop_configure(int hz);
int server_loop_active(void);
int server_loop_hz(void);
long server_loop_ticks(void);
void server_loop_pace(int tick);
int server_loop_run(void);

/*
 * Bounded player-intent queue seam (P1.7).  The queue is consumed exactly
 * once per tick, after bot programs and before movement, by server_step();
 * stale or malformed intents are rejected when submitted, never here.
 */
int server_consume_intents(int tick);

/* Lifecycle bookkeeping used by the P1.6 lifecycle oracle only. */
int server_lifecycle_active(void);
int server_lifecycle_continue(void);

/*
 * Explicit asset root and list-independent content loading (P1.14).
 *
 * content_set_root() installs an explicit asset root and derives the
 * Vehicles/Mazes/Programs/Src-Include directories beneath it.  It consults
 * no environment variable, opens no display and returns 0 on success or -1
 * (with content_last_error()) when the root is missing, empty or not a
 * directory.
 *
 * content_load_maze()/content_load_vehicle() load a single named asset from
 * that root by explicit path.  They never read the directory list and never
 * fall back to a random maze; a missing or malformed asset returns a DESC_*
 * code and a human-readable reason.  content_load_settings() parses a
 * settings file the same way (returning 0 on success, -1 on failure).
 */
int content_set_root(const char *root);
const char *content_root(void);
int content_load_maze(Mdesc *d, const char *name);
int content_load_vehicle(Vdesc *d, const char *name);
int content_load_settings(const char *path);
const char *content_last_error(void);

/*
 * Test-only content/config self-check (P1.14).  Runs from get_environment()
 * when XTANK_CONTENT_CHECK is set, loads the supplied mazes and Vanguard
 * through the explicit loaders and prints the release configuration; it
 * exits the process and never returns.  A normal front-end or server run
 * never sets XTANK_CONTENT_CHECK.
 */
void content_check_run(void);

#endif /* _SERVER_H_ */
