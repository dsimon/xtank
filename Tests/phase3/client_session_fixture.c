/*-
 * Copyright (c) 1988 Terry Donahue
 * All rights reserved.
 *
 * P3.10 multiplayer client lobby, authority and rejoin acceptance fixture.
 *
 * Two real clients (Client/session_client.c + Client/frame.c) drive the shipped
 * display-free server coordinator over a pinned-ENet loopback transport while
 * the fixture also exercises the client reduction directly:
 *
 *   two_human:
 *     - A connects alone on a two-human server and does NOT auto-start a solo
 *       match (no solo auto-start racing the second player);
 *     - B joins the shared lobby; both see the same server-authored roster;
 *     - the WELCOME player role never grants admin; only LOBBY_STATE decides;
 *     - non-admin config/start/reset are refused locally AND independently by
 *       the server (PROTOCOL_ERROR/NOT_ADMIN);
 *     - both mazes and 0..2 drones are selectable; a config change increments
 *       the revision and wipes readiness (D3-B);
 *     - both ready against the current revision; admin start -> running;
 *     - results -> lobby barrier; admin transfer on departure; in-process
 *       rejoin of a fresh session.
 *
 *   inbound:
 *     - a foreign peer, wrong channel, foreign session id or stale epoch is
 *       rejected before it can regress the client (checked via
 *       session_client_ingest).
 *
 * It links the shipped server core so it can host the authoritative server;
 * the shipped client never does.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "xtank.h"
#include "bullet.h"
#include "clfkr.h"
#include "object.h"
#include "thread.h"
#include "vehicle.h"
#include "server.h"
#include "server_events.h"
#include "server_session.h"
#include "intent.h"
#include "proto.h"
#include "gr.h"

#include "transport.h"

#include "session_client.h"
#include "frame.h"
#include "menu.h"
#include "ui.h"
#include "render.h"

#include "../../Protocol/content-manifest.h"

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

/* ---- sandbox sendmsg shim (test-harness only; see P2.7/P3.9) ---------- */
/*
 * This sandbox denies sendmsg() with an explicit destination address, so the
 * wrapper connects the shared UDP socket to the destination, sends, then
 * disconnects and rebinds the (possibly auto-assigned) local address.  That
 * lets one in-process server host serve two peers at once; with a connected
 * socket the host could only receive from the last destination.  It changes no
 * production code and no wire behaviour.
 */
ssize_t __real_sendmsg(int fd, const struct msghdr *msg, int flags);

