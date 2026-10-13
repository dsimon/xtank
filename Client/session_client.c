/*
 * Client/session_client.c -- client-side network session (P2.14; P3.10).
 *
 * See Client/session_client.h.  Pure C: no SDL, no X11, no engine.  The pinned
 * ENet transport (Transport/) is the only I/O; this unit never blocks, never
 * allocates per message and never simulates gameplay.
 *
 * P3.10 authority rules (docs/PHASE3-DECISIONS.md D3-B/D3-E):
 *   - the WELCOME role is observer/player only and NEVER grants administrator;
 *   - administrator is the server-authored LOBBY_STATE.admin_index applied to
 *     this client's self_index;
 *   - a datagram from a foreign peer, the wrong channel, a foreign session id
 *     or a stale epoch is discarded before any state change.
 *
 * Authority: docs/PHASE3-PLAN.md P3.10; docs/PROTOCOL-v1.1.md sections 3-9.
 */

#include "session_client.h"

#include <stdlib.h>
#include <string.h>

#include "transport.h"
#include "content-manifest.h"

#define XTK_SC_PUMP_BUDGET    64
#define XTK_SC_HANDSHAKE_TIMEOUT_NS (3ULL * 1000000000ULL)

#define XTK_CLIENT_BUILD      0x0001u

#define XTK_SC_INDEX_NONE     0xFFu

struct XtkSessionClient {
	Transport *t;
	TransportPeer *peer;

	XtkSessionState state;

	uint32_t session_id;
	uint32_t epoch;
	int is_admin;

	char name[XTK_MAX_NAME_BYTES + 1];

	/* Server-authored lobby/session status (LOBBY_STATE). */
	uint8_t  self_index;
	uint8_t  admin_index;
	uint8_t  human_capacity;
	uint8_t  drone_count;
	uint8_t  wire_state;
	uint16_t maze_id;
	uint16_t vehicle_id;
	uint32_t config_revision;
	int      have_lobby_state;
	int      roster_count;
	XtkRosterEntry roster[XTK_LOBBY_ROSTER_MAX];

	uint32_t ctrl_seq;
	uint32_t input_seq;
	uint32_t ping_seq;

	uint32_t client_nonce;

	int deadline_set;
	uint64_t deadline_ns;

	uint16_t last_error;
	char error_text[XTK_MAX_ERROR_TEXT_BYTES + 1];
};

/* ---- small helpers --------------------------------------------------- */

static void set_error(XtkSessionClient *c, uint16_t code, const char *text)
{
	c->last_error = code;
	if (text != NULL) {
		strncpy(c->error_text, text, XTK_MAX_ERROR_TEXT_BYTES);
		c->error_text[XTK_MAX_ERROR_TEXT_BYTES] = '\0';
	} else {
		c->error_text[0] = '\0';
	}
}

static void clear_session_state(XtkSessionClient *c)
{
	c->session_id = 0;
	c->epoch = 0;
	c->is_admin = 0;
	c->self_index = XTK_SC_INDEX_NONE;
	c->admin_index = XTK_SC_INDEX_NONE;
	c->human_capacity = 0;
	c->drone_count = 0;
	c->wire_state = (uint8_t)XTK_SESSION_LOBBY;
	c->maze_id = 0;
	c->vehicle_id = 0;
	c->config_revision = 0;
	c->have_lobby_state = 0;
	c->roster_count = 0;
	memset(c->roster, 0, sizeof(c->roster));
	c->last_error = XTK_ERR_NONE;
	c->error_text[0] = '\0';
	c->deadline_set = 0;
}

static void note_epoch(XtkSessionClient *c, uint32_t e)
{
	if (e > c->epoch) {
		c->epoch = e;
	}
}

static int sc_send(XtkSessionClient *c, XtkMessage *m, uint8_t channel)
{
	uint8_t buf[XTK_MAX_MESSAGE_BYTES];
	size_t len = 0;

	if (c == NULL || c->t == NULL || c->peer == NULL) {
		return -1;
	}
	m->env.session_id = c->session_id;
	if (channel == XTK_CH_CONTROL) {
		m->env.message_sequence = c->ctrl_seq++;
	} else {
		m->env.message_sequence = c->input_seq++;
	}
	if (xtk_encode(buf, sizeof(buf), m, &len) != XTK_ERR_NONE) {
		return -1;
	}
	return transport_send(c->t, c->peer, channel, buf, len);
}

