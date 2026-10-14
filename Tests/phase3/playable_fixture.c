/*-
 * Copyright (c) 1988 Terry Donahue
 * All rights reserved.
 *
 * P3.12 playable live-path integration fixture.
 *
 * Drives the *shipped* coordinator + admission + ENet transport + frozen codec
 * and two real P3.10 client sessions through every allowed solo / two-human
 * live profile (0, 1 and 2 drones) on both mazes, and asserts the ordinary
 * match lifecycle end to end:
 *
 *   connect -> handshake -> lobby -> config -> ready -> start -> play ->
 *   finish (natural win or a clearly labelled test-only forced result) ->
 *   immutable results -> reset -> lobby barrier -> next epoch -> clean quit.
 *
 * The natural 1,000-point winner is carried by the Arena two-human / 0-drone
 * profile (the frozen v2 command trace).  Retained four-entry results are
 * checked on the two-human / 2-drone profile (2 humans + 2 drones).  Any
 * finish that is not reached naturally is *forced* through the shipped finish
 * path and every such check is labelled `test-only` so a synthetic placement
 * or result can never be mistaken for an ordinary-match oracle.
 *
 * Departure/admission/timeout are deliberately NOT exercised here; they are
 * owned by the separate P3.7 departure driver (Tests/phase3/departure.sh) so
 * that the ordinary-match lifecycle stays free of departure side effects.
 *
 * This file links the simulation only to host the authoritative server; the
 * shipped client never does.  No gameplay rule changes.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "xtank.h"
#include "bullet.h"
#include "clfkr.h"
#include "object.h"
#include "special.h"
#include "thread.h"
#include "vehicle.h"
#include "server.h"
#include "server_view.h"
#include "server_events.h"
#include "server_session.h"
#include "server_net.h"
#include "intent.h"
#include "proto.h"
#include "gr.h"

#include "transport.h"

#include "session_client.h"

#include "../../Protocol/content-manifest.h"
#include "../../Protocol/limits.h"

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

/* ---- sandbox sendmsg shim (test-harness only; see P2.7/P3.9) --------- */
ssize_t __real_sendmsg(int fd, const struct msghdr *msg, int flags);

ssize_t
__wrap_sendmsg(int fd, const struct msghdr *msg, int flags)
{
	if (msg != NULL && msg->msg_name != NULL) {
		struct sockaddr_storage local;
		socklen_t ll = sizeof(local);
		struct msghdr m = *msg;
		struct sockaddr unspec;
		ssize_t r;
		int had;

		if (connect(fd, (struct sockaddr *) msg->msg_name,
			    msg->msg_namelen) != 0)
			return __real_sendmsg(fd, msg, flags);
		had = getsockname(fd, (struct sockaddr *) &local, &ll) == 0 &&
		      local.ss_family == AF_INET;
		m.msg_name = NULL;
		m.msg_namelen = 0;
		r = __real_sendmsg(fd, &m, flags);
		(void) memset(&unspec, 0, sizeof(unspec));
		unspec.sa_family = AF_UNSPEC;
		(void) connect(fd, &unspec, sizeof(unspec));
		if (had) {
			int on = 1;

			(void) setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
					  &on, sizeof(on));
			(void) bind(fd, (struct sockaddr *) &local, ll);
		}
		return r;
	}
	return __real_sendmsg(fd, msg, flags);
}

/* ---- acceptance bookkeeping ------------------------------------------- */
static int failures;
static int checks;

static void
check(const char *what, int ok)
{
	checks++;
	if (!ok) {
		fprintf(stderr, "  [FAIL] %s\n", what);
		failures++;
	} else {
		fprintf(stderr, "  [ok]   %s\n", what);
	}
}

/* ---- scenario command log --------------------------------------------- */
#define MAX_CMDS 32768

typedef struct {
	int tick;
	int player;
	char op[24];
	double a, b;
	char b_str[16];
} Cmd;

static Cmd g_cmds[MAX_CMDS];
static int g_ncmds;

