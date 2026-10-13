/*-
 * Copyright (c) 1988 Terry Donahue
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met: [see the project COPYRIGHT / legacy sources]
 *
 * players_fixture.c -- P3.5 two-human / four-vehicle binding and lifecycle.
 *
 * Links the shipped display-free server core (Src/sobj minus server_main.o),
 * the P3.4 coordinator (Src/server_session.c), the P2.7 admission layer
 * (Src/server_net.c), the pinned ENet transport and the frozen v1.1 codec, and
 * drives the shipping coordinator through every allowed combatant
 * configuration, asserting:
 *
 *   1. solo + one drone and two humans + 0/1/2 drones initialize on both
 *      mazes; slots are unique; every combatant is a Vanguard with the
 *      unchanged release defaults; human combatants carry the X-human flags
 *      (num_players == 1, Vehicle.teleport == TRUE) and drones the bot default;
 *   2. both humans die/respawn through the ordinary rules, each binding owns
 *      only its own tank, and ownership survives respawn but not a stale or
 *      boundary controller id;
 *   3. the configuration resets across at least three epochs with fresh
 *      bindings and no stale ownership;
 *   4. duplicate lobby names are rejected with an actionable error while the
 *      connection identity stays independent of the name;
 *   5. no Terminal is allocated and no render/input call is made (link-time
 *      --wrap counters, all required to be zero);
 *   6. a bot-free two-human trace driven through the shipping coordinator
 *      reproduces the independent frozen-X ordinary reference bytes
 *      (Tests/phase3/fixtures/players/two-human-ordinary.sha256).
 *
 * It never builds a Terminal and never labels a human a bot.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "xtank.h"
#include "bullet.h"
#include "clfkr.h"
#include "object.h"
#include "thread.h"
#include "vehicle.h"
#include "team.h"
#include "globals.h"
#include "server.h"
#include "server_events.h"
#include "server_session.h"
#include "intent.h"
#include "proto.h"
#include "gr.h"

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
extern TeamData teamdata[];
extern int num_teams;

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

static uint64_t
now_ms(void)
{
	struct timespec ts;

	(void) clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t) ts.tv_sec * 1000ULL + (uint64_t) ts.tv_nsec / 1000000ULL;
}

/* ---- message helpers -------------------------------------------------- */

static void
make_hello(XtkMessage *m, const char *name)
{
	(void) memset(m, 0, sizeof(*m));
	m->env.message_type = XTK_MSG_HELLO;
	m->body.hello.client_nonce = 0x51a75a75u;
	m->body.hello.proto_major = XTK_PROTOCOL_MAJOR;
	m->body.hello.proto_minor = XTK_PROTOCOL_MINOR;
	m->body.hello.client_build = 1;
	(void) strncpy(m->body.hello.name, name, XTK_MAX_NAME_BYTES);
	(void) memcpy(m->body.hello.content_set_digest,
		      xtk_content_set_digest, XTK_SHA256_BYTES);
}

static void
make_config(XtkMessage *m, uint16_t maze_id, uint8_t drones)
{
	(void) memset(m, 0, sizeof(*m));
	m->env.message_type = XTK_MSG_CONFIG_SELECT;
	m->body.config_select.maze_id = maze_id;
	m->body.config_select.vehicle_id = XTK_CONTENT_ID_VANGUARD;
	m->body.config_select.drone_count = drones;
}

static void
make_ready(XtkMessage *m, uint8_t ready, uint32_t rev)
{
	(void) memset(m, 0, sizeof(*m));
	m->env.message_type = XTK_MSG_READY;
	m->body.ready.ready = ready;
	m->body.ready.config_revision = rev;
}

static void
make_start_request(XtkMessage *m, uint32_t rev)
{
	(void) memset(m, 0, sizeof(*m));
	m->env.message_type = XTK_MSG_START_REQUEST;
	m->body.start_request.expected_config_revision = rev;
}

