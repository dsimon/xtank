/*
 * server_session.h -- solo lobby and network-server session (P2.8).
 *
 * This is the terminal-free network entry path: an optional listen mode that
 * preserves the Phase 1 local CLI and drives one connected peer through the
 * frozen protocol-v1 state machine
 *
 *     Disconnected -> Handshake -> Lobby -> Running -> Results -> Lobby
 *                          \-> (timeout / QUIT) Disconnected
 *
 * (docs/PROTOCOL-v1.md section 9, docs/PHASE2-DECISIONS.md D2-F).  The session
 * owns exactly one peer and one drone; the first (and only) peer administers
 * start/reset.  Two seconds without valid traffic during a match neutralizes
 * drive/fire, aborts the session (an abort result, never a death), cleans up
 * and returns to the lobby.
 *
 * The session is the glue that makes P2.6's events/results (server_events_*)
 * and P2.7's admission (server_net_*) part of the shipped server:
 *
 *   - HELLO is answered with WELCOME + CONTENT_CATALOG, and a content-digest
 *     mismatch is rejected before any match (no auto-download);
 *   - CONFIG_SELECT is validated against the allowlist (combat1/Arena,
 *     Vanguard) and answered with CONFIG_STATUS;
 *   - an admin that is configured and ready starts an epoch: the match is
 *     built on the engine globals, events reset at EPOCH_BEGIN, the peer is
 *     bound to slot 0 and the admission layer begins the epoch;
 *   - each running tick services admission, observes events, steps, delivers
 *     the authorized snapshot on channel 1 and events/results reliably on
 *     channel 0;
 *   - RESULTS are retained until the admin resets or quits.
 *
 * The engine is touched only on the simulation thread.  The module compiles
 * and runs with no transport attached (transport == NULL) so the acceptance
 * fixture can drive the state machine directly; the production listen loop
 * attaches the pinned ENet transport.
 *
 * This translation unit is linked only by the network-server build
 * (XTANK_WITH_NET); the default Phase 1 local server does not include it.
 */

#ifndef _SERVER_SESSION_H_
#define _SERVER_SESSION_H_

#include <stdint.h>

#include "server_net.h"

/* The protocol state machine (docs/PROTOCOL-v1.md section 9). */
typedef enum {
	SERVER_SESSION_DISCONNECTED = 0,
	SERVER_SESSION_HANDSHAKE,
	SERVER_SESSION_LOBBY,
	SERVER_SESSION_RUNNING,
	SERVER_SESSION_RESULTS
} ServerSessionState;

/* Phase 2 slice: one peer and one drone (D2-F). */
#define SERVER_SESSION_MAX_PEERS	1

/* Bounded log of emitted control messages retained for inspection/tests. */
#define SERVER_SESSION_REPLY_MAX	96
#define SERVER_SESSION_REPLY_BYTES	1024

/* server_session_start_match()/begin() result codes. */
#define SERVER_SESSION_OK		0
#define SERVER_SESSION_ERR_STATE	(-1)	/* not permitted in this state */
#define SERVER_SESSION_ERR_NOT_ADMIN	(-2)
#define SERVER_SESSION_ERR_NO_CONTENT	(-3)
#define SERVER_SESSION_ERR_MATCH	(-4)

typedef struct ServerSession ServerSession;

/* ---- process-wide lifecycle ------------------------------------------ */

/* Clear the single-session table (safe to call repeatedly). */
void server_session_init(void);

/* Install the transport host that the listen loop services (may be NULL). */
void server_session_set_transport(Transport *t);

/* Number of live sessions (always 0 or 1 in the Phase 2 slice). */
int server_session_count(void);

/* The live session, or NULL. */
ServerSession *server_session_current(void);

/*
 * Accept a connection into the Handshake state.  Returns the session, or NULL
 * when the slice is already occupied (the caller should reject the second peer
 * with XTK_ERR_SESSION_FULL and disconnect it).  `peer` may be NULL for a
 * transport-free test session.
 */
