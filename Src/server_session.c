/*
 * server_session.c -- solo lobby and network-server session (P2.8).
 *
 * See Src/Include/server_session.h for the contract.  This module is the glue
 * that makes the P2.6 event/result exporter and the P2.7 admission layer part
 * of the shipped server.  It owns one peer (D2-F), the lobby handshake, the
 * allowlisted content selection, start/reset administration, the two-second
 * solo timeout abort and the per-tick snapshot/event delivery.
 *
 * Everything here runs on the simulation thread.  With no transport attached
 * (transport == NULL) the state machine is driven directly, which is what the
 * P2.8 acceptance fixture does.
 */

#define _POSIX_C_SOURCE 200809L

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
#include "intent.h"
#include "proto.h"

#include "../Protocol/content-manifest.h"

/* Simulation globals owned by the entry point / test harness. */
extern int frame;
extern int num_combatants;
extern Combatant combatant[];
extern Settings settings;
extern Mdesc *mdesc;
extern Vdesc *vdesc;
extern CLFkr command_options;
extern Boolean game_paused;

/* Defined in server_main.c (production) or the fixture. */
extern char team_char[];
extern Eset actual_eset, *eset;
extern Bset actual_bset, *bset;
extern Maze maze;
extern Map real_map;
extern char *executable_name;
extern int num_veh;
extern int num_veh_alive;
extern int num_veh_dead;
extern Vehicle actual_vehicles[];
extern Vehicle *live_vehicles[];
extern Vehicle *dead_vehicles[];

#define SERVER_SESSION_STEP_BUDGET	XTK_MAX_SERVICE_WORK_PER_TICK

struct ServerSession {
	int used;
	ServerSessionState state;

	uint32_t session_id;
	uint32_t epoch;
	int is_admin;

	char name[XTK_MAX_NAME_BYTES + 1];

	uint16_t maze_id;
	uint16_t vehicle_id;
	int config_valid;
	int ready;

	Transport *t;
	TransportPeer *peer;

	ServerNetSession *net;
	int bound_slot;
	unsigned int generation;

	uint64_t last_rx_ms;
	int epoch_live;

	uint32_t snapshot_seq;
	int events_sent;

	/* bounded encoded control-reply log (inspection/tests) */
	int reply_count;
	size_t reply_len[SERVER_SESSION_REPLY_MAX];
	uint8_t reply_buf[SERVER_SESSION_REPLY_MAX][SERVER_SESSION_REPLY_BYTES];
};

static ServerSession the_session;
static int n_sessions;
static Transport *session_host;
static uint32_t next_session_id = 1;

static uint8_t txbuf[XTK_MAX_MESSAGE_BYTES];

static uint64_t
now_ms(void)
{
	struct timespec ts;

	(void) clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t) ts.tv_sec * 1000ULL + (uint64_t) ts.tv_nsec / 1000000ULL;
}

/* Find the session owning `peer` (or the first live session). */
static ServerSession *
session_for_peer(TransportPeer *peer)
{
	if (the_session.used && (peer == NULL || the_session.peer == peer))
		return &the_session;
	return (ServerSession *) NULL;
}

void
server_session_init(void)
{
	(void) memset(&the_session, 0, sizeof(the_session));
	n_sessions = 0;
	session_host = (Transport *) NULL;
	server_net_init();
}

void
server_session_set_transport(Transport *t)
{
	session_host = t;
	server_net_set_transport(t);
}

int
server_session_count(void)
{
	return n_sessions;
}

ServerSession *
server_session_current(void)
{
	return the_session.used ? &the_session : (ServerSession *) NULL;
}

TransportPeer *
server_session_peer(const ServerSession *s)
{
	return (s != (const ServerSession *) NULL) ? s->peer : (TransportPeer *) NULL;
}

/* ------------------------------------------------------------------ */
/* Accessors                                                           */
/* ------------------------------------------------------------------ */

ServerSessionState
server_session_state(const ServerSession *s)
{
	return (s != (const ServerSession *) NULL) ? s->state
						   : SERVER_SESSION_DISCONNECTED;
}

