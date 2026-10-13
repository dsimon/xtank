/*
 * server_session.c -- shared lobby and one-world match coordinator (P3.4).
 *
 * See Src/Include/server_session.h for the contract.  This module is the glue
 * that makes the P2.6 event/result exporter and the P2.7 admission layer part
 * of the shipped two-human server.
 *
 * P3.4 splits the Phase 2 singleton into:
 *
 *   - a shared coordinator (one world): state, epoch, config revision, maze /
 *     Vanguard / drone selection, the administrator index and the single match
 *     step/results/reset transaction; and
 *   - two bounded peer records: connection/controller identity, name,
 *     readiness, transport/admission handle, bound slot/generation, reply log
 *     and delivery cursors.
 *
 * One start transaction, one world init, one server_step() per tick and one
 * results finalization/reset run regardless of peer count.  The listen service
 * stays alive through empty-session lobby cycles.
 *
 * Everything here runs on the simulation thread.  With no transport attached
 * (transport == NULL) the state machine is driven directly, which is what the
 * P3.4 acceptance fixture does.
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

/*
 * Per-peer record.  Everything that is not shared world state lives here; the
 * shared coordinator below owns the one world, epoch, config and administrator.
 */
struct ServerSession {
	int used;
	int index;
	int in_handshake;	/* 1 until a valid HELLO is accepted */
	int disconnected;	/* QUIT or fatal peer error; service reclaims */

	uint32_t session_id;		/* stable connection identity */
	uint32_t controller_id;		/* stable controller identity */

	char name[XTK_MAX_NAME_BYTES + 1];

	int ready;			/* readiness against the live revision */
	int binding;			/* ServerHumanBinding index, -1 none */

	Transport *t;
	TransportPeer *peer;

	ServerNetSession *net;
	int bound_slot;
	unsigned int generation;

	uint64_t last_rx_ms;

	/* D3-E fair per-peer receive budget for the current tick. */
	int rx_tick;
	int rx_events;

	uint32_t snapshot_seq;
	int events_sent;

	/* bounded encoded control-reply log (inspection/tests) */
	int reply_count;
	size_t reply_len[SERVER_SESSION_REPLY_MAX];
	uint8_t reply_buf[SERVER_SESSION_REPLY_MAX][SERVER_SESSION_REPLY_BYTES];
};

/* Shared coordinator: exactly one world/config/epoch/administrator. */
static struct {
	ServerSessionState state;
	uint32_t epoch;
	int epoch_live;

	uint16_t maze_id;
	uint16_t vehicle_id;
	uint8_t drone_count;
	int config_valid;
	uint32_t config_revision;

	int admin_index;

	uint32_t next_session_id;
	uint32_t next_controller_id;
} lobby;

static ServerSession peers[SERVER_SESSION_MAX_PEERS];
static int n_sessions;
static int session_capacity = SERVER_SESSION_SOLO_PEERS;
static Transport *session_host;

static uint8_t txbuf[XTK_MAX_MESSAGE_BYTES];

static uint64_t
now_ms(void)
{
	struct timespec ts;

	(void) clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t) ts.tv_sec * 1000ULL + (uint64_t) ts.tv_nsec / 1000000ULL;
}

static int
peer_active(const ServerSession *s)
{
	return s != (const ServerSession *) NULL && s->used &&
	       !s->in_handshake && !s->disconnected;
}

/* Find the peer record owning `peer` (or the first live peer). */
static ServerSession *
session_for_peer(TransportPeer *peer)
{
	int i;

	for (i = 0; i < SERVER_SESSION_MAX_PEERS; i++) {
		if (!peers[i].used)
			continue;
		if (peer == NULL || peers[i].peer == peer)
			return &peers[i];
	}
	return (ServerSession *) NULL;
}

/* ------------------------------------------------------------------ */
/* Process lifecycle                                                   */
/* ------------------------------------------------------------------ */

