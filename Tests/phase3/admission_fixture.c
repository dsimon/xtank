/*-
 * Copyright (c) 1988 Terry Donahue
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met: [see the project COPYRIGHT / legacy sources]
 *
 * admission_fixture.c -- P3.6 fair bounded admission and valid-input timeouts.
 *
 * Links the shipped display-free server core (Src/sobj minus server_main.o),
 * the P3.4 coordinator (Src/server_session.c), the P2.7 admission layer
 * (Src/server_net.c), the pinned ENet transport and the frozen v1.1 codec, and
 * asserts the D3-E contract:
 *
 *   A. per-peer per-tick quotas within the global 64-event / 128-intent bounds:
 *      a peer flooding drive commands is capped at SERVER_NET_PEER_INTENT_QUOTA,
 *      the healthy peer is still admitted on the same tick, both peers can still
 *      release drive/fire, and the engine queue never exceeds 128;
 *   B. the 8-intent reserve covers BOTH peers' server-side neutralisation even
 *      after both peers have used their full wire quota;
 *   C. same command ids on different peers never collide, a cross-channel
 *      duplicate on one peer applies once, the actual transport channel is
 *      validated (wrong channel rejected with XTK_ERR_BAD_CHANNEL and no intent),
 *      epoch/ownership/finite/range/expiry validation precedes every intent;
 *   D. the valid-input timeout boundary is exactly 2,000 ms (1,999/2,000 alive,
 *      2,001 removes) and invalid/replayed/foreign traffic cannot extend it.
 *
 * The fixture may manipulate simulation state directly (it is the harness); the
 * admission layer and the coordinator are the units under test.
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
#include "server.h"
#include "server_net.h"
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

/* ---- wire helpers ------------------------------------------------------ */

static void
cmd_init(XtkCommand *c, uint32_t id, uint32_t hint, uint8_t op, uint8_t flags,
	 double a, double b)
{
	(void) memset(c, 0, sizeof(*c));
	c->command_id = id;
	c->tick_hint = hint;
	c->op = op;
	c->flags = flags;
	c->a = a;
	c->b = b;
}

static void
make_batch(XtkMessage *m, uint32_t epoch, uint32_t seq, const XtkCommand *cmds,
	   int n)
{
	int i;

	(void) memset(m, 0, sizeof(*m));
	m->env.message_type = XTK_MSG_INPUT_BATCH;
	m->env.epoch = epoch;
	m->body.input_batch.packet_sequence = seq;
	m->body.input_batch.command_count = (uint16_t) n;
	for (i = 0; i < n; i++)
		m->body.input_batch.command[i] = cmds[i];
}

static void
make_reliable(XtkMessage *m, uint32_t epoch, const XtkCommand *cmds, int n)
{
	int i;

	(void) memset(m, 0, sizeof(*m));
	m->env.message_type = XTK_MSG_RELIABLE_CMD;
	m->env.epoch = epoch;
	m->body.reliable_cmd.command_count = (uint16_t) n;
	for (i = 0; i < n; i++)
		m->body.reliable_cmd.command[i] = cmds[i];
}

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

static uint8_t
ack_status(ServerNetSession *s, uint32_t id, uint32_t *tick, uint8_t *reason)
{
	uint8_t st = 0xFF, rs = 0xFF;

	if (!server_net_session_ack_lookup(s, id, tick, &st, &rs))
		return 0xFF;
	if (reason != (uint8_t *) NULL)
		*reason = rs;
	return st;
}

/* ---- shared bring-up --------------------------------------------------- */

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
	(void) strncpy(username, "p3.6", MAX_STRING - 1);
	username[MAX_STRING - 1] = '\0';

	if (content_set_root(g_assets) != 0) {
		fprintf(stderr, "admission_fixture: %s\n", content_last_error());
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
		fprintf(stderr, "admission_fixture: cannot load Vanguard\n");
		return -1;
	}
	if (make_mdesc((char *) "combat1", &(int){0}) != DESC_LOADED) {
		fprintf(stderr, "admission_fixture: cannot load combat1\n");
		return -1;
	}
	return 0;
}

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
	server_net_init();
}

/* ---- A/B: fairness, quotas, reserve, dedup, validation ----------------- */

static ServerSession *gA, *gB;