static void
load_trace(const char *path)
{
	FILE *fp = fopen(path, "r");
	char line[256];

	if (fp == NULL) {
		fprintf(stderr, "playable_fixture: cannot open trace %s\n", path);
		exit(2);
	}
	while (fgets(line, sizeof(line), fp) != NULL && g_ncmds < MAX_CMDS) {
		char *p = line;
		char *tok, *save;
		Cmd c;

		while (*p == ' ' || *p == '\t')
			p++;
		if (*p == '#' || *p == '\n' || *p == '\0')
			continue;
		memset(&c, 0, sizeof(c));
		tok = strtok_r(p, " \t\n", &save);
		if (tok == NULL)
			continue;
		c.tick = atoi(tok);
		tok = strtok_r(NULL, " \t\n", &save);
		if (tok == NULL)
			continue;
		c.player = atoi(tok);
		tok = strtok_r(NULL, " \t\n", &save);
		if (tok == NULL)
			continue;
		strncpy(c.op, tok, sizeof(c.op) - 1);
		tok = strtok_r(NULL, " \t\n", &save);
		if (tok != NULL) {
			if (strcmp(c.op, "firehold") == 0)
				c.a = (strcmp(tok, "on") == 0) ? 1.0 : 0.0;
			else
				c.a = strtod(tok, NULL);
		}
		tok = strtok_r(NULL, " \t\n", &save);
		if (tok != NULL) {
			c.b = strtod(tok, NULL);
			strncpy(c.b_str, tok, sizeof(c.b_str) - 1);
		}
		g_cmds[g_ncmds++] = c;
	}
	fclose(fp);
}

static uint8_t
wire_op(const char *op, double *a, double *b, const char *bstr, uint8_t *flags)
{
	*flags = 0;
	if (strcmp(op, "wep") == 0) {
		*flags = XTK_FLAG_EDGE;
		*b = (strcmp(bstr, "on") == 0) ? 1.0 :
		     (strcmp(bstr, "off") == 0) ? 0.0 : 2.0;
		return XTK_OP_WEAPON;
	}
	if (strcmp(op, "drive") == 0) {
		*a = (double) (int) *a;
		*flags = XTK_FLAG_HELD;
		return XTK_OP_DRIVE_REL;
	}
	if (strcmp(op, "driveabs") == 0) {
		*a = (double) (int) *a;
		*flags = XTK_FLAG_HELD;
		return XTK_OP_DRIVE_ABS;
	}
	if (strcmp(op, "turn") == 0) {
		*flags = XTK_FLAG_EDGE;
		return XTK_OP_HEADING;
	}
	if (strcmp(op, "aim") == 0) {
		*flags = XTK_FLAG_EDGE;
		return XTK_OP_AIM_WORLD;
	}
	if (strcmp(op, "fire") == 0) {
		*flags = XTK_FLAG_EDGE;
		return XTK_OP_FIRE;
	}
	if (strcmp(op, "firehold") == 0) {
		*flags = XTK_FLAG_HELD;
		return XTK_OP_FIRE_HOLD;
	}
	return XTK_OP_NONE;
}

/* ---- bring-up ---------------------------------------------------------- */
static const char *g_settings_path;
static int g_maze_id = XTK_CONTENT_ID_ARENA;
static int g_ticks = 4000;