/* ---- lifecycle ------------------------------------------------------- */

XtkSessionClient *session_client_create(void)
{
	XtkSessionClient *c = (XtkSessionClient *)calloc(1, sizeof(*c));

	if (c == NULL) {
		return NULL;
	}
	/*
	 * A client transport holds a spare peer slot so an in-process rejoin can
	 * open its new connection while a just-released one drains.
	 */
	c->t = transport_create(0, 2);
	if (c->t == NULL) {
		free(c);
		return NULL;
	}
	c->state = XTK_SC_DISCONNECTED;
	clear_session_state(c);
	c->client_nonce = 0x58544B31u;
	c->client_nonce ^= (uint32_t)(uintptr_t)c;
	return c;
}

int session_client_connect(XtkSessionClient *c, const char *host, uint16_t port,
			   const char *name)
{
	if (c == NULL || host == NULL || port == 0) {
		return -1;
	}
	if (name != NULL) {
		strncpy(c->name, name, XTK_MAX_NAME_BYTES);
		c->name[XTK_MAX_NAME_BYTES] = '\0';
	}
	/* Rejoin in-process: drop any prior peer and forget prior session state. */
	if (c->peer != NULL && c->t != NULL) {
		transport_disconnect(c->t, c->peer);
		c->peer = NULL;
	}
	clear_session_state(c);
	c->ctrl_seq = 0;
	c->input_seq = 0;
	c->peer = transport_connect(c->t, host, port);
	if (c->peer == NULL) {
		c->state = XTK_SC_FAILED;
		set_error(c, XTK_ERR_BAD_STATE, "cannot resolve/connect to server");
		return -1;
	}
	c->state = XTK_SC_CONNECTING;
	return 0;
}

void session_client_destroy(XtkSessionClient *c)
{
	if (c == NULL) {
		return;
	}
	if (c->peer != NULL && c->t != NULL) {
		transport_disconnect(c->t, c->peer);
		c->peer = NULL;
	}
	if (c->t != NULL) {
		transport_destroy(c->t);
	}
	free(c);
}

void session_client_reconnect(XtkSessionClient *c)
{
	if (c == NULL) {
		return;
	}
	if (c->peer != NULL && c->t != NULL) {
		transport_disconnect(c->t, c->peer);
		c->peer = NULL;
	}
	clear_session_state(c);
	c->state = XTK_SC_DISCONNECTED;
	c->ctrl_seq = 0;
	c->input_seq = 0;
}

static void send_hello(XtkSessionClient *c)
{
	XtkMessage m;

	memset(&m, 0, sizeof(m));
	m.env.message_type = XTK_MSG_HELLO;
	m.env.session_id = 0;
	m.body.hello.client_nonce = c->client_nonce;
	m.body.hello.proto_major = XTK_PROTOCOL_MAJOR;
	m.body.hello.proto_minor = XTK_PROTOCOL_MINOR;
	m.body.hello.client_build = (uint16_t)XTK_CLIENT_BUILD;
	{
		const char *nm = (c->name[0] != '\0') ? c->name : "pilot";
		size_t n = strlen(nm);
		if (n > XTK_MAX_NAME_BYTES) {
			n = XTK_MAX_NAME_BYTES;
		}
		memcpy(m.body.hello.name, nm, n);
		m.body.hello.name[n] = '\0';
	}
	memcpy(m.body.hello.content_set_digest, xtk_content_set_digest,
	       XTK_SHA256_BYTES);
	(void)sc_send(c, &m, XTK_CH_CONTROL);
}

/* ---- inbound validation and state update ----------------------------- */

