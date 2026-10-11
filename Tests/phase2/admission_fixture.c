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
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 */

/*
 * admission_fixture.c -- P2.7 transport and input-admission acceptance.
 *
 * Two modes:
 *
 *   admission  drives the shipped display-free server core plus the P2.7
 *              admission layer over one terminal-free human (slot 0) and one
 *              drone, feeding decoded INPUT_BATCH / RELIABLE_CMD messages and
 *              asserting: ownership comes from the binding (never the peer),
 *              finite/range validation, stale-epoch and stale-generation
 *              rejection, cross-channel deduplication (no duplicate edge
 *              fire), assigned-tick scheduling and acknowledgments, queue
 *              saturation with a release reserve, lost/repeated/partial
 *              batches, and no-peer inertness.
 *
 *   transport  exercises the real ENet 1.3.18 wrapper over loopback: connect,
 *              reliable and unreliable send/receive, an oversized/partial
 *              payload, and a ten-second paused reader that must neither stall
 *              ticks nor grow the reliable queue without bound.
 *
 * The fixture may manipulate simulation state directly (it is the harness);
 * the admission layer itself is the unit under test.
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
#include "server_net.h"
#include "intent.h"
#include "proto.h"
#include "gr.h"

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

/*
 * Sandbox adaptation (test-harness only).
 *
 * Some CI sandboxes deny sendmsg() with an explicit destination address via
 * seccomp while allowing a send on a connected socket.  ENet always sends with
 * an explicit address, so the linker redirects sendmsg() here: the socket is
 * connected to the destination once and the send is retried with no address.
 * This changes no production code and no wire behaviour; it only selects an
 * equivalent kernel send path.  It is harmless on a normal host.
 */
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

/* ---- wire helpers ----------------------------------------------------- */

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

static uint8_t
ack_status(ServerNetSession *s, uint32_t id, uint32_t *tick)
{
	uint8_t st = 0xFF, rs = 0xFF;

	if (!server_net_session_ack_lookup(s, id, tick, &st, &rs))
		return 0xFF;
	return st;
}

/* ---- admission mode --------------------------------------------------- */