static int
server_bringup(const char *assets, const char *maze_name)
{
	ServerConfig *cfg;
	int idx = -1;

	bset = &actual_bset;
	eset = &actual_eset;
	InitConfigStruct(&command_options);
	command_options.NoDelay = TRUE;
	command_options.AutoExit = TRUE;
	command_options.PrintScores = FALSE;
	strncpy(username, "p3.12", MAX_STRING - 1);
	username[MAX_STRING - 1] = '\0';

	if (content_set_root(assets) != 0) {
		fprintf(stderr, "playable_fixture: %s\n", content_last_error());
		return -1;
	}
	cfg = server_config();
	server_config_init(cfg);
	server_config_release(cfg);
	cfg->seed = 114336;
	cfg->seed_set = 1;
	server_init(cfg);

	init_prog_descs();
	init_settings();
	if (make_mdesc((char *) maze_name, &idx) != DESC_LOADED) {
		fprintf(stderr, "playable_fixture: cannot load %s\n", maze_name);
		return -1;
	}
	if (g_settings_path != NULL &&
	    content_load_settings(g_settings_path) != 0) {
		fprintf(stderr, "playable_fixture: %s\n", content_last_error());
		return -1;
	}
	init_threader();
	init_msg_sys();
	init_box_names();
	geometry_order_objects();
	geometry_rotate_objects();
	return 0;
}

/* ---- live network path ------------------------------------------------- */
typedef struct {
	XtkSessionClient *sc;
	uint32_t pkt_seq;
} NetPeer;

static int
net_callback(void *user, const XtkMessage *m)
{
	(void) user;
	(void) m;
	return 0;
}

static void
net_step(NetPeer *p, uint64_t *ns)
{
	(void) server_session_service((*ns) / 1000000ULL);
	(void) session_client_pump(p->sc, *ns, net_callback, NULL);
	*ns += 66666667ULL;
}

static void
pump_until(NetPeer *peers, int n, int (*pred)(void *), void *arg, int max)
{
	uint64_t ns = 1000000000ULL;
	int i;
	(void) ns;

	for (i = 0; i < max && !pred(arg); i++) {
		int k;

		for (k = 0; k < n; k++)
			net_step(&peers[k], &ns);
	}
}

/* predicate adapters */
struct state_arg { NetPeer *peers; int n; XtkSessionState want; };
static int
all_state(void *a)
{
	struct state_arg *s = a;
	int i;

	for (i = 0; i < s->n; i++)
		if (session_client_state(s->peers[i].sc) != s->want)
			return 0;
	return 1;
}

/* Feed one tick of the frozen command trace to each human over the codec. */
static void
feed_input(NetPeer *peers, int n, int tick)
{
	int pi, i;

	for (pi = 0; pi < n; pi++) {
		NetPeer *p = &peers[pi];
		XtkMessage batch;
		int cnt = 0;

		memset(&batch, 0, sizeof(batch));
		batch.env.message_type = XTK_MSG_INPUT_BATCH;
		batch.env.epoch = session_client_epoch(p->sc);
		batch.body.input_batch.packet_sequence = ++p->pkt_seq;
		batch.body.input_batch.last_observed_server_tick =
			(uint32_t) frame;
		for (i = 0; i < g_ncmds; i++) {
			XtkCommand c;
			Cmd *cmd = &g_cmds[i];

			if (cmd->tick != tick || cmd->player != pi)
				continue;
			memset(&c, 0, sizeof(c));
			c.a = cmd->a;
			c.b = cmd->b;
			c.command_id = (uint32_t) (tick * 64 + pi * 16 + cnt + 1);
			c.tick_hint = (uint32_t) tick;
			c.op = wire_op(cmd->op, &c.a, &c.b, cmd->b_str,
				       &c.flags);
			batch.body.input_batch.command[cnt++] = c;
		}
		if (cnt == 0) {
			/* Idle tick keeps the session alive at 15 Hz with no
			 * engine effect (the shipped client heartbeat). */
			XtkCommand hb;

			memset(&hb, 0, sizeof(hb));
			hb.op = XTK_OP_HEARTBEAT;
			hb.command_id = (uint32_t) (tick * 64 + pi * 16 + 1);
			hb.tick_hint = (uint32_t) tick;
			batch.body.input_batch.command[0] = hb;
			cnt = 1;
		}
		batch.body.input_batch.command_count = (uint16_t) cnt;
		(void) session_client_send_input(p->sc, &batch);
	}
}

/*
 * Run one live profile.  `humans` is 1 (solo) or 2; `drones` 0..2.  `natural`
 * selects the Arena two-human / 0-drone profile that is expected to reach a
 * real 1,000-point win; every other profile is bounded and, if it does not
 * finish on its own, finished through the shipped path and reported
 * `test-only`.
 */
