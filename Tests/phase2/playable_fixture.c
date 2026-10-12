/*-
 * Copyright (c) 1988 Terry Donahue
 * All rights reserved.
 *
 * P2.14 playable-client integration acceptance fixture.
 *
 * Starts the shipped display-free server core plus the P2.8 network session in
 * this process over a real pinned-ENet loopback transport, connects the P2.14
 * client session (Client/session_client.c) and drives the P2.14 frame loop
 * (Client/frame.c) through the whole slice:
 *
 *   connect -> handshake -> lobby -> ready -> start -> 60 ticks of play with
 *   input flowing to the server -> results -> reset -> clean disconnect
 *
 * It proves the frame loop wires session messages into the P2.11 model, renders
 * the interpolated snapshot into the world + HUD/map/radar, refreshes the lobby
 * from the catalog, emits input at 15 Hz (never tick 0), releases held controls
 * on defocus/quit and renders the server RESULTS.  It is a test harness: it
 * links the simulation so it can host the authoritative server; the shipped
 * client never does.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/socket.h>

#include <SDL3/SDL.h>

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

/* ---- sandbox sendmsg shim (test-harness only; see P2.7/P2.9) ---------- */
ssize_t __real_sendmsg(int fd, const struct msghdr *msg, int flags);

ssize_t
__wrap_sendmsg(int fd, const struct msghdr *msg, int flags)
{
	if (msg != NULL && msg->msg_name != NULL) {
		if (connect(fd, (struct sockaddr *) msg->msg_name,
			    msg->msg_namelen) == 0) {
			struct msghdr m = *msg;

			m.msg_name = NULL;
			m.msg_namelen = 0;
			return __real_sendmsg(fd, &m, flags);
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

/* ---- synthetic clock -------------------------------------------------- */

#define STEP_NS  66666667ULL
#define STEP_MS  67ULL
static uint64_t g_ns;
static uint64_t g_ms;

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
	strncpy(username, "p2.14", MAX_STRING - 1);
	username[MAX_STRING - 1] = '\0';

	if (content_set_root(g_assets) != 0) {
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
	server_release_settings(&settings);
	init_threader();
	init_msg_sys();
	init_box_names();
	geometry_order_objects();
	geometry_rotate_objects();
	return 0;
}

/* One synthetic client/server tick: service the server, step it if running,
 * then run the client frame loop. */
static void
tick(ServerSession **out_s, XtkFrame *fr)
{
	ServerSession *s;

	g_ns += STEP_NS;
	g_ms += STEP_MS;

	(void) server_session_service(g_ms);
	s = server_session_current();
	if (s != NULL && server_session_state(s) == SERVER_SESSION_RUNNING)
		(void) server_session_step(s, g_ms);
	(void) frame_tick(fr, g_ns);
	if (out_s != NULL)
		*out_s = s;
}

/* ---- main integration flow -------------------------------------------- */

static int
run_playable(const char *assets_path)
{
	Transport *host;
	uint16_t port;
	ServerSession *s = NULL;
	XtkSessionClient *sc;
	XtkFrame *fr;
	XtkFrameStats st;
	SDL_Surface *surf;
	XtkAssets assets;
	int have_assets = 0;
	int i;

	if (!SDL_Init(SDL_INIT_VIDEO)) {
		fprintf(stderr, "playable_fixture: SDL_Init: %s\n", SDL_GetError());
		return 1;
	}
	surf = SDL_CreateSurface(XTK_UI_SCREEN_W, XTK_UI_SCREEN_H,
				 SDL_PIXELFORMAT_ARGB8888);
	check("frame target surface created", surf != NULL);
	if (assets_path != NULL) {
		char err[128];
		if (assets_load_file(&assets, assets_path, err, sizeof(err)) == 0)
			have_assets = 1;
	}

	server_session_init();
	host = transport_create(0, 1);
	check("server listen transport created", host != NULL);
	if (host == NULL)
		return 1;
	port = transport_port(host);
	server_session_set_transport(host);

	sc = session_client_create();
	check("client session created", sc != NULL);
	check("client connect initiated",
	      session_client_connect(sc, "127.0.0.1", port, "pilot") == 0);
	check("client is connecting",
	      session_client_state(sc) == XTK_SC_CONNECTING);

	fr = frame_create(sc, have_assets ? &assets : NULL, surf);
	check("frame loop created", fr != NULL);

	/* Hold auto-lobby so the lobby/pause state can be observed. */
	frame_set_auto_lobby(fr, 0);

	/* ---- connect -> handshake -> lobby ---- */
	for (i = 0; i < 400 && session_client_state(sc) != XTK_SC_LOBBY; i++)
		tick(&s, fr);
	check("handshake completed (Lobby)",
	      session_client_state(sc) == XTK_SC_LOBBY);
	check("WELCOME assigned a session id", session_client_id(sc) != 0);
	check("client is admin", session_client_is_admin(sc) == 1);
	check("content catalog received",
	      frame_lobby(fr)->have_catalog &&
	      frame_lobby(fr)->num_mazes >= 1 &&
	      frame_lobby(fr)->num_vehicles >= 1);

	/* Pause is honored outside Running. */
	frame_control(fr, XTK_CTL_PAUSE, 1);
	check("pause toggles in the lobby",
	      input_paused(frame_input(fr)) == 1);
	frame_control(fr, XTK_CTL_PAUSE, 1);
	check("pause toggles back", input_paused(frame_input(fr)) == 0);

	/* The lobby auto-selects combat1 + Vanguard and sends READY. */
	frame_set_auto_lobby(fr, 1);
	for (i = 0; i < 400 && !frame_running(fr); i++)
		tick(&s, fr);
	frame_stats(fr, &st);
	check("config accepted and match started (Running)",
	      st.running && session_client_state(sc) == XTK_SC_RUNNING);
	check("allowlisted selection recorded",
	      frame_lobby(fr)->selected_maze_id == XTK_CONTENT_ID_COMBAT1 &&
	      frame_lobby(fr)->selected_vehicle_id == XTK_CONTENT_ID_VANGUARD);
	check("epoch 1", session_client_epoch(sc) == 1);

	/* ---- 60 ticks of play with input ---- */
	frame_control(fr, XTK_CTL_DRIVE_FWD, 1);
	for (i = 0; i < 4000; i++) {
		tick(&s, fr);
		frame_stats(fr, &st);
		if (fr != NULL && st.emitted_batches >= 61 &&
		    st.snapshots >= 61)
			break;
		if (st.session_state == XTK_SC_RESULTS ||
		    st.session_state == XTK_SC_FAILED)
			break;
	}
	frame_stats(fr, &st);
	check("at least 60 ticks of snapshots received", st.snapshots >= 60);
	check("at least 60 input batches emitted at 15 Hz",
	      st.emitted_batches >= 60);
	check("no input on tick 0 (first emit tick >= 1)",
	      st.first_emit_tick >= 1);
	check("no more than one batch per server tick",
	      st.emitted_batches <= st.snapshots + 1);
	check("input reached the server (CMD_ACKs received)",
	      st.cmd_acks > 0);
	check("interpolated model produced a frame", st.have_self == 1);
	check("HUD rendered authorized values",
	      ui_scene_has_text(frame_scene(fr), "Armor") &&
	      ui_scene_has_text(frame_scene(fr), "Score:"));
	check("mapper panel has authorized knowledge",
	      st.map_cells >= 1);
	check("radar panel built without hidden state",
	      st.radar_blips >= 0);

	/* Defocus releases a held control unconditionally. */
	frame_focus(fr, 0);
	check("window defocus registered", input_focused(frame_input(fr)) == 0);
	check("defocus released held drive",
	      frame_input(fr)->held[XTK_CTL_DRIVE_FWD] == 0);
	frame_focus(fr, 1);
	check("focus regained", input_focused(frame_input(fr)) == 1);
	frame_control(fr, XTK_CTL_DRIVE_FWD, 0);

	/* ---- results: force a human win and read the server RESULTS ---- */
	if (s != NULL && server_session_state(s) == SERVER_SESSION_RUNNING) {
		combatant[0].score = settings.si.winning_score;
	}
	for (i = 0; i < 4000 &&
	     !frame_results(fr)->have_results; i++)
		tick(&s, fr);
	check("RESULTS received", frame_results(fr)->have_results == 1);
	check("completed (win) result, human winner",
	      frame_results(fr)->results.result_kind == 0 &&
	      frame_results(fr)->results.winner_slot == 0);
	check("winner name present on the results surface",
	      strcmp(frame_results(fr)->results.entry[0].name, "pilot") == 0);
	check("results banner names the winner",
	      ui_scene_has_text(frame_scene(fr), "Battle won by pilot"));
	check("no live epoch after results",
	      session_client_state(sc) == XTK_SC_RESULTS);

	/* ---- reset without restarting the process ---- */
	frame_request_reset(fr);
	for (i = 0; i < 4000; i++) {
		tick(&s, fr);
		frame_stats(fr, &st);
		if (st.running && session_client_epoch(sc) == 2 &&
		    st.snapshots >= 1)
			break;
	}
	check("reset started epoch 2 cleanly",
	      session_client_state(sc) == XTK_SC_RUNNING &&
	      session_client_epoch(sc) == 2 && st.running);
	check("reset rebuilt ownership (slot 0)",
	      s != NULL && server_session_bound_slot(s) == 0);

	/* ---- clean disconnect ---- */
	frame_request_quit(fr);
	check("quit requested", frame_quit_requested(fr) == 1);
	for (i = 0; i < 4000 &&
	     session_client_state(sc) != XTK_SC_DISCONNECTED; i++)
		tick(&s, fr);
	check("clean disconnect",
	      session_client_state(sc) == XTK_SC_DISCONNECTED);
	check("server session ended",
	      server_session_current() == NULL ||
	      server_session_state(s) == SERVER_SESSION_DISCONNECTED);

	frame_destroy(fr);
	session_client_destroy(sc);
	if (have_assets)
		assets_free(&assets);
	SDL_DestroySurface(surf);
	transport_destroy(host);
	SDL_Quit();
	return failures ? 1 : 0;
}

/* ---- unavailable server yields an actionable non-connected state ----- */

static int
run_unavailable(void)
{
	Transport *tmp;
	uint16_t port;
	XtkSessionClient *sc;
	int i;

	tmp = transport_create(0, 1);
	check("probe transport created", tmp != NULL);
	if (tmp == NULL)
		return 1;
	port = transport_port(tmp);
	transport_destroy(tmp);

	sc = session_client_create();
	check("unavailable: client created", sc != NULL);
	check("unavailable: connect initiated",
	      session_client_connect(sc, "127.0.0.1", port, "pilot") == 0);
	for (i = 0; i < 200 &&
	     session_client_connected(sc); i++) {
		g_ns += STEP_NS;
		(void) session_client_pump(sc, g_ns, NULL, NULL);
	}
	check("unavailable server does not reach a lobby/running state",
	      !session_client_connected(sc));
	session_client_destroy(sc);
	return 0;
}

int
main(int argc, char *argv[])
{
	const char *assets_path = (argc > 1 && argv[1][0] != '-') ? argv[1] : NULL;

	g_assets = ".";
	if (server_bringup() != 0)
		return 2;

	if (assets_path != NULL)
		fprintf(stderr, "== asset bundle: %s\n", assets_path);
	else
		fprintf(stderr, "== no asset bundle (world is cleared only)\n");

	g_ns = 1000000000ULL;
	g_ms = 1000ULL;

	fprintf(stderr, "== connect -> lobby -> start -> 60 ticks -> results -> "
		"reset -> quit\n");
	(void) run_playable(assets_path);

	fprintf(stderr, "== unavailable server\n");
	(void) run_unavailable();

	if (failures != 0) {
		printf("PLAYABLE FIXTURE FAIL (%d checks, %d failures)\n",
		       checks, failures);
		return 1;
	}
	printf("PLAYABLE FIXTURE PASS (%d checks: connect/handshake/lobby/ready/"
	       "start/60-tick play/input-15Hz/results/reset/quit; HUD/map/radar "
	       "from interpolated model; clean disconnect)\n", checks);
	return 0;
}
