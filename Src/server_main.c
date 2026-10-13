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
** server_main.c -- terminal-free local server entry point (P1.15).
**
** This is the shipped local server's main(): it builds no Terminal, opens no
** display and never enters the graphical lobby.  It selects a supplied maze,
** an explicit seed and zero to two local drone bots, then runs the approved
** release configuration (P1.14) to completion through the reusable server
** core.
**
** The X front-end entry point (main.c) and the maze/vehicle designers are not
** linked into this binary; assets and notices are resolved relative to the
** executable so a relocated bundle runs with DISPLAY, XTANK_DIR and HOME
** unset.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef MAXPATHLEN
#define MAXPATHLEN 1024
#endif

#include "xtank.h"
#include "bullet.h"
#include "clfkr.h"
#include "object.h"
#include "thread.h"
#include "vehicle.h"
#include "server.h"
#include "proto.h"
#ifdef XTANK_WITH_NET
#include "server_session.h"
#endif

extern char username[MAX_STRING];
extern Mdesc *mdesc;
extern Vdesc *vdesc;
extern int num_combatants;
extern Combatant combatant[];

char team_char[] = "NROYGBV";

Eset actual_eset, *eset;
Bset actual_bset, *bset;

Maze maze;
Map real_map;
Settings settings;
Boolean game_paused;
char *executable_name;

extern char *version1;
extern char *version2;
extern char *version3;

int num_veh;
int num_veh_alive;
int num_veh_dead;
Vehicle actual_vehicles[MAX_VEHICLES];
Vehicle *live_vehicles[MAX_VEHICLES];
Vehicle *dead_vehicles[MAX_VEHICLES];
CLFkr command_options;

/*
** Command-option defaults.  This helper lives in main.c for the X front-end;
** the server carries its own copy because main.c is not linked here.
*/
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

#define SERVER_DEFAULT_MAZE	"combat1"
#define SERVER_DEFAULT_VEHICLE	"Vanguard"
#define SERVER_DEFAULT_DRONES	2
#define SERVER_MAX_DRONES	2   /* PHASE0-BRIEF: at most four vehicles */

static void
server_usage(const char *prog)
{
	printf("usage: %s [options]\n", prog);
	printf("  --maze NAME        supplied maze to load (default %s)\n",
		   SERVER_DEFAULT_MAZE);
	printf("  --vehicle NAME     supplied vehicle (default %s)\n",
		   SERVER_DEFAULT_VEHICLE);
	printf("  --drones N         local drone players, 0..%d (default %d)\n",
		   SERVER_MAX_DRONES, SERVER_DEFAULT_DRONES);
	printf("  --human NAME       one terminal-free human controller (P2.4)\n");
	printf("  --seed N           explicit RNG seed (default XTANK_SEED)\n");
	printf("  --matches N        matches to play/reset (default 1)\n");
	printf("  --max-ticks N      bound each match to N ticks (0 = unlimited)\n");
	printf("  --assets DIR       explicit asset root\n");
	printf("  --listen PORT      run the P3.4 two-human lobby/match server\n");
	printf("  --fast             bypass wall-clock pacing (replay)\n");
	printf("  --version          print version and exit\n");
	printf("  --help             this message\n");
}

/*
** Resolve the asset root relative to the running executable: a bundle keeps
** its assets next to the binary (or under ../share/xtank).  Returns 1 and
** fills `out` on the first candidate containing a Mazes directory.
*/
static int
server_find_assets(char *out, size_t outsz)
{
	char exe[MAXPATHLEN];
	char dir[MAXPATHLEN];
	char cand[MAXPATHLEN];
	ssize_t n;
	char *slash;
	static const char *suffix[] = {
		"/../share/xtank", "/..", "", "/share/xtank", NULL
	};
	int i;

	n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
	if (n <= 0)
		return 0;
	exe[n] = '\0';
	slash = strrchr(exe, '/');
	if (slash != NULL)
		*slash = '\0';
	else
		strcpy(exe, ".");

	for (i = 0; suffix[i] != NULL; i++) {
		struct stat st;

		snprintf(cand, sizeof(cand), "%s%s", exe, suffix[i]);
		snprintf(dir, sizeof(dir), "%s/Mazes", cand);
		if (stat(dir, &st) == 0 && S_ISDIR(st.st_mode)) {
			strncpy(out, cand, outsz - 1);
			out[outsz - 1] = '\0';
			return 1;
		}
	}
	return 0;
}