static int ingest_lobby_state(XtkSessionClient *c, const XtkLobbyStatus *ls)
{
	int i, n = ls->human_count;

	if (n < 0) {
		n = 0;
	}
	if (n > (int)XTK_LOBBY_ROSTER_MAX) {
		n = (int)XTK_LOBBY_ROSTER_MAX;
	}
	c->have_lobby_state = 1;
	c->config_revision = ls->config_revision;
	c->wire_state = ls->session_state;
	c->drone_count = ls->drone_count;
	c->maze_id = ls->maze_id;
	c->vehicle_id = ls->vehicle_id;
	c->self_index = ls->self_index;
	c->admin_index = ls->admin_index;
	/* Authoritative authority: only the server-authored admin index grants it. */
	c->is_admin = (ls->admin_index != XTK_SC_INDEX_NONE &&
		       ls->admin_index == ls->self_index) ? 1 : 0;
	c->roster_count = n;
	for (i = 0; i < n; i++) {
		c->roster[i] = ls->entry[i];
	}

	switch (ls->session_state) {
	case XTK_SESSION_RUNNING:
		c->state = XTK_SC_RUNNING;
		break;
	case XTK_SESSION_RESULTS:
		c->state = XTK_SC_RESULTS;
		break;
	case XTK_SESSION_LOBBY:
	default:
		c->state = XTK_SC_LOBBY;
		break;
	}
	return 1;
}

int session_client_ingest(XtkSessionClient *c, int peer_ok, uint8_t channel,
			  const XtkMessage *msg)
{
	if (c == NULL || msg == NULL) {
		return 0;
	}
	/* A datagram from a foreign peer can never touch this session. */
	if (!peer_ok) {
		return 0;
	}
	/* The transport channel must match the message class (P3.6 boundary). */
	if (channel != msg->env.channel ||
	    channel != xtk_next_channel(msg->env.message_type)) {
		return 0;
	}
	/*
	 * A foreign session id is ignored once this client has been welcomed.  A
	 * pre-admission REJECT/PROTOCOL_ERROR carries session id 0 and is still
	 * allowed so the actionable error reaches the UI.
	 */
	if (c->session_id != 0 && msg->env.session_id != 0 &&
	    msg->env.session_id != c->session_id) {
		return 0;
	}
	/*
	 * Stale epoch: a live-epoch message from an epoch older than the highest
	 * one this client has seen cannot regress it (e.g. a late RESULTS after a
	 * new START).  START/EPOCH_BEGIN carry the new epoch and are handled below.
	 */
	if (msg->env.message_type != XTK_MSG_START &&
	    msg->env.message_type != XTK_MSG_EPOCH_BEGIN &&
	    c->epoch != 0 && msg->env.epoch != 0 && msg->env.epoch < c->epoch) {
		return 0;
	}

	switch (msg->env.message_type) {
	case XTK_MSG_WELCOME:
		c->session_id = msg->env.session_id;
		c->human_capacity = msg->body.welcome.human_capacity;
		/* Never derive administrator from the WELCOME player role. */
		c->is_admin = 0;
		c->admin_index = XTK_SC_INDEX_NONE;
		c->self_index = XTK_SC_INDEX_NONE;
		c->state = XTK_SC_LOBBY;
		break;
	case XTK_MSG_CONTENT_CATALOG:
		break;
	case XTK_MSG_LOBBY_STATE:
		return ingest_lobby_state(c, &msg->body.lobby_state);
	case XTK_MSG_START:
		note_epoch(c, msg->env.epoch);
		c->state = XTK_SC_RUNNING;
		break;
	case XTK_MSG_EPOCH_BEGIN:
		note_epoch(c, msg->body.epoch_begin.epoch);
		c->state = XTK_SC_RUNNING;
		break;
	case XTK_MSG_RESULTS:
		c->state = XTK_SC_RESULTS;
		break;
	case XTK_MSG_REJECT:
		c->state = XTK_SC_FAILED;
		c->last_error = msg->body.reject.error_code;
		strncpy(c->error_text, msg->body.reject.text,
			XTK_MAX_ERROR_TEXT_BYTES);
		c->error_text[XTK_MAX_ERROR_TEXT_BYTES] = '\0';
		break;
	case XTK_MSG_PROTOCOL_ERROR:
		c->last_error = msg->body.protocol_error.error_code;
		break;
	case XTK_MSG_QUIT:
		c->state = XTK_SC_DISCONNECTED;
		break;
	default:
		break;
	}
	return 1;
}