static void
make_reset(XtkMessage *m, uint32_t prior_epoch)
{
	(void) memset(m, 0, sizeof(*m));
	m->env.message_type = XTK_MSG_RESET;
	m->body.reset.prior_epoch = prior_epoch;
}

static int
find_reply(const ServerSession *s, int type, XtkMessage *out)
{
	int i;

	for (i = 0; i < server_session_reply_count(s); i++) {
		if (server_session_reply(s, i, out) == 0 &&
		    out->env.message_type == (uint8_t) type)
			return 1;
	}
	return 0;
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
	(void) strncpy(username, "p3.5", MAX_STRING - 1);
	username[MAX_STRING - 1] = '\0';

	if (content_set_root(g_assets) != 0) {
		fprintf(stderr, "players_fixture: %s\n", content_last_error());
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

	if (make_vdesc((char *) "Vanguard", &(int){0}) != DESC_LOADED) {
		fprintf(stderr, "players_fixture: cannot load Vanguard\n");
		return -1;
	}
	if (make_mdesc((char *) "combat1", &(int){0}) != DESC_LOADED) {
		fprintf(stderr, "players_fixture: cannot load combat1\n");
		return -1;
	}
	if (make_mdesc((char *) "Arena", &(int){0}) != DESC_LOADED) {
		fprintf(stderr, "players_fixture: cannot load Arena\n");
		return -1;
	}
	return 0;
}

/* Admit one peer through a valid HELLO; returns the session (or NULL). */
static ServerSession *
admit(const char *name, uint64_t t)
{
	ServerSession *s = server_session_accept((TransportPeer *) NULL);
	XtkMessage h;

	if (s == (ServerSession *) NULL)
		return (ServerSession *) NULL;
	make_hello(&h, name);
	if (server_session_message(s, &h, t) != XTK_ERR_NONE)
		return (ServerSession *) NULL;
	return s;
}

/* One admin config + both humans ready + explicit start. */
static int
start_match(ServerSession *admin, ServerSession *other, uint16_t maze_id,
	    uint8_t drones, uint64_t t)
{
	XtkMessage m;
	uint32_t rev;

	make_config(&m, maze_id, drones);
	if (server_session_message(admin, &m, t) != XTK_ERR_NONE)
		return -1;
	rev = server_session_config_revision(admin);
	make_ready(&m, 1, rev);
	if (server_session_message(admin, &m, t) != XTK_ERR_NONE)
		return -1;
	if (other != (ServerSession *) NULL &&
	    server_session_message(other, &m, t) != XTK_ERR_NONE)
		return -1;
	make_start_request(&m, rev);
	return server_session_message(admin, &m, t);
}

static void
teardown_session(void)
{
	int i;

	for (i = 0; i < SERVER_SESSION_MAX_PEERS; i++) {
		ServerSession *p = server_session_peer_at(i);

		if (p != (ServerSession *) NULL)
			server_session_destroy(p);
	}
	server_session_init();
}

/* ---- configuration construction --------------------------------------- */

/*
 * Every allowed release configuration must build exactly the expected
 * combatants on both mazes, with unique slots, Vanguards, unchanged human flags
 * and release defaults.
 */
static void
section_configs(void)
{
	static const uint16_t mazes[2] = {
		XTK_CONTENT_ID_COMBAT1, XTK_CONTENT_ID_ARENA
	};
	static const char *maze_name[2] = { "combat1", "Arena" };
	/* humans, requested drones, effective combatants */
	static const int cfg[4][3] = {
		{ 1, 0, 2 },	/* solo human: effective one drone */
		{ 2, 0, 2 },
		{ 2, 1, 3 },
		{ 2, 2, 4 }
	};
	int mi, ci;

	fprintf(stderr, "== allowed configurations on each maze\n");
	for (mi = 0; mi < 2; mi++) {
		for (ci = 0; ci < 4; ci++) {
			ServerSession *A, *B = (ServerSession *) NULL;
			int humans = cfg[ci][0], drones = cfg[ci][1];
			int expect = cfg[ci][2];
			uint64_t t = now_ms();
			int i, ok, vdesc0;
			char label[96];

			snprintf(label, sizeof(label), "%s: %d human(s)+%d drone(s)",
				 maze_name[mi], humans, drones);
			teardown_session();
			server_session_set_capacity(XTK_MAX_HUMANS);
			A = admit("p1", t);
			if (humans == 2)
				B = admit("p2", t);
			ok = (A != NULL) && (humans == 1 || B != NULL) &&
			     start_match(A, B, mazes[mi], (uint8_t) drones, t) ==
			     XTK_ERR_NONE;
			check(label, ok);
			if (!ok) {
				teardown_session();
				continue;
			}
			check("configuration reaches Running",
			      server_session_state(A) == SERVER_SESSION_RUNNING);
			check("combatant count matches the configuration",
			      num_combatants == expect);
			check("unique human slots 0..h",
			      server_session_bound_slot(A) == 0 &&
			      (humans != 2 ||
			       server_session_bound_slot(B) == 1));
			check("all expected vehicles are alive",
			      num_veh_alive == expect);

			vdesc0 = combatant[0].vdesc;
			ok = 1;
			for (i = 0; i < expect; i++) {
				if (combatant[i].vdesc != vdesc0)
					ok = 0;
				if (combatant[i].team >= MAX_TEAMS)
					ok = 0;
				if (combatant[i].number != i)
					ok = 0;
			}
			check("every combatant is the same Vanguard", ok);
			check("Vanguard is the only release vehicle",
			      !strcmp(vdesc[vdesc0].name, "Vanguard"));

			ok = 1;
			for (i = 0; i < humans; i++) {
				Vehicle *v = combatant[i].vehicle;

				if (combatant[i].num_players != 1 ||
				    combatant[i].num_programs != 0)
					ok = 0;
				if (v == (Vehicle *) NULL ||
				    !tstflag(v->status, VS_is_alive) ||
				    v->teleport != TRUE)
					ok = 0;
			}
			check("humans keep num_players 1 and teleport TRUE", ok);

			ok = 1;
			for (i = humans; i < expect; i++) {
				Vehicle *v = combatant[i].vehicle;

				if (combatant[i].num_players != 0 ||
				    combatant[i].num_programs != 1 ||
				    combatant[i].program[0] != 0)
					ok = 0;
				if (v == (Vehicle *) NULL ||
				    !tstflag(v->status, VS_is_alive) ||
				    v->teleport != FALSE)
					ok = 0;
			}
			check("drones keep the bot default (teleport FALSE)", ok);

			check("release defaults unchanged",
			      settings.si.game == COMBAT_GAME &&
			      settings.si.team_score == FALSE &&
			      settings.si.restart == TRUE &&
			      settings.robots_dont_win == FALSE &&
			      settings.si.winning_score == 1000 &&
			      settings.game_speed == 15);

			teardown_session();
		}
	}
}

/* ---- human identity, death/respawn and own-tank binding ---------------- */

static void
section_lifecycle(void)
{
	ServerSession *A, *B;
	uint64_t t;
	Vehicle *va, *vb;
	uint32_t ca, cb, ga, gb;
	int i, died = 0, respawned = 0;

	fprintf(stderr, "== two-human death/respawn and own-tank binding\n");
	teardown_session();
	t = now_ms();
	server_session_set_capacity(XTK_MAX_HUMANS);
	A = admit("humanA", t);
	B = admit("humanB", t);
	check("two humans admitted", A != NULL && B != NULL);
	if (A == (ServerSession *) NULL || B == (ServerSession *) NULL) {
		teardown_session();
		return;
	}
	check("start accepted",
	      start_match(A, B, XTK_CONTENT_ID_COMBAT1, 1, t) == XTK_ERR_NONE);

	check("slots are 0 and 1",
	      server_session_bound_slot(A) == 0 &&
	      server_session_bound_slot(B) == 1);
	check("connection identity independent of name",
	      server_session_id(A) != server_session_id(B) &&
	      server_session_controller_id(A) != server_session_controller_id(B));

	va = combatant[0].vehicle;
	vb = combatant[1].vehicle;
	ca = server_session_controller_id(A);
	cb = server_session_controller_id(B);
	ga = server_players_binding(0)->generation;
	gb = server_players_binding(1)->generation;

	check("each controller binds only its own tank",
	      server_players_validate(ca, 0, ga) == 1 &&
	      server_players_validate(ca, 1, ga) == 0 &&
	      server_players_validate(cb, 1, gb) == 1 &&
	      server_players_validate(cb, 0, gb) == 0);
	check("wrong generation / unknown controller rejected",
	      server_players_validate(ca, 0, ga + 1) == 0 &&
	      server_players_validate(ca + cb + 1, 0, ga) == 0);
	check("human A owns combatant 0 only",
	      va != NULL && va->owner == &combatant[0] &&
	      vb != NULL && vb->owner == &combatant[1]);

	/* Ordinary death (NULL killer) then ordinary respawn. */
	kill_vehicle(va, (Vehicle *) NULL);
	for (i = 0; i < 100000; i++) {
		int st = server_session_step(A, t);

		if (!tstflag(va->status, VS_is_alive))
			died = 1;
		if (died && tstflag(va->status, VS_is_alive)) {
			respawned = 1;
			break;
		}
		if (st != GAME_RUNNING)
			break;
	}
	check("human A dies through the ordinary rules", died);
	check("human A respawns through the ordinary rules", respawned);
	check("human B was never removed", tstflag(vb->status, VS_is_alive));
	check("ownership survives respawn (same controller/generation/slot)",
	      server_players_validate(ca, 0, ga) == 1 &&
	      server_players_validate(cb, 1, gb) == 1);
	check("respawned tank is still combatant 0's vehicle",
	      combatant[0].vehicle->owner == &combatant[0]);
	check("party tank binds only itself",
	      server_players_validate(ca, 1, ga) == 0 &&
	      server_players_validate(cb, 0, gb) == 0);

	teardown_session();
}

/* ---- reset across at least three epochs ------------------------------- */

static void
section_epochs(void)
{
	ServerSession *A, *B;
	uint64_t t = now_ms();
	int e;

	fprintf(stderr, "== three-epoch reset with fresh bindings\n");
	teardown_session();
	server_session_set_capacity(XTK_MAX_HUMANS);
	A = admit("epochA", t);
	B = admit("epochB", t);
	check("two humans admitted", A != NULL && B != NULL);
	if (A == (ServerSession *) NULL || B == (ServerSession *) NULL) {
		teardown_session();
		return;
	}

	for (e = 1; e <= 3; e++) {
		XtkMessage m;
		uint32_t ca = server_session_controller_id(A);
		uint32_t cb = server_session_controller_id(B);
		unsigned int gen0, gen1;
		char label[64];

		if (e == 1)
			check("epoch 1 start accepted",
			      start_match(A, B, XTK_CONTENT_ID_COMBAT1, 2, t) ==
			      XTK_ERR_NONE);

		snprintf(label, sizeof(label), "epoch %d is Running", e);
		check(label, server_session_state(A) == SERVER_SESSION_RUNNING &&
		      server_session_epoch(A) == (uint32_t) e);
		check("epoch reuses unique slots 0/1 and correct count",
		      server_session_bound_slot(A) == 0 &&
		      server_session_bound_slot(B) == 1 &&
		      num_combatants == 4);
		gen0 = server_players_binding(0)->generation;
		gen1 = server_players_binding(1)->generation;
		check("both bindings are current",
		      server_players_validate(ca, 0, gen0) == 1 &&
		      server_players_validate(cb, 1, gen1) == 1 &&
		      server_players_count() == 2);

		/* Admin reset returns through the lobby barrier. */
		make_reset(&m, (uint32_t) e);
		check("admin reset accepted",
		      server_session_message(A, &m, t) == XTK_ERR_NONE &&
		      server_session_state(A) == SERVER_SESSION_LOBBY);
		check("selection preserved across reset",
		      server_session_config_valid(A) == 1 &&
		      server_session_maze_id(A) == XTK_CONTENT_ID_COMBAT1);

		/* The reset unbinds every controller; the old ownership no longer
		 * validates even with the exact generation it held. */
		check("controller unbound by reset (stale ownership rejected)",
		      server_players_validate(ca, 0, gen0) == 0 &&
		      server_players_validate(cb, 1, gen1) == 0);

		if (e < 3) {
			XtkMessage cfg, rd, sr;
			uint32_t rev;

			make_config(&cfg, XTK_CONTENT_ID_COMBAT1, 2);
			(void) server_session_message(A, &cfg, t);
			rev = server_session_config_revision(A);
			make_ready(&rd, 1, rev);
			(void) server_session_message(A, &rd, t);
			(void) server_session_message(B, &rd, t);
			make_start_request(&sr, rev);
			check("next epoch start accepted",
			      server_session_message(A, &sr, t) ==
			      XTK_ERR_NONE);
		}
	}
	check("three epochs observed", server_session_epoch(A) == 3);
	teardown_session();
}

/* ---- duplicate names and boundary identities -------------------------- */

static void
section_duplicate(void)
{
	ServerSession *A, *dup;
	uint64_t t = now_ms();
	XtkMessage h, m;

	fprintf(stderr, "== duplicate-name lobby rejection\n");
	teardown_session();
	server_session_set_capacity(XTK_MAX_HUMANS);
	A = admit("clash", t);
	check("first name admitted", A != NULL);
	dup = server_session_accept((TransportPeer *) NULL);
	check("second peer accepted into handshake", dup != NULL);
	make_hello(&h, "clash");
	check("duplicate name rejected",
	      server_session_message(dup, &h, t) == XTK_ERR_DUPLICATE_NAME);
	check("duplicate error is actionable (code + text)",
	      find_reply(dup, XTK_MSG_REJECT, &m) == 1 &&
	      m.body.reject.error_code == XTK_ERR_DUPLICATE_NAME &&
	      m.body.reject.text[0] != '\0');
	check("duplicate peer is disconnected",
	      server_session_state(dup) == SERVER_SESSION_DISCONNECTED);
	check("connection identity is independent of the name",
	      server_session_id(dup) != 0 &&
	      server_session_id(dup) != server_session_id(A));
	server_session_destroy(dup);
	check("duplicate slot released", server_session_count() == 1);
	teardown_session();
}

static void
section_boundary(void)
{
	ServerSession *A, *B;
	uint64_t t = now_ms();
	int i0, i1;
	unsigned int g0, g1;

	fprintf(stderr, "== stale controllers and controller-id boundaries\n");

	/* Direct binding-table boundaries, full-width u32 identity. */
	server_players_init();
	i0 = server_players_add("bound0", 0u);
	i1 = server_players_add("boundmax", 0xFFFFFFFFu);
	check("controller 0 binds", i0 >= 0 && server_players_bind_slot(i0, 0) == 0);
	check("controller 0xFFFFFFFF binds",
	      i1 >= 0 && server_players_bind_slot(i1, 1) == 0);
	g0 = server_players_binding(i0)->generation;
	g1 = server_players_binding(i1)->generation;
	check("0xFFFFFFFF is a distinct identity, not -1",
	      server_players_validate(0u, 0, g0) == 1 &&
	      server_players_validate(0xFFFFFFFFu, 1, g1) == 1 &&
	      server_players_validate(0u, 1, g0) == 0 &&
	      server_players_validate(0xFFFFFFFFu, 0, g1) == 0);
	check("unused boundary ids are rejected",
	      server_players_validate(0x80000000u, 0, g0) == 0 &&
	      server_players_validate(0x7FFFFFFFu, 0, g0) == 0 &&
	      server_players_validate(1u, 0, g0) == 0);
	check("duplicate controller id rejected",
	      server_players_add("again", 0u) < 0);

	/* Stale generation after an unbind is rejected. */
	server_players_unbind_slot(0);
	check("unbound controller can no longer drive the slot",
	      server_players_validate(0u, 0, g0) == 0);

	/* The session assigns a full-width controller id per live peer. */
	teardown_session();
	server_session_set_capacity(XTK_MAX_HUMANS);
	A = admit("ctrlA", t);
	B = admit("ctrlB", t);
	check("session assigns distinct u32 controller ids",
	      A != NULL && B != NULL &&
	      server_session_controller_id(A) != 0 &&
	      server_session_controller_id(B) != 0 &&
	      server_session_controller_id(A) !=
	      server_session_controller_id(B));
	check("a peer cannot validate another peer's controller",
	      server_players_validate(server_session_controller_id(A) + 7u,
				      0, 0u) == 0);
	teardown_session();
}

/* ---- frozen two-human ordinary trace ---------------------------------- */

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
state_hash(void)
{
	unsigned long h = 2166136261UL;
	int i;

	hbytes(&h, &frame, sizeof(frame));
	hbytes(&h, &num_veh, sizeof(num_veh));
	hbytes(&h, &num_veh_alive, sizeof(num_veh_alive));
	hbytes(&h, &num_veh_dead, sizeof(num_veh_dead));
	for (i = 0; i < num_veh; i++) {
		Vehicle *v = &actual_vehicles[i];
		Combatant *o = v->owner;

		hbytes(&h, &v->number, sizeof(v->number));
		hbytes(&h, &v->team, sizeof(v->team));
		hbytes(&h, &v->status, sizeof(v->status));
		hbytes(&h, &v->loc->x, sizeof(v->loc->x));
		hbytes(&h, &v->loc->y, sizeof(v->loc->y));
		hbytes(&h, &v->vector.heading, sizeof(v->vector.heading));
		hbytes(&h, &v->fuel, sizeof(v->fuel));
		hbytes(&h, &v->armor, sizeof(v->armor));
		hbytes(&h, &v->num_discs, sizeof(v->num_discs));
		hbytes(&h, &v->death_timer, sizeof(v->death_timer));
		if (o != (Combatant *) NULL) {
			hbytes(&h, &o->score, sizeof(o->score));
			hbytes(&h, &o->kills, sizeof(o->kills));
			hbytes(&h, &o->deaths, sizeof(o->deaths));
			hbytes(&h, o->name, strnlen(o->name, MAX_STRING));
		}
	}
	hbytes(&h, &bset->number, sizeof(bset->number));
	for (i = 0; i < bset->number; i++) {
		Bullet *b = bset->list[i];
		int own = b->owner ? (int) b->owner->number : -1;

		hbytes(&h, &own, sizeof(own));
		hbytes(&h, &b->type, sizeof(b->type));
		hbytes(&h, &b->life, sizeof(b->life));
		hbytes(&h, &b->loc->x, sizeof(b->loc->x));
		hbytes(&h, &b->loc->y, sizeof(b->loc->y));
	}
	hbytes(&h, &eset->number, sizeof(eset->number));
	return h;
}

#define TRACE_TICKS 200

static void
push_intent(int tick, int player, IntentOp op, double a, double b)
{
	Intent in;

	(void) memset(&in, 0, sizeof(in));
	in.tick = tick;
	in.player = player;
	in.sequence = (unsigned int) (tick * 8 + player * 4 + (int) op);
	in.op = op;
	in.a = a;
	in.b = b;
	(void) intent_push(&in);
}

/*
 * The independent frozen-X ordinary reference: two terminal-free humans on
 * combat1/Vanguard, seed 114336, a deterministic bot-free 200-tick script.
 * The shipping coordinator constructs the combatants and drives the single
 * shared step; inputs are pushed directly exactly as the P3.2 reference does.
 */
static unsigned long
frozen_trace(int repeat)
{
	ServerSession *A, *B;
	uint64_t t;
	int tick;

	teardown_session();
	server_session_set_capacity(XTK_MAX_HUMANS);
	server_frame_reset();
	server_seed_random(114336u);

	t = now_ms();
	A = admit("humanA", t);
	B = admit("humanB", t);
	if (A == (ServerSession *) NULL || B == (ServerSession *) NULL) {
		fprintf(stderr, "frozen trace: admission failed\n");
		return 0;
	}
	if (start_match(A, B, XTK_CONTENT_ID_COMBAT1, 0, t) != XTK_ERR_NONE) {
		fprintf(stderr, "frozen trace: start failed\n");
		return 0;
	}
	if (server_session_state(A) != SERVER_SESSION_RUNNING) {
		fprintf(stderr, "frozen trace: not running\n");
		return 0;
	}

	for (tick = 1; tick <= TRACE_TICKS; tick++) {
		push_intent(tick, 0, INTENT_DRIVE_REL,
			    (double) ((tick % 9) - 4), 0.0);
		push_intent(tick, 1, INTENT_HEADING,
			    (double) (tick % 7) - 3.0, 0.0);
		if ((tick % 5) == 0) {
			push_intent(tick, 0, INTENT_FIRE, 0.0, 0.0);
			push_intent(tick, 1, INTENT_FIRE, 0.0, 0.0);
		}
		if (server_session_step(A, t) != GAME_RUNNING) {
			fprintf(stderr, "frozen trace: ended at tick %d\n", tick);
			return 0;
		}
	}
	if (repeat == 0)
		check("bot-free two-human trace runs to 200 ticks",
		      frame == TRACE_TICKS && num_veh_alive == 2);

	return state_hash();
}

static void
section_frozen(void)
{
	unsigned long h0, h1, h2;

	fprintf(stderr, "== frozen two-human ordinary reference\n");
	h0 = frozen_trace(0);
	h1 = frozen_trace(1);
	h2 = frozen_trace(2);
	check("three candidate repeats are byte-identical",
	      h0 == h1 && h1 == h2);
	check("trace has two live human tanks", num_veh_alive == 2);
	printf("PLAYERS hash-ordinary=%016lx ticks=%d\n", h0, TRACE_TICKS);
	teardown_session();
}

/* ----------------------------------------------------------------------- */

int
main(int argc, char *argv[])
{
	g_assets = (argc > 1) ? argv[1] : ".";

	if (server_bringup() != 0)
		return 2;

	section_configs();
	section_lifecycle();
	section_epochs();
	section_duplicate();
	section_boundary();
	section_frozen();

	check("no Terminal was ever allocated", n_make_terminal == 0);
	check("no set_terminal call", n_set_terminal == 0);
	check("no display_terminal render call", n_display_terminal == 0);
	check("no get_events input call", n_get_events == 0);
	check("no sync_terminals call", n_sync_terminals == 0);

	if (failures != 0) {
		printf("PLAYERS FIXTURE FAIL (%d/%d checks)\n", failures, checks);
		return 1;
	}
	printf("PLAYERS FIXTURE PASS (checks=%d; %d configs x 2 mazes, "
	       "two-human death/respawn, 3 epochs, duplicate + boundary "
	       "rejection, zero Terminal/render/input)\n",
	       checks, 4);
	return 0;
}