uint32_t
server_session_id(const ServerSession *s)
{
	return (s != (const ServerSession *) NULL) ? s->session_id : 0;
}

uint32_t
server_session_epoch(const ServerSession *s)
{
	return (s != (const ServerSession *) NULL) ? s->epoch : 0;
}

int
server_session_is_admin(const ServerSession *s)
{
	return (s != (const ServerSession *) NULL) && s->is_admin;
}

int
server_session_ready(const ServerSession *s)
{
	return (s != (const ServerSession *) NULL) && s->ready;
}

uint16_t
server_session_maze_id(const ServerSession *s)
{
	return (s != (const ServerSession *) NULL) ? s->maze_id : 0;
}

uint16_t
server_session_vehicle_id(const ServerSession *s)
{
	return (s != (const ServerSession *) NULL) ? s->vehicle_id : 0;
}

int
server_session_config_valid(const ServerSession *s)
{
	return (s != (const ServerSession *) NULL) && s->config_valid;
}

int
server_session_bound_slot(const ServerSession *s)
{
	return (s != (const ServerSession *) NULL) ? s->bound_slot : -1;
}

uint32_t
server_session_snapshot_seq(const ServerSession *s)
{
	return (s != (const ServerSession *) NULL) ? s->snapshot_seq : 0;
}

int
server_session_events_sent(const ServerSession *s)
{
	return (s != (const ServerSession *) NULL) ? s->events_sent : 0;
}