static int
run_admission(const char *assets)
{
	ServerConfig *cfg;
	ServerNetSession *s;
	XtkMessage m, m2;
	XtkCommand c[4];
	uint32_t gen, tick;
	int vdesc_index, mdesc_index, hb, t;
	ServerNetStats st;
	int base;

	bset = &actual_bset;
	eset = &actual_eset;
	InitConfigStruct(&command_options);
	command_options.NoDelay = TRUE;
	command_options.AutoExit = TRUE;
	(void) strncpy(username, "p2.7", MAX_STRING - 1);
	username[MAX_STRING - 1] = '\0';

	if (content_set_root(assets) != 0) {
		fprintf(stderr, "admission_fixture: %s\n", content_last_error());
		return 2;
	}

	cfg = server_config();
	server_config_init(cfg);
	server_config_release(cfg);
	cfg->seed = 114336;
	cfg->seed_set = 1;
	server_init(cfg);
	server_net_init();

	init_prog_descs();
	init_settings();
	server_release_settings(&settings);
	init_threader();
	init_msg_sys();
	init_box_names();
	geometry_order_objects();
	geometry_rotate_objects();

	if (make_vdesc((char *) "Vanguard", &vdesc_index) != DESC_LOADED ||
	    make_mdesc((char *) "combat1", &mdesc_index) != DESC_LOADED) {
		fprintf(stderr, "admission_fixture: cannot load content\n");
		return 2;
	}
	settings.mdesc = &mdesc[mdesc_index];
	settings.si.game = settings.mdesc->type;

	server_players_init();
	hb = server_players_add("humanA", 7);
	if (hb < 0 || server_players_bind_slot(hb, 0) != 0) {
		fprintf(stderr, "admission_fixture: cannot bind human\n");
		return 2;
	}
	gen = server_players_binding(hb)->generation;

	num_combatants = 2;
	combatant[0].num_players = 1;
	combatant[0].player[0] = hb;
	combatant[0].num_programs = 0;
	combatant[0].team = 1 % MAX_TEAMS;
	combatant[0].vdesc = vdesc_index;
	(void) strncpy(combatant[0].name, "humanA", MAX_STRING - 1);
	combatant[0].name[MAX_STRING - 1] = '\0';
	combatant[1].num_players = 0;
	combatant[1].num_programs = 1;
	combatant[1].program[0] = 0;
	combatant[1].team = 2 % MAX_TEAMS;
	combatant[1].vdesc = vdesc_index;
	(void) strncpy(combatant[1].name, "drone", MAX_STRING - 1);
	combatant[1].name[MAX_STRING - 1] = '\0';

	if (server_match_init() != GAME_RUNNING) {
		check("match initializes", 0);
		return 1;
	}
	check("match initializes", 1);

	/* No-peer inertness: an empty net services nothing and leaves state
	   unchanged. */
	{
		int h0 = server_frame();
		int alive0 = num_veh_alive;

		check("no peer: server_net_active() is 0", server_net_active() == 0);
		check("no peer: pre_step is a no-op", server_net_pre_step(1) == 0);
		server_net_post_step(0);
		check("no peer: state unchanged",
		      server_frame() == h0 && num_veh_alive == alive0);
	}

	/* One admission-only session bound to controller 7 -> slot 0. */
	s = server_net_session_create((TransportPeer *) NULL);
	check("session created", s != NULL);
	if (s == NULL)
		return 1;
	check("bind succeeds", server_net_session_bind(s, 7, 0, gen) == 0);
	check("session bound", server_net_session_bound(s) == 1);
	server_net_session_begin_epoch(s, 1, 0);
	check("epoch set", server_net_session_epoch(s) == 1);

	base = server_frame() + 1;

	/* ---- accept + ownership + assigned tick + applied ack ----------- */
	fprintf(stderr, "  -- accept / ownership / ack\n");
	{
		int at = base + 1;

		cmd_init(&c[0], 100, (uint32_t) at, XTK_OP_DRIVE_REL,
			 XTK_FLAG_HELD, 5.0, 0.0);
		make_batch(&m, 1, 1, c, 1);
		check("drive accepted",
		      server_net_admit_message(s, &m, at) == 1);
		check("drive queued at the assigned tick",
		      intent_pending(at) == 1);
		check("accepted ack reports the assigned tick",
		      ack_status(s, 100, &tick) == XTK_ACK_ACCEPTED &&
		      tick == (uint32_t) at);
	}

	/* ---- cross-channel duplicate: ch2 then ch0 ---------------------- */
	fprintf(stderr, "  -- duplicate suppression (no duplicate edge fire)\n");
	{
		int at = base + 2;
		int before;

		cmd_init(&c[0], 200, (uint32_t) at, XTK_OP_FIRE, XTK_FLAG_EDGE,
			 0.0, 0.0);
		make_batch(&m, 1, 2, c, 1);
		check("edge fire accepted",
		      server_net_admit_message(s, &m, at) == 1);
		before = intent_pending(at);
		make_reliable(&m2, 1, c, 1);	/* reliable ch0 companion */
		check("reliable duplicate not applied",
		      server_net_admit_message(s, &m2, at) == 0);
		check("no duplicate edge fire", intent_pending(at) == before);
		check("duplicate ack reason",
		      ack_status(s, 200, &tick) == XTK_ACK_REJECTED);

		/* Same packet_sequence replay is ignored entirely. */
		make_batch(&m2, 1, 2, c, 1);
		check("replayed packet sequence ignored",
		      server_net_admit_message(s, &m2, at) == 0);
	}

	/* ---- ownership mapping: held fire lands on the bound slot -------- */
	fprintf(stderr, "  -- ownership mapping\n");
	{
		int at = base + 3;

		cmd_init(&c[0], 300, (uint32_t) at, XTK_OP_FIRE_HOLD,
			 XTK_FLAG_HELD, 1.0, 0.0);
		make_batch(&m, 1, 3, c, 1);
		(void) server_net_admit_message(s, &m, at);
		(void) intent_consume(at);
		check("held fire applied to the bound slot 0",
		      intent_held_fire(0) == 1);
		check("held fire not applied to slot 1", intent_held_fire(1) == 0);

		/* release */
		cmd_init(&c[0], 301, (uint32_t) at, XTK_OP_FIRE_HOLD,
			 XTK_FLAG_HELD, 0.0, 0.0);
		make_batch(&m, 1, 4, c, 1);
		(void) server_net_admit_message(s, &m, at);
		(void) intent_consume(at);
		check("held fire released", intent_held_fire(0) == 0);
	}

	/* ---- stale generation (spoofed owner) --------------------------- */
	fprintf(stderr, "  -- stale generation\n");
	{
		int at = base + 4;
		int before = intent_queued();

		server_players_unbind_slot(0);	/* bumps the binding generation */
		cmd_init(&c[0], 400, (uint32_t) at, XTK_OP_DRIVE_REL,
			 XTK_FLAG_HELD, 3.0, 0.0);
		make_batch(&m, 1, 5, c, 1);
		check("stale generation rejected",
		      server_net_admit_message(s, &m, at) == 0);
		check("stale generation has no effect",
		      intent_queued() == before);
		(void) server_players_bind_slot(hb, 0);
		gen = server_players_binding(hb)->generation;
		server_net_session_bind(s, 7, 0, gen);
	}

	/* ---- stale epoch ------------------------------------------------- */
	fprintf(stderr, "  -- stale epoch\n");
	{
		int at = base + 5;
		int before = intent_queued();

		cmd_init(&c[0], 500, (uint32_t) at, XTK_OP_DRIVE_REL,
			 XTK_FLAG_HELD, 4.0, 0.0);
		make_batch(&m, 0, 6, c, 1);	/* epoch 0 != session epoch 1 */
		check("stale epoch rejected",
		      server_net_admit_message(s, &m, at) == 0);
		check("stale epoch has no effect", intent_queued() == before);
	}

	/* ---- validation: nonfinite / range / unauthorized ---------------- */
	fprintf(stderr, "  -- finite/range/op validation\n");
	{
		int at = base + 6;

		cmd_init(&c[0], 600, (uint32_t) at, XTK_OP_HEADING, 0, 1.0 / 0.0,
			 0.0);
		make_batch(&m, 1, 7, c, 1);
		check("nonfinite operand rejected",
		      server_net_admit_message(s, &m, at) == 0);

		cmd_init(&c[0], 601, (uint32_t) at, XTK_OP_DRIVE_REL,
			 XTK_FLAG_HELD, 99.0, 0.0);
		make_batch(&m, 1, 8, c, 1);
		check("out-of-range drive rejected",
		      server_net_admit_message(s, &m, at) == 0);

		cmd_init(&c[0], 602, (uint32_t) at, XTK_OP_CHASE,
			 XTK_FLAG_HELD, 0.0, 0.0);
		make_batch(&m, 1, 9, c, 1);
		check("test-only op rejected on the network path",
		      server_net_admit_message(s, &m, at) == 0);
	}

	/* ---- lost / redundant batches ------------------------------------ */
	fprintf(stderr, "  -- lost/redundant batches\n");
	{
		int at = base + 7;

		/* batch 1 (id 700) is lost; batch 2 repeats it and adds id 701. */
		cmd_init(&c[0], 700, (uint32_t) at, XTK_OP_DRIVE_REL,
			 XTK_FLAG_HELD, 2.0, 0.0);
		cmd_init(&c[1], 701, (uint32_t) at, XTK_OP_HEADING, 0, 0.5, 0.0);
		make_batch(&m, 1, 10, c, 2);
		check("redundant batch admits both",
		      server_net_admit_message(s, &m, at) == 2);
		check("both commands queued", intent_pending(at) == 2);

		/* The late original batch (older packet_sequence) is ignored. */
		cmd_init(&c[0], 700, (uint32_t) at, XTK_OP_DRIVE_REL,
			 XTK_FLAG_HELD, 2.0, 0.0);
		make_batch(&m, 1, 9, c, 1);
		check("late older batch ignored",
		      server_net_admit_message(s, &m, at) == 0);
		check("no duplicate from late batch", intent_pending(at) == 2);
	}

	/* ---- partial datagram (decode-level) ----------------------------- */
	fprintf(stderr, "  -- partial / malformed datagram\n");
	{
		uint8_t buf[XTK_MAX_MESSAGE_BYTES];
		size_t len = 0;
		XtkMessage out;

		cmd_init(&c[0], 800, (uint32_t) (base + 8), XTK_OP_DRIVE_REL,
			 XTK_FLAG_HELD, 1.0, 0.0);
		make_batch(&m, 1, 11, c, 1);
		check("encode ok",
		      xtk_encode(buf, sizeof(buf), &m, &len) == XTK_ERR_NONE &&
		      len > XTK_ENVELOPE_BYTES);
		check("truncated datagram rejected",
		      xtk_decode(buf, len - 4, &out) != XTK_ERR_NONE);
		/* Declare a payload longer than the datagram. */
		xtk_store_u32(buf + 8, (uint32_t) (len + 100));
		check("oversized payload_length rejected",
		      xtk_decode(buf, len, &out) != XTK_ERR_NONE);
	}

	/* ---- queue saturation with a release reserve --------------------- */
	fprintf(stderr, "  -- queue saturation / release reserve\n");
	{
		int at = base + 20;
		uint32_t id = 1000;
		int i, accepted = 0, saturated = 0;

		intent_reset();
		for (i = 0; i < 140; i++) {
			cmd_init(&c[0], id++, (uint32_t) at, XTK_OP_DRIVE_REL,
				 XTK_FLAG_HELD, 5.0, 0.0);
			make_batch(&m, 1, (uint32_t) (100 + i), c, 1);
			if (server_net_admit_message(s, &m, at) == 1)
				accepted++;
			else
				saturated++;
		}
		check("normal commands saturate before the queue is full",
		      saturated > 0);
		check("queue stays within the engine bound",
		      intent_queued() <= INTENT_QUEUE_MAX);

		/* A neutral drive is admitted despite saturation. */
		cmd_init(&c[0], 2000, (uint32_t) at, XTK_OP_DRIVE_REL,
			 XTK_FLAG_HELD, 0.0, 0.0);
		make_batch(&m, 1, 500, c, 1);
		check("release drive admitted under saturation",
		      server_net_admit_message(s, &m, at) == 1);

		/* A fire release is admitted too. */
		cmd_init(&c[0], 2001, (uint32_t) at, XTK_OP_FIRE_HOLD,
			 XTK_FLAG_HELD, 0.0, 0.0);
		make_batch(&m, 1, 501, c, 1);
		check("release fire admitted under saturation",
		      server_net_admit_message(s, &m, at) == 1);

		/* Another normal command is still rejected. */
		cmd_init(&c[0], 2002, (uint32_t) at, XTK_OP_DRIVE_REL,
			 XTK_FLAG_HELD, 5.0, 0.0);
		make_batch(&m, 1, 502, c, 1);
		check("normal command still rejected under saturation",
		      server_net_admit_message(s, &m, at) == 0);
		(void) accepted;
	}

	/* ---- applied acknowledgment after a real step -------------------- */
	fprintf(stderr, "  -- applied ack after step\n");
	{
		int at = server_frame() + 1;
		int before = server_frame();

		intent_reset();
		cmd_init(&c[0], 3000, (uint32_t) at, XTK_OP_DRIVE_REL,
			 XTK_FLAG_HELD, 1.0, 0.0);
		make_batch(&m, 1, 600, c, 1);
		(void) server_net_admit_message(s, &m, at);
		(void) server_step();
		server_net_post_step(server_frame());
		check("tick advanced", server_frame() == before + 1);
		check("applied ack after the step",
		      ack_status(s, 3000, &tick) == XTK_ACK_APPLIED);
	}

	server_net_stats(&st);
	fprintf(stderr, "  stats: accepted=%lu rejected=%lu dup=%lu stale=%lu "
		"foreign=%lu saturated=%lu pushed=%lu acks=%lu applied=%lu\n",
		st.cmds_accepted, st.cmds_rejected, st.cmds_duplicate,
		st.cmds_stale_epoch, st.cmds_foreign, st.cmds_saturated,
		st.intents_pushed, st.acks_sent, st.applied_marks);
	check("some commands were accepted", st.cmds_accepted > 0);
	check("some commands were rejected", st.cmds_rejected > 0);
	check("duplicates were suppressed", st.cmds_duplicate > 0);
	check("stale epoch was rejected", st.cmds_stale_epoch > 0);
	check("foreign owner was rejected", st.cmds_foreign > 0);
	check("saturation was reported", st.cmds_saturated > 0);

	(void) t;
	return failures ? 1 : 0;
}

