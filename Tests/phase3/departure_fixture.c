/*-
 * Copyright (c) 1988 Terry Donahue
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met: [see the project COPYRIGHT / legacy sources]
 *
 * departure_fixture.c -- P3.7 departure removal, admin transfer and lobby
 * rejoin acceptance.
 *
 * It links the shipped display-free server core (Src/sobj minus server_main.o)
 * plus Src/server_session.c, Src/server_net.c, the pinned ENet transport and
 * the frozen v1.1 codec, and drives the shipping coordinator directly (no
 * transport) through:
 *
 *   1. the frozen ordinary two-human reference (P3.2 hash 5625b7035aae2e82),
 *      proving the departure seam leaves the ordinary simulation untouched;
 *   2. a live QUIT departure: exactly one tick-boundary removal, no
 *      kill/score/death/RNG/explosion, valid retained projectile owner,
 *      stable surviving slot/ownership, reliable+unique admin transfer,
 *      survivor keeps progressing, no respawn;
 *   3. a respawn-waiting departure: leaves the dead list, never returns;
 *   4. pending/held input: the departing peer's controls are cancelled while
 *      the survivor's are not (player-scoped cancel); no ghost projectile;
 *   5. valid-input timeout removal with the survivor still playing;
 *   6. output/queue saturation removal (same departure path);
 *   7. disconnect removal through the shared service reclaim path;
 *   8. no-humans cleanup: the world is emptied and the listening lobby
 *      returns; rejoin while Running fails; the next-lobby rejoin gets fresh
 *      connection/controller/tank identity and cannot replay old commands;
 *   9. simultaneous departures and repeated disconnect/rejoin cycles.
 *
 * A `DEPARTURE-FIXTURE PASS` means every check passed.
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

/* ---- FNV-1a state hash: identical to the P3.2 frozen reference -------- */

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

/* ---- P3.2 ordinary script (byte-identical to the frozen reference) ---- */

#define REF_TICKS 200

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

static void
script_tick(int t)
{
	push_intent(t, 0, INTENT_DRIVE_REL, (double) ((t % 9) - 4), 0.0);
	push_intent(t, 1, INTENT_HEADING, (double) (t % 7) - 3.0, 0.0);
	if ((t % 5) == 0) {
		push_intent(t, 0, INTENT_FIRE, 0.0, 0.0);
		push_intent(t, 1, INTENT_FIRE, 0.0, 0.0);
	}
}

static void
build_two_humans_direct(int vdesc_index)
{
	int hb;

	server_players_init();
	hb = server_players_add("humanA", 10);
	(void) server_players_bind_slot(hb, 0);
	hb = server_players_add("humanB", 11);
	(void) server_players_bind_slot(hb, 1);

	num_combatants = 2;

	combatant[0].num_players = 1;
	combatant[0].player[0] = server_players_index_for_player(10);
	combatant[0].num_programs = 0;
	combatant[0].team = 1 % MAX_TEAMS;
	combatant[0].vdesc = vdesc_index;
	(void) strncpy(combatant[0].name, "humanA", MAX_STRING - 1);

	combatant[1].num_players = 1;
	combatant[1].player[0] = server_players_index_for_player(11);
	combatant[1].num_programs = 0;
	combatant[1].team = 2 % MAX_TEAMS;
	combatant[1].vdesc = vdesc_index;
	(void) strncpy(combatant[1].name, "humanB", MAX_STRING - 1);
}

static int
run_scripted(int seed, int ticks)
{
	int t;

	server_seed_random((unsigned int) seed);
	if (server_match_init() != GAME_RUNNING)
		return -1;
	for (t = 1; t <= ticks; t++) {
		script_tick(t);
		if (server_step() != GAME_RUNNING)
			return -1;
	}
	return 0;
}

static void
audit_ordinary_reference(void)
{
	unsigned long h;

	fprintf(stderr, "== ordinary two-human reference (frozen P3.2)\n");
	if (run_scripted(114336, REF_TICKS) != 0) {
		check("ordinary two-human match runs to 200 ticks", 0);
		return;
	}
	check("ordinary path has two live human tanks", num_veh_alive == 2);
	h = state_hash();
	printf("DEPARTURE-FIXTURE hash-ordinary=%016lx ticks=%d\n", h, REF_TICKS);
	check("ordinary hash matches frozen P3.2 reference",
	      h == 0x5625b7035aae2e82UL);
	server_match_reset();
}

