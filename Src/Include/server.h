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

#define SERVER_MAX_PLAYERS	MAX_TERMINALS

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
 * Server-owned match configuration.  seed == 0 selects the legacy
 * time()-based default; tick_hz == 0 selects the legacy game_speed pacing.
 */
typedef struct {
	ServerPlayers players;
	unsigned int seed;
	int tick_hz;
} ServerConfig;

/* The single server configuration used by this process. */
ServerConfig *server_config(void);
void server_config_init(ServerConfig *cfg);
int server_config_add_player(ServerConfig *cfg, const char *name);
const char *server_player_name(int terminal_index);

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

/* Server-owned simulation state. */
int server_frame(void);
void server_frame_reset(void);
void server_seed_random(unsigned int seed);
void server_tick_pace(void);

/* Lifecycle bookkeeping used by the P1.6 lifecycle oracle only. */
int server_lifecycle_active(void);
int server_lifecycle_continue(void);

#endif /* _SERVER_H_ */