/* Bring up a live two-human match and return the two admission sessions. */
static int
live_two_human(ServerNetSession **na, ServerNetSession **nb, uint32_t *epoch)
{
	uint64_t t = now_ms();

	teardown_session();
	server_session_set_capacity(XTK_MAX_HUMANS);
	gA = admit("fairA", t);
	gB = admit("fairB", t);
	if (gA == (ServerSession *) NULL || gB == (ServerSession *) NULL)
		return -1;
	if (start_match(gA, gB, XTK_CONTENT_ID_COMBAT1, 0, t) != XTK_ERR_NONE)
		return -1;
	if (server_session_state(gA) != SERVER_SESSION_RUNNING)
		return -1;
	if (server_net_session_count() != 2)
		return -1;
	*na = server_net_session_at(0);
	*nb = server_net_session_at(1);
	*epoch = server_session_epoch(gA);
	return (*na != NULL && *nb != NULL) ? 0 : -1;
}

static void
section_fairness(void)
{
	ServerNetSession *A, *B;
	uint32_t epoch, T;
	XtkCommand c;
	XtkMessage m;
	int i, accepted, queued_a;

	fprintf(stderr, "== per-peer quotas and cross-peer fairness\n");

	check("fairness constants stay within the global bounds",
	      SERVER_NET_PEER_INTENT_QUOTA * XTK_MAX_HUMANS +
	      SERVER_NET_RELEASE_RESERVE <= (int) XTK_INTENT_QUEUE_MAX);
	check("peer release quota covers drive + fire neutralisation",
	      SERVER_NET_PEER_RELEASE_QUOTA >= 2);
	{
		int r0 = server_net_rotation();

		server_net_begin_tick(9001);
		check("peer-first cursor rotates on a tick change",
		      server_net_rotation() ==
		      (r0 + 1) % SERVER_NET_MAX_SESSIONS);
	}

	if (live_two_human(&A, &B, &epoch) != 0) {
		check("two-human match for fairness", 0);
		teardown_session();
		return;
	}
	check("two-human match for fairness", 1);

	intent_reset();
	T = (uint32_t) (server_frame() + 1);

	/* Flood peer A with unique non-release drives. */
	for (i = 0; i < 200; i++) {
		cmd_init(&c, (uint32_t) (10000 + i), T, XTK_OP_DRIVE_REL,
			 XTK_FLAG_HELD, 5.0, 0.0);
		make_batch(&m, epoch, (uint32_t) (1 + i), &c, 1);
		(void) server_net_admit_message_on(A, &m, XTK_CH_INPUT, (int) T);
	}
	queued_a = intent_queued();
	check("flooding peer is capped at its per-tick quota",
	      queued_a == SERVER_NET_PEER_INTENT_QUOTA);
	check("flooding peer stays within its quota",
	      queued_a <= SERVER_NET_PEER_INTENT_QUOTA);

	/* The healthy peer is admitted on the SAME tick despite the flood. */
	cmd_init(&c, 20001, T, XTK_OP_DRIVE_REL, XTK_FLAG_HELD, -3.0, 0.0);
	make_batch(&m, epoch, 100000, &c, 1);
	accepted = server_net_admit_message_on(B, &m, XTK_CH_INPUT, (int) T);
	check("healthy peer admitted on the flood tick", accepted == 1);
	check("healthy peer's command is queued",
	      intent_queued() == queued_a + 1);

	/* Both peers can still release drive and fire under saturation. */
	cmd_init(&c, 21000, T, XTK_OP_DRIVE_REL, XTK_FLAG_HELD, 0.0, 0.0);
	make_batch(&m, epoch, 100001, &c, 1);
	check("flooding peer release drive admitted",
	      server_net_admit_message_on(A, &m, XTK_CH_INPUT, (int) T) == 1);
	cmd_init(&c, 21001, T, XTK_OP_FIRE_HOLD, XTK_FLAG_HELD, 0.0, 0.0);
	make_batch(&m, epoch, 100002, &c, 1);
	check("flooding peer release fire admitted",
	      server_net_admit_message_on(A, &m, XTK_CH_INPUT, (int) T) == 1);
	cmd_init(&c, 21002, T, XTK_OP_DRIVE_REL, XTK_FLAG_HELD, 0.0, 0.0);
	make_batch(&m, epoch, 100003, &c, 1);
	check("healthy peer release drive admitted",
	      server_net_admit_message_on(B, &m, XTK_CH_INPUT, (int) T) == 1);
	cmd_init(&c, 21003, T, XTK_OP_FIRE_HOLD, XTK_FLAG_HELD, 0.0, 0.0);
	make_batch(&m, epoch, 100004, &c, 1);
	check("healthy peer release fire admitted",
	      server_net_admit_message_on(B, &m, XTK_CH_INPUT, (int) T) == 1);

	/* Fill the healthy peer's non-release quota too, then confirm the
	   combined wire load leaves the reserve free. */
	for (i = 0; i < 200; i++) {
		cmd_init(&c, (uint32_t) (30000 + i), T, XTK_OP_DRIVE_REL,
			 XTK_FLAG_HELD, 4.0, 0.0);
		make_batch(&m, epoch, (uint32_t) (200000 + i), &c, 1);
		(void) server_net_admit_message_on(B, &m, XTK_CH_INPUT, (int) T);
	}
	check("wire load never consumes the neutralisation reserve",
	      intent_queued() <=
	      INTENT_QUEUE_MAX - SERVER_NET_RELEASE_RESERVE);
	check("engine queue stays within 128", intent_queued() <= INTENT_QUEUE_MAX);

	/* The reserve covers BOTH peers' server-side departures. */
	{
		Intent in;
		int slotA = server_net_session_slot(A);
		int slotB = server_net_session_slot(B);
		int okA, okB;

		(void) memset(&in, 0, sizeof(in));
		in.tick = (int) T;
		in.player = slotA;
		in.sequence = 0xFFFFFFFEu;
		in.op = INTENT_DRIVE_REL;
		okA = (intent_push(&in) == INTENT_OK);
		in.sequence = 0xFFFFFFFFu;
		in.op = INTENT_FIRE_HOLD;
		okA = okA && (intent_push(&in) == INTENT_OK);

		(void) memset(&in, 0, sizeof(in));
		in.tick = (int) T;
		in.player = slotB;
		in.sequence = 0xFFFFFFFEu;
		in.op = INTENT_DRIVE_REL;
		okB = (intent_push(&in) == INTENT_OK);
		in.sequence = 0xFFFFFFFFu;
		in.op = INTENT_FIRE_HOLD;
		okB = okB && (intent_push(&in) == INTENT_OK);

		check("reserve neutralises both departures", okA && okB);
		check("neutralised queue still within 128",
		      intent_queued() <= INTENT_QUEUE_MAX);
	}

	teardown_session();
}

