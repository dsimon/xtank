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
 * saturated session can always neutralise drive/fire. */
#define SERVER_NET_RELEASE_RESERVE	8

typedef struct ServerNetSession ServerNetSession;

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
	unsigned long intents_pushed;
	unsigned long heartbeats;
	unsigned long acks_sent;
	unsigned long applied_marks;
	unsigned long sessions_aborted;
	unsigned long service_events;
	unsigned long service_max_events;
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
 * Admit a raw command array (the message payload) at the next unconsumed tick.
 * `reliable` is 1 for a channel-0 RELIABLE_CMD and 0 for a channel-2 batch;
 * deduplication is shared, so the channel does not affect at-most-once apply.
 */
int server_net_admit_commands(ServerNetSession *s, const XtkCommand *cmds,
			      int count, int reliable, int assign_tick);

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
