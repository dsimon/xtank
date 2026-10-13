/*
 * server_net.h -- bounded input admission and connection ownership (P2.7).
 *
 * This is the seam between the transport (Transport/transport.h) and the
 * simulation's bounded intent queue (Src/Include/intent.h).  It owns, per
 * connected peer:
 *
 *   - connection -> controller -> vehicle slot ownership.  The peer never
 *     supplies an authoritative player: every admitted command is applied to
 *     the slot the session's P2.4 ServerHumanBinding owns, and only while the
 *     binding's generation is current.  Commands name the running epoch; a
 *     command from another epoch is rejected without effect.
 *   - finite/range validation of every wire operand (PROTOCOL-v1 section 6).
 *   - cross-packet/channel deduplication of command ids, so a channel-2 copy
 *     may apply first but a channel-0 repeat never applies twice (no duplicate
 *     edge fire).
 *   - assigned-tick scheduling: accepted commands go to the next unconsumed
 *     engine tick, preserving admission order; the wire tick_hint is only a
 *     freshness hint and never backdates or reserves a future tick.
 *   - acknowledgments (accepted / applied / rejected) that report the
 *     assigned tick and a reason code.
 *
 * The engine is touched only from the simulation thread, inside the bounded
 * pre-step admission point.  No socket or DNS call is made here.
 *
 * This translation unit is not part of the shipped Phase 1 server build; it is
 * linked by the P2.7 fixtures and, from P2.8, by the networked server.  The
 * server loop reaches it through weak symbols, so a build without this module
 * is byte-for-byte unchanged.
 */

#ifndef _SERVER_NET_H_
#define _SERVER_NET_H_

#include <stdint.h>
#include <stddef.h>

#include "../../Protocol/wire.h"
#include "../../Transport/transport.h"

/* Room kept free in the intent queue for release/neutralizing commands so a
 * saturated session can always neutralise drive/fire.  Both peers' server-side
 * departures need two intents each, so eight covers both with slack (D3-E). */
#define SERVER_NET_RELEASE_RESERVE	8

/*
 * D3-E fair bounded admission bounds.  Per connected peer, per simulation
 * tick:
 *
 *   - at most SERVER_NET_PEER_EVENT_QUOTA received transport events are
 *     serviced (the router's per-peer share of the global budget);
 *   - at most SERVER_NET_PEER_INTENT_QUOTA non-release engine intents are
 *     admitted;
 *   - at most SERVER_NET_PEER_RELEASE_QUOTA release/neutralising engine
 *     intents are admitted.
 *
 * Two peers can therefore occupy at most 2*(56+4) = 120 of the 128-entry
 * engine queue in one tick, so a flooding peer can never consume the
 * SERVER_NET_RELEASE_RESERVE head-room and starve the other peer's drive or
 * fire release (nor either peer's server-side neutralisation).
 */
#define SERVER_NET_PEER_INTENT_QUOTA	56
#define SERVER_NET_PEER_RELEASE_QUOTA	4
#define SERVER_NET_PEER_EVENT_QUOTA	32

/* Nonblocking transport work admitted per simulation tick (mirrors the frozen
 * XTK_MAX_SERVICE_WORK_PER_TICK budget in Protocol/limits.h). */
#define SERVER_NET_SERVICE_BUDGET	64

/* Bounded admission-session table (the rotating peer-first cursor wraps here). */
#define SERVER_NET_MAX_SESSIONS		8

typedef struct ServerNetSession ServerNetSession;

/*
 * P3.9 receive/router callback.  server_net_drain() decodes each received
 * datagram and hands it, with the transport peer it arrived on and its actual
 * channel, to this handler.  The session coordinator registers a handler that
 * dispatches every message type; the admission-only pre-step passes NULL and
 * uses the internal command admission path.  This keeps transport_service()
 * and transport_receive() single-owner (this translation unit).
 */
typedef void (*ServerNetRxFn)(void *ctx, TransportPeer *peer,
			      const XtkMessage *msg, uint8_t channel,
			      int assign_tick);

typedef struct {
	unsigned long packets_rx;
	unsigned long packets_tx;
	unsigned long cmds_accepted;
	unsigned long cmds_rejected;
	unsigned long cmds_duplicate;
	unsigned long cmds_stale_epoch;
	unsigned long cmds_foreign;
	unsigned long cmds_nonfinite;
	unsigned long cmds_range;
	unsigned long cmds_unauthorized;
	unsigned long cmds_saturated;
	unsigned long cmds_bad_channel;
	unsigned long intents_pushed;
	unsigned long quota_rejects;
	unsigned long events_skipped;
	unsigned long heartbeats;
	unsigned long acks_sent;
	unsigned long applied_marks;
	unsigned long sessions_aborted;
	unsigned long service_events;
	unsigned long service_max_events;
	/* P3.9 largest number of datagrams decoded in one tick (<=64). */
	unsigned long drain_max_per_tick;
} ServerNetStats;

/* Clear all admission state; safe to call repeatedly. */
void server_net_init(void);

/* Install the server host that server_net_pre_step() services (may be NULL). */
void server_net_set_transport(Transport *t);

/* 1 while a session or a transport is live (the server-loop activation test). */
int server_net_active(void);

/* Number of live sessions and read-only access to one. */
int server_net_session_count(void);
ServerNetSession *server_net_session_at(int index);