/* ---- transport mode --------------------------------------------------- */

static int
wait_connect(Transport *server, Transport *client, TransportPeer *cp,
	     TransportPeer **sp_out)
{
	int i;

	*sp_out = (TransportPeer *) NULL;
	for (i = 0; i < 5000; i++) {
		TransportPeer *p;

		(void) transport_service(server, 64);
		(void) transport_service(client, 64);
		while (transport_poll_connect(server, &p))
			*sp_out = p;
		if (*sp_out != (TransportPeer *) NULL &&
		    transport_peer_connected(client, cp))
			return 1;
	}
	return 0;
}

static int
recv_one(Transport *server, TransportPeer **peer, uint8_t *buf, size_t cap,
	 size_t *len, uint8_t *ch)
{
	int i;

	for (i = 0; i < 500; i++) {
		int r;

		(void) transport_service(server, 64);
		r = transport_receive(server, peer, buf, cap, len, ch);
		if (r != 0)
			return r;
	}
	return 0;
}

static int
run_transport(void)
{
	Transport *server, *client;
	TransportPeer *cp, *sp = (TransportPeer *) NULL;
	uint16_t port;
	uint8_t sbuf[XTK_MAX_MESSAGE_BYTES], rbuf[XTK_MAX_MESSAGE_BYTES];
	size_t len, rlen;
	uint8_t rch;
	XtkMessage m;
	XtkCommand c[1];
	TransportStats tstats;
	int r;

	server = transport_create(0, 4);
	check("server transport created", server != NULL);
	if (server == NULL)
		return 1;
	port = transport_port(server);
	check("server bound to an ephemeral port", port != 0);

	client = transport_create(0, 1);
	check("client transport created", client != NULL);
	if (client == NULL)
		return 1;
	cp = transport_connect(client, "127.0.0.1", port);
	check("connect initiated", cp != NULL);
	check("connection established",
	      cp != NULL && wait_connect(server, client, cp, &sp));
	if (sp == (TransportPeer *) NULL)
		return 1;

	/* Reliable control message round-trip (client -> server). */
	cmd_init(&c[0], 1, 1, XTK_OP_DRIVE_REL, XTK_FLAG_HELD, 4.0, 0.0);
	make_batch(&m, 1, 1, c, 1);
	check("encode ok", xtk_encode(sbuf, sizeof(sbuf), &m, &len) ==
	      XTK_ERR_NONE);
	check("reliable send ok",
	      transport_send(client, cp, XTK_CH_CONTROL, sbuf, len) == 0);
	r = recv_one(server, &sp, rbuf, sizeof(rbuf), &rlen, &rch);
	check("reliable receive ok", r == 1);
	check("reliable bytes round-trip", rlen == len &&
	      memcmp(rbuf, sbuf, len) == 0);
	check("reliable arrived on channel 0", rch == XTK_CH_CONTROL);

	/* Unreliable input batch, sent twice (repeated delivery). */
	check("unreliable send 1 ok",
	      transport_send(client, cp, XTK_CH_INPUT, sbuf, len) == 0);
	check("unreliable send 2 ok",
	      transport_send(client, cp, XTK_CH_INPUT, sbuf, len) == 0);
	{
		int got = 0;
		int i;

		for (i = 0; i < 200 && got < 2; i++) {
			(void) transport_service(server, 64);
			while (transport_receive(server, &sp, rbuf, sizeof(rbuf),
						 &rlen, &rch) == 1)
				got++;
		}
		check("repeated unreliable delivery observed", got >= 2);
	}

	/* A short/partial datagram is delivered but must not decode. */
	check("short send ok",
	      transport_send(client, cp, XTK_CH_INPUT, sbuf, 5) == 0);
	r = recv_one(server, &sp, rbuf, sizeof(rbuf), &rlen, &rch);
	check("short datagram delivered", r == 1 && rlen == 5);
	{
		XtkMessage out;

		check("short datagram rejected by decode",
		      xtk_decode(rbuf, rlen, &out) != XTK_ERR_NONE);
	}

	/* Full bounded pre-step path: a batch sent by the peer is serviced,
	   decoded and admitted for the assigned tick, and its acknowledgment is
	   returned reliably.  (A HEARTBEAT has no engine effect, so this works
	   without building a world.) */
	fprintf(stderr, "  -- bounded pre-step routing + ack\n");
	{
		ServerNetSession *s;
		ServerNetStats st;
		uint32_t tick = 0;
		uint8_t ackst = 0xFF;
		int hb, i, sent = 0;

		server_net_init();
		server_net_set_transport(server);
		server_players_init();
		hb = server_players_add("remote", 1);
		(void) server_players_bind_slot(hb, 0);
		s = server_net_session_create(sp);
		check("session bound to the peer",
		      s != NULL && server_players_binding(hb) != NULL &&
		      server_net_session_bind(s, 1, 0,
			      server_players_binding(hb)->generation) == 0);
		server_net_session_begin_epoch(s, 1, 0);

		cmd_init(&c[0], 42, 1, XTK_OP_HEARTBEAT, 0, 0.0, 0.0);
		make_batch(&m, 1, 1, c, 1);
		check("heartbeat encode ok",
		      xtk_encode(sbuf, sizeof(sbuf), &m, &len) == XTK_ERR_NONE);
		check("heartbeat send ok",
		      transport_send(client, cp, XTK_CH_INPUT, sbuf, len) == 0);

		for (i = 0; i < 300 && !sent; i++) {
			(void) server_net_pre_step(1);
			server_net_stats(&st);
			if (st.heartbeats > 0)
				sent = 1;
		}
		check("batch admitted through the bounded pre-step", sent == 1);
		server_net_post_step(1);
		check("accepted ack recorded",
		      server_net_session_ack_lookup(s, 42, &tick, &ackst,
						    (uint8_t *) NULL) == 1 &&
		      ackst == XTK_ACK_ACCEPTED);

		/* The client receives the CMD_ACK the server flushed. */
		r = recv_one(client, &cp, rbuf, sizeof(rbuf), &rlen, &rch);
		check("client received an acknowledgment message", r == 1);
		if (r == 1) {
			XtkMessage ack;

			check("ack decodes",
			      xtk_decode(rbuf, rlen, &ack) == XTK_ERR_NONE &&
			      ack.env.message_type == XTK_MSG_CMD_ACK &&
			      ack.body.cmd_ack.count >= 1);
		}
	}

	/* Ten-second paused reader: the client never services; the server must
	   keep ticking (nonblocking) and its reliable backlog must stay
	   bounded (we stop/abort at the approved cap). */
	fprintf(stderr, "  -- paused reader (10 s of ticks)\n");
	{
		uint8_t big[4096];
		size_t max_backlog = 0;
		uint64_t t0, elapsed;
		int i;

		(void) memset(big, 0xAB, sizeof(big));
		t0 = now_ms();
		for (i = 0; i < 900; i++) {
			size_t backlog;

			/* Service the server only (zero timeout: never blocks). */
			(void) transport_service(server, 64);
			if (transport_send(server, sp, XTK_CH_CONTROL, big,
					   sizeof(big)) != 0)
				break;
			backlog = transport_peer_reliable_bytes(server, sp);
			if (backlog > max_backlog)
				max_backlog = backlog;
			if (backlog > XTK_MAX_RELIABLE_QUEUE_BYTES)
				break;	/* the caller would abort here */
		}
		elapsed = now_ms() - t0;
		check("paused reader does not stall ticks", elapsed < 5000);
		check("reliable backlog stays bounded",
		      max_backlog <= XTK_MAX_RELIABLE_QUEUE_BYTES +
				     sizeof(big));
		fprintf(stderr, "  paused-reader: 900 services in %llu ms, "
			"max backlog %zu bytes\n",
			(unsigned long long) elapsed, max_backlog);
	}

	/* Backpressure abort: a session whose peer backlog exceeds the bound is
	   destroyed. */
	{
		ServerNetSession *s;
		uint8_t big[4096];
		int i;

		(void) memset(big, 0xCD, sizeof(big));
		server_net_init();
		server_net_set_transport(server);
		s = server_net_session_create(sp);
		check("session created for the peer", s != NULL);
		for (i = 0; i < 200 &&
		     transport_peer_reliable_bytes(server, sp) <=
			     XTK_MAX_RELIABLE_QUEUE_BYTES; i++)
			(void) transport_send(server, sp, XTK_CH_CONTROL, big,
					      sizeof(big));
		check("backpressure aborts a saturated session",
		      server_net_session_backpressure(s) == 1);
		check("saturated session is gone", server_net_session_count() == 0);
	}

	transport_stats(server, &tstats);
	fprintf(stderr, "  transport stats: rx=%lu tx=%lu connects=%lu "
		"max_events=%lu\n", tstats.packets_rx, tstats.packets_tx,
		tstats.connects, tstats.service_max_events);
	check("transport received packets", tstats.packets_rx > 0);
	check("transport sent packets", tstats.packets_tx > 0);
	check("service work stayed within budget",
	      tstats.service_max_events <= XTK_MAX_SERVICE_WORK_PER_TICK);

	transport_destroy(client);
	transport_destroy(server);
	return failures ? 1 : 0;
}

int
main(int argc, char *argv[])
{
	const char *assets = (argc > 1) ? argv[1] : ".";
	const char *mode = (argc > 2) ? argv[2] : "admission";

	if (!strcmp(mode, "transport")) {
		(void) run_transport();
		if (failures != 0) {
			printf("ADMISSION FIXTURE FAIL (%d checks, transport)\n",
			       failures);
			return 1;
		}
		printf("ADMISSION FIXTURE PASS (transport: ENet loopback, "
		       "reliable/unreliable round-trip, partial decode, "
		       "paused-reader bounded, backpressure abort)\n");
		return 0;
	}

	(void) run_admission(assets);
	if (failures != 0) {
		printf("ADMISSION FIXTURE FAIL (%d checks, admission)\n",
		       failures);
		return 1;
	}
	printf("ADMISSION FIXTURE PASS (admission: ownership, validation, "
	       "dedup, stale epoch/generation, lost/repeated/partial, "
	       "saturation+release reserve, applied ack)\n");
	return 0;
}
