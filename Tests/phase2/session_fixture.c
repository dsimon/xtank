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
 * session_fixture.c -- P2.8 solo lobby / network-server acceptance.
 *
 * Two modes, matching PHASE2-PLAN P2.8 / docs/PROTOCOL-v1.md section 9:
 *
 *   state    drives the session state machine directly (no transport):
 *            handshake and content-digest rejection before any match,
 *            second-peer rejection, allowlisted CONFIG_SELECT validation,
 *            start/reset administration, no controls outside Running,
 *            completed vs aborted results, the two-second timeout abort and
 *            repeated results/reset ownership.
 *
 *   network  exercises the real ENet transport through the production
 *            server_session_service() path over loopback: HELLO/WELCOME/
 *            CONFIG/READY/START, a few ticks of heartbeat + snapshot/ack,
 *            a full win with RESULTS, RESET into a second epoch and QUIT.
 *
 * It links the shipped display-free server core (Src/sobj minus server_main.o)
 * plus Src/server_session.c, Src/server_net.c, the pinned ENet transport and
 * the frozen P2.3 codec.
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
make_config(XtkMessage *m, uint16_t maze, uint16_t veh)
{
	(void) memset(m, 0, sizeof(*m));
	m->env.message_type = XTK_MSG_CONFIG_SELECT;
	m->body.config_select.maze_id = maze;
	m->body.config_select.vehicle_id = veh;
}