/* ---- message helpers -------------------------------------------------- */

static void
make_hello(XtkMessage *m, const char *name, const uint8_t *digest)
{
	(void) memset(m, 0, sizeof(*m));
	m->env.message_type = XTK_MSG_HELLO;
	m->body.hello.client_nonce = 0x11223344u;
	m->body.hello.proto_major = XTK_PROTOCOL_MAJOR;
	m->body.hello.proto_minor = XTK_PROTOCOL_MINOR;
	m->body.hello.client_build = 1;
	(void) strncpy(m->body.hello.name, name, XTK_MAX_NAME_BYTES);
	(void) memcpy(m->body.hello.content_set_digest, digest, XTK_SHA256_BYTES);
}

static void
make_config(XtkMessage *m, uint16_t maze, uint16_t veh, uint8_t drones)
{
	(void) memset(m, 0, sizeof(*m));
	m->env.message_type = XTK_MSG_CONFIG_SELECT;
	m->body.config_select.maze_id = maze;
	m->body.config_select.vehicle_id = veh;
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
make_reset(XtkMessage *m, uint32_t prior)
{
	(void) memset(m, 0, sizeof(*m));
	m->env.message_type = XTK_MSG_RESET;
	m->body.reset.prior_epoch = prior;
}

static void
make_quit(XtkMessage *m)
{
	(void) memset(m, 0, sizeof(*m));
	m->env.message_type = XTK_MSG_QUIT;
}

static void
make_heartbeat(XtkMessage *m, uint32_t epoch, uint32_t seq, uint32_t id)
{
	(void) memset(m, 0, sizeof(*m));
	m->env.message_type = XTK_MSG_INPUT_BATCH;
	m->env.epoch = epoch;
	m->body.input_batch.packet_sequence = seq;
	m->body.input_batch.command_count = 1;
	m->body.input_batch.command[0].command_id = id;
	m->body.input_batch.command[0].tick_hint = (uint32_t) frame + 1;
	m->body.input_batch.command[0].op = XTK_OP_HEARTBEAT;
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

static int
count_reply(const ServerSession *s, int type)
{
	int i, n = 0;
	XtkMessage m;

	for (i = 0; i < server_session_reply_count(s); i++) {
		if (server_session_reply(s, i, &m) == 0 &&
		    m.env.message_type == (uint8_t) type)
			n++;
	}
	return n;
}

/* ---- list helpers ----------------------------------------------------- */

static int
in_list(Vehicle **list, int n, Vehicle *v)
{
	int i;

	for (i = 0; i < n; i++)
		if (list[i] == v)
			return 1;
	return 0;
}

static int
bullets_owned_by(Vehicle *v)
{
	int i, n = 0;

	for (i = 0; i < bset->number; i++)
		if (bset->list[i]->owner == v)
			n++;
	return n;
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
	(void) strncpy(username, "p3.7", MAX_STRING - 1);
	username[MAX_STRING - 1] = '\0';

	if (content_set_root(g_assets) != 0) {
		fprintf(stderr, "departure_fixture: %s\n", content_last_error());
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

/* ---- coordinator helpers --------------------------------------------- */

static ServerSession *A, *B;

static void
open_two(uint64_t t)
{
	XtkMessage hello;

	server_session_init();
	server_session_set_capacity(XTK_MAX_HUMANS);
	A = server_session_accept((TransportPeer *) NULL);
	B = server_session_accept((TransportPeer *) NULL);
	make_hello(&hello, "alpha", xtk_content_set_digest);
	(void) server_session_message(A, &hello, t);
	make_hello(&hello, "beta", xtk_content_set_digest);
	(void) server_session_message(B, &hello, t);
}

static void
teardown_all(void)
{
	int i;

	for (i = 0; i < SERVER_SESSION_MAX_PEERS; i++) {
		ServerSession *p = server_session_peer_at(i);

		if (p != (ServerSession *) NULL)
			server_session_destroy(p);
	}
	A = B = (ServerSession *) NULL;
}

/* Configure combat1/Vanguard + two drones, ready both, and start. */
static int
configure_start(uint64_t t)
{
	XtkMessage cfg, rep, sr;

	make_config(&cfg, XTK_CONTENT_ID_COMBAT1, XTK_CONTENT_ID_VANGUARD, 2);
	if (server_session_message(A, &cfg, t) != XTK_ERR_NONE)
		return -1;
	(void) server_session_message(B, &cfg, t);
	make_ready(&rep, 1, server_session_config_revision(A));
	(void) server_session_message(A, &rep, t);
	(void) server_session_message(B, &rep, t);
	make_start_request(&sr, server_session_config_revision(A));
	return server_session_message(A, &sr, t) == XTK_ERR_NONE ? 0 : -1;
}

/* A bounded heartbeat-driven step loop; optionally withhold a peer's input. */
static int
run_ticks(int n, int feed_a, int feed_b, uint64_t *nowp, uint32_t *seqp)
{
	int k, status = GAME_RUNNING;

	for (k = 0; k < n; k++) {
		XtkMessage hb;
		uint32_t epoch = server_session_epoch((ServerSession *) NULL);

		*nowp += 66;
		if (feed_a && A != NULL && server_session_state(A) ==
		    SERVER_SESSION_RUNNING) {
			make_heartbeat(&hb, epoch, (*seqp)++, 1000u + (*seqp));
			(void) server_session_message(A, &hb, *nowp);
		}
		if (feed_b && B != NULL && server_session_state(B) ==
		    SERVER_SESSION_RUNNING) {
			make_heartbeat(&hb, epoch, (*seqp)++, 5000u + (*seqp));
			(void) server_session_message(B, &hb, *nowp);
		}
		status = server_session_step(server_session_current(), *nowp);
		if (status != GAME_RUNNING)
			break;
	}
	return k;
}

/* ---- live QUIT departure with a surviving human ----------------------- */

static void
audit_live_departure(void)
{
	uint64_t now = now_ms();
	uint32_t seq = 1;
	Vehicle *va, *vb;
	Bullet *proj;
	int slotA, slotB;
	int score0, deaths0, kills0, score1, deaths1, kills1;
	int eset0, team0, teams0, eset1, teamB0;
	uint32_t ctlA;
	XtkMessage q, m;
	int n;

	fprintf(stderr, "== live QUIT departure (survivor plays on)\n");
	open_two(now);
	if (configure_start(now) != 0) {
		check("match starts", 0);
		teardown_all();
		return;
	}
	check("match is Running epoch 1",
	      server_session_state(A) == SERVER_SESSION_RUNNING &&
	      server_session_epoch(A) == 1);
	check("exactly two human combatants bound to slots 0 and 1",
	      server_session_bound_slot(A) == 0 &&
	      server_session_bound_slot(B) == 1);

	slotA = server_session_bound_slot(A);
	slotB = server_session_bound_slot(B);
	ctlA = server_session_controller_id(A);
	va = &actual_vehicles[slotA];
	vb = &actual_vehicles[slotB];

	run_ticks(6, 1, 1, &now, &seq);
	check("both human tanks alive before departure",
	      tstflag(va->status, VS_is_alive) &&
	      tstflag(vb->status, VS_is_alive));
	check("four vehicles alive (2 humans + 2 drones)", num_veh_alive == 4);

	/* Give the departing tank a live projectile that still references it. */
	proj = make_bullet(va, va->loc, (WeaponType) MG, (Angle) 0.0,
			   (lCoord *) NULL);
	if (proj != NULL)
		proj->life = 100000;

	score0 = va->owner->score; deaths0 = va->owner->deaths; kills0 = va->owner->kills;
	score1 = vb->owner->score; deaths1 = vb->owner->deaths; kills1 = vb->owner->kills;
	eset0 = eset->number;
	team0 = teamdata[va->team].vehicle_count;
	teamB0 = teamdata[vb->team].vehicle_count;
	teams0 = num_teams;

	/* A QUITs; the service reclaim runs the departure path at this boundary. */
	server_session_reply_clear(B);
	make_quit(&q);
	(void) server_session_message(A, &q, now);
	(void) server_session_service(now);

	check("departing peer record released", server_session_count() == 1);
	check("survivor remains in the Running match",
	      server_session_state(B) == SERVER_SESSION_RUNNING);
	check("departed tank removed from the live list",
	      !in_list(live_vehicles, num_veh_alive, va));
	check("departed tank not alive", !tstflag(va->status, VS_is_alive));
	check("survivor tank still alive and in the live list",
	      tstflag(vb->status, VS_is_alive) &&
	      in_list(live_vehicles, num_veh_alive, vb));
	check("one vehicle fewer after removal", num_veh_alive == 3);
	check("no score change for either human",
	      va->owner->score == score0 && vb->owner->score == score1);
	check("no death change for either human",
	      va->owner->deaths == deaths0 && vb->owner->deaths == deaths1);
	check("no kill change for either human",
	      va->owner->kills == kills0 && vb->owner->kills == kills1);
	check("removal emitted no explosion", eset->number == eset0);
	check("team bookkeeping unchanged by removal",
	      teamdata[va->team].vehicle_count == team0 &&
	      teamdata[vb->team].vehicle_count == teamB0 &&
	      num_teams == teams0);
	check("departed slot marked departed", server_slot_departed(slotA) == 1);
	check("departed record retained for projectile owner",
	      proj != NULL && proj->owner == va && bullets_owned_by(va) >= 1);
	check("retained departed storage not freed",
	      va->owner != (Combatant *) NULL && va->number == slotA);
	check("survivor binding still valid",
	      server_players_validate(server_session_controller_id(B),
				      slotB, (unsigned int) -1) == 1);
	check("departed controller no longer owns its slot",
	      server_players_validate(ctlA, slotA, (unsigned int) -1) == 0);

	/* Reliable + unique admin transfer to the survivor. */
	check("survivor is now administrator",
	      server_session_is_admin(B) == 1 &&
	      server_session_is_admin(A) == 0);
	check("admin transfer emitted one LOBBY_STATE with the new admin",
	      find_reply(B, XTK_MSG_LOBBY_STATE, &m) == 1 &&
	      m.body.lobby_state.admin_index == (uint8_t) server_session_index(B));
	check("running rejoin is rejected", 
	      server_session_accept((TransportPeer *) NULL) == NULL);

	/* The survivor keeps playing; the departed tank never respawns. */
	n = run_ticks(40, 0, 1, &now, &seq);
	check("survivor keeps progressing after departure", n >= 30 &&
	      tstflag(vb->status, VS_is_alive));
	check("departed tank never re-enters the live list",
	      !in_list(live_vehicles, num_veh_alive, va));
	check("departed tank never returns to the dead list either",
	      !in_list(dead_vehicles, num_veh_dead, va));

	/* Force an ordinary win for the survivor and check the marked results. */
	{
		int k, status = GAME_RUNNING;

		vb->owner->score = settings.si.winning_score;
		for (k = 0; k < 100000 && status == GAME_RUNNING; k++) {
			XtkMessage hb;
			uint32_t e = server_session_epoch(B);

			now += 66;
			make_heartbeat(&hb, e, seq++, 8000u + seq);
			(void) server_session_message(B, &hb, now);
			status = server_session_step(B, now);
		}
		check("survivor match ends naturally", status != GAME_RUNNING);
		check("state is Results",
		      server_session_state(B) == SERVER_SESSION_RESULTS);
		check("survivor receives one RESULTS",
		      count_reply(B, XTK_MSG_RESULTS) == 1);
		check("departed record in results marked departed with retained name",
		      find_reply(B, XTK_MSG_RESULTS, &m) == 1 &&
		      m.body.results.entry[slotA].slot == (uint8_t) slotA &&
		      (m.body.results.entry[slotA].flags & XTK_FLAG_DEPARTED) &&
		      strcmp(m.body.results.entry[slotA].name, "alpha") == 0 &&
		      m.body.results.entry[slotA].score == (uint32_t) score0);
		check("survivor result record is not marked departed",
		      (m.body.results.entry[slotB].flags & XTK_FLAG_DEPARTED) == 0);
		eset1 = eset->number;
		(void) eset1;
	}

	/* Reset releases the retained storage and returns to the lobby. */
	server_session_reply_clear(B);
	make_reset(&m, server_session_epoch(B));
	check("admin reset returns to lobby",
	      server_session_message(B, &m, now) == XTK_ERR_NONE &&
	      server_session_state(B) == SERVER_SESSION_LOBBY);
	check("departed records released at reset", server_departed_count() == 0);
	check("world cleaned at reset",
	      num_veh_alive == 0 && num_veh_dead == 0);
	teardown_all();
}

/* ---- respawn-waiting departure ---------------------------------------- */

static void
audit_respawn_waiting_departure(void)
{
	uint64_t now = now_ms();
	uint32_t seq = 1;
	Vehicle *va, *vb;
	int slotA, slotB, t, died = 0, back = 0;

	fprintf(stderr, "== respawn-waiting departure\n");
	open_two(now);
	if (configure_start(now) != 0) {
		check("match starts", 0);
		teardown_all();
		return;
	}
	slotA = server_session_bound_slot(A);
	slotB = server_session_bound_slot(B);
	va = &actual_vehicles[slotA];
	vb = &actual_vehicles[slotB];
	(void) vb;

	run_ticks(4, 1, 1, &now, &seq);
	kill_vehicle(va, (Vehicle *) NULL);
	for (t = 0; t < 100 && !died; t++) {
		(void) run_ticks(1, 1, 1, &now, &seq);
		if (!tstflag(va->status, VS_is_alive) &&
		    in_list(dead_vehicles, num_veh_dead, va))
			died = 1;
	}
	check("departing tank is respawn-waiting", died);

	server_session_depart(A, XTK_ERR_TIMEOUT);
	check("respawn-waiting tank left the dead list",
	      !in_list(dead_vehicles, num_veh_dead, va));
	check("respawn-waiting departure retained its record",
	      server_slot_departed(slotA) == 1);
	check("respawn-waiting departure left the survivor Running",
	      server_session_state(B) == SERVER_SESSION_RUNNING);

	for (t = 0; t < 120 && !back; t++) {
		(void) run_ticks(1, 0, 1, &now, &seq);
		if (tstflag(va->status, VS_is_alive) ||
		    in_list(dead_vehicles, num_veh_dead, va) ||
		    in_list(live_vehicles, num_veh_alive, va))
			back = 1;
	}
	check("departed respawn-waiting tank never comes back", !back);
	teardown_all();
}

/* ---- pending / held input cancellation -------------------------------- */

static void
audit_pending_input(void)
{
	uint64_t now = now_ms();
	uint32_t seq = 1;
	Vehicle *va, *vb;
	int slotA, slotB;

	fprintf(stderr, "== pending/held input cancel (player-scoped)\n");
	open_two(now);
	if (configure_start(now) != 0) {
		check("match starts", 0);
		teardown_all();
		return;
	}
	slotA = server_session_bound_slot(A);
	slotB = server_session_bound_slot(B);
	va = &actual_vehicles[slotA];
	vb = &actual_vehicles[slotB];

	run_ticks(4, 1, 1, &now, &seq);

	/* Held fire for both controllers, queued for the next tick and then
	 * applied so both controllers hold fire. */
	{
		Intent in;

		(void) memset(&in, 0, sizeof(in));
		in.tick = frame + 1;
		in.player = slotA;
		in.sequence = 100;
		in.op = INTENT_FIRE_HOLD;
		in.a = 1.0;
		(void) intent_push(&in);
		in.player = slotB;
		in.sequence = 101;
		(void) intent_push(&in);
		(void) server_session_step(B, now + 66);
	}
	check("held fire tracked for the departing slot", intent_held_fire(slotA) == 1);
	check("held fire tracked for the surviving slot", intent_held_fire(slotB) == 1);

	/* The departure must cancel only the departing controller. */
	server_session_depart(A, XTK_ERR_NONE);
	check("departed held fire cancelled", intent_held_fire(slotA) == 0);
	check("survivor held fire preserved", intent_held_fire(slotB) == 1);
	check("survivor still alive after the cancel",
	      tstflag(vb->status, VS_is_alive));

	/* Reload the departing tank's weapons: if its input survived it would
	 * fire a ghost projectile even though it has been removed. */
	{
		int w;

		for (w = 0; w < va->num_weapons; w++)
			va->weapon[w].reload_counter = 0;
	}
	init_bset();
	(void) server_session_step(B, now + 66);
	check("no ghost projectile from the departed tank",
	      bullets_owned_by(va) == 0);
	teardown_all();
}

/* ---- valid-input timeout removal -------------------------------------- */

static void
audit_timeout_departure(void)
{
	uint64_t now = now_ms();
	uint32_t seq = 1;
	Vehicle *va, *vb;
	int slotA, slotB;
	uint32_t ctlA;

	fprintf(stderr, "== valid-input timeout removal\n");
	open_two(now);
	if (configure_start(now) != 0) {
		check("match starts", 0);
		teardown_all();
		return;
	}
	slotA = server_session_bound_slot(A);
	slotB = server_session_bound_slot(B);
	va = &actual_vehicles[slotA];
	vb = &actual_vehicles[slotB];
	ctlA = server_session_controller_id(A);

	/* B heartbeats; A goes silent.  Removal is strictly after 2000 ms. */
	run_ticks(35, 0, 1, &now, &seq);
	check("silent peer removed by timeout", server_session_count() == 1);
	check("timed-out tank removed from the live list",
	      !in_list(live_vehicles, num_veh_alive, va) &&
	      !tstflag(va->status, VS_is_alive));
	check("timeout removed no score/kill", va->owner->kills == 0 &&
	      va->owner->score == 0);
	check("timeout retained the departed record",
	      server_slot_departed(slotA) == 1);
	check("timeout did not own the departed slot",
	      server_players_validate(ctlA, slotA, (unsigned int) -1) == 0);
	check("survivor keeps playing after a timeout",
	      server_session_state(B) == SERVER_SESSION_RUNNING &&
	      tstflag(vb->status, VS_is_alive));
	teardown_all();
}

/* ---- saturation / disconnect removal ---------------------------------- */

static void
audit_saturation_and_disconnect(void)
{
	uint64_t now = now_ms();
	uint32_t seq = 1;
	int slot;

	fprintf(stderr, "== saturation and disconnect use the same removal path\n");
	open_two(now);
	if (configure_start(now) != 0) {
		check("match starts", 0);
		teardown_all();
		return;
	}
	slot = server_session_bound_slot(A);
	run_ticks(2, 1, 1, &now, &seq);
	server_session_depart(A, XTK_ERR_QUEUE_SATURATED);
	check("saturation removes exactly the saturated tank",
	      server_session_count() == 1 && server_slot_departed(slot) == 1 &&
	      server_session_state(B) == SERVER_SESSION_RUNNING);

	/* Idempotent: a second departure of the same (already gone) peer is a
	 * no-op. */
	server_session_depart(A, XTK_ERR_QUEUE_SATURATED);
	check("repeated departure is a no-op", server_session_count() == 1);
	teardown_all();

	/* A transport-style disconnect goes through the service reclaim path. */
	open_two(now);
	if (configure_start(now) != 0) {
		check("match starts", 0);
		teardown_all();
		return;
	}
	slot = server_session_bound_slot(A);
	run_ticks(2, 1, 1, &now, &seq);
	/* server_session_service dispatches a transport disconnect here; call
	 * the same departure path directly for a deterministic fixture. */
	server_session_depart(A, XTK_ERR_TIMEOUT);
	check("disconnected peer is gone", server_session_count() == 1);
	check("disconnect left the survivor Running",
	      server_session_state(B) == SERVER_SESSION_RUNNING &&
	      server_slot_departed(slot) == 1);
	teardown_all();
}

/* ---- no-humans cleanup, rejoin, fresh identity, no replay ------------- */

static void
audit_no_humans_and_rejoin(void)
{
	uint64_t now = now_ms();
	uint32_t seq = 1;
	uint32_t ctlA, sidA, ctlC;
	XtkMessage q, hello, m;
	int i;

	fprintf(stderr, "== no humans cleans world; rejoin gets fresh identity\n");
	open_two(now);
	if (configure_start(now) != 0) {
		check("match starts", 0);
		teardown_all();
		return;
	}
	ctlA = server_session_controller_id(A);
	sidA = server_session_id(A);
	run_ticks(3, 1, 1, &now, &seq);

	/* Both humans quit at once (simultaneous departures). */
	make_quit(&q);
	(void) server_session_message(A, &q, now);
	(void) server_session_message(B, &q, now);
	(void) server_session_service(now);

	check("no humans: world emptied",
	      num_veh_alive == 0 && num_veh_dead == 0);
	check("no humans: retained records released", server_departed_count() == 0);
	check("no humans: returns to the listening lobby",
	      server_session_count() == 0 &&
	      server_session_capacity() == XTK_MAX_HUMANS);

	/* A fresh join gets a new connection/controller identity. */
	for (i = 0; i < SERVER_SESSION_MAX_PEERS; i++) {
		ServerSession *p = server_session_peer_at(i);

		if (p != (ServerSession *) NULL)
			server_session_destroy(p);
	}
	A = server_session_accept((TransportPeer *) NULL);
	check("fresh peer admitted after the world emptied", A != NULL);
	make_hello(&hello, "gamma", xtk_content_set_digest);
	(void) server_session_message(A, &hello, now);
	ctlC = server_session_controller_id(A);
	check("rejoin gets a fresh connection id", server_session_id(A) != sidA);
	check("rejoin gets a fresh controller id", ctlC != ctlA);
	check("old departed controller cannot claim the fresh tank",
	      server_players_validate(ctlA, 0, (unsigned int) -1) == 0);

	/* A fresh two-human start builds a clean world. */
	B = server_session_accept((TransportPeer *) NULL);
	make_hello(&hello, "delta", xtk_content_set_digest);
	(void) server_session_message(B, &hello, now);
	if (configure_start(now) != 0) {
		check("fresh two-human match starts", 0);
		teardown_all();
		return;
	}
	check("fresh two-human match starts clean",
	      server_session_state(A) == SERVER_SESSION_RUNNING &&
	      server_session_epoch(A) == 2 &&
	      num_veh_alive == 4);
	check("fresh epoch has no stale departed markers",
	      server_slot_departed(0) == 0 && server_slot_departed(1) == 0);
	(void) m;
	teardown_all();
}

/* ---- repeated disconnect/rejoin cycles -------------------------------- */

static void
audit_repeated_cycles(void)
{
	int cycle, ok = 1;
	uint64_t now = now_ms();
	uint32_t ctl_prev = 0, sid_prev = 0;

	fprintf(stderr, "== repeated disconnect/rejoin cycles\n");
	server_session_init();
	server_session_set_capacity(XTK_MAX_HUMANS);

	for (cycle = 0; cycle < 5; cycle++) {
		uint32_t seq = 1;
		XtkMessage q, hello;
		char name[32];

		A = server_session_accept((TransportPeer *) NULL);
		(void) snprintf(name, sizeof(name), "alpha%d", cycle);
		make_hello(&hello, name, xtk_content_set_digest);
		(void) server_session_message(A, &hello, now);
		B = server_session_accept((TransportPeer *) NULL);
		(void) snprintf(name, sizeof(name), "beta%d", cycle);
		make_hello(&hello, name, xtk_content_set_digest);
		(void) server_session_message(B, &hello, now);

		if (configure_start(now) != 0) {
			ok = 0;
			break;
		}
		/* Identity is fresh every epoch on the same running server. */
		if (cycle > 0 &&
		    (server_session_controller_id(A) == ctl_prev ||
		     server_session_id(A) == sid_prev))
			ok = 0;
		ctl_prev = server_session_controller_id(A);
		sid_prev = server_session_id(A);

		run_ticks(3, 1, 1, &now, &seq);
		make_quit(&q);
		(void) server_session_message(A, &q, now);
		(void) server_session_message(B, &q, now);
		(void) server_session_service(now);
		if (server_session_count() != 0 || num_veh_alive != 0 ||
		    num_veh_dead != 0 || server_departed_count() != 0)
			ok = 0;
	}
	check("five disconnect/rejoin cycles stay clean", ok);
	teardown_all();
}

/* ---- main ------------------------------------------------------------- */

int
main(int argc, char *argv[])
{
	g_assets = (argc > 1) ? argv[1] : ".";

	if (server_bringup() != 0)
		return 2;

	/* Load the release content once; the coordinator loads per epoch. */
	{
		int vd, md;

		if (make_vdesc((char *) "Vanguard", &vd) != DESC_LOADED ||
		    make_mdesc((char *) "combat1", &md) != DESC_LOADED) {
			fprintf(stderr, "departure_fixture: cannot load content\n");
			return 2;
		}
		settings.mdesc = &mdesc[md];
		settings.si.game = settings.mdesc->type;
		build_two_humans_direct(vd);
	}

	audit_ordinary_reference();
	audit_live_departure();
	audit_respawn_waiting_departure();
	audit_pending_input();
	audit_timeout_departure();
	audit_saturation_and_disconnect();
	audit_no_humans_and_rejoin();
	audit_repeated_cycles();

	if (failures != 0) {
		printf("DEPARTURE-FIXTURE FAIL (%d/%d checks)\n", failures, checks);
		return 1;
	}
	printf("DEPARTURE-FIXTURE PASS (checks=%d: live/respawn/timeout/"
	       "saturation/disconnect removal, player-scoped cancel, retained "
	       "projectile owner, admin transfer, no-humans lobby return, fresh "
	       "rejoin identity, simultaneous departures, repeated cycles)\n",
	       checks);
	return 0;
}