static void
run_profile(const char *label, int humans, int drones, const char *maze_name,
	    int natural)
{
	Transport *host;
	uint16_t port;
	ServerSession *s = NULL;
	NetPeer peers[2];
	struct state_arg sa;
	uint64_t ns = 1000000000ULL;
	int e, t, i, pi, status;

	memset(peers, 0, sizeof(peers));
	server_session_init();
	server_session_set_capacity(humans);
	host = transport_create(0, SERVER_SESSION_MAX_PEERS);
	check("live: listen transport created", host != NULL);
	if (host == NULL)
		return;
	port = transport_port(host);
	server_session_set_transport(host);

	for (i = 0; i < humans; i++)
		peers[i].sc = session_client_create();
	check("live: clients created", peers[0].sc != NULL &&
	      (humans < 2 || peers[1].sc != NULL));

	for (i = 0; i < humans; i++) {
		char nm[16];

		snprintf(nm, sizeof(nm), "player%c", 'A' + i);
		(void) session_client_connect(peers[i].sc, "127.0.0.1", port, nm);
	}
	sa.peers = peers; sa.n = humans; sa.want = XTK_SC_LOBBY;
	pump_until(peers, humans, all_state, &sa, 8000);
	check("live: all humans in lobby",
	      session_client_state(peers[0].sc) == XTK_SC_LOBBY &&
	      (humans < 2 || session_client_state(peers[1].sc) == XTK_SC_LOBBY));
	check("live: first peer is admin", session_client_is_admin(peers[0].sc) == 1);

	(void) session_client_send_config(peers[0].sc, (uint16_t) g_maze_id,
					  XTK_CONTENT_ID_VANGUARD,
					  (uint8_t) drones);
	for (i = 0; i < 8000 && session_client_config_revision(peers[0].sc) == 0; i++) {
		int k;

		for (k = 0; k < humans; k++)
			net_step(&peers[k], &ns);
	}
	for (i = 0; i < humans; i++)
		(void) session_client_send_ready(peers[i].sc, 1);

	for (e = 0; e < 2; e++) {
		char what[96];

		if (e > 0) {
			/* The previous epoch already reset to the lobby
			 * barrier; re-arm readiness for the next start. */
			for (i = 0; i < humans; i++)
				(void) session_client_send_ready(peers[i].sc, 1);
			for (i = 0; i < 200; i++) {
				int k;

				for (k = 0; k < humans; k++)
					net_step(&peers[k], &ns);
			}
		}
		server_seed_random(114336);
		for (i = 0; i < humans; i++)
			peers[i].pkt_seq = 0;
		(void) session_client_send_start(peers[0].sc);

		s = NULL;
		for (i = 0; i < 8000; i++) {
			int k;

			for (k = 0; k < humans; k++)
				net_step(&peers[k], &ns);
			s = server_session_current();
			if (s != NULL &&
			    server_session_state(s) == SERVER_SESSION_RUNNING)
				break;
		}
		check("live: match running", s != NULL &&
		      server_session_state(s) == SERVER_SESSION_RUNNING);

		{
			int forced = 0;
			int win = 0;
			XtkResults r;
			int have_r;

			status = GAME_RUNNING;
			for (t = 1; t <= g_ticks; t++) {
				int k;

				feed_input(peers, humans, t);
				for (k = 0; k < humans; k++)
					net_step(&peers[k], &ns);
				s = server_session_current();
				if (s == NULL || server_session_state(s) !=
				    SERVER_SESSION_RUNNING)
					break;
				status = server_session_step(s, ns / 1000000ULL);
				if (status != GAME_RUNNING) {
					win = (status == GAME_RESET);
					break;
				}
			}
			if (t > g_ticks) {
				/* No natural finish within the bound.  Finish
				 * through the shipped path; this is synthetic
				 * and labelled test-only. */
				server_session_finish(s, GAME_RESET);
				forced = 1;
			}

			/* settle RESULTS to the clients */
			for (i = 0; i < 400; i++) {
				int k;

				for (k = 0; k < humans; k++)
					net_step(&peers[k], &ns);
			}

			snprintf(what, sizeof(what),
				 "live: %s epoch %d results retained", label, e);
			have_r = server_events_results_valid() &&
				 server_events_results(&r) == SERVER_EVENTS_OK;
			/* The server normalizes solo to a fixed drone count
			 * (effective_drones), so the expected participant count
			 * is humans + the effective drones. */
			{
				int eff = (humans <= 1) ?
					(int) XTK_SOLO_DRONES : drones;

				check(what, have_r &&
				      r.vehicle_count ==
				      (uint8_t) (humans + eff));
			}

			snprintf(what, sizeof(what),
				 "live: %s epoch %d results -> clients", label, e);
			check(what, session_client_state(peers[0].sc) ==
			      XTK_SC_RESULTS &&
			      (humans < 2 ||
			       session_client_state(peers[1].sc) ==
			       XTK_SC_RESULTS));

			if (natural) {
				int scored = 0;

				for (i = 0; i < (int) r.vehicle_count; i++)
					if (r.entry[i].score >= 1000)
						scored = 1;
				snprintf(what, sizeof(what),
					 "live: %s epoch %d natural 1000-point win",
					 label, e);
				check(what, !forced && win &&
				      r.result_kind == 0 &&
				      r.winner_slot != 0xFF && scored);
			} else if (forced) {
				snprintf(what, sizeof(what),
					 "live: %s epoch %d forced result (test-only)",
					 label, e);
				check(what, 1);
			}

			(void) session_client_send_reset(peers[0].sc);
			sa.want = XTK_SC_LOBBY;
			pump_until(peers, humans, all_state, &sa, 8000);
			snprintf(what, sizeof(what),
				 "live: %s epoch %d reset -> lobby barrier",
				 label, e);
			check(what, session_client_state(peers[0].sc) ==
			      XTK_SC_LOBBY &&
			      (humans < 2 ||
			       session_client_state(peers[1].sc) ==
			       XTK_SC_LOBBY));
		}
	}

	for (pi = 0; pi < humans; pi++)
		session_client_destroy(peers[pi].sc);
	transport_destroy(host);
}