/*
** Build the local combatant set: an optional terminal-free human in Vanguard
** slot 0, then `n` drone bots in Vanguards, on distinct teams.  This replaces
** the graphical lobby's customized-combatants grid.  The human is bound
** explicitly (name -> controller 0 -> slot 0) and is never given a Terminal.
*/
static void
server_build_combatants(int n, int vdesc_index, const char *human)
{
	extern Combatant combatant[];
	extern int num_combatants;
	Combatant *c;
	int i, total;

	total = n + (human != NULL ? 1 : 0);
	if (total == 0)
		total = 1;			/* callers validate; keep a sane default */
	num_combatants = total;

	i = 0;
	if (human != NULL) {
		int b;

		server_players_init();
		b = server_players_add(human, 0);
		if (b >= 0)
			(void) server_players_bind_slot(b, 0);

		c = &combatant[0];
		c->num_players = 1;
		c->player[0] = (b >= 0) ? b : 255;	/* 255 == UNDEFINED */
		c->num_programs = 0;
		c->team = 1 % MAX_TEAMS;
		c->vdesc = vdesc_index;
		(void) strncpy(c->name, human, MAX_STRING - 1);
		c->name[MAX_STRING - 1] = '\0';
		i = 1;
	}

	for (; i < total; i++) {
		c = &combatant[i];
		c->num_players = 0;
		c->num_programs = 1;
		c->program[0] = 0;	/* drone is the only registered program */
		c->team = (i + 1) % MAX_TEAMS;
		c->vdesc = vdesc_index;
	}
}