ssize_t
__wrap_sendmsg(int fd, const struct msghdr *msg, int flags)
{
	if (msg != NULL && msg->msg_name != NULL) {
		if (connect(fd, (struct sockaddr *) msg->msg_name,
			    msg->msg_namelen) == 0) {
			struct sockaddr_storage local;
			socklen_t ll = sizeof(local);
			struct msghdr m = *msg;
			struct sockaddr unspec;
			ssize_t r;
			int had;

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

/* ---- synthetic clock and stepping ------------------------------------- */

#define STEP_NS  66666667ULL
#define STEP_MS  67ULL
static uint64_t g_ns;
static uint64_t g_ms;

typedef struct {
	XtkSessionClient *sc;
	XtkFrame *fr;
	const char *name;
} Peer;

static void
step_server(void)
{
	ServerSession *s;

	g_ns += STEP_NS;
	g_ms += STEP_MS;
	(void) server_session_service(g_ms);
	s = server_session_current();
	if (s != NULL && server_session_state(s) == SERVER_SESSION_RUNNING)
		(void) server_session_step(s, g_ms);
}

static void
step(Peer *peers, int n)
{
	int i;

	step_server();
	for (i = 0; i < n; i++)
		(void) frame_tick(peers[i].fr, g_ns);
}

static int
pump_until(Peer *peers, int n, int (*done)(Peer *, int), int max_ticks)
{
	int i;

	for (i = 0; i < max_ticks; i++) {
		step(peers, n);
		if (done(peers, n))
			return 1;
	}
	return done(peers, n);
}

static int
state_is_lobby(Peer *peers, int n)
{
	int i;

	for (i = 0; i < n; i++)
		if (session_client_state(peers[i].sc) != XTK_SC_LOBBY)
			return 0;
	return 1;
}

static int
state_is_running(Peer *peers, int n)
{
	int i;

	for (i = 0; i < n; i++)
		if (session_client_state(peers[i].sc) != XTK_SC_RUNNING)
			return 0;
	return 1;
}

static int
state_is_results(Peer *peers, int n)
{
	int i;

	for (i = 0; i < n; i++)
		if (session_client_state(peers[i].sc) != XTK_SC_RESULTS)
			return 0;
	return 1;
}

static int
has_results(Peer *peers, int n)
{
	int i;

	for (i = 0; i < n; i++)
		if (!frame_results(peers[i].fr)->have_results)
			return 0;
	return 1;
}

/* ---- shared setup ----------------------------------------------------- */

static const char *g_assets;

static int
server_bringup(void)
{
	ServerConfig *cfg;

	bset = &actual_bset;
	eset = &actual_eset;
	InitConfigStruct(&command_options);
	command_options.NoDelay = TRUE;
	command_options.AutoExit = TRUE;
	command_options.PrintScores = FALSE;
	(void) strncpy(username, "p3.10", MAX_STRING - 1);
	username[MAX_STRING - 1] = '\0';

	if (content_set_root(g_assets) != 0) {
		fprintf(stderr, "client_session_fixture: %s\n",
			content_last_error());
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
	server_release_settings(&settings);
	init_threader();
	init_msg_sys();
	init_box_names();
	geometry_order_objects();
	geometry_rotate_objects();
	return 0;
}

/* ---- two-human lobby, authority and rejoin ---------------------------- */

static int
run_two_human(void)
{
	Transport *host;
	uint16_t port;
	Peer A, B;
	XtkUiScene scene;
	int i;

	server_session_init();
	server_session_set_capacity(XTK_MAX_HUMANS);
	host = transport_create(0, SERVER_SESSION_MAX_PEERS);
	check("server listen transport created", host != NULL);
	if (host == NULL)
		return 1;
	port = transport_port(host);
	server_session_set_transport(host);

	memset(&A, 0, sizeof(A));
	memset(&B, 0, sizeof(B));

	/* ---- A alone must not race a solo auto-start ---- */
	A.sc = session_client_create();
	A.fr = frame_create(A.sc, NULL, NULL);
	A.name = "alpha";
	check("A created", A.sc != NULL && A.fr != NULL);
	check("A connect initiated",
	      session_client_connect(A.sc, "127.0.0.1", port, "alpha") == 0);
	pump_until(&A, 1, state_is_lobby, 400);
	check("A reached the shared lobby",
	      session_client_state(A.sc) == XTK_SC_LOBBY);
	check("A is admin from LOBBY_STATE (not WELCOME role)",
	      session_client_is_admin(A.sc) == 1 &&
	      session_client_admin_index(A.sc) ==
	      session_client_self_index(A.sc));
	check("A knows the two-human capacity",
	      session_client_human_capacity(A.sc) == XTK_MAX_HUMANS);
	/* Give the server many idle ticks: it must stay in the lobby. */
	for (i = 0; i < 20; i++)
		step(&A, 1);
	check("A alone does not solo-auto-start on a two-human server",
	      session_client_state(A.sc) == XTK_SC_LOBBY &&
	      server_session_count() == 1 &&
	      server_session_state(server_session_current()) ==
	      SERVER_SESSION_LOBBY);
	check("A auto-lobby did not configure while alone",
	      session_client_config_revision(A.sc) == 0);

	/* ---- B joins the shared lobby ---- */
	B.sc = session_client_create();
	B.fr = frame_create(B.sc, NULL, NULL);
	B.name = "bravo";
	check("B created", B.sc != NULL && B.fr != NULL);
	check("B connect initiated",
	      session_client_connect(B.sc, "127.0.0.1", port, "bravo") == 0);
	{
		Peer both[2];
		both[0] = A;
		both[1] = B;
		pump_until(both, 2, state_is_lobby, 400);
		check("both clients in the shared lobby",
		      session_client_state(A.sc) == XTK_SC_LOBBY &&
		      session_client_state(B.sc) == XTK_SC_LOBBY);
		check("A is admin, B is not (WELCOME role grants nothing)",
		      session_client_is_admin(A.sc) == 1 &&
		      session_client_is_admin(B.sc) == 0);
		check("B sees A as the administrator",
		      session_client_admin_index(B.sc) ==
		      session_client_self_index(A.sc));
		check("B has a distinct roster index",
		      session_client_self_index(B.sc) !=
		      session_client_self_index(A.sc));

		/* Roster and config are visible to both clients. */
		check("A roster has two named players",
		      session_client_roster_count(A.sc) == 2);
		check("B roster has two named players",
		      session_client_roster_count(B.sc) == 2);
		{
			XtkRosterEntry e0, e1;
			(void) session_client_roster(A.sc, 0, &e0);
			(void) session_client_roster(A.sc, 1, &e1);
			check("roster names are server-authored",
			      (strcmp(e0.name, "alpha") == 0 &&
			       strcmp(e1.name, "bravo") == 0) ||
			      (strcmp(e0.name, "bravo") == 0 &&
			       strcmp(e1.name, "alpha") == 0));
			check("roster readiness starts clear",
			      e0.ready == 0 && e1.ready == 0);
		}
		menu_build_lobby(frame_lobby(A.fr), &scene);
		check("lobby scene lists both player names",
		      ui_scene_has_text(&scene, "alpha") &&
		      ui_scene_has_text(&scene, "bravo"));
		check("lobby scene shows mazes/vehicles/drones/revision",
		      ui_scene_has_text(&scene, "Mazes") &&
		      ui_scene_has_text(&scene, "Vehicles") &&
		      ui_scene_has_text(&scene, "Drones:") &&
		      ui_scene_has_text(&scene, "Config rev:"));
	}

	/* ---- non-admin controls are disabled and independently rejected ---- */
	check("B cannot configure locally",
	      frame_lobby_select(B.fr, XTK_CONTENT_ID_COMBAT1,
				 XTK_CONTENT_ID_VANGUARD) == -1);
	check("B cannot start locally", frame_request_start(B.fr) == -1);
	check("B cannot reset locally", frame_request_reset(B.fr) == -1);
	check("B control gates report disabled",
	      frame_can_config(B.fr) == 0 && frame_can_start(B.fr) == 0 &&
	      frame_can_reset(B.fr) == 0);
	check("A control gates report enabled",
	      frame_can_config(A.fr) == 1 && frame_can_start(A.fr) == 1);

	/* Server rejection is independent of the local gate. */
	(void) session_client_send_config(B.sc, XTK_CONTENT_ID_COMBAT1,
					  XTK_CONTENT_ID_VANGUARD, 0);
	{
		Peer both[2] = { A, B };
		for (i = 0; i < 20 &&
		     session_client_last_error(B.sc) == XTK_ERR_NONE; i++)
			step(both, 2);
	}
	check("server refused non-admin config (NOT_ADMIN)",
	      session_client_last_error(B.sc) == XTK_ERR_NOT_ADMIN);
	check("non-admin config did not change the revision",
	      session_client_config_revision(A.sc) == 0);
	(void) session_client_send_start(B.sc);
	{
		Peer both[2] = { A, B };
		for (i = 0; i < 20 &&
		     session_client_last_error(B.sc) == XTK_ERR_NONE; i++)
			step(both, 2);
	}
	check("server refused non-admin start (NOT_ADMIN)",
	      session_client_last_error(B.sc) == XTK_ERR_NOT_ADMIN);

	/* ---- admin selects both mazes and a drone count ---- */
	{
		Peer both[2] = { A, B };

		check("admin selects combat1 + 2 drones",
		      frame_lobby_select_config(A.fr, XTK_CONTENT_ID_COMBAT1,
						XTK_CONTENT_ID_VANGUARD, 2) == 0);
		for (i = 0; i < 40 &&
		     session_client_config_revision(B.sc) < 1; i++)
			step(both, 2);
		check("revision 1 shared by both clients",
		      session_client_config_revision(A.sc) == 1 &&
		      session_client_config_revision(B.sc) == 1);
		check("config (maze/vehicle/drones) visible to B",
		      session_client_maze_id(B.sc) == XTK_CONTENT_ID_COMBAT1 &&
		      session_client_vehicle_id(B.sc) ==
		      XTK_CONTENT_ID_VANGUARD &&
		      session_client_drone_count(B.sc) == 2);
		check("readiness is clear after the config change",
		      frame_lobby(A.fr)->ready == 0 &&
		      frame_lobby(B.fr)->ready == 0);

		/* The second maze is selectable too. */
		check("admin selects Arena + 1 drone",
		      frame_lobby_select_config(A.fr, XTK_CONTENT_ID_ARENA,
						XTK_CONTENT_ID_VANGUARD, 1) == 0);
		for (i = 0; i < 40 &&
		     session_client_config_revision(B.sc) < 2; i++)
			step(both, 2);
		check("revision 2 after the second selection",
		      session_client_config_revision(A.sc) == 2 &&
		      session_client_config_revision(B.sc) == 2);
		check("Arena visible to B",
		      session_client_maze_id(B.sc) == XTK_CONTENT_ID_ARENA &&
		      session_client_drone_count(B.sc) == 1);

		/* The zero-drone configuration is selectable too (D3-B 0..2). */
		check("admin selects combat1 + 0 drones",
		      frame_lobby_select_config(A.fr, XTK_CONTENT_ID_COMBAT1,
						XTK_CONTENT_ID_VANGUARD, 0) == 0);
		for (i = 0; i < 40 &&
		     session_client_config_revision(B.sc) < 3; i++)
			step(both, 2);
		check("revision 3 after the zero-drone selection",
		      session_client_config_revision(A.sc) == 3 &&
		      session_client_config_revision(B.sc) == 3);
		check("zero drones visible to B",
		      session_client_drone_count(B.sc) == 0);

		/* ---- explicit readiness and start ---- */
		check("A readies", frame_ready(A.fr, 1) == 0);
		check("B readies", frame_ready(B.fr, 1) == 0);
		for (i = 0; i < 40 &&
		     !(frame_lobby(A.fr)->roster_count == 2 &&
		       frame_lobby(A.fr)->roster[0].ready == 1 &&
		       frame_lobby(A.fr)->roster[1].ready == 1); i++)
			step(both, 2);
		check("both roster entries show ready",
		      frame_lobby(A.fr)->roster_count == 2 &&
		      frame_lobby(A.fr)->roster[0].ready == 1 &&
		      frame_lobby(A.fr)->roster[1].ready == 1);
		check("admin start accepted",
		      frame_request_start(A.fr) == 0);
		pump_until(both, 2, state_is_running, 400);
		check("both clients running epoch 1",
		      session_client_state(A.sc) == XTK_SC_RUNNING &&
		      session_client_state(B.sc) == XTK_SC_RUNNING &&
		      session_client_epoch(A.sc) == 1 &&
		      session_client_epoch(B.sc) == 1);

		/* Non-admin reset is refused by the server while running. */
		session_client_clear_error(B.sc);
		(void) session_client_send_reset(B.sc);
		for (i = 0; i < 20 && session_client_last_error(B.sc) !=
		     XTK_ERR_NOT_ADMIN; i++)
			step(both, 2);
		check("server refused non-admin reset (NOT_ADMIN)",
		      session_client_last_error(B.sc) == XTK_ERR_NOT_ADMIN);
		check("non-admin reset did not stop the match",
		      session_client_state(A.sc) == XTK_SC_RUNNING);
	}

	/* ---- results -> lobby barrier, admin transfer, rejoin ---- */
	combatant[0].score = settings.si.winning_score;
	{
		Peer both[2] = { A, B };
		pump_until(both, 2, has_results, 4000);
		check("both clients received RESULTS",
		      frame_results(A.fr)->have_results == 1 &&
		      frame_results(B.fr)->have_results == 1);
		check("winner is the first human",
		      frame_results(A.fr)->results.result_kind == 0 &&
		      frame_results(A.fr)->results.winner_slot == 0);

		check("admin reset from results accepted",
		      frame_request_reset(A.fr) == 0);
		pump_until(both, 2, state_is_lobby, 400);
		check("results -> lobby without restart",
		      session_client_state(A.sc) == XTK_SC_LOBBY &&
		      session_client_state(B.sc) == XTK_SC_LOBBY);
		check("results cleared on the lobby barrier",
		      frame_results(A.fr)->have_results == 0 &&
		      frame_results(B.fr)->have_results == 0);
	}

	/* A departs; B inherits the administrator role. */
	(void) session_client_send_quit(A.sc);
	{
		Peer just_b[1] = { B };
		for (i = 0; i < 400 && server_session_count() > 1; i++)
			step(just_b, 1);
		check("departed admin removed",
		      server_session_count() == 1);
		check("admin transferred to B",
		      session_client_is_admin(B.sc) == 1 &&
		      session_client_admin_index(B.sc) ==
		      session_client_self_index(B.sc));
		check("B roster now has one player",
		      session_client_roster_count(B.sc) == 1);
	}

	/* A rejoins a fresh session in-process (no restart). */
	{
		Peer both[2] = { A, B };
		frame_reconnect(A.fr);
		check("A reconnected in-process",
		      session_client_connect(A.sc, "127.0.0.1", port,
					     "alpha") == 0);
		pump_until(both, 2, state_is_lobby, 400);
		check("A rejoined the shared lobby",
		      session_client_state(A.sc) == XTK_SC_LOBBY &&
		      session_client_roster_count(A.sc) == 2);
		check("B remains administrator after the rejoin",
		      session_client_is_admin(B.sc) == 1 &&
		      session_client_is_admin(A.sc) == 0 &&
		      session_client_admin_index(A.sc) ==
		      session_client_self_index(B.sc));
	}

	frame_destroy(A.fr);
	frame_destroy(B.fr);
	session_client_destroy(A.sc);
	session_client_destroy(B.sc);
	transport_destroy(host);
	return 0;
}

/* ---- inbound validation (foreign/stale cannot regress) ---------------- */

static void
env_set(XtkMessage *m, uint8_t type, uint32_t session_id, uint32_t epoch)
{
	memset(m, 0, sizeof(*m));
	m->env.message_type = type;
	m->env.channel = xtk_next_channel(type);
	m->env.session_id = session_id;
	m->env.epoch = epoch;
}

static int
run_inbound(void)
{
	XtkSessionClient *c;
	XtkMessage m;

	c = session_client_create();
	check("inbound: client created", c != NULL);
	if (c == NULL)
		return 1;

	/* WELCOME player role must not grant admin. */
	env_set(&m, XTK_MSG_WELCOME, 7, 0);
	m.body.welcome.role = 1;
	m.body.welcome.human_capacity = XTK_MAX_HUMANS;
	check("inbound: WELCOME accepted",
	      session_client_ingest(c, 1, XTK_CH_CONTROL, &m) == 1);
	check("inbound: WELCOME role does not grant admin",
	      session_client_is_admin(c) == 0);

	/* LOBBY_STATE grants admin only when admin_index == self_index. */
	env_set(&m, XTK_MSG_LOBBY_STATE, 7, 0);
	m.body.lobby_state.self_index = 0;
	m.body.lobby_state.admin_index = 0;
	m.body.lobby_state.human_count = 1;
	check("inbound: LOBBY_STATE accepted",
	      session_client_ingest(c, 1, XTK_CH_CONTROL, &m) == 1);
	check("inbound: server admin_index grants admin",
	      session_client_is_admin(c) == 1);

	/* A foreign peer cannot change state. */
	env_set(&m, XTK_MSG_LOBBY_STATE, 7, 0);
	m.body.lobby_state.self_index = 1;
	m.body.lobby_state.admin_index = 1;
	m.body.lobby_state.human_count = 1;
	check("inbound: foreign peer rejected",
	      session_client_ingest(c, 0, XTK_CH_CONTROL, &m) == 0);
	check("inbound: admin unchanged after foreign peer",
	      session_client_is_admin(c) == 1 &&
	      session_client_admin_index(c) == 0);

	/* Wrong channel rejected. */
	env_set(&m, XTK_MSG_LOBBY_STATE, 7, 0);
	check("inbound: wrong channel rejected",
	      session_client_ingest(c, 1, XTK_CH_SNAPSHOT, &m) == 0);

	/* Foreign session id rejected. */
	env_set(&m, XTK_MSG_LOBBY_STATE, 999, 0);
	m.body.lobby_state.self_index = 1;
	m.body.lobby_state.admin_index = 1;
	check("inbound: foreign session rejected",
	      session_client_ingest(c, 1, XTK_CH_CONTROL, &m) == 0);
	check("inbound: admin unchanged after foreign session",
	      session_client_admin_index(c) == 0);

	/* Enter a live epoch, then a stale-epoch RESULTS must not regress. */
	env_set(&m, XTK_MSG_EPOCH_BEGIN, 7, 5);
	m.body.epoch_begin.epoch = 5;
	m.body.epoch_begin.first_tick = 1;
	check("inbound: EPOCH_BEGIN accepted",
	      session_client_ingest(c, 1, XTK_CH_CONTROL, &m) == 1);
	check("inbound: running at epoch 5",
	      session_client_state(c) == XTK_SC_RUNNING &&
	      session_client_epoch(c) == 5);

	env_set(&m, XTK_MSG_RESULTS, 7, 4);
	m.body.results.result_kind = 0;
	check("inbound: stale-epoch RESULTS rejected",
	      session_client_ingest(c, 1, XTK_CH_CONTROL, &m) == 0);
	check("inbound: state not regressed by stale RESULTS",
	      session_client_state(c) == XTK_SC_RUNNING);

	env_set(&m, XTK_MSG_RESULTS, 7, 5);
	m.body.results.result_kind = 0;
	check("inbound: current-epoch RESULTS accepted",
	      session_client_ingest(c, 1, XTK_CH_CONTROL, &m) == 1);
	check("inbound: results state applied",
	      session_client_state(c) == XTK_SC_RESULTS);

	session_client_destroy(c);
	return 0;
}

int
main(int argc, char *argv[])
{
	g_assets = (argc > 1 && argv[1][0] != '-') ? argv[1] : ".";
	if (server_bringup() != 0)
		return 2;

	g_ns = 1000000000ULL;
	g_ms = 1000ULL;

	fprintf(stderr, "== two-human lobby / authority / rejoin\n");
	(void) run_two_human();

	fprintf(stderr, "== inbound validation\n");
	(void) run_inbound();

	if (failures != 0) {
		printf("CLIENT-SESSION FIXTURE FAIL (%d checks, %d failures)\n",
		       checks, failures);
		return 1;
	}
	printf("CLIENT-SESSION FIXTURE PASS (%d checks: shared roster/config/"
	       "drone/readiness visible; WELCOME role not admin; admin-gated "
	       "config/start/reset with independent server rejection; both "
	       "mazes; revision readiness rule; results->lobby; admin transfer; "
	       "in-process rejoin; foreign/stale inbound rejected)\n", checks);
	return 0;
}
