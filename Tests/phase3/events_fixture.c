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
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 */

/*
 * events_fixture.c -- P3.8 recipient-authorized, bounded event delivery.
 *
 * Drives the shipping session coordinator with two human recipients (slots
 * A/B) through one running epoch and independently checks the
 * docs/EVENT-VIEW-AUTHZ-v1.md rules:
 *
 *   - remote SCORE is not broadcast (own only; public only at RESULTS);
 *   - DEATH reaches the victim and killer; EXPLOSION/RESPAWN obey the
 *     reference viewport recipient table;
 *   - per-recipient event_sequence is contiguous: a filtered private event is
 *     skipped without a sequence gap, so filtering is never read as loss;
 *   - capture_tick is the occurrence tick, preserved through delayed delivery;
 *   - more than 256 events in one epoch deliver without silent loss (bounded
 *     cursors), and retirement below a recipient cursor is an explicit
 *     slow-peer failure, never a silent drop;
 *   - RESULTS capture all required participants before the score reset, once,
 *     and survive a later departure;
 *   - 0/1/5 observations leave the simulation byte-identical.
 *
 * The fixture is the harness and may manipulate state directly; the exporter
 * and delivery path are the read-only units under test.
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
#include "server_view.h"
#include "server_events.h"
#include "server_session.h"
#include "wire.h"
#include "proto.h"
#include "gr.h"

#include "../Protocol/content-manifest.h"

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

/* ---- read-only simulation hash ---------------------------------------- */

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
sim_hash(void)
{
	unsigned long h = 2166136261UL;
	int i;

	hbytes(&h, &frame, sizeof(frame));
	hbytes(&h, &num_veh, sizeof(num_veh));
	hbytes(&h, &num_veh_alive, sizeof(num_veh_alive));
	hbytes(&h, &num_veh_dead, sizeof(num_veh_dead));
	hbytes(&h, actual_vehicles, (size_t) num_veh * sizeof(Vehicle));
	hbytes(&h, combatant, sizeof(Combatant) * (size_t) num_combatants);
	hbytes(&h, real_map, sizeof(real_map));
	hbytes(&h, bset->array, sizeof(actual_bset.array));
	hbytes(&h, &bset->number, sizeof(bset->number));
	hbytes(&h, eset->array, sizeof(actual_eset.array));
	hbytes(&h, &eset->number, sizeof(eset->number));
	for (i = 0; i < num_combatants; i++)
		hbytes(&h, &combatant[i].score, sizeof(combatant[i].score));
	return h;
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
}

/* ---- delivered-event collection --------------------------------------- */

#define MAX_SEEN 64

typedef struct {
	int count;
	XtkEvent ev[MAX_SEEN];
	uint32_t env_tick[MAX_SEEN];
} Seen;

static void
collect_events(const ServerSession *s, Seen *out)
{
	int i;

	(void) memset(out, 0, sizeof(*out));
	for (i = 0; i < server_session_reply_count(s); i++) {
		XtkMessage m;

		if (server_session_reply(s, i, &m) != 0)
			continue;
		if (m.env.message_type != XTK_MSG_EVENT)
			continue;
		if (out->count >= MAX_SEEN)
			break;
		out->ev[out->count] = m.body.event;
		out->env_tick[out->count] = (uint32_t) m.env.tick;
		out->count++;
	}
}

static int
find_reply_type(const ServerSession *s, int type)
{
	int i;

	for (i = 0; i < server_session_reply_count(s); i++) {
		XtkMessage m;

		if (server_session_reply(s, i, &m) == 0 &&
		    m.env.message_type == (uint8_t) type)
			return 1;
	}
	return 0;
}

static int
seen_has(const Seen *s, int type, int id, int *seq_out)
{
	int i;

	for (i = 0; i < s->count; i++)
		if (s->ev[i].event_type == (uint8_t) type &&
		    (int) s->ev[i].entity_id == id) {
			if (seq_out != NULL)
				*seq_out = (int) s->ev[i].event_sequence;
			return 1;
		}
	return 0;
}

static int
count_type(const Seen *s, int type)
{
	int i, n = 0;

	for (i = 0; i < s->count; i++)
		if (s->ev[i].event_type == (uint8_t) type)
			n++;
	return n;
}

/* Contiguous, no-gap recipient-local sequencing starting at 0. */
static int
seq_contiguous(const Seen *s)
{
	uint32_t expect = 0;
	int i;

	for (i = 0; i < s->count; i++)
		if (s->ev[i].event_sequence != expect++)
			return 0;
	return 1;
}