ServerSession *server_session_accept(TransportPeer *peer);

/* Drop a session and release its binding ownership; disconnects its peer. */
void server_session_destroy(ServerSession *s);

/*
 * Reject an inbound peer without creating a session: send REJECT with
 * `error_code`/`text` on channel 0 and disconnect it.  Used for a second peer.
 */
void server_session_reject_peer(Transport *t, TransportPeer *peer,
				uint16_t error_code, const char *text);

/* ---- read-only accessors --------------------------------------------- */

ServerSessionState server_session_state(const ServerSession *s);
uint32_t server_session_id(const ServerSession *s);
uint32_t server_session_epoch(const ServerSession *s);
int server_session_is_admin(const ServerSession *s);
int server_session_ready(const ServerSession *s);
uint16_t server_session_maze_id(const ServerSession *s);
uint16_t server_session_vehicle_id(const ServerSession *s);
int server_session_config_valid(const ServerSession *s);
int server_session_bound_slot(const ServerSession *s);
uint32_t server_session_snapshot_seq(const ServerSession *s);
int server_session_events_sent(const ServerSession *s);

/* Assign/clear the admin flag (the accept path sets it; tests may clear it). */
void server_session_set_admin(ServerSession *s, int admin);

/*
 * The session's bound transport peer, or NULL.  The accept loop uses this to
 * match a received packet's peer to the session.
 */
TransportPeer *server_session_peer(const ServerSession *s);

/* ---- bounded control-reply log (inspection/tests) -------------------- */

int server_session_reply_count(const ServerSession *s);
int server_session_reply(const ServerSession *s, int index, XtkMessage *out);
void server_session_reply_clear(ServerSession *s);

/* ---- state machine ---------------------------------------------------- */

/*
 * Handle one decoded inbound message.  Returns XTK_ERR_NONE on success; for a
 * protocol-level rejection the appropriate REJECT / PROTOCOL_ERROR is emitted
 * and no state is changed.  `now_ms` is the monotonic receive time (timeout).
 */
int server_session_message(ServerSession *s, const XtkMessage *msg,
			   uint64_t now_ms);

/*
 * 1 when the session may start: admin, running epoch not live, a config has
 * been accepted and the peer is ready.
 */
int server_session_can_start(const ServerSession *s);

/*
 * Build and start an epoch from the accepted config.  Chooses/loads the
 * allowlisted maze and Vanguard, builds one human (slot 0) + one drone,
 * resets events at `start_tick`, binds the peer and emits START + EPOCH_BEGIN.
 * Engine globals are touched here (simulation thread only).  Returns
 * SERVER_SESSION_OK or a SERVER_SESSION_ERR_* code.
 */
int server_session_begin(ServerSession *s, uint32_t start_tick, uint64_t now_ms);

/*
 * Advance one running tick: timeout check/abort, bounded admission, one
 * engine step, event observation, snapshot/event delivery and post-step
 * acknowledgments.  Returns the engine status (GAME_RUNNING ..) or
 * SERVER_SESSION_ERR_STATE when the session is not running.
 */
int server_session_step(ServerSession *s, uint64_t now_ms);

/*
 * Settle a finished match: capture results before the score reset, deliver
 * RESULTS reliably, and enter the Results state (retained until reset).
 */
void server_session_finish(ServerSession *s, int status);

/*
 * Return a finished/aborted session to the lobby after cleanup.  Called by the
 * timeout abort and after a QUIT-free reset boundary.
 */
void server_session_return_to_lobby(ServerSession *s);

/* ---- production listen loop ------------------------------------------ */

/*
 * Run the network server: create the listening transport on `port`, service
 * peers, drive lobby and matches until the peer quits or the loop is stopped.
 * `fast` bypasses wall-clock pacing; `max_ticks` (0 = unlimited) caps each
 * epoch through the normal results/reset path.  Returns 0 on clean exit.
 */
int server_session_run(uint16_t port, int fast, long max_ticks);

#endif /* _SERVER_SESSION_H_ */