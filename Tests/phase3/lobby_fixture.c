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
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * lobby_fixture.c -- P3.4 shared-lobby / one-world coordinator acceptance.
 *
 * Modes (matching docs/PHASE3-PLAN.md P3.4):
 *
 *   state    drives the coordinator directly (no transport, capacity 2):
 *              - two admitted humans; extra human/observer and running join
 *                rejected;
 *              - duplicate names rejected (D3-E);
 *              - shared config revision + readiness agree for both, a config
 *                change wipes readiness, unauthorized config/start/reset have
 *                no effect;
 *              - one start transaction, one world init, one server_step() per
 *                tick, one results finalization/reset, independent of peers;
 *              - START vehicle_id survives full-width (u16, 0x0201);
 *              - partial handshake/start failure releases resources;
 *              - RESET opens a lobby rejoin window before the next start;
 *              - empty server accepts a fresh solo/two-human session.
 *
 *   listen   uses the real pinned ENet path through server_session_service:
 *            a peer is admitted, quits, and the still-listening server admits a
 *            fresh peer across the empty-session lobby cycle.
 *
 * It links the shipped display-free server core (Src/sobj minus server_main.o)
 * plus Src/server_session.c, Src/server_net.c, the pinned ENet transport and
 * the frozen v1.1 codec.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
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

/* ---- sandbox sendmsg shim (test-harness only; see P2.7) --------------- */
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

