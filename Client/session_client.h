/*
 * Client/session_client.h -- client-side network session (P2.14).
 *
 * The client half of the frozen protocol-v1 state machine
 * (docs/PROTOCOL-v1.md section 9):
 *
 *     Disconnected -> Connecting -> Handshake -> Lobby -> Running -> Results
 *                        \-> FAILED (no server / timeout)     \-> Lobby (reset)
 *
 * It owns the pinned ENet transport (Transport/, D2-B) and the HELLO / config /
 * ready / reset / quit / input-batch sends.  It decodes every inbound message,
 * updates the session-level state (session id, epoch, role) and hands each
 * decoded message to a caller callback for presentation.  No presentation,
 * simulation or SDL type appears here; the frame loop (Client/frame.c) is the
 * consumer.
 *
 * Every call is nonblocking.  DNS/connection setup happens once in
 * session_client_connect() (outside the frame loop); session_client_pump()
 * only services the transport with a bounded, zero-timeout budget.
 *
 * Authority: docs/PHASE2-PLAN.md P2.14; docs/PROTOCOL-v1.md sections 3-9;
 * docs/PHASE2-DECISIONS.md D2-B/D2-F.
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
 * Message callback.  Invoked once per decoded, validated inbound message in
 * arrival order.  Return 0 to continue; a nonzero return is ignored.
 */
typedef int (*XtkSessionMessageFn)(void *user, const XtkMessage *msg);

/* Create a client with its own transport, in the Disconnected state. */
XtkSessionClient *session_client_create(void);

/*
 * Resolve `host`:`port` and begin a connection; on the transport CONNECT event
 * the session moves to Handshake and sends HELLO with `name`.  Returns 0 on
 * success, -1 on a bad argument or an immediate connect failure.  Connection
 * setup is not part of the frame loop.
 */
int session_client_connect(XtkSessionClient *c, const char *host, uint16_t port,
			   const char *name);

/* Tear down the transport and free the client. */
void session_client_destroy(XtkSessionClient *c);

/*
 * Service the transport without blocking, update the session state, and invoke
 * `fn(user, msg)` for each decoded message.  `now_ns` is the monotonic clock
 * used for the connect/handshake timeout (returns FAILED after ~3 s with no
 * server).  Returns 0, or -1 on a fatal transport error.
 */
int session_client_pump(XtkSessionClient *c, uint64_t now_ns,
			XtkSessionMessageFn fn, void *user);

/* ---- outbound controls (channel 0) ---------------------------------- */

int session_client_send_config(XtkSessionClient *c, uint16_t maze,
			       uint16_t vehicle);
int session_client_send_ready(XtkSessionClient *c);
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

#endif /* XTK_CLIENT_SESSION_CLIENT_H */
