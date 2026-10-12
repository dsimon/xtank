/*
 * Client/session_client.c -- client-side network session (P2.14).
 *
 * See Client/session_client.h.  Pure C: no SDL, no X11, no engine.  The pinned
 * ENet transport (Transport/) is the only I/O; this unit never blocks, never
 * allocates per message and never simulates gameplay.
 *
 * Authority: docs/PHASE2-PLAN.md P2.14; docs/PROTOCOL-v1.md sections 3-9.
 */

#include "session_client.h"

#include <stdlib.h>
#include <string.h>

#include "transport.h"
#include "content-manifest.h"

#define XTK_SC_PUMP_BUDGET    64
#define XTK_SC_HANDSHAKE_TIMEOUT_NS (3ULL * 1000000000ULL)

#define XTK_CLIENT_BUILD      0x0001u

struct XtkSessionClient {
	Transport *t;
	TransportPeer *peer;

	XtkSessionState state;

	uint32_t session_id;
	uint32_t epoch;
	int is_admin;

	char name[XTK_MAX_NAME_BYTES + 1];

	uint32_t ctrl_seq;
	uint32_t input_seq;

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
	c->t = transport_create(0, 1);
	if (c->t == NULL) {
		free(c);
		return NULL;
	}
	c->state = XTK_SC_DISCONNECTED;
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
	c->peer = transport_connect(c->t, host, port);
	if (c->peer == NULL) {
		c->state = XTK_SC_FAILED;
		set_error(c, XTK_ERR_BAD_STATE, "cannot resolve/connect to server");
		return -1;
	}
	c->state = XTK_SC_CONNECTING;
	c->deadline_set = 0;
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

/* ---- inbound state update -------------------------------------------- */

static void update_state(XtkSessionClient *c, const XtkMessage *m)
{
	switch (m->env.message_type) {
	case XTK_MSG_WELCOME:
		c->state = XTK_SC_LOBBY;
		c->session_id = m->env.session_id;
		c->is_admin = (m->body.welcome.role == 1) ? 1 : 0;
		break;
	case XTK_MSG_CONTENT_CATALOG:
		/* Presentation stores the catalog; session only notes arrival. */
		break;
	case XTK_MSG_START:
		c->state = XTK_SC_RUNNING;
		c->epoch = m->env.epoch;
		break;
	case XTK_MSG_EPOCH_BEGIN:
		c->state = XTK_SC_RUNNING;
		c->epoch = m->body.epoch_begin.epoch;
		break;
	case XTK_MSG_RESULTS:
		c->state = XTK_SC_RESULTS;
		break;
	case XTK_MSG_REJECT:
		c->state = XTK_SC_FAILED;
		c->last_error = m->body.reject.error_code;
		strncpy(c->error_text, m->body.reject.text,
			XTK_MAX_ERROR_TEXT_BYTES);
		c->error_text[XTK_MAX_ERROR_TEXT_BYTES] = '\0';
		break;
	case XTK_MSG_PROTOCOL_ERROR:
		c->last_error = m->body.protocol_error.error_code;
		break;
	case XTK_MSG_QUIT:
		c->state = XTK_SC_DISCONNECTED;
		break;
	default:
		break;
	}
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
		update_state(c, &msg);
		if (fn != NULL) {
			(void)fn(user, &msg);
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

int session_client_send_config(XtkSessionClient *c, uint16_t maze,
			       uint16_t vehicle)
{
	XtkMessage m;

	if (c == NULL) {
		return -1;
	}
	memset(&m, 0, sizeof(m));
	m.env.message_type = XTK_MSG_CONFIG_SELECT;
	m.body.config_select.maze_id = maze;
	m.body.config_select.vehicle_id = vehicle;
	return sc_send(c, &m, XTK_CH_CONTROL);
}

int session_client_send_ready(XtkSessionClient *c)
{
	return send_simple(c, XTK_MSG_READY);
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
