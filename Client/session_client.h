/*
 * Client/session_client.h -- client-side network session (P2.14; P3.10).
 *
 * The client half of the frozen protocol-v1.1 state machine
 * (docs/PROTOCOL-v1.1.md section 8):
 *
 *     Disconnected -> Connecting -> Handshake -> Lobby -> Running -> Results
 *                        \-> FAILED (no server / timeout)     \-> Lobby (reset)
 *
 * It owns the pinned ENet transport (Transport/, D2-B) and the HELLO / config /
 * ready / start / reset / quit / input-batch sends.  It decodes every inbound
 * message, validates it against the bound peer, channel, session id and epoch,
 * updates the session-level state (session id, epoch, authoritative
 * administrator and roster) and hands each accepted message to a caller
 * callback for presentation.  No presentation, simulation or SDL type appears
 * here; the frame loop (Client/frame.c) is the consumer.
 *
 * P3.10: administrator authority is NEVER guessed from the WELCOME role (the
 * server always sends role = player).  It comes only from the server-authored
 * LOBBY_STATE.admin_index, compared with this client's self_index.  A stale,
 * foreign-peer, wrong-channel, wrong-session or stale-epoch datagram is
 * discarded before it can regress the client's state.
 *
 * Every call is nonblocking.  DNS/connection setup happens once in
 * session_client_connect() (outside the frame loop); session_client_pump()
 * only services the transport with a bounded, zero-timeout budget.
 *
 * Authority: docs/PHASE3-PLAN.md P3.10; docs/PROTOCOL-v1.1.md sections 3-9;
 * docs/PHASE3-DECISIONS.md D3-A/D3-B/D3-E; docs/PHASE2-PLAN.md P2.14.
 */

#ifndef XTK_CLIENT_SESSION_CLIENT_H
#define XTK_CLIENT_SESSION_CLIENT_H

#include <stdint.h>

#include "wire.h"

/* Client-side session state (mirror of the protocol state machine). */
typedef enum {
	XTK_SC_DISCONNECTED = 0,
	XTK_SC_CONNECTING,
	XTK_SC_HANDSHAKE,
	XTK_SC_LOBBY,
	XTK_SC_RUNNING,
	XTK_SC_RESULTS,
	XTK_SC_FAILED
} XtkSessionState;

typedef struct XtkSessionClient XtkSessionClient;

/*
 * Message callback.  Invoked once per decoded, validated, accepted inbound
 * message in arrival order.  Return 0 to continue; a nonzero return is ignored.
 */
typedef int (*XtkSessionMessageFn)(void *user, const XtkMessage *msg);

/* Create a client with its own transport, in the Disconnected state. */
XtkSessionClient *session_client_create(void);

/*
 * Resolve `host`:`port` and begin a connection; on the transport CONNECT event
 * the session moves to Handshake and sends HELLO with `name`.  Returns 0 on
 * success, -1 on a bad argument or an immediate connect failure.  Connection
 * setup is not part of the frame loop.  Calling it again after a failure or a
 * disconnect reuses the same client (no process restart) and resets the
 * per-session state first.
 */
int session_client_connect(XtkSessionClient *c, const char *host, uint16_t port,
			   const char *name);

/* Tear down the transport and free the client. */
void session_client_destroy(XtkSessionClient *c);

/*
 * Return the client to the Disconnected state without destroying it: drop the
 * current peer and clear the session id, epoch, administrator, roster and
 * error so session_client_connect() can join again in-process.
 */
void session_client_reconnect(XtkSessionClient *c);

/*
 * Service the transport without blocking, update the session state, and invoke
 * `fn(user, msg)` for each decoded, accepted message.  `now_ns` is the
 * monotonic clock used for the connect/handshake timeout (returns FAILED after
 * ~3 s with no server).  Returns 0, or -1 on a fatal transport error.
 */
int session_client_pump(XtkSessionClient *c, uint64_t now_ns,
			XtkSessionMessageFn fn, void *user);

/*
 * Validate and apply one already-decoded inbound message.  `peer_ok` is 1 when
 * the datagram arrived from this client's bound peer.  A wrong peer, channel,
 * session id or stale epoch is rejected (returns 0) with no state change.
 * The pump uses this; the acceptance fixture calls it directly to prove that
 * foreign/stale datagrams cannot regress the client.  Returns 1 if accepted.
 */
int session_client_ingest(XtkSessionClient *c, int peer_ok, uint8_t channel,
			  const XtkMessage *msg);

/* ---- outbound controls (channel 0) ---------------------------------- */

int session_client_send_ping(XtkSessionClient *c);
int session_client_send_config(XtkSessionClient *c, uint16_t maze,
			       uint16_t vehicle, uint8_t drone_count);
int session_client_send_ready(XtkSessionClient *c, int ready);
int session_client_send_start(XtkSessionClient *c);
int session_client_send_reset(XtkSessionClient *c);
int session_client_send_quit(XtkSessionClient *c);

/* Send one INPUT_BATCH (channel 2) produced by Client/input.c. */
int session_client_send_input(XtkSessionClient *c, const XtkMessage *batch);

/* ---- read-only accessors -------------------------------------------- */

XtkSessionState session_client_state(const XtkSessionClient *c);
int      session_client_connected(const XtkSessionClient *c);
uint32_t session_client_id(const XtkSessionClient *c);
uint32_t session_client_epoch(const XtkSessionClient *c);
int      session_client_is_admin(const XtkSessionClient *c);
uint16_t session_client_last_error(const XtkSessionClient *c);
const char *session_client_error_text(const XtkSessionClient *c);

/* Clear the sticky last-error latch (e.g. after the user acknowledges it). */
void session_client_clear_error(XtkSessionClient *c);

/* Server-authored lobby/session state (valid once WELCOME/LOBBY_STATE arrive). */
uint8_t  session_client_self_index(const XtkSessionClient *c);
uint8_t  session_client_admin_index(const XtkSessionClient *c);
uint8_t  session_client_human_capacity(const XtkSessionClient *c);
uint8_t  session_client_drone_count(const XtkSessionClient *c);
uint8_t  session_client_wire_state(const XtkSessionClient *c);
uint16_t session_client_maze_id(const XtkSessionClient *c);
uint16_t session_client_vehicle_id(const XtkSessionClient *c);
uint32_t session_client_config_revision(const XtkSessionClient *c);
int      session_client_have_lobby_state(const XtkSessionClient *c);
int      session_client_roster_count(const XtkSessionClient *c);
int      session_client_roster(const XtkSessionClient *c, int index,
			       XtkRosterEntry *out);

#endif /* XTK_CLIENT_SESSION_CLIENT_H */