/* ---- pump ------------------------------------------------------------ */

int session_client_pump(XtkSessionClient *c, uint64_t now_ns,
			XtkSessionMessageFn fn, void *user)
{
	int events;

	if (c == NULL) {
		return -1;
	}
	if (c->state == XTK_SC_CONNECTING || c->state == XTK_SC_HANDSHAKE) {
		if (!c->deadline_set) {
			c->deadline_ns = now_ns + XTK_SC_HANDSHAKE_TIMEOUT_NS;
			c->deadline_set = 1;
		}
	}

	events = transport_service(c->t, XTK_SC_PUMP_BUDGET);
	if (events < 0) {
		c->state = XTK_SC_FAILED;
		set_error(c, XTK_ERR_BAD_STATE, "transport service failure");
		return -1;
	}

	{
		TransportPeer *p;
		while (transport_poll_connect(c->t, &p)) {
			c->peer = p;
			if (c->state == XTK_SC_CONNECTING) {
				c->state = XTK_SC_HANDSHAKE;
				send_hello(c);
			}
		}
		while (transport_poll_disconnect(c->t, &p)) {
			if (p == c->peer) {
				c->peer = NULL;
				if (c->state != XTK_SC_FAILED) {
					c->state = XTK_SC_DISCONNECTED;
				}
			}
		}
	}

	for (;;) {
		uint8_t buf[XTK_MAX_MESSAGE_BYTES];
		size_t len = 0;
		uint8_t ch = 0;
		TransportPeer *rp = NULL;
		XtkMessage msg;
		int r = transport_receive(c->t, &rp, buf, sizeof(buf), &len, &ch);

		if (r <= 0) {
			break;
		}
		if (xtk_decode(buf, len, &msg) != XTK_ERR_NONE) {
			continue;
		}
		if (session_client_ingest(c, rp == c->peer, ch, &msg)) {
			if (fn != NULL) {
				(void)fn(user, &msg);
			}
		}
	}

	/* No server / no handshake answer: fail with an actionable error. */
	if ((c->state == XTK_SC_CONNECTING || c->state == XTK_SC_HANDSHAKE) &&
	    c->deadline_set && now_ns > c->deadline_ns) {
		c->state = XTK_SC_FAILED;
		set_error(c, XTK_ERR_TIMEOUT,
			  "no response from server (connection timed out)");
	}
	return 0;
}

/* ---- outbound controls ----------------------------------------------- */

static int send_simple(XtkSessionClient *c, uint8_t type)
{
	XtkMessage m;

	memset(&m, 0, sizeof(m));
	m.env.message_type = type;
	return sc_send(c, &m, XTK_CH_CONTROL);
}

int session_client_send_ping(XtkSessionClient *c)
{
	XtkMessage m;

	if (c == NULL) {
		return -1;
	}
	memset(&m, 0, sizeof(m));
	m.env.message_type = XTK_MSG_PING;
	m.body.ping.nonce = ++c->ping_seq;
	return sc_send(c, &m, XTK_CH_CONTROL);
}

int session_client_send_config(XtkSessionClient *c, uint16_t maze,
			       uint16_t vehicle, uint8_t drone_count)
{
	XtkMessage m;

	if (c == NULL) {
		return -1;
	}
	memset(&m, 0, sizeof(m));
	m.env.message_type = XTK_MSG_CONFIG_SELECT;
	m.body.config_select.maze_id = maze;
	m.body.config_select.vehicle_id = vehicle;
	m.body.config_select.drone_count = drone_count;
	return sc_send(c, &m, XTK_CH_CONTROL);
}

int session_client_send_ready(XtkSessionClient *c, int ready)
{
	XtkMessage m;

	if (c == NULL) {
		return -1;
	}
	memset(&m, 0, sizeof(m));
	m.env.message_type = XTK_MSG_READY;
	m.body.ready.ready = ready ? 1 : 0;
	/* D3-B: readiness is always expressed against the current revision. */
	m.body.ready.config_revision = c->config_revision;
	return sc_send(c, &m, XTK_CH_CONTROL);
}