int
main(int argc, char *argv[])
{
	const char *mode, *assets, *maze, *settings, *trace;
	int drones;

	if (argc < 7) {
		fprintf(stderr, "usage: %s <solo|duo> <assets> <maze> "
			"<settings> <trace> <drone-count>\n", argv[0]);
		return 2;
	}
	mode = argv[1];
	assets = argv[2];
	maze = argv[3];
	g_settings_path = argv[4];
	trace = argv[5];
	drones = atoi(argv[6]);
	{
		const char *tk = getenv("XTANK_P3_TICKS");

		if (tk != NULL && atoi(tk) > 0)
			g_ticks = atoi(tk);
	}

	if (strcmp(maze, "combat1") == 0)
		g_maze_id = XTK_CONTENT_ID_COMBAT1;
	else
		g_maze_id = XTK_CONTENT_ID_ARENA;

	if (server_bringup(assets, maze) != 0)
		return 2;
	load_trace(trace);

	if (strcmp(mode, "solo") == 0)
		run_profile("solo", 1, drones, maze, 0);
	else if (strcmp(mode, "duo") == 0)
		run_profile("duo", 2, drones, maze,
			    (drones == 0 && g_maze_id == XTK_CONTENT_ID_ARENA));
	else
		return 2;

	if (failures != 0) {
		printf("PLAYABLE FIXTURE FAIL (%d checks, %d failures)\n",
		       checks, failures);
		return 1;
	}
	printf("PLAYABLE FIXTURE PASS (%d checks, mode=%s, drones=%d)\n",
	       checks, mode, drones);
	return 0;
}