static void
make_ready(XtkMessage *m)
{
	(void) memset(m, 0, sizeof(*m));
	m->env.message_type = XTK_MSG_READY;
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
	(void) strncpy(username, "p2.8", MAX_STRING - 1);
	username[MAX_STRING - 1] = '\0';

	if (content_set_root(g_assets) != 0) {
		fprintf(stderr, "session_fixture: %s\n", content_last_error());
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

/* A fresh session in Handshake with a matching HELLO already acknowledged. */
static ServerSession *
session_open_lobby(uint64_t t)
{
	ServerSession *s;
	XtkMessage hello;

	server_session_init();
	s = server_session_accept((TransportPeer *) NULL);
	if (s == NULL)
		return (ServerSession *) NULL;
	make_hello(&hello, "humanA", xtk_content_set_digest);
	(void) server_session_message(s, &hello, t);
	return s;
}

/* ---- state mode ------------------------------------------------------- */

static int
run_state(void)
{
	uint64_t t0 = now_ms();
	ServerSession *s;

	fprintf(stderr, "== handshake / content identity\n");
	{
		XtkMessage hello, rep;
		uint8_t wrong[XTK_SHA256_BYTES];

		/* A mismatched content digest is rejected before any catalog/match. */
		server_session_init();
		s = server_session_accept((TransportPeer *) NULL);
		check("first peer accepted", s != NULL);
		check("state is Handshake",
		      server_session_state(s) == SERVER_SESSION_HANDSHAKE);
		check("first peer is admin", server_session_is_admin(s) == 1);

		(void) memcpy(wrong, xtk_content_set_digest, XTK_SHA256_BYTES);
		wrong[0] ^= 0xFF;
		make_hello(&hello, "bad", wrong);
		check("mismatched handshake rejected",
		      server_session_message(s, &hello, t0) ==
		      XTK_ERR_CONTENT_MISMATCH);
		check("mismatch leaves the session Disconnected",
		      server_session_state(s) == SERVER_SESSION_DISCONNECTED);
		check("no catalog/START before a valid handshake",
		      find_reply(s, XTK_MSG_WELCOME, &rep) == 0 &&
		      find_reply(s, XTK_MSG_START, &rep) == 0);
		check("no match started", frame == 0 && num_combatants == 0);
		check("REJECT carried CONTENT_MISMATCH",
		      find_reply(s, XTK_MSG_REJECT, &rep) == 1 &&
		      rep.body.reject.error_code == XTK_ERR_CONTENT_MISMATCH);
		server_session_destroy(s);

		/* A version mismatch is rejected too (before any state effect). */
		server_session_init();
		s = server_session_accept((TransportPeer *) NULL);
		make_hello(&hello, "old", xtk_content_set_digest);
		hello.body.hello.proto_major = 2;
		check("version mismatch rejected",
		      server_session_message(s, &hello, t0) ==
		      XTK_ERR_VERSION_MISMATCH);
		check("version mismatch REJECT",
		      find_reply(s, XTK_MSG_REJECT, &rep) == 1 &&
		      rep.body.reject.error_code == XTK_ERR_VERSION_MISMATCH);
		server_session_destroy(s);

		/* Second peer is rejected outright. */
		server_session_init();
		s = server_session_accept((TransportPeer *) NULL);
		check("second peer rejected in the one-peer slice",
		      server_session_accept((TransportPeer *) NULL) == NULL);
		check("only one live session", server_session_count() == 1);
		make_hello(&hello, "humanA", xtk_content_set_digest);
		(void) server_session_message(s, &hello, t0);
		check("valid handshake -> Lobby",
		      server_session_state(s) == SERVER_SESSION_LOBBY);
		check("WELCOME delivered",
		      find_reply(s, XTK_MSG_WELCOME, &rep) == 1);
		check("CONTENT_CATALOG delivered with the content digest",
		      find_reply(s, XTK_MSG_CONTENT_CATALOG, &rep) == 1 &&
		      memcmp(rep.body.catalog.content_set_digest,
			     xtk_content_set_digest, XTK_SHA256_BYTES) == 0);
	}

	fprintf(stderr, "== lobby config / no controls outside Running\n");
	{
		XtkMessage cfg, rep, hb;

		/* Controls are ignored outside Running. */
		make_heartbeat(&hb, 0, 1, 1);
		check("input outside Running has no effect",
		      server_session_message(s, &hb, t0) == XTK_ERR_BAD_STATE);
		check("input outside Running is a BAD_STATE protocol error",
		      find_reply(s, XTK_MSG_PROTOCOL_ERROR, &rep) == 1);

		/* Unknown content id is refused. */
		make_config(&cfg, 0x7F7F, XTK_CONTENT_ID_VANGUARD);
		(void) server_session_message(s, &cfg, t0);
		check("unknown maze id refused",
		      find_reply(s, XTK_MSG_CONFIG_STATUS, &rep) == 1 &&
		      rep.body.config_status.status == 1 &&
		      rep.body.config_status.error_code == XTK_ERR_BAD_CONTENT_ID);
		check("config not accepted after a bad id",
		      server_session_config_valid(s) == 0);

		/* Allowlisted selection is accepted. */
		server_session_reply_clear(s);
		make_config(&cfg, XTK_CONTENT_ID_COMBAT1, XTK_CONTENT_ID_VANGUARD);
		(void) server_session_message(s, &cfg, t0);
		check("combat1 + Vanguard accepted",
		      find_reply(s, XTK_MSG_CONFIG_STATUS, &rep) == 1 &&
		      rep.body.config_status.status == 0);
		check("config is now valid", server_session_config_valid(s) == 1);
		check("cannot start before READY",
		      server_session_can_start(s) == 0);

		/* READY then start. */
		make_ready(&cfg);
		(void) server_session_message(s, &cfg, t0);
		check("READY sets ready", server_session_ready(s) == 1);
		check("admin + config + ready can start",
		      server_session_can_start(s) == 1);

		/* A non-admin cannot start. */
		server_session_set_admin(s, 0);
		check("non-admin cannot begin", server_session_begin(
		      s, (uint32_t) (frame + 1), t0) ==
		      SERVER_SESSION_ERR_NOT_ADMIN);
		server_session_set_admin(s, 1);
	}

	fprintf(stderr, "== start / snapshot / event delivery\n");
	{
		XtkMessage rep;
		int k;

		server_session_reply_clear(s);
		check("begin succeeds", server_session_begin(
		      s, (uint32_t) (frame + 1), t0) == SERVER_SESSION_OK);
		check("state is Running",
		      server_session_state(s) == SERVER_SESSION_RUNNING);
		check("epoch 1", server_session_epoch(s) == 1);
		check("bound to slot 0", server_session_bound_slot(s) == 0);
		check("START delivered",
		      find_reply(s, XTK_MSG_START, &rep) == 1);
		check("EPOCH_BEGIN delivered",
		      find_reply(s, XTK_MSG_EPOCH_BEGIN, &rep) == 1 &&
		      rep.body.epoch_begin.epoch == 1);

		for (k = 0; k < 20; k++)
			(void) server_session_step(s, t0 + (uint64_t) (k + 1) * 60);
		check("engine advanced", frame >= 20);
		check("authorized snapshots were delivered",
		      server_session_snapshot_seq(s) >= 1);

		/* Inside Running a heartbeat batch is admitted without effect. */
		{
			XtkMessage hb;
			uint32_t before = intent_queued();

			make_heartbeat(&hb, 1, 1, 777);
			check("heartbeat admitted inside Running",
			      server_session_message(s, &hb, t0) ==
			      XTK_ERR_NONE &&
			      intent_queued() == before);
		}
	}

	fprintf(stderr, "== completed match: RESULTS + RESET (epoch 2)\n");
	{
		XtkMessage rep, rst, hb;
		int status = GAME_RUNNING, k;
		uint64_t now = t0;

		combatant[0].score = settings.si.winning_score;
		for (k = 0; k < 100000 && status == GAME_RUNNING; k++) {
			/* Model an active client: valid traffic each tick keeps the
			 * two-second solo timeout from firing during the match. */
			now += 66;
			make_heartbeat(&hb, 1, (uint32_t) k + 1,
				       (uint32_t) 5000 + k);
			(void) server_session_message(s, &hb, now);
			status = server_session_step(s, now);
		}
		check("completed match reaches GAME_RESET", status == GAME_RESET);
		check("state is Results",
		      server_session_state(s) == SERVER_SESSION_RESULTS);
		check("win RESULTS delivered (result_kind 0)",
		      find_reply(s, XTK_MSG_RESULTS, &rep) == 1 &&
		      rep.body.results.result_kind == 0 &&
		      rep.body.results.winner_slot == 0);

		/* Only the admin resets; a wrong prior epoch is refused. */
		server_session_reply_clear(s);
		make_reset(&rst, 99);
		(void) server_session_message(s, &rst, t0);
		check("reset with a stale prior epoch refused",
		      find_reply(s, XTK_MSG_PROTOCOL_ERROR, &rep) == 1 &&
		      rep.body.protocol_error.error_code == XTK_ERR_STALE_EPOCH);
		check("still in Results after a rejected reset",
		      server_session_state(s) == SERVER_SESSION_RESULTS);

		server_session_reply_clear(s);
		make_reset(&rst, 1);
		(void) server_session_message(s, &rst, t0);
		check("admin reset starts epoch 2",
		      server_session_state(s) == SERVER_SESSION_RUNNING &&
		      server_session_epoch(s) == 2);
		check("reset re-emitted EPOCH_BEGIN for epoch 2",
		      find_reply(s, XTK_MSG_EPOCH_BEGIN, &rep) == 1 &&
		      rep.body.epoch_begin.epoch == 2);
		check("ownership is clean (slot 0 again)",
		      server_session_bound_slot(s) == 0);
	}

	fprintf(stderr, "== timeout abort vs completed match\n");
	{
		XtkMessage rep;
		uint32_t before_score;

		/* No traffic for >2s: neutralise, abort (result_kind 1), lobby. */
		server_session_reply_clear(s);
		before_score = combatant[0].score;
		check("timeout aborts the running session",
		      server_session_step(s, now_ms() + XTK_NEUTRAL_TIMEOUT_MS +
				       1000) == GAME_OVER);
		check("abort returns to the Lobby",
		      server_session_state(s) == SERVER_SESSION_LOBBY);
		check("abort RESULTS delivered (result_kind 1)",
		      find_reply(s, XTK_MSG_RESULTS, &rep) == 1 &&
		      rep.body.results.result_kind == 1);
		check("abort awards no score",
		      combatant[0].score == before_score);

		/* Window is exactly the approved two seconds. */
		check("timeout threshold is 2000 ms",
		      XTK_NEUTRAL_TIMEOUT_MS == 2000u);
	}

	fprintf(stderr, "== repeated reset / clean ownership after abort\n");
	{
		XtkMessage cfg, rst, rep;

		make_config(&cfg, XTK_CONTENT_ID_ARENA, XTK_CONTENT_ID_VANGUARD);
		(void) server_session_message(s, &cfg, t0);
		check("Arena selected", server_session_config_valid(s) == 1 &&
		      server_session_maze_id(s) == XTK_CONTENT_ID_ARENA);
		make_ready(&cfg);
		(void) server_session_message(s, &cfg, t0);
		check("can start again after abort",
		      server_session_can_start(s) == 1);
		check("second start succeeds", server_session_begin(
		      s, (uint32_t) (frame + 1), t0) == SERVER_SESSION_OK);
		check("epoch 3", server_session_epoch(s) == 3 &&
		      server_session_state(s) == SERVER_SESSION_RUNNING);

		server_session_reply_clear(s);
		make_reset(&rst, 3);
		(void) server_session_message(s, &rst, t0);
		check("reset to epoch 4",
		      server_session_epoch(s) == 4 &&
		      server_session_state(s) == SERVER_SESSION_RUNNING);
		check("still owns slot 0", server_session_bound_slot(s) == 0);

		/* QUIT drops the session cleanly. */
		{
			XtkMessage quit;

			make_quit(&quit);
			(void) server_session_message(s, &quit, t0);
			check("QUIT disconnects",
			      server_session_state(s) ==
			      SERVER_SESSION_DISCONNECTED);
			server_session_destroy(s);
			check("session table empty", server_session_count() == 0);
		}
		(void) rep;
	}

	return failures ? 1 : 0;
}

/* ---- network mode ----------------------------------------------------- */

static int
client_recv(Transport *client, TransportPeer *cp, XtkMessage *out, int *got_type)
{
	int i;

	for (i = 0; i < 2000; i++) {
		uint8_t buf[XTK_MAX_MESSAGE_BYTES];
		size_t len = 0;
		uint8_t ch = 0;
		TransportPeer *p;
		int type;

		(void) transport_service(client, 64);
		if (transport_receive(client, &p, buf, sizeof(buf), &len, &ch) == 1) {
			if (xtk_decode(buf, len, out) == XTK_ERR_NONE) {
				type = out->env.message_type;
				if (got_type != NULL)
					*got_type = type;
				return type;
			}
		}
		/* Let the server make progress too. */
		(void) server_session_service(now_ms());
	}
	return -1;
}

static int
run_network(void)
{
	Transport *host, *client;
	TransportPeer *cp = (TransportPeer *) NULL, *sp = (TransportPeer *) NULL;
	uint16_t port;
	XtkMessage hello, cfg, m;
	ServerSession *s;
	int i, type;

	server_session_init();
	host = transport_create(0, 1);
	check("listen transport created", host != NULL);
	if (host == NULL)
		return 1;
	port = transport_port(host);
	server_session_set_transport(host);

	client = transport_create(0, 1);
	cp = transport_connect(client, "127.0.0.1", port);
	check("client connect initiated", cp != NULL);

	for (i = 0; i < 5000 && server_session_current() == NULL; i++) {
		(void) server_session_service(now_ms());
		(void) transport_service(client, 64);
	}
	s = server_session_current();
	check("server accepted the connection", s != NULL);
	sp = server_session_peer(s);
	check("session in Handshake",
	      s != NULL && server_session_state(s) == SERVER_SESSION_HANDSHAKE);

	make_hello(&hello, "netA", xtk_content_set_digest);
	{
		uint8_t buf[XTK_MAX_MESSAGE_BYTES];
		size_t len = 0;

		check("HELLO encodes",
		      xtk_encode(buf, sizeof(buf), &hello, &len) ==
		      XTK_ERR_NONE);
		check("HELLO sends",
		      transport_send(client, cp, XTK_CH_CONTROL, buf, len) == 0);
	}
	check("WELCOME received", client_recv(client, cp, &m, &type) ==
	      XTK_MSG_WELCOME || type == XTK_MSG_CONTENT_CATALOG);
	check("WELCOME/CATALOG delivered",
	      find_reply(s, XTK_MSG_WELCOME, &m) == 1 ||
	      find_reply(s, XTK_MSG_CONTENT_CATALOG, &m) == 1);

	make_config(&cfg, XTK_CONTENT_ID_COMBAT1, XTK_CONTENT_ID_VANGUARD);
	{
		uint8_t buf[XTK_MAX_MESSAGE_BYTES];
		size_t len = 0;

		(void) xtk_encode(buf, sizeof(buf), &cfg, &len);
		(void) transport_send(client, cp, XTK_CH_CONTROL, buf, len);
	}
	/* The client queue may still hold WELCOME/CATALOG; drain until the
	 * server's CONFIG_STATUS arrives (or the config is simply accepted). */
	type = -1;
	for (i = 0; i < 2000 && type != XTK_MSG_CONFIG_STATUS &&
	     server_session_config_valid(s) == 0; i++)
		type = client_recv(client, cp, &m, &type);
	check("CONFIG_STATUS accepted",
	      type == XTK_MSG_CONFIG_STATUS ||
	      server_session_config_valid(s) == 1);

	{
		XtkMessage ready;
		uint8_t buf[XTK_MAX_MESSAGE_BYTES];
		size_t len = 0;

		make_ready(&ready);
		(void) xtk_encode(buf, sizeof(buf), &ready, &len);
		(void) transport_send(client, cp, XTK_CH_CONTROL, buf, len);
	}

	/* Service until the server auto-starts and delivers START/EPOCH_BEGIN. */
	for (i = 0; i < 5000 &&
	     server_session_state(s) != SERVER_SESSION_RUNNING; i++)
		(void) server_session_service(now_ms());
	check("server started the match over the wire",
	      server_session_state(s) == SERVER_SESSION_RUNNING);
	check("START delivered over the wire",
	      find_reply(s, XTK_MSG_START, &m) == 1);

	/* A few ticks with a heartbeat each; expect snapshots and acks. */
	{
		int snaps = 0, acks = 0, events = 0;

		for (i = 0; i < 60; i++) {
			XtkMessage hb;
			uint8_t buf[XTK_MAX_MESSAGE_BYTES];
			size_t len = 0;

			make_heartbeat(&hb, server_session_epoch(s),
				       (uint32_t) i + 1, (uint32_t) 1000 + i);
			(void) xtk_encode(buf, sizeof(buf), &hb, &len);
			(void) transport_send(client, cp, XTK_CH_INPUT, buf, len);

			(void) server_session_service(now_ms());
			if (server_session_state(s) == SERVER_SESSION_RUNNING)
				(void) server_session_step(s, now_ms());

			for (;;) {
				uint8_t rbuf[XTK_MAX_MESSAGE_BYTES];
				size_t rlen = 0;
				uint8_t ch = 0;
				XtkMessage rm;
				TransportPeer *p;

				(void) transport_service(client, 64);
				if (transport_receive(client, &p, rbuf,
						      sizeof(rbuf), &rlen, &ch) != 1)
					break;
				if (xtk_decode(rbuf, rlen, &rm) != XTK_ERR_NONE)
					continue;
				if (rm.env.message_type == XTK_MSG_VIEW_SNAPSHOT)
					snaps++;
				else if (rm.env.message_type == XTK_MSG_CMD_ACK)
					acks++;
				else if (rm.env.message_type == XTK_MSG_EVENT)
					events++;
			}
		}
		check("view snapshots received over channel 1", snaps > 0);
		check("command acknowledgments received", acks > 0);
		(void) events;
	}

	/* QUIT cleanly ends the session. */
	{
		XtkMessage quit;
		uint8_t buf[XTK_MAX_MESSAGE_BYTES];
		size_t len = 0;

		make_quit(&quit);
		(void) xtk_encode(buf, sizeof(buf), &quit, &len);
		(void) transport_send(client, cp, XTK_CH_CONTROL, buf, len);
		for (i = 0; i < 2000 &&
		     server_session_count() > 0; i++)
			(void) server_session_service(now_ms());
		check("QUIT ended the session", server_session_count() == 0);
	}

	transport_destroy(client);
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

	if (!strcmp(mode, "network")) {
		(void) run_network();
		if (failures != 0) {
			printf("SESSION FIXTURE FAIL (%d checks, network)\n",
			       failures);
			return 1;
		}
		printf("SESSION FIXTURE PASS (network: ENet handshake/lobby/start/"
		       "snapshots/acks/quit)\n");
		return 0;
	}

	(void) run_state();
	if (failures != 0) {
		printf("SESSION FIXTURE FAIL (%d checks, state)\n", failures);
		return 1;
	}
	printf("SESSION FIXTURE PASS (state: handshake+content, second-peer, "
	       "allowlist, admin start/reset, no-controls-outside-Running, "
	       "completed vs abort results, timeout, repeated reset)\n");
	return 0;
}