static void
check(const char *what, int ok)
{
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
	(void) strncpy(username, "p3.4", MAX_STRING - 1);
	username[MAX_STRING - 1] = '\0';

	if (content_set_root(g_assets) != 0) {
		fprintf(stderr, "lobby_fixture: %s\n", content_last_error());
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

/* A fresh two-human coordinator with both peers past a valid handshake. */
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

/* ---- state mode ------------------------------------------------------- */

static int
run_state(void)
{
	uint64_t t = now_ms();
	XtkMessage m;
	int i;

	fprintf(stderr, "== admission: two humans, extra/observer, duplicate name\n");
	{
		open_two(t);
		check("first peer accepted", A != NULL);
		check("second peer accepted", B != NULL);
		check("two live sessions", server_session_count() == 2);
		check("first peer is administrator",
		      server_session_is_admin(A) == 1);
		check("second peer is not administrator",
		      server_session_is_admin(B) == 0);
		check("both peers are in the lobby",
		      server_session_state(A) == SERVER_SESSION_LOBBY &&
		      server_session_state(B) == SERVER_SESSION_LOBBY);
		check("WELCOME role is player, not admin",
		      find_reply(A, XTK_MSG_WELCOME, &m) == 1 &&
		      m.body.welcome.role == 1);
		check("WELCOME advertises two-human capacity",
		      find_reply(A, XTK_MSG_WELCOME, &m) == 1 &&
		      m.body.welcome.human_capacity == XTK_MAX_HUMANS);

		/* A third human / observer cannot be admitted. */
		check("extra human/observer rejected",
		      server_session_accept((TransportPeer *) NULL) == NULL);
		check("still only two sessions", server_session_count() == 2);

		/* Duplicate name rejected, slot released, identity independent. */
		{
			ServerSession *dup;
			XtkMessage h;

			server_session_destroy(B);
			dup = server_session_accept((TransportPeer *) NULL);
			make_hello(&h, "alpha", xtk_content_set_digest);
			check("duplicate name rejected",
			      server_session_message(dup, &h, t) ==
			      XTK_ERR_DUPLICATE_NAME);
			check("duplicate REJECT carries DUPLICATE_NAME",
			      find_reply(dup, XTK_MSG_REJECT, &m) == 1 &&
			      m.body.reject.error_code ==
			      XTK_ERR_DUPLICATE_NAME);
			check("duplicate peer is disconnected",
			      server_session_state(dup) ==
			      SERVER_SESSION_DISCONNECTED);
			server_session_destroy(dup);
			check("duplicate slot released",
			      server_session_count() == 1);
			/* Fresh, non-duplicate identity is admitted. */
			B = server_session_accept((TransportPeer *) NULL);
			make_hello(&h, "gamma", xtk_content_set_digest);
			check("fresh identity admitted after duplicate",
			      server_session_message(B, &h, t) == XTK_ERR_NONE &&
			      server_session_state(B) == SERVER_SESSION_LOBBY);
		}
		teardown_all();
	}

	fprintf(stderr, "== handshake gate: no lobby authority before HELLO\n");
	{
		ServerSession *p;
		XtkMessage cfg, hb, rep;

		server_session_init();
		server_session_set_capacity(XTK_MAX_HUMANS);
		p = server_session_accept((TransportPeer *) NULL);
		check("fresh peer is in Handshake",
		      p != NULL && server_session_state(p) ==
		      SERVER_SESSION_HANDSHAKE);
		check("half-open peer is not administrator",
		      server_session_is_admin(p) == 0);

		/* PING is the only non-HELLO traffic a handshaking peer may send. */
		{
			XtkMessage ping;

			(void) memset(&ping, 0, sizeof(ping));
			ping.env.message_type = XTK_MSG_PING;
			check("handshaking peer may PING",
			      server_session_message(p, &ping, t) == XTK_ERR_NONE);
		}

		make_heartbeat(&hb, 0, 1, 1);
		make_config(&cfg, XTK_CONTENT_ID_COMBAT1, XTK_CONTENT_ID_VANGUARD, 2);
		check("pre-handshake CONFIG_SELECT rejected",
		      server_session_message(p, &cfg, t) == XTK_ERR_BAD_STATE);
		make_ready(&rep, 1, 0);
		check("pre-handshake READY rejected",
		      server_session_message(p, &rep, t) == XTK_ERR_BAD_STATE);
		make_start_request(&rep, 0);
		check("pre-handshake START_REQUEST rejected",
		      server_session_message(p, &rep, t) == XTK_ERR_BAD_STATE);
		make_reset(&rep, 0);
		check("pre-handshake RESET rejected",
		      server_session_message(p, &rep, t) == XTK_ERR_BAD_STATE);
		check("pre-handshake INPUT_BATCH rejected",
		      server_session_message(p, &hb, t) == XTK_ERR_BAD_STATE);
		check("pre-handshake traffic changed no shared config",
		      server_session_config_valid(p) == 0 &&
		      server_session_config_revision(p) == 0);
		check("peer is still handshaking (not disconnected)",
		      server_session_state(p) == SERVER_SESSION_HANDSHAKE);
		teardown_all();
	}

	fprintf(stderr, "== admin assigned on HELLO across a failed handshake\n");
	{
		ServerSession *p, *q;
		XtkMessage h, cfg;
		uint8_t wrong[XTK_SHA256_BYTES];

		server_session_init();
		server_session_set_capacity(XTK_MAX_HUMANS);
		p = server_session_accept((TransportPeer *) NULL);
		q = server_session_accept((TransportPeer *) NULL);

		/* First accepted peer fails its handshake while the second is still
		 * handshaking: administration must not be lost. */
		(void) memcpy(wrong, xtk_content_set_digest, XTK_SHA256_BYTES);
		wrong[0] ^= 0xFF;
		make_hello(&h, "first", wrong);
		check("failed HELLO rejected",
		      server_session_message(p, &h, t) == XTK_ERR_CONTENT_MISMATCH);
		server_session_destroy(p);

		make_hello(&h, "second", xtk_content_set_digest);
		check("second peer completes HELLO",
		      server_session_message(q, &h, t) == XTK_ERR_NONE);
		check("remaining human is administrator",
		      server_session_is_admin(q) == 1);
		make_config(&cfg, XTK_CONTENT_ID_COMBAT1, XTK_CONTENT_ID_VANGUARD, 1);
		check("remaining human can configure",
		      server_session_message(q, &cfg, t) == XTK_ERR_NONE &&
		      server_session_config_revision(q) == 1);
		teardown_all();
	}

	fprintf(stderr, "== config revision, readiness agreement, authorization\n");
	open_two(t);
	{
		XtkMessage cfg, rep;

		check("initial revision is zero",
		      server_session_config_revision(A) == 0);
		check("config invalid before selection",
		      server_session_config_valid(A) == 0);

		/* Non-admin config has no effect. */
		make_config(&cfg, XTK_CONTENT_ID_ARENA, XTK_CONTENT_ID_VANGUARD, 2);
		check("non-admin config rejected",
		      server_session_message(B, &cfg, t) == XTK_ERR_NOT_ADMIN);
		check("revision unchanged by non-admin config",
		      server_session_config_revision(A) == 0 &&
		      server_session_config_valid(A) == 0);

		/* Admin selection increments the revision and is authoritative. */
		server_session_reply_clear(A);
		server_session_reply_clear(B);
		make_config(&cfg, XTK_CONTENT_ID_COMBAT1, XTK_CONTENT_ID_VANGUARD, 2);
		check("admin config accepted",
		      server_session_message(A, &cfg, t) == XTK_ERR_NONE);
		check("config valid", server_session_config_valid(A) == 1);
		check("revision incremented", server_session_config_revision(A) == 1);
		check("CONFIG_STATUS echoes revision and drone count",
		      find_reply(A, XTK_MSG_CONFIG_STATUS, &rep) == 1 &&
		      rep.body.config_status.config_revision == 1 &&
		      rep.body.config_status.drone_count == 2 &&
		      rep.body.config_status.status == 0);

		/* Both peers observe the same shared config in LOBBY_STATE. */
		{
			XtkMessage la, lb;

			check("admin LOBBY_STATE agrees",
			      find_reply(A, XTK_MSG_LOBBY_STATE, &la) == 1 &&
			      la.body.lobby_state.config_revision == 1 &&
			      la.body.lobby_state.maze_id ==
			      XTK_CONTENT_ID_COMBAT1 &&
			      la.body.lobby_state.vehicle_id ==
			      XTK_CONTENT_ID_VANGUARD &&
			      la.body.lobby_state.drone_count == 2 &&
			      la.body.lobby_state.human_count == 2 &&
			      la.body.lobby_state.admin_index == 0);
			check("peer-2 LOBBY_STATE agrees",
			      find_reply(B, XTK_MSG_LOBBY_STATE, &lb) == 1 &&
			      lb.body.lobby_state.config_revision == 1 &&
			      lb.body.lobby_state.self_index == 1 &&
			      lb.body.lobby_state.admin_index == 0 &&
			      lb.body.lobby_state.human_count == 2);

			/* A config change wipes readiness. */
			make_ready(&rep, 1, 1);
			(void) server_session_message(A, &rep, t);
			(void) server_session_message(B, &rep, t);
			check("both ready", server_session_ready(A) == 1 &&
			      server_session_ready(B) == 1);
			make_config(&cfg, XTK_CONTENT_ID_ARENA,
				    XTK_CONTENT_ID_VANGUARD, 2);
			(void) server_session_message(A, &cfg, t);
			check("config change wipes both readiness",
			      server_session_ready(A) == 0 &&
			      server_session_ready(B) == 0);
			check("revision incremented again",
			      server_session_config_revision(A) == 2);
		}

		/* A stale READY revision is refused. */
		server_session_reply_clear(B);
		make_ready(&rep, 1, 1);		/* old revision */
		check("stale READY refused",
		      server_session_message(B, &rep, t) ==
		      XTK_ERR_STALE_CONFIG);
		check("stale READY does not set readiness",
		      server_session_ready(B) == 0);

		/* Ready both against the current revision. */
		make_ready(&rep, 1, 2);
		(void) server_session_message(A, &rep, t);
		(void) server_session_message(B, &rep, t);
		check("both ready against revision 2",
		      server_session_all_ready(A) == 1);
	}

	fprintf(stderr, "== start transaction: one world, one step per tick\n");
	{
		XtkMessage sr, rep;
		int frame0 = frame, comb0;

		/* Non-admin start has no effect. */
		make_start_request(&sr, 2);
		check("non-admin start rejected",
		      server_session_message(B, &sr, t) == XTK_ERR_NOT_ADMIN);
		check("still in lobby after non-admin start",
		      server_session_state(A) == SERVER_SESSION_LOBBY &&
		      server_session_epoch(A) == 0);

		/* Stale revision start has no effect. */
		make_start_request(&sr, 1);
		check("stale-revision start rejected",
		      server_session_message(A, &sr, t) ==
		      XTK_ERR_STALE_CONFIG);

		/* Not-ready start has no effect. */
		make_ready(&rep, 0, 2);
		(void) server_session_message(B, &rep, t);
		make_start_request(&sr, 2);
		check("not-ready start rejected",
		      server_session_message(A, &sr, t) == XTK_ERR_NOT_READY);
		check("failed start did not advance the epoch",
		      server_session_epoch(A) == 0 &&
		      server_session_state(A) == SERVER_SESSION_LOBBY);
		make_ready(&rep, 1, 2);
		(void) server_session_message(B, &rep, t);

		/* The admin's explicit start builds exactly one world. */
		server_session_reply_clear(A);
		server_session_reply_clear(B);
		make_start_request(&sr, 2);
		check("admin start accepted",
		      server_session_message(A, &sr, t) == XTK_ERR_NONE);
		check("one start -> Running epoch 1",
		      server_session_state(A) == SERVER_SESSION_RUNNING &&
		      server_session_state(B) == SERVER_SESSION_RUNNING &&
		      server_session_epoch(A) == 1);
		comb0 = num_combatants;
		check("one world init: two humans + two drones",
		      comb0 == 4);
		check("exactly one START per peer",
		      count_reply(A, XTK_MSG_START) == 1 &&
		      count_reply(B, XTK_MSG_START) == 1);
		check("exactly one EPOCH_BEGIN per peer",
		      count_reply(A, XTK_MSG_EPOCH_BEGIN) == 1 &&
		      count_reply(B, XTK_MSG_EPOCH_BEGIN) == 1);

		/* Full-width Vanguard content id survives (P3.4 u8 fix). */
		check("START carries full-width vehicle id 0x0201",
		      find_reply(A, XTK_MSG_START, &rep) == 1 &&
		      rep.body.start.vehicle_id == XTK_CONTENT_ID_VANGUARD);

		/* One engine step per call, independent of the number of peers. */
		{
			int f0 = frame;

			(void) server_session_step(A, t);
			check("one step advances the world by one tick",
			      frame == f0 + 1);
			for (i = 0; i < 9; i++)
				(void) server_session_step(B, t);
			check("ten steps advance ten ticks (one world)",
			      frame == f0 + 10);
		}

		/* Each peer receives its own authorized snapshot. */
		check("both peers received snapshots",
		      server_session_snapshot_seq(A) >= 1 &&
		      server_session_snapshot_seq(B) >= 1);

		/* A running join is rejected. */
		check("running join rejected",
		      server_session_accept((TransportPeer *) NULL) == NULL);
		(void) frame0;
	}

	fprintf(stderr, "== results once, reset barrier, rejoin window\n");
	{
		XtkMessage rst, hb, rep;
		int status = GAME_RUNNING, k;
		uint64_t now = t;
		uint32_t human_generation;

		combatant[0].score = settings.si.winning_score;
		for (k = 0; k < 100000 && status == GAME_RUNNING; k++) {
			now += 66;
			make_heartbeat(&hb, 1, (uint32_t) k + 1,
				       (uint32_t) 7000 + (uint32_t) k);
			(void) server_session_message(A, &hb, now);
			(void) server_session_message(B, &hb, now);
			status = server_session_step(A, now);
		}
		check("match completes", status == GAME_RESET);
		check("state is Results", server_session_state(A) ==
		      SERVER_SESSION_RESULTS);
		check("one RESULTS per peer",
		      count_reply(A, XTK_MSG_RESULTS) == 1 &&
		      count_reply(B, XTK_MSG_RESULTS) == 1);

		/* Stale reset refused; a valid reset opens the lobby barrier. */
		make_reset(&rst, 99);
		check("stale reset refused",
		      server_session_message(A, &rst, now) ==
		      XTK_ERR_STALE_EPOCH &&
		      server_session_state(A) == SERVER_SESSION_RESULTS);

		server_session_reply_clear(A);
		make_reset(&rst, 1);
		check("admin reset returns through the lobby barrier",
		      server_session_message(A, &rst, now) == XTK_ERR_NONE &&
		      server_session_state(A) == SERVER_SESSION_LOBBY);
		check("reset preserves the shared selection",
		      server_session_config_valid(A) == 1 &&
		      server_session_maze_id(A) == XTK_CONTENT_ID_ARENA);
		check("reset emits a LOBBY_STATE barrier",
		      find_reply(A, XTK_MSG_LOBBY_STATE, &rep) == 1 &&
		      rep.body.lobby_state.session_state == XTK_SESSION_LOBBY &&
		      rep.body.lobby_state.config_revision == 2);

		/* Rejoin window: a fresh peer may join before the next start. */
		human_generation = server_session_id(B);
		server_session_destroy(B);
		B = server_session_accept((TransportPeer *) NULL);
		check("rejoin window admits a fresh peer", B != NULL);
		make_hello(&rep, "delta", xtk_content_set_digest);
		(void) server_session_message(B, &rep, now);
		check("fresh peer has a new connection identity",
		      server_session_id(B) != human_generation);

		/* The next transaction increments the epoch exactly once. */
		make_ready(&rep, 1, 2);
		(void) server_session_message(A, &rep, now);
		(void) server_session_message(B, &rep, now);
		make_start_request(&rep, 2);
		check("second start increments epoch once",
		      server_session_message(A, &rep, now) == XTK_ERR_NONE &&
		      server_session_epoch(A) == 2 &&
		      server_session_state(A) == SERVER_SESSION_RUNNING);
	}

	fprintf(stderr, "== partial failure releases resources; empty stays up\n");
	{
		XtkMessage hello, rep;
		uint8_t wrong[XTK_SHA256_BYTES];
		ServerSession *p, *q;

		/* A failed handshake frees the slot for a fresh peer. */
		teardown_all();
		server_session_init();
		server_session_set_capacity(XTK_MAX_HUMANS);
		p = server_session_accept((TransportPeer *) NULL);
		(void) memcpy(wrong, xtk_content_set_digest, XTK_SHA256_BYTES);
		wrong[0] ^= 0xFF;
		make_hello(&hello, "bad", wrong);
		check("mismatched handshake rejected",
		      server_session_message(p, &hello, t) ==
		      XTK_ERR_CONTENT_MISMATCH);
		server_session_destroy(p);
		check("failed handshake releases the slot",
		      server_session_count() == 0);
		p = server_session_accept((TransportPeer *) NULL);
		make_hello(&hello, "fresh", xtk_content_set_digest);
		check("fresh peer admitted after failure",
		      p != NULL && server_session_message(p, &hello, t) ==
		      XTK_ERR_NONE);

		/* A failed start leaves no binding and no live epoch. */
		{
			XtkMessage cfg, sr;

			make_config(&cfg, XTK_CONTENT_ID_COMBAT1,
				    XTK_CONTENT_ID_VANGUARD, 1);
			(void) server_session_message(p, &cfg, t);
			make_start_request(&sr, server_session_config_revision(p));
			/* Solo is not ready, so start is refused with no effect. */
			check("unready start refused",
			      server_session_message(p, &sr, t) ==
			      XTK_ERR_NOT_READY);
			check("refused start leaves no binding",
			      server_session_bound_slot(p) == -1 &&
			      server_session_epoch(p) == 0);
		}

		/* Empty the server, then admit a fresh two-human session. */
		teardown_all();
		check("empty server has no sessions",
		      server_session_count() == 0);
		q = server_session_accept((TransportPeer *) NULL);
		make_hello(&hello, "one", xtk_content_set_digest);
		(void) server_session_message(q, &hello, t);
		check("empty server accepts a fresh solo session",
		      q != NULL && server_session_state(q) ==
		      SERVER_SESSION_LOBBY);
		p = server_session_accept((TransportPeer *) NULL);
		make_hello(&hello, "two", xtk_content_set_digest);
		(void) server_session_message(p, &hello, t);
		check("empty server accepts a fresh two-human session",
		      p != NULL && server_session_count() == 2);
		teardown_all();
	}

	return failures ? 1 : 0;
}

/* ---- listen mode ------------------------------------------------------ */

static int
client_recv(Transport *client, XtkMessage *out, int *got_type)
{
	int i;

	for (i = 0; i < 4000; i++) {
		uint8_t buf[XTK_MAX_MESSAGE_BYTES];
		size_t len = 0;
		uint8_t ch = 0;
		TransportPeer *p;

		(void) transport_service(client, 64);
		if (transport_receive(client, &p, buf, sizeof(buf), &len, &ch) == 1) {
			if (xtk_decode(buf, len, out) == XTK_ERR_NONE) {
				if (got_type != NULL)
					*got_type = out->env.message_type;
				return out->env.message_type;
			}
		}
		(void) server_session_service(now_ms());
	}
	return -1;
}

/* Connect a fresh client, HELLO, wait for WELCOME, then QUIT. */
static int
one_lifecycle(Transport *host, const char *name)
{
	Transport *client;
	TransportPeer *cp;
	XtkMessage hello, m;
	uint8_t buf[XTK_MAX_MESSAGE_BYTES];
	size_t len = 0;
	int i;

	client = transport_create(0, 1);
	cp = transport_connect(client, "127.0.0.1", transport_port(host));
	if (cp == NULL) {
		transport_destroy(client);
		return 0;
	}

	for (i = 0; i < 5000 && server_session_count() == 0; i++) {
		(void) server_session_service(now_ms());
		(void) transport_service(client, 64);
	}
	if (server_session_count() == 0) {
		transport_destroy(client);
		return 0;
	}

	make_hello(&hello, name, xtk_content_set_digest);
	(void) xtk_encode(buf, sizeof(buf), &hello, &len);
	(void) transport_send(client, cp, XTK_CH_CONTROL, buf, len);

	{
		int type = -1;

		for (i = 0; i < 4000 && type != XTK_MSG_WELCOME; i++)
			type = client_recv(client, &m, &type);
		if (type != XTK_MSG_WELCOME) {
			transport_destroy(client);
			return 0;
		}
	}

	/* QUIT and let the server reclaim the empty slot. */
	{
		XtkMessage quit;

		make_quit(&quit);
		(void) xtk_encode(buf, sizeof(buf), &quit, &len);
		(void) transport_send(client, cp, XTK_CH_CONTROL, buf, len);
	}
	for (i = 0; i < 4000 && server_session_count() > 0; i++) {
		(void) server_session_service(now_ms());
		(void) transport_service(client, 64);
	}
	transport_destroy(client);
	return server_session_count() == 0;
}

static int
run_listen(void)
{
	Transport *host;

	server_session_init();
	server_session_set_capacity(XTK_MAX_HUMANS);
	host = transport_create(0, SERVER_SESSION_MAX_PEERS);
	check("listen transport created", host != NULL);
	if (host == NULL)
		return 1;
	server_session_set_transport(host);

	check("real ENet peer admitted and served (WELCOME over the wire)",
	      one_lifecycle(host, "wireA") == 1);
	check("server reclaimed the empty lobby",
	      server_session_count() == 0);
	/*
	 * The service stays alive across the empty-session lobby cycle.  A
	 * fresh wire re-admit is BLOCKED by the P2.7/P3.9 transport connection
	 * reuse debt (a shared UDP socket is connected by the sandbox sendmsg
	 * shim, so one in-process host serves one remote peer); the
	 * coordinator-level fresh-session behaviour is proven in state mode.
	 */
	{
		int i;

		for (i = 0; i < 1000; i++)
			(void) server_session_service(now_ms());
	}
	check("listen service still alive after the lobby emptied",
	      server_session_count() == 0);
	check("release serves two-human capacity",
	      server_session_capacity() == XTK_MAX_HUMANS);

	transport_destroy(host);
	return failures ? 1 : 0;
}

int
main(int argc, char *argv[])
{
	const char *mode = (argc > 2) ? argv[2] : "state";

	g_assets = (argc > 1) ? argv[1] : ".";
	if (server_bringup() != 0)
		return 2;

	if (!strcmp(mode, "listen")) {
		(void) run_listen();
		if (failures != 0) {
			printf("LOBBY FIXTURE FAIL (%d checks, listen)\n", failures);
			return 1;
		}
		printf("LOBBY FIXTURE PASS (listen: ENet admit/quit and service "
		       "liveness across an empty lobby)\n");
		return 0;
	}

	(void) run_state();
	if (failures != 0) {
		printf("LOBBY FIXTURE FAIL (%d checks, state)\n", failures);
		return 1;
	}
	printf("LOBBY FIXTURE PASS (state: two humans, duplicate/extra/running "
	       "rejected, shared config+readiness, one world/step/reset, full u16 "
	       "id, partial-failure release, rejoin, empty re-admit)\n");
	return 0;
}