int
main(int argc, char *argv[])
{
	const char *prog = (argc > 0) ? argv[0] : "xtank-server";
	const char *maze_name = SERVER_DEFAULT_MAZE;
	const char *vehicle_name = SERVER_DEFAULT_VEHICLE;
	const char *assets = NULL;
	const char *human_name = NULL;
	long drones = -1;
	long matches = 1;
	long max_ticks = 0;
	long listen_port = -1;
	int seed_given = 0;
	unsigned long seed = 0;
	int fast = 0;
	int i, vdesc_index, mdesc_index;
	ServerConfig *cfg;

	/*
	 * A bundle may be relocated anywhere (including a path with spaces);
	 * resolve assets relative to the executable unless the caller overrides.
	 * The environment is only a fallback, never required, so an unset
	 * DISPLAY/XTANK_DIR/HOME cannot affect the run.
	 */
	assets = getenv("XTANK_SERVER_ASSETS");
	if (assets == NULL)
		assets = getenv("XTANK_ASSET_ROOT");

	for (i = 1; i < argc; i++) {
		const char *a = argv[i];
		int has_next = (i + 1 < argc);

		if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
			server_usage(prog);
			return 0;
		} else if (!strcmp(a, "--version")) {
			puts(version1);
			puts(version2);
			puts(version3);
			return 0;
		} else if (!strcmp(a, "--maze") && has_next) {
			maze_name = argv[++i];
		} else if (!strcmp(a, "--vehicle") && has_next) {
			vehicle_name = argv[++i];
		} else if (!strcmp(a, "--human") && has_next) {
			human_name = argv[++i];
			if (human_name[0] == '\0') {
				fprintf(stderr, "%s: --human needs a non-empty name\n",
						prog);
				return 2;
			}
		} else if (!strcmp(a, "--assets") && has_next) {
			assets = argv[++i];
		} else if (!strcmp(a, "--listen") && has_next) {
			char *end;

			listen_port = strtol(argv[++i], &end, 10);
			if (*end != '\0' || listen_port < 0 ||
			    listen_port > 65535) {
				fprintf(stderr, "%s: malformed --listen port\n",
						prog);
				return 2;
			}
		} else if (!strcmp(a, "--drones") && has_next) {
			drones = atol(argv[++i]);
		} else if (!strcmp(a, "--matches") && has_next) {
			matches = atol(argv[++i]);
		} else if (!strcmp(a, "--max-ticks") && has_next) {
			max_ticks = atol(argv[++i]);
		} else if (!strcmp(a, "--seed") && has_next) {
			char *end;

			seed = strtoul(argv[++i], &end, 10);
			if (*end != '\0') {
				fprintf(stderr, "%s: malformed --seed\n", prog);
				return 2;
			}
			seed_given = 1;
		} else if (!strcmp(a, "--fast")) {
			fast = 1;
		} else {
			fprintf(stderr, "%s: unknown option '%s'\n", prog, a);
			server_usage(prog);
			return 2;
		}
	}

	if (drones < 0) {
		const char *e = getenv("XTANK_SERVER_DRONES");

		drones = (e != NULL) ? atol(e) : SERVER_DEFAULT_DRONES;
	}
	if (drones > SERVER_MAX_DRONES) {
		fprintf(stderr, "%s: at most %d local drones are supported\n",
				prog, SERVER_MAX_DRONES);
		return 2;
	}
	if (matches < 1)
		matches = 1;
	if (max_ticks > 0) {
		char b[32];

		snprintf(b, sizeof(b), "%ld", max_ticks);
		(void) setenv("XTANK_SERVER_MATCH_TICKS", b, 1);
	}

	bset = &actual_bset;
	eset = &actual_eset;

	InitConfigStruct(&command_options);
	command_options.NoDelay = fast ? TRUE : FALSE;
	command_options.PrintScores = TRUE;
	command_options.AutoExit = TRUE;

	/*
	 * Explicit asset root.  When none was supplied, derive one relative to
	 * the executable so the bundle is relocatable.
	 */
	if (assets == NULL) {
		char found[MAXPATHLEN];

		if (!server_find_assets(found, sizeof(found))) {
			fprintf(stderr,
					"%s: cannot locate assets relative to %s; "
					"use --assets DIR\n", prog, prog);
			return 2;
		}
		assets = found;
	}
	if (content_set_root(assets) != 0) {
		fprintf(stderr, "%s: %s\n", prog, content_last_error());
		return 2;
	}

	(void) strncpy(username, "server", MAX_STRING - 1);
	username[MAX_STRING - 1] = '\0';

	/* Configuration: approved release preset, 15 Hz, at most four vehicles. */
	cfg = server_config();
	server_config_init(cfg);
	server_config_release(cfg);

	if (seed_given) {
		cfg->seed = (unsigned int) seed;
		cfg->seed_set = 1;
	} else if (server_config_seed_env(cfg) < 0) {
		fprintf(stderr, "%s: malformed XTANK_SEED; expected a decimal "
				"integer.\n", prog);
		return 2;
	}

	server_init(cfg);

	init_prog_descs();
	init_settings();
	server_release_settings(&settings);
	init_threader();
	init_msg_sys();
	init_box_names();
	geometry_order_objects();
	geometry_rotate_objects();

	/*
	 * P2.8 optional listen mode: one peer + one drone lobby/session driven
	 * entirely by the protocol (content is chosen and loaded per START).
	 * This preserves the Phase 1 local CLI above; without a networking build
	 * the option is reported unavailable rather than silently ignored.
	 */
	if (listen_port >= 0) {
#ifdef XTANK_WITH_NET
		return server_session_run((uint16_t) listen_port, fast,
					  max_ticks);
#else
		fprintf(stderr, "%s: --listen is unavailable: this server was "
				"built without networking\n", prog);
		return 2;
#endif
	}

	/* Load the supplied assets by explicit name, with no directory list. */
	if (make_vdesc((char *) vehicle_name, &vdesc_index) != DESC_LOADED) {
		fprintf(stderr, "%s: cannot load vehicle '%s' under %s\n",
				prog, vehicle_name, content_root());
		return 2;
	}
	if (make_mdesc((char *) maze_name, &mdesc_index) != DESC_LOADED) {
		fprintf(stderr, "%s: cannot load maze '%s' under %s\n",
				prog, maze_name, content_root());
		return 2;
	}
	settings.mdesc = &mdesc[mdesc_index];
	settings.si.game = settings.mdesc->type;

	if (drones < 1 && human_name == NULL) {
		fprintf(stderr, "%s: at least one local drone is required to run a "
				"match (or pass --human NAME)\n", prog);
		return 2;
	}
	if (human_name != NULL && drones + 1 > SERVER_RELEASE_MAX_VEHICLES) {
		fprintf(stderr, "%s: at most %d vehicles are supported\n",
				prog, SERVER_RELEASE_MAX_VEHICLES);
		return 2;
	}
	server_build_combatants((int) drones, vdesc_index, human_name);

	printf("SERVER maze=%s vehicle=%s human=%s drones=%ld assets=%s seed=%u\n",
		   maze_name, vehicle_name,
		   (human_name != NULL) ? human_name : "-", drones, content_root(),
		   cfg->seed_set ? cfg->seed : 0);

	for (i = 0; i < matches; i++) {
		int status = play_game();

		if (status == GAME_FAILED) {
			fprintf(stderr, "%s: match %d failed\n", prog, i + 1);
			return 1;
		}
		printf("SERVER-MATCH %d status=%d\n", i + 1, status);
	}

	printf("SERVER-DONE matches=%ld\n", matches);
	return 0;
}