int session_client_send_start(XtkSessionClient *c)
{
	XtkMessage m;

	if (c == NULL) {
		return -1;
	}
	memset(&m, 0, sizeof(m));
	m.env.message_type = XTK_MSG_START_REQUEST;
	m.body.start_request.expected_config_revision = c->config_revision;
	return sc_send(c, &m, XTK_CH_CONTROL);
}

int session_client_send_reset(XtkSessionClient *c)
{
	XtkMessage m;

	if (c == NULL) {
		return -1;
	}
	memset(&m, 0, sizeof(m));
	m.env.message_type = XTK_MSG_RESET;
	m.body.reset.prior_epoch = c->epoch;
	return sc_send(c, &m, XTK_CH_CONTROL);
}

int session_client_send_quit(XtkSessionClient *c)
{
	return send_simple(c, XTK_MSG_QUIT);
}

int session_client_send_input(XtkSessionClient *c, const XtkMessage *batch)
{
	XtkMessage m;

	if (c == NULL || batch == NULL) {
		return -1;
	}
	m = *batch;
	return sc_send(c, &m, XTK_CH_INPUT);
}

/* ---- accessors ------------------------------------------------------- */

XtkSessionState session_client_state(const XtkSessionClient *c)
{
	return (c != NULL) ? c->state : XTK_SC_DISCONNECTED;
}

int session_client_connected(const XtkSessionClient *c)
{
	if (c == NULL) {
		return 0;
	}
	return c->state == XTK_SC_HANDSHAKE || c->state == XTK_SC_LOBBY ||
	       c->state == XTK_SC_RUNNING || c->state == XTK_SC_RESULTS;
}

uint32_t session_client_id(const XtkSessionClient *c)
{
	return (c != NULL) ? c->session_id : 0;
}

uint32_t session_client_epoch(const XtkSessionClient *c)
{
	return (c != NULL) ? c->epoch : 0;
}

int session_client_is_admin(const XtkSessionClient *c)
{
	return (c != NULL) && c->is_admin;
}

uint16_t session_client_last_error(const XtkSessionClient *c)
{
	return (c != NULL) ? c->last_error : 0;
}

const char *session_client_error_text(const XtkSessionClient *c)
{
	return (c != NULL) ? c->error_text : "";
}

void session_client_clear_error(XtkSessionClient *c)
{
	if (c != NULL) {
		c->last_error = XTK_ERR_NONE;
		c->error_text[0] = '\0';
	}
}

uint8_t session_client_self_index(const XtkSessionClient *c)
{
	return (c != NULL) ? c->self_index : XTK_SC_INDEX_NONE;
}

uint8_t session_client_admin_index(const XtkSessionClient *c)
{
	return (c != NULL) ? c->admin_index : XTK_SC_INDEX_NONE;
}

uint8_t session_client_human_capacity(const XtkSessionClient *c)
{
	return (c != NULL) ? c->human_capacity : 0;
}

uint8_t session_client_drone_count(const XtkSessionClient *c)
{
	return (c != NULL) ? c->drone_count : 0;
}

uint8_t session_client_wire_state(const XtkSessionClient *c)
{
	return (c != NULL) ? c->wire_state : (uint8_t)XTK_SESSION_LOBBY;
}

uint16_t session_client_maze_id(const XtkSessionClient *c)
{
	return (c != NULL) ? c->maze_id : 0;
}

uint16_t session_client_vehicle_id(const XtkSessionClient *c)
{
	return (c != NULL) ? c->vehicle_id : 0;
}

uint32_t session_client_config_revision(const XtkSessionClient *c)
{
	return (c != NULL) ? c->config_revision : 0;
}

int session_client_have_lobby_state(const XtkSessionClient *c)
{
	return (c != NULL) ? c->have_lobby_state : 0;
}

int session_client_roster_count(const XtkSessionClient *c)
{
	return (c != NULL) ? c->roster_count : 0;
}

int session_client_roster(const XtkSessionClient *c, int index,
			  XtkRosterEntry *out)
{
	if (c == NULL || out == NULL || index < 0 || index >= c->roster_count) {
		return -1;
	}
	*out = c->roster[index];
	return 0;
}