void
server_session_set_admin(ServerSession *s, int admin)
{
	if (s != (ServerSession *) NULL)
		s->is_admin = admin ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* Session lifecycle                                                   */
/* ------------------------------------------------------------------ */

ServerSession *
server_session_accept(TransportPeer *peer)
{
	if (the_session.used || n_sessions >= SERVER_SESSION_MAX_PEERS)
		return (ServerSession *) NULL;

	(void) memset(&the_session, 0, sizeof(the_session));
	the_session.used = 1;
	the_session.state = SERVER_SESSION_HANDSHAKE;
	the_session.session_id = next_session_id++;
	the_session.epoch = 0;
	the_session.is_admin = 1;	/* first (and only) peer administers */
	the_session.t = session_host;
	the_session.peer = peer;
	the_session.bound_slot = -1;
	n_sessions = 1;
	return &the_session;
}

void
server_session_destroy(ServerSession *s)
{
	if (s == (ServerSession *) NULL || !s->used)
		return;
	if (s->net != (ServerNetSession *) NULL) {
		server_net_session_destroy(s->net);
		s->net = (ServerNetSession *) NULL;
	} else if (s->t != (Transport *) NULL &&
		   s->peer != (TransportPeer *) NULL) {
		transport_disconnect(s->t, s->peer);
	}
	if (s->bound_slot >= 0)
		server_players_unbind_slot(s->bound_slot);
	s->used = 0;
	n_sessions = 0;
}

void
server_session_reject_peer(Transport *t, TransportPeer *peer,
			   uint16_t error_code, const char *text)
{
	XtkMessage m;
	size_t len;
	int e;

	if (t == (Transport *) NULL || peer == (TransportPeer *) NULL)
		return;
	(void) memset(&m, 0, sizeof(m));
	m.env.message_type = XTK_MSG_REJECT;
	m.env.session_id = 0;
	m.body.reject.error_code = error_code;
	m.body.reject.detail_code = 0;
	if (text != NULL) {
		(void) strncpy(m.body.reject.text, text,
			       XTK_MAX_ERROR_TEXT_BYTES);
		m.body.reject.text[XTK_MAX_ERROR_TEXT_BYTES] = '\0';
	}
	e = xtk_encode(txbuf, sizeof(txbuf), &m, &len);
	if (e == XTK_ERR_NONE)
		(void) transport_send(t, peer, XTK_CH_CONTROL, txbuf, len);
	transport_disconnect(t, peer);
}

/* ------------------------------------------------------------------ */
/* Outbound messages                                                   */
/* ------------------------------------------------------------------ */

/* Encode, send (when attached) and log a message in the reply ring.  Returns
 * the encoded length, or 0 on an encode failure. */
static size_t
session_emit(ServerSession *s, const XtkMessage *m)
{
	size_t len = 0;
	int e = xtk_encode(txbuf, sizeof(txbuf), m, &len);

	if (e != XTK_ERR_NONE)
		return 0;
	if (s->t != (Transport *) NULL && s->peer != (TransportPeer *) NULL)
		(void) transport_send(s->t, s->peer,
				      xtk_next_channel(m->env.message_type),
				      txbuf, len);
	if (len <= SERVER_SESSION_REPLY_BYTES &&
	    s->reply_count < SERVER_SESSION_REPLY_MAX) {
		int i = s->reply_count++;

		(void) memcpy(s->reply_buf[i], txbuf, len);
		s->reply_len[i] = len;
	}
	return len;
}

int
server_session_reply_count(const ServerSession *s)
{
	return (s != (const ServerSession *) NULL) ? s->reply_count : 0;
}

int
server_session_reply(const ServerSession *s, int index, XtkMessage *out)
{
	if (s == (const ServerSession *) NULL || index < 0 ||
	    index >= s->reply_count || out == (XtkMessage *) NULL)
		return -1;
	return (xtk_decode(s->reply_buf[index], s->reply_len[index], out) ==
		XTK_ERR_NONE) ? 0 : -1;
}

void
server_session_reply_clear(ServerSession *s)
{
	if (s != (ServerSession *) NULL)
		s->reply_count = 0;
}

static void
session_envelope(ServerSession *s, XtkMessage *m, uint8_t type, int32_t tick)
{
	(void) memset(m, 0, sizeof(*m));
	m->env.message_type = type;
	m->env.session_id = s->session_id;
	m->env.epoch = s->epoch_live ? s->epoch : 0;
	m->env.tick = tick;
}

static void
emit_welcome(ServerSession *s, uint32_t server_nonce)
{
	XtkMessage m;

	session_envelope(s, &m, XTK_MSG_WELCOME, 0);
	m.body.welcome.server_nonce = server_nonce;
	m.body.welcome.role = 1;		/* player */
	m.body.welcome.tick_hz = (uint8_t) XTK_SERVER_TICK_HZ;
	m.body.welcome.max_players = (uint8_t) XTK_MAX_PLAYERS;
	(void) session_emit(s, &m);
}

static void
emit_catalog(ServerSession *s)
{
	XtkMessage m;
	size_t i, n = xtk_content_entry_count;

	if (n > XTK_MAX_CATALOG_ENTRIES)
		n = XTK_MAX_CATALOG_ENTRIES;
	session_envelope(s, &m, XTK_MSG_CONTENT_CATALOG, 0);
	(void) memcpy(m.body.catalog.content_set_digest, xtk_content_set_digest,
		      XTK_SHA256_BYTES);
	m.body.catalog.entry_count = (uint16_t) n;
	for (i = 0; i < n; i++) {
		const XtkContentEntry *e = &xtk_content_entries[i];
		XtkCatalogEntry *ce = &m.body.catalog.entry[i];
		size_t len = strlen(e->name);

		if (len > XTK_MAX_NAME_BYTES)
			len = XTK_MAX_NAME_BYTES;
		ce->id = e->id;
		ce->kind = e->kind;
		(void) memset(ce->name, 0, sizeof(ce->name));
		(void) memcpy(ce->name, e->name, len);
		(void) memcpy(ce->source_sha256, e->sha256, XTK_SHA256_BYTES);
	}
	(void) session_emit(s, &m);
}

static void
emit_reject(ServerSession *s, uint16_t code, const char *text)
{
	XtkMessage m;

	session_envelope(s, &m, XTK_MSG_REJECT, 0);
	m.env.session_id = 0;
	m.body.reject.error_code = code;
	if (text != NULL) {
		(void) strncpy(m.body.reject.text, text,
			       XTK_MAX_ERROR_TEXT_BYTES);
		m.body.reject.text[XTK_MAX_ERROR_TEXT_BYTES] = '\0';
	}
	(void) session_emit(s, &m);
}

static void
emit_protocol_error(ServerSession *s, uint16_t code, uint16_t detail,
		    uint16_t offending_type)
{
	XtkMessage m;

	session_envelope(s, &m, XTK_MSG_PROTOCOL_ERROR, 0);
	m.body.protocol_error.error_code = code;
	m.body.protocol_error.detail = detail;
	m.body.protocol_error.offending_type = offending_type;
	(void) session_emit(s, &m);
}

/* ------------------------------------------------------------------ */
/* Message handling                                                    */
/* ------------------------------------------------------------------ */

static int
handle_hello(ServerSession *s, const XtkMessage *msg, uint64_t now_ms)
{
	const XtkHello *h = &msg->body.hello;

	if (s->state != SERVER_SESSION_HANDSHAKE) {
		emit_protocol_error(s, XTK_ERR_BAD_STATE, s->state,
				    XTK_MSG_HELLO);
		return XTK_ERR_BAD_STATE;
	}
	if (h->proto_major != XTK_PROTOCOL_MAJOR ||
	    h->proto_minor != XTK_PROTOCOL_MINOR) {
		emit_reject(s, XTK_ERR_VERSION_MISMATCH,
			    "unsupported protocol version");
		s->state = SERVER_SESSION_DISCONNECTED;
		return XTK_ERR_VERSION_MISMATCH;
	}
	if (memcmp(h->content_set_digest, xtk_content_set_digest,
		   XTK_SHA256_BYTES) != 0) {
		/* Content mismatch fails before any catalog is sent; no download. */
		emit_reject(s, XTK_ERR_CONTENT_MISMATCH,
			    "content set does not match the server");
		s->state = SERVER_SESSION_DISCONNECTED;
		return XTK_ERR_CONTENT_MISMATCH;
	}

	(void) memset(s->name, 0, sizeof(s->name));
	(void) strncpy(s->name, h->name, XTK_MAX_NAME_BYTES);
	s->name[XTK_MAX_NAME_BYTES] = '\0';
	if (s->name[0] == '\0')
		(void) strcpy(s->name, "player");

	s->last_rx_ms = now_ms;
	emit_welcome(s, h->client_nonce);
	emit_catalog(s);
	s->state = SERVER_SESSION_LOBBY;
	return XTK_ERR_NONE;
}

static int
handle_config_select(ServerSession *s, const XtkMessage *msg)
{
	const XtkContentEntry *mz, *vh;
	XtkMessage out;
	uint16_t maze = msg->body.config_select.maze_id;
	uint16_t veh = msg->body.config_select.vehicle_id;

	if (s->state != SERVER_SESSION_LOBBY) {
		emit_protocol_error(s, XTK_ERR_BAD_STATE, s->state,
				    XTK_MSG_CONFIG_SELECT);
		return XTK_ERR_BAD_STATE;
	}
	mz = xtk_content_lookup(maze);
	vh = xtk_content_lookup(veh);
	if (mz == (const XtkContentEntry *) NULL ||
	    mz->kind != XTK_CONTENT_MAZE ||
	    vh == (const XtkContentEntry *) NULL ||
	    vh->kind != XTK_CONTENT_VEHICLE) {
		session_envelope(s, &out, XTK_MSG_CONFIG_STATUS, 0);
		out.body.config_status.maze_id = maze;
		out.body.config_status.vehicle_id = veh;
		out.body.config_status.status = 1;
		out.body.config_status.error_code = XTK_ERR_BAD_CONTENT_ID;
		(void) session_emit(s, &out);
		s->config_valid = 0;
		return XTK_ERR_NONE;
	}

	s->maze_id = maze;
	s->vehicle_id = veh;
	s->config_valid = 1;
	s->ready = 0;
	session_envelope(s, &out, XTK_MSG_CONFIG_STATUS, 0);
	out.body.config_status.maze_id = maze;
	out.body.config_status.vehicle_id = veh;
	out.body.config_status.status = 0;
	out.body.config_status.error_code = XTK_ERR_NONE;
	(void) session_emit(s, &out);
	return XTK_ERR_NONE;
}

static int
handle_ready(ServerSession *s)
{
	if (s->state != SERVER_SESSION_LOBBY) {
		emit_protocol_error(s, XTK_ERR_BAD_STATE, s->state,
				    XTK_MSG_READY);
		return XTK_ERR_BAD_STATE;
	}
	if (!s->config_valid) {
		emit_protocol_error(s, XTK_ERR_BAD_STATE, 0, XTK_MSG_READY);
		return XTK_ERR_BAD_STATE;
	}
	s->ready = 1;
	return XTK_ERR_NONE;
}

static int
handle_input(ServerSession *s, const XtkMessage *msg)
{
	if (s->state != SERVER_SESSION_RUNNING ||
	    s->net == (ServerNetSession *) NULL) {
		/* No controls outside Running. */
		emit_protocol_error(s, XTK_ERR_BAD_STATE, s->state,
				    msg->env.message_type);
		return XTK_ERR_BAD_STATE;
	}
	(void) server_net_admit_message(s->net, msg, frame + 1);
	return XTK_ERR_NONE;
}

static int
handle_reset(ServerSession *s, const XtkMessage *msg, uint64_t now_ms)
{
	if (s->state != SERVER_SESSION_RUNNING &&
	    s->state != SERVER_SESSION_RESULTS) {
		emit_protocol_error(s, XTK_ERR_BAD_STATE, s->state,
				    XTK_MSG_RESET);
		return XTK_ERR_BAD_STATE;
	}
	if (!s->is_admin) {
		emit_protocol_error(s, XTK_ERR_NOT_ADMIN, 0, XTK_MSG_RESET);
		return XTK_ERR_NOT_ADMIN;
	}
	if (msg->body.reset.prior_epoch != s->epoch) {
		emit_protocol_error(s, XTK_ERR_STALE_EPOCH,
				    (uint16_t) s->epoch, XTK_MSG_RESET);
		return XTK_ERR_STALE_EPOCH;
	}
	/* RESET is valid in Running and Results (PROTOCOL-v1 section 9): an
	 * admin may end a live epoch early.  Clean up the running match first
	 * so ownership and the world are released before the next epoch. */
	if (s->state == SERVER_SESSION_RUNNING)
		server_session_return_to_lobby(s);
	if (server_session_begin(s, (uint32_t) (frame + 1), now_ms) !=
	    SERVER_SESSION_OK) {
		emit_protocol_error(s, XTK_ERR_BAD_STATE, s->state,
				    XTK_MSG_RESET);
		return XTK_ERR_BAD_STATE;
	}
	return XTK_ERR_NONE;
}

int
server_session_message(ServerSession *s, const XtkMessage *msg, uint64_t now_ms)
{
	if (s == (ServerSession *) NULL || !s->used ||
	    msg == (const XtkMessage *) NULL)
		return XTK_ERR_BAD_STATE;

	switch (msg->env.message_type) {
	  case XTK_MSG_PING: {
		XtkMessage pong;

		session_envelope(s, &pong, XTK_MSG_PONG, frame);
		pong.body.ping.nonce = msg->body.ping.nonce;
		(void) session_emit(s, &pong);
		s->last_rx_ms = now_ms;
		return XTK_ERR_NONE;
	  }
	  case XTK_MSG_QUIT:
		s->state = SERVER_SESSION_DISCONNECTED;
		return XTK_ERR_NONE;
	  case XTK_MSG_HELLO:
		return handle_hello(s, msg, now_ms);
	  case XTK_MSG_CONFIG_SELECT:
		s->last_rx_ms = now_ms;
		return handle_config_select(s, msg);
	  case XTK_MSG_READY:
		s->last_rx_ms = now_ms;
		return handle_ready(s);
	  case XTK_MSG_RESET:
		s->last_rx_ms = now_ms;
		return handle_reset(s, msg, now_ms);
	  case XTK_MSG_INPUT_BATCH:
	  case XTK_MSG_RELIABLE_CMD:
		s->last_rx_ms = now_ms;
		return handle_input(s, msg);
	  default:
		emit_protocol_error(s, XTK_ERR_BAD_STATE, s->state,
				    msg->env.message_type);
		return XTK_ERR_BAD_STATE;
	}
}

/* ------------------------------------------------------------------ */
/* Match lifecycle                                                     */
/* ------------------------------------------------------------------ */

int
server_session_can_start(const ServerSession *s)
{
	return s != (const ServerSession *) NULL && s->used && s->is_admin &&
	       s->state == SERVER_SESSION_LOBBY && s->config_valid && s->ready;
}

static void
build_combatants(int vdesc_index, const char *human, int human_binding)
{
	Combatant *c;

	num_combatants = 2;

	c = &combatant[0];
	c->num_players = 1;
	c->player[0] = (human_binding >= 0) ? human_binding : 255;
	c->num_programs = 0;
	c->team = 1 % MAX_TEAMS;
	c->vdesc = vdesc_index;
	(void) strncpy(c->name, human, MAX_STRING - 1);
	c->name[MAX_STRING - 1] = '\0';

	c = &combatant[1];
	c->num_players = 0;
	c->num_programs = 1;
	c->program[0] = 0;		/* drone is the only shipped program */
	c->team = 2 % MAX_TEAMS;
	c->vdesc = vdesc_index;
	(void) strncpy(c->name, "drone", MAX_STRING - 1);
	c->name[MAX_STRING - 1] = '\0';
}

int
server_session_begin(ServerSession *s, uint32_t start_tick, uint64_t now_ms)
{
	const XtkContentEntry *mz, *vh;
	XtkMessage m;
	int vdesc_index, mdesc_index, hb;
	unsigned int gen;

	if (s == (ServerSession *) NULL || !s->used)
		return SERVER_SESSION_ERR_STATE;
	if (!s->is_admin)
		return SERVER_SESSION_ERR_NOT_ADMIN;
	if (s->state == SERVER_SESSION_RUNNING)
		return SERVER_SESSION_ERR_STATE;
	if (!s->config_valid)
		return SERVER_SESSION_ERR_STATE;

	mz = xtk_content_lookup(s->maze_id);
	vh = xtk_content_lookup(s->vehicle_id);
	if (mz == (const XtkContentEntry *) NULL ||
	    vh == (const XtkContentEntry *) NULL)
		return SERVER_SESSION_ERR_NO_CONTENT;
	if (make_vdesc((char *) vh->name, &vdesc_index) != DESC_LOADED)
		return SERVER_SESSION_ERR_NO_CONTENT;
	if (make_mdesc((char *) mz->name, &mdesc_index) != DESC_LOADED)
		return SERVER_SESSION_ERR_NO_CONTENT;

	settings.mdesc = &mdesc[mdesc_index];
	settings.si.game = settings.mdesc->type;

	/* Rebase the monotonic pacer for the new epoch (no-op in fast mode). */
	server_loop_epoch_begin();

	s->epoch++;
	s->ready = 0;

	server_players_init();
	hb = server_players_add(s->name, (int) s->session_id);
	if (hb < 0)
		hb = server_players_add("player", (int) s->session_id);
	if (hb < 0 || server_players_bind_slot(hb, 0) != 0)
		return SERVER_SESSION_ERR_MATCH;
	gen = server_players_binding(hb)->generation;

	build_combatants(vdesc_index, s->name, hb);

	server_events_reset(s->epoch, start_tick);
	if (server_match_init() != GAME_RUNNING)
		return SERVER_SESSION_ERR_MATCH;

	if (s->net == (ServerNetSession *) NULL)
		s->net = server_net_session_create(s->peer);
	if (s->net == (ServerNetSession *) NULL)
		return SERVER_SESSION_ERR_MATCH;
	(void) server_net_session_bind(s->net, (int) s->session_id, 0, gen);
	server_net_session_begin_epoch(s->net, s->epoch, (int) start_tick);

	s->bound_slot = 0;
	s->generation = gen;
	s->snapshot_seq = 0;
	s->events_sent = 0;
	s->epoch_live = 1;
	s->last_rx_ms = now_ms;
	s->state = SERVER_SESSION_RUNNING;

	session_envelope(s, &m, XTK_MSG_START, (int32_t) start_tick);
	m.env.epoch = s->epoch;
	m.body.start.start_tick = start_tick;
	m.body.start.maze_id = s->maze_id;
	m.body.start.vehicle_id = (uint8_t) s->vehicle_id;
	(void) session_emit(s, &m);

	session_envelope(s, &m, XTK_MSG_EPOCH_BEGIN, (int32_t) start_tick);
	m.env.epoch = s->epoch;
	m.body.epoch_begin.epoch = s->epoch;
	m.body.epoch_begin.first_tick = start_tick;
	(void) session_emit(s, &m);

	return SERVER_SESSION_OK;
}

static void
session_deliver(ServerSession *s, int status)
{
	XtkMessage m;

	/* Authorized snapshot on channel 1 (unreliable sequenced). */
	{
		static XtkViewSnapshot snap;
		uint16_t flags = (s->snapshot_seq == 0) ? XTK_FLAG_KEYFRAME : 0;

		if (server_view_export(s->bound_slot, 0, s->snapshot_seq, flags,
				       &snap) == SERVER_VIEW_OK) {
			session_envelope(s, &m, XTK_MSG_VIEW_SNAPSHOT, frame);
			m.env.epoch = s->epoch;
			m.body.view_snapshot = snap;
			m.body.view_snapshot.snapshot_sequence = s->snapshot_seq;
			m.body.view_snapshot.server_tick = (uint32_t) frame;
			m.body.view_snapshot.flags = flags;
			(void) session_emit(s, &m);
			s->snapshot_seq++;
		}
	}

	/* Pending events on channel 0 (reliable ordered), dedup by sequence. */
	{
		int n = server_events_count();
		int i;

		for (i = s->events_sent; i < n; i++) {
			const XtkEvent *e = server_events_event(i);

			if (e == (const XtkEvent *) NULL)
				continue;
			session_envelope(s, &m, XTK_MSG_EVENT, frame);
			m.env.epoch = s->epoch;
			m.body.event = *e;
			(void) session_emit(s, &m);
		}
		if (n > s->events_sent)
			s->events_sent = n;
	}
	(void) status;
}

static void
session_neutralize(ServerSession *s)
{
	Intent in;

	/* Neutral drive then neutral fire: stop, do not fire, at teardown. */
	(void) memset(&in, 0, sizeof(in));
	in.tick = frame + 1;
	in.player = s->bound_slot;
	in.sequence = 0;
	in.op = INTENT_DRIVE_REL;
	in.a = 0.0;
	(void) intent_push(&in);
	in.sequence = 1;
	in.op = INTENT_FIRE_HOLD;
	in.a = 0.0;
	(void) intent_push(&in);
}

int
server_session_step(ServerSession *s, uint64_t now_ms)
{
	int status;

	if (s == (ServerSession *) NULL || s->state != SERVER_SESSION_RUNNING)
		return SERVER_SESSION_ERR_STATE;

	/* D2-F solo abort: two seconds without valid traffic. */
	if (s->last_rx_ms != 0 &&
	    now_ms - s->last_rx_ms > (uint64_t) XTK_NEUTRAL_TIMEOUT_MS) {
		session_neutralize(s);
		server_session_finish(s, GAME_OVER);
		server_session_return_to_lobby(s);
		return GAME_OVER;
	}

	status = server_step();
	server_events_observe();
	server_net_post_step(frame);
	session_deliver(s, status);

	if (status != GAME_RUNNING)
		server_session_finish(s, status);
	return status;
}

void
server_session_finish(ServerSession *s, int status)
{
	XtkMessage m;
	XtkResults r;

	if (s == (ServerSession *) NULL || !s->used)
		return;
	if (s->state != SERVER_SESSION_RUNNING)
		return;

	/* Finalize once through the shared result path (captures results before
	 * the per-match score reset); no graphical stats wait. */
	(void) game_results(status);

	if (server_events_results_valid() &&
	    server_events_results(&r) == SERVER_EVENTS_OK) {
		session_envelope(s, &m, XTK_MSG_RESULTS, (int32_t) r.end_tick);
		m.env.epoch = s->epoch;
		m.body.results = r;
		(void) session_emit(s, &m);
	}
	s->state = SERVER_SESSION_RESULTS;
}

void
server_session_return_to_lobby(ServerSession *s)
{
	if (s == (ServerSession *) NULL || !s->used)
		return;
	server_match_reset();
	if (s->bound_slot >= 0)
		server_players_unbind_slot(s->bound_slot);
	s->epoch_live = 0;
	s->ready = 0;
	s->state = SERVER_SESSION_LOBBY;
}

/* ------------------------------------------------------------------ */
/* Bounded service and listen loop                                     */
/* ------------------------------------------------------------------ */

int
server_session_service(uint64_t now_ms)
{
	Transport *t = session_host;
	ServerSession *s;
	TransportPeer *p;
	int events = 0;

	if (t == (Transport *) NULL)
		return 0;

	events = transport_service(t, (int) SERVER_SESSION_STEP_BUDGET);
	if (events < 0)
		events = 0;

	while (transport_poll_connect(t, &p)) {
		if (server_session_count() > 0) {
			/* One-peer slice: reject the second peer explicitly. */
			server_session_reject_peer(t, p, XTK_ERR_SESSION_FULL,
						   "session full");
		} else {
			(void) server_session_accept(p);
		}
	}
	while (transport_poll_disconnect(t, &p)) {
		s = session_for_peer(p);
		if (s != (ServerSession *) NULL)
			server_session_destroy(s);
	}

	for (;;) {
		uint8_t buf[TRANSPORT_MAX_PACKET];
		size_t len = 0;
		uint8_t ch = 0;
		XtkMessage msg;
		int r = transport_receive(t, &p, buf, sizeof(buf), &len, &ch);

		if (r <= 0)
			break;
		s = session_for_peer(p);
		if (s == (ServerSession *) NULL)
			continue;
		if (xtk_decode(buf, len, &msg) != XTK_ERR_NONE)
			continue;
		(void) server_session_message(s, &msg, now_ms);
	}

	s = server_session_current();
	if (s != (ServerSession *) NULL &&
	    s->state == SERVER_SESSION_DISCONNECTED)
		server_session_destroy(s);

	/* Start automatically once admin + config + ready. */
	s = server_session_current();
	if (s != (ServerSession *) NULL && server_session_can_start(s))
		(void) server_session_begin(s, (uint32_t) (frame + 1), now_ms);

	return events;
}

int
server_session_run(uint16_t port, int fast, long max_ticks)
{
	Transport *t;
	long epoch_ticks = 0;
	int had_peer = 0;

	server_session_init();

	t = transport_create(port, SERVER_SESSION_MAX_PEERS);
	if (t == (Transport *) NULL) {
		fprintf(stderr, "xtank-server: cannot listen on port %u\n",
			(unsigned) port);
		return 1;
	}
	server_session_set_transport(t);

	printf("SERVER-LISTEN port=%u hz=%d\n", (unsigned) transport_port(t),
	       server_loop_hz());
	(void) fflush(stdout);

	for (;;) {
		ServerSession *s;
		uint64_t now = now_ms();

		(void) server_session_service(now);

		s = server_session_current();
		if (s == (ServerSession *) NULL) {
			struct timespec ts = { 0, 5000000 };

			if (had_peer) {
				/* The peer quit; the listen run is complete. */
				break;
			}
			(void) nanosleep(&ts, NULL);
			continue;
		}

		had_peer = 1;

		if (s == (ServerSession *) NULL ||
		    s->state != SERVER_SESSION_RUNNING) {
			struct timespec ts = { 0, 5000000 };

			(void) nanosleep(&ts, NULL);
			continue;
		}

		if (s->snapshot_seq == 0)
			epoch_ticks = 0;
		if (!fast)
			server_loop_pace(frame + 1);
		{
			int status = server_session_step(s, now_ms());

			epoch_ticks++;
			if (status == GAME_RUNNING && max_ticks > 0 &&
			    epoch_ticks >= max_ticks) {
				/* Bounded match: finalize through the results
				 * path, then return to the lobby. */
				server_session_finish(s, GAME_OVER);
				server_session_return_to_lobby(s);
			}
		}
	}

	transport_destroy(t);
	return 0;
}

/* end of server_session.c */