static void
section_identity_and_validation(void)
{
	ServerNetSession *A, *B;
	uint32_t epoch, T, tick;
	uint8_t reason;
	XtkCommand c;
	XtkMessage m;
	uint64_t t = now_ms();
	int before, i;

	fprintf(stderr, "== identity, channel and validation precedence\n");
	if (live_two_human(&A, &B, &epoch) != 0) {
		check("two-human match for identity", 0);
		teardown_session();
		return;
	}
	/* Advance the world past the expiry window so a stale edge is testable. */
	for (i = 0; i < (int) XTK_CMD_EXPIRY_TICKS + 5; i++)
		(void) server_session_step(A, t);
	intent_reset();
	T = (uint32_t) (server_frame() + 1);

	/* Same command id on different peers must not collide. */
	cmd_init(&c, 55000, T, XTK_OP_DRIVE_REL, XTK_FLAG_HELD, 2.0, 0.0);
	make_batch(&m, epoch, 1, &c, 1);
	check("command id on peer A accepted",
	      server_net_admit_message_on(A, &m, XTK_CH_INPUT, (int) T) == 1);
	cmd_init(&c, 55000, T, XTK_OP_DRIVE_REL, XTK_FLAG_HELD, -2.0, 0.0);
	make_batch(&m, epoch, 1, &c, 1);
	check("same id on peer B accepted (no cross-peer collision)",
	      server_net_admit_message_on(B, &m, XTK_CH_INPUT, (int) T) == 1);

	/* Cross-channel duplicate on one peer applies once. */
	before = intent_queued();
	cmd_init(&c, 55100, T, XTK_OP_FIRE, XTK_FLAG_EDGE, 0.0, 0.0);
	make_batch(&m, epoch, 2, &c, 1);
	check("edge accepted on the input channel",
	      server_net_admit_message_on(A, &m, XTK_CH_INPUT, (int) T) == 1);
	make_reliable(&m, epoch, &c, 1);
	check("same id on the control channel is a duplicate",
	      server_net_admit_message_on(A, &m, XTK_CH_CONTROL, (int) T) == 0);
	check("cross-channel duplicate applied once",
	      intent_queued() == before + 1);
	check("duplicate ack carries the duplicate reason",
	      ack_status(A, 55100, &tick, &reason) == XTK_ACK_REJECTED &&
	      reason == XTK_ERR_DUPLICATE);

	/* Wrong actual channel precedes any intent. */
	before = intent_queued();
	cmd_init(&c, 55200, T, XTK_OP_DRIVE_REL, XTK_FLAG_HELD, 1.0, 0.0);
	make_batch(&m, epoch, 3, &c, 1);
	check("INPUT_BATCH on the control channel rejected",
	      server_net_admit_message_on(A, &m, XTK_CH_CONTROL, (int) T) == 0);
	check("bad-channel ack reason",
	      ack_status(A, 55200, &tick, &reason) == XTK_ACK_REJECTED &&
	      reason == XTK_ERR_BAD_CHANNEL);
	make_reliable(&m, epoch, &c, 1);
	check("RELIABLE_CMD on the input channel rejected",
	      server_net_admit_message_on(A, &m, XTK_CH_INPUT, (int) T) == 0);
	check("bad channel made no intent", intent_queued() == before);

	/* Stale epoch precedes intents. */
	before = intent_queued();
	cmd_init(&c, 55300, T, XTK_OP_DRIVE_REL, XTK_FLAG_HELD, 1.0, 0.0);
	make_batch(&m, epoch + 1, 4, &c, 1);
	check("stale epoch rejected",
	      server_net_admit_message_on(A, &m, XTK_CH_INPUT, (int) T) == 0);
	check("stale epoch made no intent", intent_queued() == before);

	/* Stale expiry (tick_hint far in the past) precedes intents. */
	before = intent_queued();
	cmd_init(&c, 55400, 1, XTK_OP_DRIVE_REL, XTK_FLAG_HELD, 1.0, 0.0);
	make_batch(&m, epoch, 5, &c, 1);
	check("expired command rejected",
	      server_net_admit_message_on(A, &m, XTK_CH_INPUT,
					  (int) (server_frame() + 100)) == 0);
	check("expired command made no intent", intent_queued() == before);

	/* Finite/range/op validation precedes intents. */
	cmd_init(&c, 55500, T, XTK_OP_HEADING, 0, 1.0 / 0.0, 0.0);
	make_batch(&m, epoch, 6, &c, 1);
	check("nonfinite operand rejected",
	      server_net_admit_message_on(A, &m, XTK_CH_INPUT, (int) T) == 0);
	cmd_init(&c, 55501, T, XTK_OP_DRIVE_REL, XTK_FLAG_HELD, 99.0, 0.0);
	make_batch(&m, epoch, 7, &c, 1);
	check("out-of-range drive rejected",
	      server_net_admit_message_on(A, &m, XTK_CH_INPUT, (int) T) == 0);
	cmd_init(&c, 55502, T, XTK_OP_CHASE, XTK_FLAG_HELD, 0.0, 0.0);
	make_batch(&m, epoch, 8, &c, 1);
	check("test-only op rejected",
	      server_net_admit_message_on(A, &m, XTK_CH_INPUT, (int) T) == 0);

	/* Ownership: unbinding the slot invalidates foreign traffic. */
	{
		int slotA = server_net_session_slot(A);

		server_players_unbind_slot(slotA);
		before = intent_queued();
		cmd_init(&c, 55600, T, XTK_OP_DRIVE_REL, XTK_FLAG_HELD, 1.0, 0.0);
		make_batch(&m, epoch, 9, &c, 1);
		check("stale owner / generation rejected",
		      server_net_admit_message_on(A, &m, XTK_CH_INPUT,
						  (int) T) == 0);
		check("foreign owner made no intent", intent_queued() == before);
	}

	teardown_session();
}

