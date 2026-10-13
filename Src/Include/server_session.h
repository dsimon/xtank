/*
 * server_session.h -- shared lobby and one-world match coordinator (P3.4).
 *
 * This is the terminal-free network entry path: an optional listen mode that
 * preserves the Phase 1 local CLI and drives up to two admitted human peers
 * through the frozen protocol-v1.1 state machine
 *
 *     Disconnected -> Handshake -> Lobby -> Running -> Results -> Lobby
 *                          \-> (timeout / QUIT) Disconnected
 *
 * (docs/PROTOCOL-v1.1.md section 8, docs/PHASE3-DECISIONS.md D3-B/D3-E).  The
 * coordinator owns exactly one shared world, one epoch, one config revision
 * and one administrator; per-peer records hold connection/controller identity,
 * readiness and the bound vehicle slot.  The listen service stays alive across
 * empty-session lobby cycles.
 *
 * Shared vs per-peer state (the P3.4 split):
 *
 *   - SHARED (one world): state (Lobby/Running/Results), epoch, config
 *     (maze/Vanguard/drone count), config revision, administrator index, the
 *     bound match and the single engine step/results/reset transaction.
 *   - PER PEER: connection/controller identity, name, readiness, transport and
 *     admission handle, bound slot/generation, reply log and delivery cursors.
 *
 * One start transaction, one world init, one server_step() per tick and one
 * results finalization/reset are performed regardless of peer count.  A
 * config change increments the revision and wipes every peer's readiness.  A
 * RESET returns through a shared lobby barrier so a rejoin window is open
 * before the next ready/start transaction increments the epoch.
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

/* The protocol state machine (docs/PROTOCOL-v1.1.md section 8). */
typedef enum {
	SERVER_SESSION_DISCONNECTED = 0,
	SERVER_SESSION_HANDSHAKE,
	SERVER_SESSION_LOBBY,
	SERVER_SESSION_RUNNING,
	SERVER_SESSION_RESULTS
} ServerSessionState;

/*
 * Compile-time bound on admitted human records (D3-B/D3-E: two humans).  The
 * *default* runtime capacity of a freshly initialised session table is one;
 * server_session_run() raises it to the release capacity and fixtures may set
 * an explicit solo/two-human profile with server_session_set_capacity().
 */
#define SERVER_SESSION_MAX_PEERS	XTK_MAX_HUMANS
#define SERVER_SESSION_SOLO_PEERS	1

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

/* Clear the session table and shared coordinator (safe to call repeatedly). */
void server_session_init(void);

/* Install the transport host that the listen loop services (may be NULL). */
void server_session_set_transport(Transport *t);

/* Number of live peer records (0 .. capacity). */
int server_session_count(void);

/* Human admission capacity currently enforced (1 or 2). */
int server_session_capacity(void);

/* Set the human admission capacity; clamps to [1, SERVER_SESSION_MAX_PEERS]. */
void server_session_set_capacity(int capacity);

/* The first live peer (the coordinator's current peer), or NULL. */
ServerSession *server_session_current(void);

/* The peer at roster index `index`, or NULL when unused. */
ServerSession *server_session_peer_at(int index);

/*
 * Accept a connection into the Handshake state.  Returns the session, or NULL
 * when the shared state is not Lobby or the admission capacity is reached (the
 * caller should reject with XTK_ERR_SESSION_FULL and disconnect it).  `peer`
 * may be NULL for a transport-free test session.
 */
ServerSession *server_session_accept(TransportPeer *peer);

/* Drop one peer and release its binding ownership; disconnects its peer. */
void server_session_destroy(ServerSession *s);

/*
 * Reject an inbound peer without creating a session: send REJECT with
 * `error_code`/`text` on channel 0 and disconnect it.  Used for an extra peer.
 */
void server_session_reject_peer(Transport *t, TransportPeer *peer,
				uint16_t error_code, const char *text);

/* ---- read-only accessors --------------------------------------------- */

ServerSessionState server_session_state(const ServerSession *s);
uint32_t server_session_id(const ServerSession *s);
uint32_t server_session_controller_id(const ServerSession *s);
int server_session_index(const ServerSession *s);
uint32_t server_session_epoch(const ServerSession *s);
int server_session_is_admin(const ServerSession *s);
int server_session_ready(const ServerSession *s);
uint16_t server_session_maze_id(const ServerSession *s);
uint16_t server_session_vehicle_id(const ServerSession *s);
int server_session_drone_count(const ServerSession *s);
uint32_t server_session_config_revision(const ServerSession *s);
int server_session_config_valid(const ServerSession *s);
int server_session_bound_slot(const ServerSession *s);
uint32_t server_session_snapshot_seq(const ServerSession *s);
int server_session_events_sent(const ServerSession *s);

/* 1 when every admitted (post-handshake) peer is ready against the revision. */
int server_session_all_ready(const ServerSession *s);

/* Assign/clear the administrator (shared).  Tests may clear it. */
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
 * 1 when the shared coordinator may start: administrator present, running
 * epoch not live, a config has been accepted and every admitted peer is ready.
 */
int server_session_can_start(const ServerSession *s);

/*
 * Build and start one epoch from the shared accepted config.  Loads the
 * allowlisted maze and Vanguard, binds each admitted peer to a deterministic
 * slot, adds the configured drones, resets events at `start_tick`, and emits
 * START + EPOCH_BEGIN to every peer.  Engine globals are touched here
 * (simulation thread only).  Returns SERVER_SESSION_OK or a
 * SERVER_SESSION_ERR_* code; a partial failure releases every resource it
 * acquired and leaves the coordinator in the lobby.
 */
int server_session_begin(ServerSession *s, uint32_t start_tick, uint64_t now_ms);

/*
 * Advance one shared running tick: peer timeout check/abort, one engine step,
 * event observation, per-peer snapshot/event delivery and post-step
 * acknowledgments.  Returns the engine status (GAME_RUNNING ..) or
 * SERVER_SESSION_ERR_STATE when the coordinator is not running.
 */
int server_session_step(ServerSession *s, uint64_t now_ms);

/*
 * Settle a finished match once: capture results before the score reset, deliver
 * RESULTS reliably to every peer, and enter the Results state (retained until
 * reset).
 */
void server_session_finish(ServerSession *s, int status);

/*
 * Return a finished/aborted epoch to the shared lobby after cleanup and emit
 * the LOBBY_STATE barrier to every peer.  Called by the timeout abort and by
 * RESET.
 */
void server_session_return_to_lobby(ServerSession *s);

/* ---- production listen loop ------------------------------------------ */

/*
 * Bounded service pass: poll the transport, admit/reject connects, drop
 * disconnected peers, route decoded messages and (solo profile) auto-start.
 * Always serviced from the simulation thread.  Returns the number of transport
 * events serviced (>= 0).  Exposed for the acceptance fixture.
 */
int server_session_service(uint64_t now_ms);

/*
 * Run the network server: create the listening transport on `port`, service
 * peers, drive lobby and matches until the loop is stopped.  The service stays
 * alive across empty-session lobby cycles.  `fast` bypasses wall-clock pacing;
 * `max_ticks` (0 = unlimited) caps each epoch through the normal results/reset
 * path.  Returns 0 on clean exit.
 */
int server_session_run(uint16_t port, int fast, long max_ticks);

#endif /* _SERVER_SESSION_H_ */