/*
 * Create a session for a connected peer.  `peer` may be NULL for an
 * admission-only session (no transport); such a session never sends or
 * receives but still admits commands.  Returns NULL when the table is full.
 */
ServerNetSession *server_net_session_create(TransportPeer *peer);

/* Drop a session and release its binding ownership (bumps generation). */
void server_net_session_destroy(ServerNetSession *s);

/*
 * Bind the session to a controller -> slot mapping with a generation.  The
 * session admits commands only while server_players_validate() agrees, so a
 * rebound slot or stale generation stops the old controller.  Returns 0 or -1.
 */
int server_net_session_bind(ServerNetSession *s, int controller, int slot,
			    unsigned int generation);

int server_net_session_bound(const ServerNetSession *s);
int server_net_session_slot(const ServerNetSession *s);
int server_net_session_controller(const ServerNetSession *s);
unsigned int server_net_session_generation(const ServerNetSession *s);

/* Start an epoch; commands from any other epoch are rejected. */
void server_net_session_begin_epoch(ServerNetSession *s, uint32_t epoch,
				    int first_tick);
uint32_t server_net_session_epoch(const ServerNetSession *s);

/*
 * Admit one decoded INPUT_BATCH or RELIABLE_CMD message.  Returns the number
 * of commands accepted (0..count).  Every command produces an acknowledgment
 * (accepted, applied or rejected) recorded in the session's ack log.
 */
int server_net_admit_message(ServerNetSession *s, const XtkMessage *msg,
			     int assign_tick);

/*
 * Channel-checked admission (P3.6).  `channel` is the actual transport channel
 * the datagram arrived on (transport_receive's out_channel); the message is
 * rejected without engine effect when it does not match the channel its type
 * requires (INPUT_BATCH -> XTK_CH_INPUT, RELIABLE_CMD -> XTK_CH_CONTROL).
 * Every other validation (peer/session binding, epoch, dedup, finite/range,
 * expiry, ownership generation, queue quota/reserve) still precedes the
 * intent.  Returns the number of commands accepted.
 */
int server_net_admit_message_on(ServerNetSession *s, const XtkMessage *msg,
				uint8_t channel, int assign_tick);

/*
 * Admit a raw command array (the message payload) at the next unconsumed tick.
 * `reliable` is 1 for a channel-0 RELIABLE_CMD and 0 for a channel-2 batch;
 * deduplication is shared, so the channel does not affect at-most-once apply.
 */
int server_net_admit_commands(ServerNetSession *s, const XtkCommand *cmds,
			      int count, int reliable, int assign_tick);

/*
 * Begin the admission pass for `assign_tick`: advance the rotating peer-first
 * cursor and reset every session's per-tick quota counters exactly once when
 * the tick changes.  Safe to call repeatedly; admit paths also call it
 * implicitly so a direct-call fixture stays deterministic.
 */
void server_net_begin_tick(int assign_tick);

/* The current rotating peer-first cursor (0 .. SERVER_NET_MAX_SESSIONS-1). */
int server_net_rotation(void);

/*
 * Per-peer transport-event allowance for the current tick.  Returns 1 (and
 * consumes one unit) while the session is within SERVER_NET_PEER_EVENT_QUOTA,
 * else 0.  The router uses this to bound one flooding peer's share of the
 * global service budget without starving the other.
 */
int server_net_session_allow_event(ServerNetSession *s);

/*
 * P3.9 single transport owner.  server_net_service() is the only caller of
 * transport_service() in the tree; server_net_drain() is the only caller of
 * transport_receive().  Both are nonblocking and account the aggregate
 * transport work for `assign_tick` against XTK_MAX_SERVICE_WORK_PER_TICK (64
 * events) even when called more than once per tick, so the frozen bound holds
 * across two peers, reconnects and maximum snapshots.
 *
 * server_net_service() services the transport and returns the number of events
 * processed (0 when the host is absent or already saturated for this tick).
 * server_net_drain() decodes and routes at most SERVER_NET_SERVICE_BUDGET
 * received datagrams through `fn` (or the internal INPUT_BATCH/RELIABLE_CMD
 * admission when `fn` is NULL) and returns the number drained.
 */
int server_net_service(int assign_tick);
int server_net_drain(int assign_tick, ServerNetRxFn fn, void *ctx);

/*
 * Bounded pre-step admission point: service each session's transport within a
 * fixed work budget, then decode and admit every received INPUT_BATCH /
 * RELIABLE_CMD for `assign_tick` (the next unconsumed engine tick).  Returns
 * the number of transport events serviced.  Never blocks.
 */
int server_net_pre_step(int assign_tick);

/*
 * Post-step bookkeeping for the tick that just ran: mark the commands assigned
 * to `tick` applied, expire stale pending commands, and flush acknowledgments.
 */
void server_net_post_step(int tick);

/*
 * Abort a session whose reliable output backlog exceeds the approved bound.
 * Returns 1 (and disconnects/destroys the session) when saturated, else 0.
 */
int server_net_session_backpressure(ServerNetSession *s);

/* Look up the most recent acknowledgment recorded for `command_id`. */
int server_net_session_ack_lookup(const ServerNetSession *s,
				  uint32_t command_id, uint32_t *assigned_tick,
				  uint8_t *status, uint8_t *reason);

/* Cumulative admission counters. */
void server_net_stats(ServerNetStats *out);

#endif /* _SERVER_NET_H_ */