/* ---- D: valid-input timeout boundary ----------------------------------- */

static void
section_timeout(void)
{
	uint64_t t0;
	ServerSession *A, *B;
	XtkCommand c;
	XtkMessage m;

	fprintf(stderr, "== valid-input timeout boundary (2000 ms)\n");

	check("frozen timeout constant is 2000 ms",
	      XTK_NEUTRAL_TIMEOUT_MS == 2000U);

	/* Scenario 1: silence times out at 2,001 ms, not before. */
	teardown_session();
	server_session_set_capacity(XTK_MAX_HUMANS);
	t0 = now_ms();
	A = admit("timeA", t0);
	B = admit("timeB", t0);
	check("timeout scenario admitted",
	      A != NULL && B != NULL &&
	      start_match(A, B, XTK_CONTENT_ID_COMBAT1, 0, t0) == XTK_ERR_NONE);
	check("timeout scenario is Running",
	      server_session_state(A) == SERVER_SESSION_RUNNING);
	check("1999 ms is still alive",
	      server_session_step(A, t0 + 1999) == GAME_RUNNING);
	check("2000 ms is still alive",
	      server_session_step(A, t0 + 2000) == GAME_RUNNING);
	check("2001 ms removes the silent peer",
	      server_session_step(A, t0 + 2001) == GAME_OVER);
	/*
	 * P3.7 supersedes the P3.6 shared-teardown behaviour this assertion
	 * originally pinned: a timeout now removes the peer through the shared
	 * departure path.  With both peers silent the world ends and both peer
	 * records are released back to the listening lobby.  The 1,999/2,000/
	 * 2,001 ms boundary coverage above is unchanged.
	 */
	check("after timeout the peers are removed and the world is emptied",
	      server_session_count() == 0 && num_veh_alive == 0 &&
	      server_departed_count() == 0);
	teardown_session();

	/* Scenario 2: invalid / replayed / foreign traffic does not extend. */
	server_session_set_capacity(XTK_MAX_HUMANS);
	t0 = now_ms();
	A = admit("extA", t0);
	B = admit("extB", t0);
	check("extension scenario admitted",
	      A != NULL && B != NULL &&
	      start_match(A, B, XTK_CONTENT_ID_COMBAT1, 0, t0) == XTK_ERR_NONE);

	/* A nonfinite heading is rejected and must not count as alive. */
	cmd_init(&c, 60000, (uint32_t) (frame + 1), XTK_OP_HEADING, 0,
		 1.0 / 0.0, 0.0);
	make_batch(&m, server_session_epoch(A), 1, &c, 1);
	check("invalid input acknowledged without extending liveness",
	      server_session_message_ch(A, &m, XTK_CH_INPUT, t0 + 1500) ==
	      XTK_ERR_NONE);

	/* A replayed duplicate must not extend liveness either. */
	cmd_init(&c, 60001, (uint32_t) (frame + 1), XTK_OP_DRIVE_REL,
		 XTK_FLAG_HELD, 3.0, 0.0);
	make_batch(&m, server_session_epoch(A), 2, &c, 1);
	check("valid input admitted",
	      server_session_message_ch(A, &m, XTK_CH_INPUT, t0 + 1600) ==
	      XTK_ERR_NONE);
	make_batch(&m, server_session_epoch(A), 3, &c, 1);
	check("replayed duplicate admitted as no-op",
	      server_session_message_ch(A, &m, XTK_CH_INPUT, t0 + 1700) ==
	      XTK_ERR_NONE);

	/* Last genuine fresh traffic was at t0+1600; 2,001 ms after it (== the
	   2000 ms boundary relative to t0+1601) the peer must be removed. */
	check("stale/invalid traffic did not extend the timeout",
	      server_session_step(A, t0 + 1600 + XTK_NEUTRAL_TIMEOUT_MS + 1) ==
	      GAME_OVER);
	teardown_session();
}

/* ----------------------------------------------------------------------- */

int
main(int argc, char *argv[])
{
	g_assets = (argc > 1) ? argv[1] : ".";

	if (server_bringup() != 0)
		return 2;

	section_fairness();
	section_identity_and_validation();
	section_timeout();

	check("no Terminal was ever allocated", n_make_terminal == 0);
	check("no set_terminal call", n_set_terminal == 0);
	check("no display_terminal render call", n_display_terminal == 0);
	check("no get_events input call", n_get_events == 0);
	check("no sync_terminals call", n_sync_terminals == 0);

	if (failures != 0) {
		printf("ADMISSION FIXTURE FAIL (%d/%d checks)\n", failures, checks);
		return 1;
	}
	printf("ADMISSION FIXTURE PASS (checks=%d; per-peer quotas within the "
	       "64/128 bounds, cross-peer fairness, both-departure reserve, "
	       "dedup/channel/epoch/ownership, 2000 ms timeout boundary)\n",
	       checks);
	return 0;
}