/* ---- setup ------------------------------------------------------------ */

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
	(void) strncpy(username, "p3.8", MAX_STRING - 1);
	username[MAX_STRING - 1] = '\0';

	if (content_set_root(g_assets) != 0) {
		fprintf(stderr, "events_fixture: %s\n", content_last_error());
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

	if (make_vdesc((char *) "Vanguard", &(int){0}) != DESC_LOADED ||
	    make_mdesc((char *) "combat1", &(int){0}) != DESC_LOADED) {
		fprintf(stderr, "events_fixture: cannot load content\n");
		return -1;
	}
	return 0;
}

int
main(int argc, char *argv[])
{
	ServerSession *A, *B;
	uint64_t t = 1000;
	int i;

	g_assets = (argc > 1) ? argv[1] : ".";

	if (server_bringup() != 0)
		return 2;

	server_session_init();
	server_session_set_capacity(XTK_MAX_HUMANS);

	A = admit("humanA", t);
	t += 10;
	B = admit("humanB", t);
	if (A == NULL || B == NULL) {
		fprintf(stderr, "events_fixture: admit failed\n");
		return 2;
	}
	if (start_match(A, B, XTK_CONTENT_ID_COMBAT1, 0, t) != XTK_ERR_NONE) {
		fprintf(stderr, "events_fixture: start failed\n");
		return 2;
	}
	t += 10;

	/* Let the epoch settle; then reset the per-recipient delivery cursors. */
	(void) server_session_step(A, t);
	server_session_reply_clear(A);
	server_session_reply_clear(B);

	{
		Vehicle *va = combatant[server_session_bound_slot(A)].vehicle;
		Vehicle *vb = combatant[server_session_bound_slot(B)].vehicle;

		/* Separate cameras well beyond one viewport. */
		va->loc->x = 1000.0; va->loc->y = 1000.0;
		vb->loc->x = 5000.0; vb->loc->y = 5000.0;

		fprintf(stderr, "== authorization and per-recipient sequencing\n");
		{
			/*
			 * Advance the frame so the end-of-tick observation is not
			 * skipped as an idempotent repeat of the settle step.
			 */
			uint32_t F;
			int baseA = server_session_events_sent(A);
			int baseB = server_session_events_sent(B);
			Seen seenA, seenB;

			frame++;
			F = (uint32_t) frame;

			/* Private SCORE for each owner. */
			combatant[0].score += 7;
			combatant[1].score += 9;
			server_events_observe();

			/* Gameplay DEATH with A victim, B killer. */
			server_events_note_death(0, 1);

			/* Remote RESPAWN: one inside A's viewport, one far away. */
			server_events_note_spawn(1, va->loc->x + 10.0,
						 va->loc->y);
			server_events_note_spawn(1, 9000.0, 9000.0);

			/* Transient EXPLOSION: one inside A's viewport, one far.
			   Bump the frame so this observation is not an
			   idempotent repeat of the SCORE observation above. */
			frame++;
			memset(&actual_eset, 0, sizeof(actual_eset));
			eset->number = 2;
			eset->list[0] = &eset->array[7];
			eset->array[7].x = va->loc->x + 20.0;
			eset->array[7].y = va->loc->y;
			eset->array[7].color = 5;
			eset->list[1] = &eset->array[8];
			eset->array[8].x = 9000.0;
			eset->array[8].y = 9000.0;
			eset->array[8].color = 6;
			server_events_observe();

			(void) server_session_step(A, (uint64_t) (t + 5));

			collect_events(A, &seenA);
			collect_events(B, &seenB);
			(void) baseA;
			(void) baseB;

			/* SCORE is SELF only. */
			check("A receives its own SCORE",
			      seen_has(&seenA, XTK_EV_SCORE, 0, NULL));
			check("A does not receive remote SCORE",
			      !seen_has(&seenA, XTK_EV_SCORE, 1, NULL));
			check("B receives its own SCORE",
			      seen_has(&seenB, XTK_EV_SCORE, 1, NULL));
			check("B does not receive remote SCORE",
			      !seen_has(&seenB, XTK_EV_SCORE, 0, NULL));

			/* DEATH reaches both participants. */
			check("victim receives DEATH",
			      seen_has(&seenA, XTK_EV_DEATH, 0, NULL));
			check("killer receives DEATH",
			      seen_has(&seenB, XTK_EV_DEATH, 0, NULL));

			/* RESPAWN: own always; remote only in viewport. */
			check("B receives both own RESPAWNs",
			      count_type(&seenB, XTK_EV_RESPAWN) == 2);
			check("A receives only the in-view remote RESPAWN",
			      count_type(&seenA, XTK_EV_RESPAWN) == 1);

			/* EXPLOSION: viewport-limited only. */
			check("A receives the in-view EXPLOSION",
			      seen_has(&seenA, XTK_EV_EXPLOSION, 7, NULL));
			check("A does not receive the out-of-view EXPLOSION",
			      !seen_has(&seenA, XTK_EV_EXPLOSION, 8, NULL));
			check("B receives no out-of-view EXPLOSION",
			      count_type(&seenB, XTK_EV_EXPLOSION) == 0);

			/* Private skips leave a contiguous recipient sequence. */
			check("A recipient-local sequence is contiguous",
			      seq_contiguous(&seenA));
			check("B recipient-local sequence is contiguous",
			      seq_contiguous(&seenB));

			/* Occurrence tick preserved through delayed delivery. */
			{
				int seq;
				int okA = seen_has(&seenA, XTK_EV_DEATH, 0, &seq);
				int idx = -1, j;

				for (j = 0; j < seenA.count; j++)
					if (seenA.ev[j].event_type ==
					    XTK_EV_DEATH)
						idx = j;
				check("capture_tick is the occurrence tick",
				      okA && idx >= 0 &&
				      seenA.ev[idx].capture_tick == F);
				check("delivery envelope tick is later than capture",
				      okA && idx >= 0 &&
				      seenA.env_tick[idx] > seenA.ev[idx].capture_tick);
			}
		}

		/* ---------------- >256 events, bounded cursor ---------------- */
		fprintf(stderr, "== bounded delivery (>256 events) and slow peer\n");
		{
			int baseA = server_session_events_sent(A);
			uint32_t ring_next = server_events_next_ordinal();
			int k;

			for (k = 0; k < 300; k++)
				server_events_note_death(0, -1);

			(void) server_session_step(A, (uint64_t) (t + 10));

			check("no event was retired for the healthy recipient",
			      server_events_first_ordinal() <= ring_next);
			check("all 300 authorized events were delivered",
			      server_session_events_sent(A) - baseA == 300);
			check("recipient cursor advanced over every ordinal",
			      server_events_next_ordinal() > ring_next);
		}

		/* Clear harness-forced transients. */
		(void) memset(&actual_eset, 0, sizeof(actual_eset));
		(void) memset(&actual_bset, 0, sizeof(actual_bset));
	}

	/* ---------------- RESULTS: once, all participants, survives ------- */
	fprintf(stderr, "== results capture (before reset, once, survives)\n");
	{
		XtkResults r1, r2;
		Seen seenA;
		int valid;

		/* Force A to the winning score and run to the natural reset. */
		server_session_reply_clear(A);
		server_session_reply_clear(B);
		combatant[server_session_bound_slot(A)].score =
			settings.si.winning_score;
		for (i = 0; i < 100000; i++) {
			int st = server_session_step(A, (uint64_t) (t + 20));

			if (st != GAME_RUNNING)
				break;
		}
		valid = server_events_results_valid() &&
			server_events_results(&r1) == SERVER_EVENTS_OK;
		check("results captured", valid);
		check("results include every required participant",
		      valid && r1.vehicle_count == 2);
		check("results are a win for A",
		      valid && r1.result_kind == 0 &&
		      r1.winner_slot == (uint8_t) server_session_bound_slot(A));
		/* Result generations are full-width u32, not truncated. */
		{
			int e, genok = valid;

			for (e = 0; e < (int) r1.vehicle_count; e++)
				if (r1.entry[e].generation !=
				    server_events_generation(
					    XTK_ENTITY_VEHICLE,
					    r1.entry[e].slot))
					genok = 0;
			check("result generations are full-width u32", genok);
		}

		/* The public winner/result reaches both recipients reliably. */
		check("A receives RESULTS", find_reply_type(A, XTK_MSG_RESULTS));
		check("B receives RESULTS", find_reply_type(B, XTK_MSG_RESULTS));
		/* MATCH_END was captured in the ring and is public. */
		{
			int k, have = 0;
			uint32_t first = server_events_first_ordinal();
			uint32_t next = server_events_next_ordinal();

			for (k = (int) first; k < (int) next; k++) {
				const XtkEvent *e = server_events_event_at(
					(uint32_t) k);

				if (e != NULL && e->event_type ==
				    XTK_EV_MATCH_END) {
					have = 1;
					check("MATCH_END authorized to every recipient",
					      server_events_authorized(
						      (uint32_t) k, 1,
						      0.0, 0.0) == 1);
				}
			}
			check("MATCH_END captured before score reset", have);
		}
		(void) seenA;

		/* A later departure does not corrupt the captured results. */
		server_session_depart(B, XTK_ERR_TIMEOUT);
		check("results survive a departure",
		      server_events_results_valid() &&
		      server_events_results(&r2) == SERVER_EVENTS_OK &&
		      memcmp(&r1, &r2, sizeof(r1)) == 0);
	}

	teardown_session();

	/* ---------------- session-level explicit slow-peer failure -------- */
	fprintf(stderr, "== session slow-peer failure (retirement outran peer)\n");
	{
		ServerSession *A2, *B2;
		uint64_t t2 = 5000;
		int k;

		server_session_init();
		server_session_set_capacity(XTK_MAX_HUMANS);
		A2 = admit("slowA", t2);
		t2 += 10;
		B2 = admit("slowB", t2);
		if (A2 == NULL || B2 == NULL ||
		    start_match(A2, B2, XTK_CONTENT_ID_COMBAT1, 0, t2) !=
		    XTK_ERR_NONE) {
			check("slow-peer match starts", 0);
		} else {
			/* Never step: the recipients' cursors stay at 0 while
			   more than a full ring of events is produced. */
			for (k = 0; k < SERVER_EVENTS_RING + 100; k++)
				server_events_note_death(0, -1);
			check("retirement outran the recipient cursor",
			      server_events_first_ordinal() > 0);

			(void) server_session_step(A2, (uint64_t) (t2 + 5));

			check("slow recipients are failed explicitly, not silently",
			      server_session_count() == 0);
			teardown_session();
		}
	}

	/* ---------------- events-layer: retirement + capture tick --------- */
	fprintf(stderr, "== bounded ring: retirement and slow-peer detection\n");
	{
		uint32_t first0, next0;
		int k;

		server_events_reset(77, 0);
		first0 = server_events_first_ordinal();
		next0 = server_events_next_ordinal();
		check("fresh epoch ring is empty", first0 == next0);

		/* 300 events fit in the ring and are all retained in order. */
		for (k = 0; k < 300; k++)
			server_events_note_death(0, -1);
		check("300 events retained without retirement",
		      server_events_first_ordinal() == next0);
		check("ordinal access returns the oldest retained event",
		      server_events_event_at(next0) != NULL);
		check("nonexistent future ordinal returns NULL",
		      server_events_event_at(server_events_next_ordinal()) ==
		      NULL);

		/* A further 300 (> ring) retires the oldest; a cursor at the
		   old first ordinal is now explicitly stale. */
		for (k = 0; k < 300; k++)
			server_events_note_death(0, -1);
		check("retirement advanced the first retained ordinal",
		      server_events_first_ordinal() > next0);
		check("retired ordinal is no longer retrievable",
		      server_events_event_at(next0) == NULL);
		check("a cursor below the retained window is detectable",
		      next0 < server_events_first_ordinal());
	}

	/* ---------------- read-only parity (0/1/5 observations) ---------- */
	fprintf(stderr, "== read-only parity (0/1/5 observations)\n");
	{
		unsigned long h0, h1, h5;

		frame++;
		h0 = sim_hash();
		server_events_observe();
		h1 = sim_hash();
		for (i = 0; i < 4; i++)
			server_events_observe();
		h5 = sim_hash();
		check("simulation unchanged after 1 observation", h0 == h1);
		check("simulation unchanged after 5 observations", h0 == h5);
	}

	if (failures != 0) {
		printf("EVENTS FIXTURE FAIL (%d checks)\n", failures);
		return 1;
	}
	printf("EVENTS FIXTURE PASS (2 recipients; SCORE/DEATH/RESPAWN/EXPLOSION/"
	       "MATCH_END authorization; contiguous sequences; occurrence tick; "
	       "bounded >256 delivery; explicit slow-peer detection; results "
	       "before reset; read-only 0/1/5)\n");
	return 0;
}