void
server_session_init(void)
{
	(void) memset(peers, 0, sizeof(peers));
	n_sessions = 0;
	session_host = (Transport *) NULL;

	(void) memset(&lobby, 0, sizeof(lobby));
	lobby.state = SERVER_SESSION_LOBBY;
	lobby.admin_index = -1;
	lobby.next_session_id = 1;
	lobby.next_controller_id = 1;
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

int
server_session_capacity(void)
{
	return session_capacity;
}

void
server_session_set_capacity(int capacity)
{
	if (capacity < SERVER_SESSION_SOLO_PEERS)
		capacity = SERVER_SESSION_SOLO_PEERS;
	if (capacity > SERVER_SESSION_MAX_PEERS)
		capacity = SERVER_SESSION_MAX_PEERS;
	session_capacity = capacity;
}

ServerSession *
server_session_current(void)
{
	int i;

	for (i = 0; i < SERVER_SESSION_MAX_PEERS; i++) {
		if (peers[i].used)
			return &peers[i];
	}
	return (ServerSession *) NULL;
}

ServerSession *
server_session_peer_at(int index)
{
	if (index < 0 || index >= SERVER_SESSION_MAX_PEERS || !peers[index].used)
		return (ServerSession *) NULL;
	return &peers[index];
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
	if (s == (const ServerSession *) NULL || !s->used)
		return SERVER_SESSION_DISCONNECTED;
	if (s->disconnected)
		return SERVER_SESSION_DISCONNECTED;
	if (s->in_handshake)
		return SERVER_SESSION_HANDSHAKE;
	return lobby.state;
}

uint32_t
server_session_id(const ServerSession *s)
{
	return (s != (const ServerSession *) NULL) ? s->session_id : 0;
}

uint32_t
server_session_controller_id(const ServerSession *s)
{
	return (s != (const ServerSession *) NULL) ? s->controller_id : 0;
}

int
server_session_index(const ServerSession *s)
{
	return (s != (const ServerSession *) NULL) ? s->index : -1;
}

uint32_t
server_session_epoch(const ServerSession *s)
{
	(void) s;
	return lobby.epoch;
}

int
server_session_is_admin(const ServerSession *s)
{
	return s != (const ServerSession *) NULL && s->used &&
	       !s->disconnected && lobby.admin_index == s->index;
}

int
server_session_ready(const ServerSession *s)
{
	return (s != (const ServerSession *) NULL) && s->ready;
}

uint16_t
server_session_maze_id(const ServerSession *s)
{
	(void) s;
	return lobby.maze_id;
}

uint16_t
server_session_vehicle_id(const ServerSession *s)
{
	(void) s;
	return lobby.vehicle_id;
}

int
server_session_drone_count(const ServerSession *s)
{
	(void) s;
	return lobby.drone_count;
}

uint32_t
server_session_config_revision(const ServerSession *s)
{
	(void) s;
	return lobby.config_revision;
}

int
server_session_config_valid(const ServerSession *s)
{
	(void) s;
	return lobby.config_valid;
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

int
server_session_all_ready(const ServerSession *s)
{
	int i, admitted = 0;

	(void) s;
	for (i = 0; i < SERVER_SESSION_MAX_PEERS; i++) {
		if (!peer_active(&peers[i]))
			continue;
		admitted++;
		if (!peers[i].ready)
			return 0;
	}
	return admitted > 0;
}

void
server_session_set_admin(ServerSession *s, int admin)
{
	if (s == (ServerSession *) NULL || !s->used)
		return;
	lobby.admin_index = admin ? s->index : -1;
}

/* ------------------------------------------------------------------ */
/* Outbound messages                                                   */
/* ------------------------------------------------------------------ */

/* Encode, send (when attached) and log a message in the peer reply ring. */
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
	m->env.epoch = lobby.epoch_live ? lobby.epoch : 0;
	m->env.tick = tick;
}

static void
emit_welcome(ServerSession *s, uint32_t server_nonce)
{
	XtkMessage m;

	session_envelope(s, &m, XTK_MSG_WELCOME, 0);
	m.body.welcome.server_nonce = server_nonce;
	m.body.welcome.role = 1;		/* player; never administrator */
	m.body.welcome.tick_hz = (uint8_t) XTK_SERVER_TICK_HZ;
	m.body.welcome.max_players = (uint8_t) XTK_MAX_PLAYERS;
	m.body.welcome.human_capacity = (uint8_t) session_capacity;
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

static uint8_t
wire_session_state(void)
{
	switch (lobby.state) {
	  case SERVER_SESSION_RUNNING: return (uint8_t) XTK_SESSION_RUNNING;
	  case SERVER_SESSION_RESULTS: return (uint8_t) XTK_SESSION_RESULTS;
	  default:                     return (uint8_t) XTK_SESSION_LOBBY;
	}
}

/* One LOBBY_STATE for a specific recipient (self_index differs per peer). */
static void
emit_lobby_state(ServerSession *to)
{
	XtkMessage m;
	int i, idx = 0;

	session_envelope(to, &m, XTK_MSG_LOBBY_STATE, frame);
	m.body.lobby_state.config_revision = lobby.config_revision;
	m.body.lobby_state.session_state = wire_session_state();
	m.body.lobby_state.drone_count = lobby.drone_count;
	m.body.lobby_state.maze_id = lobby.maze_id;
	m.body.lobby_state.vehicle_id = lobby.vehicle_id;
	m.body.lobby_state.self_index = (uint8_t) to->index;
	m.body.lobby_state.admin_index =
		(lobby.admin_index >= 0) ? (uint8_t) lobby.admin_index : 0xFF;

	for (i = 0; i < SERVER_SESSION_MAX_PEERS; i++) {
		ServerSession *p = &peers[i];
		XtkRosterEntry *e;

		if (!peer_active(p))
			continue;
		e = &m.body.lobby_state.entry[idx++];
		e->connection_id = p->session_id;
		e->controller_id = p->controller_id;
		e->slot = (p->bound_slot >= 0) ? (uint8_t) p->bound_slot : 0xFF;
		e->ready = (uint8_t) p->ready;
		e->flags = 0;
		e->departure_reason = 0;
		(void) memset(e->name, 0, sizeof(e->name));
		(void) strncpy(e->name, p->name, XTK_MAX_NAME_BYTES);
	}
	m.body.lobby_state.human_count = (uint8_t) idx;
	(void) session_emit(to, &m);
}

static void
broadcast_lobby_state(void)
{
	int i;

	for (i = 0; i < SERVER_SESSION_MAX_PEERS; i++) {
		if (peer_active(&peers[i]))
			emit_lobby_state(&peers[i]);
	}
}

/* ------------------------------------------------------------------ */
/* Message handling                                                    */
/* ------------------------------------------------------------------ */

static int
handle_hello(ServerSession *s, const XtkMessage *msg, uint64_t t)
{
	const XtkHello *h = &msg->body.hello;
	char name[XTK_MAX_NAME_BYTES + 1];
	int i;

	if (!s->in_handshake) {
		emit_protocol_error(s, XTK_ERR_BAD_STATE, s->index, XTK_MSG_HELLO);
		return XTK_ERR_BAD_STATE;
	}
	if (h->proto_major != XTK_PROTOCOL_MAJOR ||
	    h->proto_minor != XTK_PROTOCOL_MINOR) {
		emit_reject(s, XTK_ERR_VERSION_MISMATCH,
			    "unsupported protocol version");
		s->disconnected = 1;
		return XTK_ERR_VERSION_MISMATCH;
	}
	if (memcmp(h->content_set_digest, xtk_content_set_digest,
		   XTK_SHA256_BYTES) != 0) {
		emit_reject(s, XTK_ERR_CONTENT_MISMATCH,
			    "content set does not match the server");
		s->disconnected = 1;
		return XTK_ERR_CONTENT_MISMATCH;
	}

	(void) memset(name, 0, sizeof(name));
	(void) strncpy(name, h->name, XTK_MAX_NAME_BYTES);
	name[XTK_MAX_NAME_BYTES] = '\0';
	if (name[0] == '\0')
		(void) strcpy(name, "player");

	/* D3-E: duplicate names are rejected; connection identity is separate. */
	for (i = 0; i < SERVER_SESSION_MAX_PEERS; i++) {
		if (&peers[i] == s || !peer_active(&peers[i]))
			continue;
		if (strcmp(peers[i].name, name) == 0) {
			emit_reject(s, XTK_ERR_DUPLICATE_NAME,
				    "player name already in use");
			s->disconnected = 1;
			return XTK_ERR_DUPLICATE_NAME;
		}
	}

	if (lobby.state != SERVER_SESSION_LOBBY) {
		emit_reject(s, XTK_ERR_SESSION_FULL, "a match is in progress");
		s->disconnected = 1;
		return XTK_ERR_SESSION_FULL;
	}

	(void) strncpy(s->name, name, XTK_MAX_NAME_BYTES);
	s->name[XTK_MAX_NAME_BYTES] = '\0';
	s->last_rx_ms = t;
	s->in_handshake = 0;

	/* The first admitted (post-handshake) human administers; assigning here
	 * rather than at accept means a half-open connection can never hold the
	 * administrator role. */
	if (lobby.admin_index < 0)
		lobby.admin_index = s->index;

	emit_welcome(s, h->client_nonce);
	emit_catalog(s);
	broadcast_lobby_state();
	return XTK_ERR_NONE;
}

static int
handle_config_select(ServerSession *s, const XtkMessage *msg)
{
	const XtkContentEntry *mz, *vh;
	XtkMessage out;
	uint16_t maze = msg->body.config_select.maze_id;
	uint16_t veh = msg->body.config_select.vehicle_id;
	uint8_t drones = msg->body.config_select.drone_count;
	int i;

	if (lobby.state != SERVER_SESSION_LOBBY) {
		emit_protocol_error(s, XTK_ERR_BAD_STATE, lobby.state,
				    XTK_MSG_CONFIG_SELECT);
		return XTK_ERR_BAD_STATE;
	}
	/* Server-authoritative administration: only the administrator configures. */
	if (lobby.admin_index != s->index) {
		emit_protocol_error(s, XTK_ERR_NOT_ADMIN, 0,
				    XTK_MSG_CONFIG_SELECT);
		return XTK_ERR_NOT_ADMIN;
	}
	if (drones > XTK_MAX_DRONES)
		drones = XTK_MAX_DRONES;

	mz = xtk_content_lookup(maze);
	vh = xtk_content_lookup(veh);
	if (mz == (const XtkContentEntry *) NULL ||
	    mz->kind != XTK_CONTENT_MAZE ||
	    vh == (const XtkContentEntry *) NULL ||
	    vh->kind != XTK_CONTENT_VEHICLE) {
		session_envelope(s, &out, XTK_MSG_CONFIG_STATUS, 0);
		out.body.config_status.maze_id = maze;
		out.body.config_status.vehicle_id = veh;
		out.body.config_status.drone_count = drones;
		out.body.config_status.status = 1;
		out.body.config_status.error_code = XTK_ERR_BAD_CONTENT_ID;
		out.body.config_status.config_revision = lobby.config_revision;
		(void) session_emit(s, &out);
		return XTK_ERR_NONE;
	}

	lobby.maze_id = maze;
	lobby.vehicle_id = veh;
	lobby.drone_count = drones;
	lobby.config_valid = 1;
	lobby.config_revision++;
	/* D3-B: a config change wipes every peer's readiness. */
	for (i = 0; i < SERVER_SESSION_MAX_PEERS; i++)
		peers[i].ready = 0;

	session_envelope(s, &out, XTK_MSG_CONFIG_STATUS, 0);
	out.body.config_status.maze_id = maze;
	out.body.config_status.vehicle_id = veh;
	out.body.config_status.drone_count = drones;
	out.body.config_status.status = 0;
	out.body.config_status.error_code = XTK_ERR_NONE;
	out.body.config_status.config_revision = lobby.config_revision;
	(void) session_emit(s, &out);
	broadcast_lobby_state();
	return XTK_ERR_NONE;
}

static int
handle_ready(ServerSession *s, const XtkMessage *msg)
{
	if (lobby.state != SERVER_SESSION_LOBBY) {
		emit_protocol_error(s, XTK_ERR_BAD_STATE, lobby.state,
				    XTK_MSG_READY);
		return XTK_ERR_BAD_STATE;
	}
	if (!lobby.config_valid) {
		emit_protocol_error(s, XTK_ERR_BAD_STATE, 0, XTK_MSG_READY);
		return XTK_ERR_BAD_STATE;
	}
	if (msg->body.ready.config_revision != lobby.config_revision) {
		emit_protocol_error(s, XTK_ERR_STALE_CONFIG,
				    (uint16_t) lobby.config_revision,
				    XTK_MSG_READY);
		return XTK_ERR_STALE_CONFIG;
	}
	s->ready = msg->body.ready.ready ? 1 : 0;
	broadcast_lobby_state();
	return XTK_ERR_NONE;
}

static int
handle_start_request(ServerSession *s, const XtkMessage *msg, uint64_t t)
{
	if (lobby.state != SERVER_SESSION_LOBBY) {
		emit_protocol_error(s, XTK_ERR_BAD_STATE, lobby.state,
				    XTK_MSG_START_REQUEST);
		return XTK_ERR_BAD_STATE;
	}
	if (lobby.admin_index != s->index) {
		emit_protocol_error(s, XTK_ERR_NOT_ADMIN, 0,
				    XTK_MSG_START_REQUEST);
		return XTK_ERR_NOT_ADMIN;
	}
	if (msg->body.start_request.expected_config_revision !=
	    lobby.config_revision) {
		emit_protocol_error(s, XTK_ERR_STALE_CONFIG,
				    (uint16_t) lobby.config_revision,
				    XTK_MSG_START_REQUEST);
		return XTK_ERR_STALE_CONFIG;
	}
	if (!server_session_all_ready(s)) {
		emit_protocol_error(s, XTK_ERR_NOT_READY, 0,
				    XTK_MSG_START_REQUEST);
		return XTK_ERR_NOT_READY;
	}
	if (server_session_begin(s, (uint32_t) (frame + 1), t) !=
	    SERVER_SESSION_OK) {
		emit_protocol_error(s, XTK_ERR_BAD_STATE, lobby.state,
				    XTK_MSG_START_REQUEST);
		return XTK_ERR_BAD_STATE;
	}
	return XTK_ERR_NONE;
}

/* Release the current epoch's world and per-peer match ownership. */
static void
shared_clear_match(void)
{
	int i;

	server_match_reset();
	for (i = 0; i < SERVER_SESSION_MAX_PEERS; i++) {
		ServerSession *p = &peers[i];

		if (!p->used)
			continue;
		if (p->bound_slot >= 0)
			server_players_unbind_slot(p->bound_slot);
		p->bound_slot = -1;
		p->generation = 0;
		p->snapshot_seq = 0;
		p->events_sent = 0;
		p->ready = 0;
	}
	lobby.epoch_live = 0;
	lobby.state = SERVER_SESSION_LOBBY;
}

static int
handle_reset(ServerSession *s, const XtkMessage *msg, uint64_t t)
{
	if (lobby.state != SERVER_SESSION_RUNNING &&
	    lobby.state != SERVER_SESSION_RESULTS) {
		emit_protocol_error(s, XTK_ERR_BAD_STATE, lobby.state,
				    XTK_MSG_RESET);
		return XTK_ERR_BAD_STATE;
	}
	if (lobby.admin_index != s->index) {
		emit_protocol_error(s, XTK_ERR_NOT_ADMIN, 0, XTK_MSG_RESET);
		return XTK_ERR_NOT_ADMIN;
	}
	if (msg->body.reset.prior_epoch != lobby.epoch) {
		emit_protocol_error(s, XTK_ERR_STALE_EPOCH,
				    (uint16_t) lobby.epoch, XTK_MSG_RESET);
		return XTK_ERR_STALE_EPOCH;
	}
	(void) t;
	/* A reset from a live match is a labeled abort. */
	if (lobby.state == SERVER_SESSION_RUNNING)
		server_session_finish(s, GAME_OVER);
	shared_clear_match();
	broadcast_lobby_state();
	return XTK_ERR_NONE;
}

static int
handle_input(ServerSession *s, const XtkMessage *msg, uint8_t channel,
	     uint64_t t)
{
	int accepted;

	if (lobby.state != SERVER_SESSION_RUNNING ||
	    s->net == (ServerNetSession *) NULL) {
		emit_protocol_error(s, XTK_ERR_BAD_STATE, lobby.state,
				    msg->env.message_type);
		return XTK_ERR_BAD_STATE;
	}

	/* The actual transport channel is validated before any intent. */
	if (channel != xtk_next_channel(msg->env.message_type)) {
		emit_protocol_error(s, XTK_ERR_BAD_CHANNEL, channel,
				    msg->env.message_type);
		return XTK_ERR_BAD_CHANNEL;
	}

	/*
	 * Only valid, fresh traffic counts as alive (D3-E): a saturated,
	 * duplicate, stale, foreign or malformed batch admits nothing and must
	 * not extend the valid-input timeout.  Heartbeats/NONE count as valid
	 * liveness traffic without an engine effect.
	 */
	accepted = server_net_admit_message_on(s->net, msg, channel, frame + 1);
	if (accepted > 0)
		s->last_rx_ms = t;
	return XTK_ERR_NONE;
}

int
server_session_message_ch(ServerSession *s, const XtkMessage *msg,
			  uint8_t channel, uint64_t t)
{
	if (s == (ServerSession *) NULL || !s->used ||
	    msg == (const XtkMessage *) NULL || s->disconnected)
		return XTK_ERR_BAD_STATE;

	/*
	 * A peer that has not completed a valid HELLO is only allowed to
	 * handshake, ping or quit.  It must not be able to configure the shared
	 * lobby, set readiness, start/reset or inject input before it is admitted.
	 */
	if (s->in_handshake &&
	    msg->env.message_type != XTK_MSG_HELLO &&
	    msg->env.message_type != XTK_MSG_PING &&
	    msg->env.message_type != XTK_MSG_QUIT) {
		emit_protocol_error(s, XTK_ERR_BAD_STATE, 0,
				    msg->env.message_type);
		return XTK_ERR_BAD_STATE;
	}

	/*
	 * Lobby traffic only refreshes the liveness clock when it is actually
	 * accepted; a rejected/stale message must not extend the timeout.
	 */
	switch (msg->env.message_type) {
	  case XTK_MSG_PING: {
		XtkMessage pong;

		session_envelope(s, &pong, XTK_MSG_PONG, frame);
		pong.body.ping.nonce = msg->body.ping.nonce;
		(void) session_emit(s, &pong);
		s->last_rx_ms = t;
		return XTK_ERR_NONE;
	  }
	  case XTK_MSG_QUIT:
		s->disconnected = 1;
		return XTK_ERR_NONE;
	  case XTK_MSG_HELLO:
		return handle_hello(s, msg, t);
	  case XTK_MSG_CONFIG_SELECT: {
		int r = handle_config_select(s, msg);

		if (r == XTK_ERR_NONE)
			s->last_rx_ms = t;
		return r;
	  }
	  case XTK_MSG_READY: {
		int r = handle_ready(s, msg);

		if (r == XTK_ERR_NONE)
			s->last_rx_ms = t;
		return r;
	  }
	  case XTK_MSG_START_REQUEST: {
		int r = handle_start_request(s, msg, t);

		if (r == XTK_ERR_NONE)
			s->last_rx_ms = t;
		return r;
	  }
	  case XTK_MSG_RESET: {
		int r = handle_reset(s, msg, t);

		if (r == XTK_ERR_NONE)
			s->last_rx_ms = t;
		return r;
	  }
	  case XTK_MSG_INPUT_BATCH:
	  case XTK_MSG_RELIABLE_CMD:
		return handle_input(s, msg, channel, t);
	  default:
		emit_protocol_error(s, XTK_ERR_BAD_STATE, lobby.state,
				    msg->env.message_type);
		return XTK_ERR_BAD_STATE;
	}
}

int
server_session_message(ServerSession *s, const XtkMessage *msg, uint64_t t)
{
	if (msg == (const XtkMessage *) NULL)
		return server_session_message_ch(s, msg, 0, t);
	return server_session_message_ch(s, msg,
					 xtk_next_channel(msg->env.message_type),
					 t);
}

/* ------------------------------------------------------------------ */
/* Session admission                                                   */
/* ------------------------------------------------------------------ */

ServerSession *
server_session_accept(TransportPeer *peer)
{
	ServerSession *s;
	int i;

	/* Lobby-only joins: a running/results epoch admits no new human. */
	if (lobby.state != SERVER_SESSION_LOBBY)
		return (ServerSession *) NULL;
	if (n_sessions >= session_capacity)
		return (ServerSession *) NULL;

	for (i = 0; i < SERVER_SESSION_MAX_PEERS; i++) {
		if (!peers[i].used)
			break;
	}
	if (i >= SERVER_SESSION_MAX_PEERS)
		return (ServerSession *) NULL;

	s = &peers[i];
	(void) memset(s, 0, sizeof(*s));
	s->used = 1;
	s->index = i;
	s->in_handshake = 1;
	s->session_id = lobby.next_session_id++;
	/*
	 * Bounded controller identity: hand out a u32 id inside the
	 * non-negative range and wrap back to 0 rather than overflowing into a
	 * value that a signed int cast would turn negative.  The wrapped value
	 * is only ever reused after 2^31 assignments and any stale owner is
	 * rejected by the binding generation check.
	 */
	s->controller_id = lobby.next_controller_id;
	if (lobby.next_controller_id >= (uint32_t) INT32_MAX)
		lobby.next_controller_id = 0;
	else
		lobby.next_controller_id++;
	s->binding = -1;
	s->t = session_host;
	s->peer = peer;
	s->bound_slot = -1;
	n_sessions++;
	/* Administrator is assigned on successful HELLO (handle_hello), so a
	 * half-open connection cannot hold the shared administrator role. */
	return s;
}

void
server_session_destroy(ServerSession *s)
{
	int i, was_admin;

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

	was_admin = (lobby.admin_index == s->index);
	s->used = 0;
	if (n_sessions > 0)
		n_sessions--;

	if (was_admin) {
		lobby.admin_index = -1;
		for (i = 0; i < SERVER_SESSION_MAX_PEERS; i++) {
			if (peer_active(&peers[i])) {
				lobby.admin_index = i;
				break;
			}
		}
	}

	if (n_sessions == 0) {
		/* Empty server: clean the world and wait in the lobby. */
		if (lobby.state != SERVER_SESSION_LOBBY)
			server_match_reset();
		lobby.epoch_live = 0;
		lobby.state = SERVER_SESSION_LOBBY;
	} else {
		broadcast_lobby_state();
	}
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
/* Match lifecycle                                                     */
/* ------------------------------------------------------------------ */

int
server_session_can_start(const ServerSession *s)
{
	return lobby.state == SERVER_SESSION_LOBBY &&
	       lobby.config_valid && lobby.admin_index >= 0 &&
	       server_session_all_ready(s);
}

/* Solo fixes exactly one drone; two humans select 0..2 (D3-B). */
static uint8_t
effective_drones(int n_humans)
{
	uint8_t d;

	if (n_humans <= 1)
		return (uint8_t) XTK_SOLO_DRONES;
	d = lobby.drone_count;
	if (d > XTK_MAX_DRONES)
		d = XTK_MAX_DRONES;
	return d;
}

/* Bind every admitted peer to a deterministic slot (roster order -> slot). */
static int
bind_humans(int *out_count)
{
	int i, n = 0;

	for (i = 0; i < SERVER_SESSION_MAX_PEERS; i++) {
		ServerSession *p = &peers[i];
		int b;

		if (!peer_active(p))
			continue;
		b = server_players_add(p->name, p->controller_id);
		if (b < 0)
			return -1;
		if (server_players_bind_slot(b, n) != 0)
			return -1;
		p->binding = b;
		p->bound_slot = n;
		p->generation = server_players_binding(b)->generation;
		n++;
	}
	*out_count = n;
	return 0;
}

static void
build_combatants(int n_humans, int drones, int vdesc_index)
{
	int i, j;

	num_combatants = n_humans + drones;
	for (i = 0; i < MAX_VEHICLES; i++) {
		combatant[i].num_players = 0;
		combatant[i].num_programs = 0;
	}

	for (i = 0; i < SERVER_SESSION_MAX_PEERS; i++) {
		ServerSession *p = &peers[i];
		Combatant *c;
		int slot;

		if (p->bound_slot < 0)
			continue;
		slot = p->bound_slot;
		c = &combatant[slot];
		c->num_players = 1;
		c->player[0] = p->binding;
		c->num_programs = 0;
		c->team = (slot + 1) % MAX_TEAMS;
		c->vdesc = vdesc_index;
		(void) memset(c->name, 0, sizeof(c->name));
		(void) strncpy(c->name, p->name, MAX_STRING - 1);
		c->name[MAX_STRING - 1] = '\0';
	}

	for (j = 0; j < drones; j++) {
		Combatant *c = &combatant[n_humans + j];

		c->num_players = 0;
		c->num_programs = 1;
		c->program[0] = 0;	/* drone is the only shipped program */
		c->team = (n_humans + j + 1) % MAX_TEAMS;
		c->vdesc = vdesc_index;
		(void) memset(c->name, 0, sizeof(c->name));
		(void) strncpy(c->name, "drone", MAX_STRING - 1);
		c->name[MAX_STRING - 1] = '\0';
	}
}

static void
begin_unbind_all(void)
{
	int i;

	for (i = 0; i < SERVER_SESSION_MAX_PEERS; i++) {
		ServerSession *p = &peers[i];

		if (p->bound_slot >= 0)
			server_players_unbind_slot(p->bound_slot);
		p->bound_slot = -1;
		p->generation = 0;
	}
}

int
server_session_begin(ServerSession *s, uint32_t start_tick, uint64_t t)
{
	const XtkContentEntry *mz, *vh;
	XtkMessage m;
	int vdesc_index, mdesc_index;
	int n_humans = 0, i;
	uint8_t drones;

	if (lobby.state != SERVER_SESSION_LOBBY)
		return SERVER_SESSION_ERR_STATE;
	if (lobby.admin_index < 0)
		return SERVER_SESSION_ERR_NOT_ADMIN;
	if (s != (ServerSession *) NULL && s->used &&
	    s->index != lobby.admin_index)
		return SERVER_SESSION_ERR_NOT_ADMIN;
	if (!lobby.config_valid)
		return SERVER_SESSION_ERR_STATE;

	mz = xtk_content_lookup(lobby.maze_id);
	vh = xtk_content_lookup(lobby.vehicle_id);
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

	for (i = 0; i < SERVER_SESSION_MAX_PEERS; i++) {
		ServerSession *p = &peers[i];

		if (peer_active(p))
			n_humans++;
	}
	if (n_humans < 1)
		return SERVER_SESSION_ERR_STATE;
	drones = effective_drones(n_humans);

	lobby.epoch++;
	server_players_init();
	for (i = 0; i < SERVER_SESSION_MAX_PEERS; i++) {
		ServerSession *p = &peers[i];

		if (!p->used)
			continue;
		p->bound_slot = -1;
		p->generation = 0;
		p->snapshot_seq = 0;
		p->events_sent = 0;
		p->ready = 0;
	}

	if (bind_humans(&n_humans) != 0) {
		begin_unbind_all();
		lobby.epoch--;
		lobby.state = SERVER_SESSION_LOBBY;
		return SERVER_SESSION_ERR_MATCH;
	}
	build_combatants(n_humans, drones, vdesc_index);

	server_events_reset(lobby.epoch, start_tick);
	if (server_match_init() != GAME_RUNNING) {
		begin_unbind_all();
		server_match_reset();
		lobby.epoch--;
		lobby.state = SERVER_SESSION_LOBBY;
		return SERVER_SESSION_ERR_MATCH;
	}

	for (i = 0; i < SERVER_SESSION_MAX_PEERS; i++) {
		ServerSession *p = &peers[i];

		if (!peer_active(p))
			continue;
		if (p->net == (ServerNetSession *) NULL)
			p->net = server_net_session_create(p->peer);
		if (p->net == (ServerNetSession *) NULL) {
			begin_unbind_all();
			server_match_reset();
			lobby.epoch--;
			lobby.state = SERVER_SESSION_LOBBY;
			return SERVER_SESSION_ERR_MATCH;
		}
		(void) server_net_session_bind(p->net, (int) p->controller_id,
					       p->bound_slot, p->generation);
		server_net_session_begin_epoch(p->net, lobby.epoch,
					       (int) start_tick);
		p->last_rx_ms = t;
	}

	lobby.epoch_live = 1;
	lobby.state = SERVER_SESSION_RUNNING;

	for (i = 0; i < SERVER_SESSION_MAX_PEERS; i++) {
		ServerSession *p = &peers[i];

		if (!peer_active(p))
			continue;
		session_envelope(p, &m, XTK_MSG_START, (int32_t) start_tick);
		m.env.epoch = lobby.epoch;
		m.body.start.start_tick = start_tick;
		m.body.start.maze_id = lobby.maze_id;
		m.body.start.vehicle_id = lobby.vehicle_id;	/* full u16 (P3.4 fix) */
		m.body.start.drone_count = drones;
		m.body.start.config_revision = lobby.config_revision;
		(void) session_emit(p, &m);

		session_envelope(p, &m, XTK_MSG_EPOCH_BEGIN,
				 (int32_t) start_tick);
		m.env.epoch = lobby.epoch;
		m.body.epoch_begin.epoch = lobby.epoch;
		m.body.epoch_begin.first_tick = start_tick;
		(void) session_emit(p, &m);
	}
	return SERVER_SESSION_OK;
}

static void
session_deliver(ServerSession *p)
{
	XtkMessage m;

	if (p->bound_slot >= 0) {
		static XtkViewSnapshot snap;
		uint16_t flags = (p->snapshot_seq == 0) ? XTK_FLAG_KEYFRAME : 0;

		if (server_view_export(p->bound_slot, 0, p->snapshot_seq, flags,
				       &snap) == SERVER_VIEW_OK) {
			session_envelope(p, &m, XTK_MSG_VIEW_SNAPSHOT, frame);
			m.env.epoch = lobby.epoch;
			m.body.view_snapshot = snap;
			m.body.view_snapshot.snapshot_sequence = p->snapshot_seq;
			m.body.view_snapshot.server_tick = (uint32_t) frame;
			m.body.view_snapshot.flags = flags;
			(void) session_emit(p, &m);
			p->snapshot_seq++;
		}
	}

	{
		int n = server_events_count();
		int i;

		for (i = p->events_sent; i < n; i++) {
			const XtkEvent *e = server_events_event(i);

			if (e == (const XtkEvent *) NULL)
				continue;
			session_envelope(p, &m, XTK_MSG_EVENT, frame);
			m.env.epoch = lobby.epoch;
			m.body.event = *e;
			(void) session_emit(p, &m);
		}
		if (n > p->events_sent)
			p->events_sent = n;
	}
}

static void
session_neutralize(ServerSession *p)
{
	Intent in;

	if (p->bound_slot < 0)
		return;
	(void) memset(&in, 0, sizeof(in));
	in.tick = frame + 1;
	in.player = p->bound_slot;
	/*
	 * Neutralisation uses the top of the sequence space so it can never
	 * collide with a peer's own (low, monotonically increasing) admitted
	 * sequences, and so it is ordered last within the tick.
	 */
	in.sequence = 0xFFFFFFFEu;
	in.op = INTENT_DRIVE_REL;
	in.a = 0.0;
	(void) intent_push(&in);
	in.sequence = 0xFFFFFFFFu;
	in.op = INTENT_FIRE_HOLD;
	in.a = 0.0;
	(void) intent_push(&in);
}

int
server_session_step(ServerSession *s, uint64_t t)
{
	int status, i;

	(void) s;
	if (lobby.state != SERVER_SESSION_RUNNING)
		return SERVER_SESSION_ERR_STATE;

	/* D2-F timeout: two seconds without valid traffic from any peer. */
	for (i = 0; i < SERVER_SESSION_MAX_PEERS; i++) {
		ServerSession *p = &peers[i];

		if (!peer_active(p) || p->last_rx_ms == 0)
			continue;
		if (t - p->last_rx_ms > (uint64_t) XTK_NEUTRAL_TIMEOUT_MS) {
			session_neutralize(p);
			server_session_finish(s, GAME_OVER);
			server_session_return_to_lobby(s);
			return GAME_OVER;
		}
	}

	status = server_step();
	server_events_observe();
	server_net_post_step(frame);
	for (i = 0; i < SERVER_SESSION_MAX_PEERS; i++) {
		if (peer_active(&peers[i]))
			session_deliver(&peers[i]);
	}

	if (status != GAME_RUNNING)
		server_session_finish(s, status);
	return status;
}

void
server_session_finish(ServerSession *s, int status)
{
	XtkMessage m;
	XtkResults r;
	int i, valid;

	(void) s;
	if (lobby.state != SERVER_SESSION_RUNNING)
		return;

	/* Finalize once through the shared result path (captures results before
	 * the per-match score reset); no graphical stats wait. */
	(void) game_results(status);

	valid = server_events_results_valid() &&
		server_events_results(&r) == SERVER_EVENTS_OK;
	for (i = 0; i < SERVER_SESSION_MAX_PEERS; i++) {
		ServerSession *p = &peers[i];

		if (!peer_active(p) || !valid)
			continue;
		session_envelope(p, &m, XTK_MSG_RESULTS, (int32_t) r.end_tick);
		m.env.epoch = lobby.epoch;
		m.body.results = r;
		(void) session_emit(p, &m);
	}
	lobby.state = SERVER_SESSION_RESULTS;
}

void
server_session_return_to_lobby(ServerSession *s)
{
	(void) s;
	if (lobby.state == SERVER_SESSION_LOBBY)
		return;
	shared_clear_match();
	broadcast_lobby_state();
}

/* ------------------------------------------------------------------ */
/* Bounded service and listen loop                                     */
/* ------------------------------------------------------------------ */

int
server_session_service(uint64_t t)
{
	Transport *host = session_host;
	ServerSession *s;
	TransportPeer *p;
	int events = 0, i;

	if (host == (Transport *) NULL)
		return 0;

	events = transport_service(host, (int) SERVER_SESSION_STEP_BUDGET);
	if (events < 0)
		events = 0;

	/* Fresh per-tick admission quotas and the rotating peer-first cursor. */
	server_net_begin_tick(frame + 1);

	while (transport_poll_connect(host, &p)) {
		if (lobby.state != SERVER_SESSION_LOBBY ||
		    n_sessions >= session_capacity) {
			server_session_reject_peer(host, p, XTK_ERR_SESSION_FULL,
						   "session full");
		} else {
			(void) server_session_accept(p);
		}
	}
	while (transport_poll_disconnect(host, &p)) {
		s = session_for_peer(p);
		if (s != (ServerSession *) NULL)
			server_session_destroy(s);
	}

	{
		int served = 0;
		int budget = (int) SERVER_NET_SERVICE_BUDGET;

		while (served < budget) {
			uint8_t buf[TRANSPORT_MAX_PACKET];
			size_t len = 0;
			uint8_t ch = 0;
			XtkMessage msg;
			int r = transport_receive(host, &p, buf, sizeof(buf),
						  &len, &ch);

			if (r <= 0)
				break;
			served++;
			s = session_for_peer(p);
			if (s == (ServerSession *) NULL)
				continue;
			/* Bound one flooding peer's share of this tick; the
			 * healthy peer is still serviced below. */
			if (s->rx_tick != frame + 1) {
				s->rx_tick = frame + 1;
				s->rx_events = 0;
			}
			if (s->rx_events >= SERVER_NET_PEER_EVENT_QUOTA)
				continue;
			s->rx_events++;
			if (xtk_decode(buf, len, &msg) != XTK_ERR_NONE)
				continue;
			(void) server_session_message_ch(s, &msg, ch, t);
		}
	}

	/* Reclaim peers that quit or failed their handshake. */
	for (i = 0; i < SERVER_SESSION_MAX_PEERS; i++) {
		if (peers[i].used && peers[i].disconnected)
			server_session_destroy(&peers[i]);
	}

	/* Solo auto-start: suppressed as soon as a second human is present. */
	if (lobby.state == SERVER_SESSION_LOBBY && lobby.config_valid &&
	    n_sessions == 1 && server_session_can_start(
		    server_session_current()))
		(void) server_session_begin(server_session_current(),
					    (uint32_t) (frame + 1), t);

	return events;
}

int
server_session_run(uint16_t port, int fast, long max_ticks)
{
	Transport *host;
	long epoch_ticks = 0;

	server_session_init();
	server_session_set_capacity(XTK_MAX_HUMANS);

	host = transport_create(port, SERVER_SESSION_MAX_PEERS);
	if (host == (Transport *) NULL) {
		fprintf(stderr, "xtank-server: cannot listen on port %u\n",
			(unsigned) port);
		return 1;
	}
	server_session_set_transport(host);

	printf("SERVER-LISTEN port=%u hz=%d humans=%d\n",
	       (unsigned) transport_port(host), server_loop_hz(),
	       server_session_capacity());
	(void) fflush(stdout);

	/* The service stays alive across empty-session lobby cycles. */
	for (;;) {
		ServerSession *s;
		uint64_t now = now_ms();

		(void) server_session_service(now);

		s = server_session_current();
		if (s == (ServerSession *) NULL || lobby.state != SERVER_SESSION_RUNNING) {
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

	transport_destroy(host);
	return 0;
}

/* end of server_session.c */
