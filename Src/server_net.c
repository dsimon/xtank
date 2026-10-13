/*
 * server_net.c -- bounded input admission and connection ownership (P2.7).
 *
 * See Src/Include/server_net.h for the contract.  This file is the only place
 * wire commands become engine intents.  It never trusts a peer for the
 * authoritative player: the session's binding supplies the slot, and the
 * binding's generation must still be current.  All engine access happens on
 * the simulation thread at the bounded pre-step admission point.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "server.h"
#include "intent.h"
#include "server_net.h"
#include "server_events.h"

/* Ack log ring: one entry per emitted acknowledgment. */
#define SERVER_NET_ACK_LOG		XTK_CMD_DEDUP_WINDOW

/* Outbound ack queue between post-step flushes (bounded). */
#define SERVER_NET_OUT_ACK		(XTK_CMD_DEDUP_WINDOW * 2)

/* Accepted commands not yet applied (bounded by the dedup window). */
#define SERVER_NET_INFLIGHT		XTK_CMD_DEDUP_WINDOW

typedef struct {
	uint32_t command_id;
	int assigned_tick;
	uint8_t status;
	uint8_t reason;
} NetAck;

struct ServerNetSession {
	int used;
	Transport *t;
	TransportPeer *peer;

	int controller;
	int slot;
	unsigned int generation;
	int bound;

	uint32_t epoch;
	int have_epoch;
	int first_tick;

	uint32_t last_packet_seq;
	int have_packet_seq;

	uint32_t admit_seq;

	uint32_t dedup[XTK_CMD_DEDUP_WINDOW];
	int dedup_head;
	int dedup_count;

	NetAck acklog[SERVER_NET_ACK_LOG];
	int acklog_head;
	int acklog_count;

	NetAck outack[SERVER_NET_OUT_ACK];
	int outack_count;

	struct {
		uint32_t command_id;
		int assigned_tick;
	} inflight[SERVER_NET_INFLIGHT];
	int inflight_count;

	/* D3-E fair per-peer per-tick admission counters.  They are reset when
	 * the tick changes (server_net_begin_tick/note_tick). */
	int quota_tick;
	int quota_intents;		/* non-release engine intents admitted */
	int quota_releases;		/* release/neutralising intents admitted */
	int quota_events;		/* transport events admitted */
};

static ServerNetSession sessions[SERVER_NET_MAX_SESSIONS];
static int nsessions;
static Transport *net_transport;
static ServerNetStats stats;

/* Rotating peer-first admission cursor and the tick the per-peer quotas
 * belong to. */
static int net_tick = 0;
static int net_rotate = 0;

/*
 * P3.9 aggregate transport-work accounting.  server_net_service() is the only
 * caller of transport_service() and server_net_drain() the only caller of
 * transport_receive() in the tree, so both the admission-only pre-step and the
 * session coordinator share one nonblocking owner and one per-tick bound.
 */
static int service_used;	/* transport events serviced this tick */
static int service_tick;	/* the tick service_used belongs to */
static int drain_used;		/* datagrams decoded this tick */
static int drain_tick;		/* the tick drain_used belongs to */

static uint8_t rxbuf[TRANSPORT_MAX_PACKET];
static XtkMessage rxmsg;
static uint8_t txbuf[1024];

void
server_net_init(void)
{
	(void) memset(sessions, 0, sizeof(sessions));
	(void) memset(&stats, 0, sizeof(stats));
	nsessions = 0;
	net_transport = (Transport *) NULL;
}

void
server_net_set_transport(Transport *t)
{
	net_transport = t;
}

/*
 * Advance the rotating peer-first cursor and reset every session's per-tick
 * quota counters exactly once when the simulation tick changes.  A direct-call
 * fixture that never calls server_net_begin_tick() still gets a deterministic
 * reset because every admit/allow path notes the tick here.
 */
static void
note_tick(int assign_tick)
{
	int i;

	if (assign_tick == net_tick)
		return;
	net_tick = assign_tick;
	net_rotate = (net_rotate + 1) % SERVER_NET_MAX_SESSIONS;
	for (i = 0; i < SERVER_NET_MAX_SESSIONS; i++) {
		sessions[i].quota_tick = assign_tick;
		sessions[i].quota_intents = 0;
		sessions[i].quota_releases = 0;
		sessions[i].quota_events = 0;
	}
}

void
server_net_begin_tick(int assign_tick)
{
	note_tick(assign_tick);
}

int
server_net_rotation(void)
{
	return net_rotate;
}

int
server_net_session_allow_event(ServerNetSession *s)
{
	if (s == (ServerNetSession *) NULL || !s->used)
		return 0;
	if (s->quota_events >= SERVER_NET_PEER_EVENT_QUOTA) {
		stats.events_skipped++;
		return 0;
	}
	s->quota_events++;
	return 1;
}

int
server_net_active(void)
{
	return nsessions > 0 || net_transport != (Transport *) NULL;
}

int
server_net_session_count(void)
{
	return nsessions;
}

ServerNetSession *
server_net_session_at(int index)
{
	int i, n = 0;

	if (index < 0)
		return (ServerNetSession *) NULL;
	for (i = 0; i < SERVER_NET_MAX_SESSIONS; i++) {
		if (!sessions[i].used)
			continue;
		if (n == index)
			return &sessions[i];
		n++;
	}
	return (ServerNetSession *) NULL;
}

static ServerNetSession *
session_for_peer(TransportPeer *peer)
{
	int i;

	for (i = 0; i < SERVER_NET_MAX_SESSIONS; i++)
		if (sessions[i].used && sessions[i].peer == peer)
			return &sessions[i];
	return (ServerNetSession *) NULL;
}

ServerNetSession *
server_net_session_create(TransportPeer *peer)
{
	int i;

	for (i = 0; i < SERVER_NET_MAX_SESSIONS; i++) {
		ServerNetSession *s = &sessions[i];

		if (s->used)
			continue;
		(void) memset(s, 0, sizeof(*s));
		s->used = 1;
		s->t = net_transport;
		s->peer = peer;
		s->controller = -1;
		s->slot = -1;
		s->bound = 0;
		nsessions++;
		return s;
	}
	return (ServerNetSession *) NULL;
}

void
server_net_session_destroy(ServerNetSession *s)
{
	if (s == (ServerNetSession *) NULL || !s->used)
		return;
	if (s->bound && s->slot >= 0)
		server_players_unbind_slot(s->slot);
	if (s->t != (Transport *) NULL && s->peer != (TransportPeer *) NULL)
		transport_disconnect(s->t, s->peer);
	s->used = 0;
	s->peer = (TransportPeer *) NULL;
	if (nsessions > 0)
		nsessions--;
}

int
server_net_session_bind(ServerNetSession *s, int controller, int slot,
			unsigned int generation)
{
	if (s == (ServerNetSession *) NULL || !s->used)
		return -1;
	if (controller < 0 || slot < 0)
		return -1;
	s->controller = controller;
	s->slot = slot;
	s->generation = generation;
	s->bound = 1;
	return 0;
}

int
server_net_session_bound(const ServerNetSession *s)
{
	return s != (const ServerNetSession *) NULL && s->used && s->bound;
}

int
server_net_session_slot(const ServerNetSession *s)
{
	return (s != (const ServerNetSession *) NULL) ? s->slot : -1;
}

int
server_net_session_controller(const ServerNetSession *s)
{
	return (s != (const ServerNetSession *) NULL) ? s->controller : -1;
}

unsigned int
server_net_session_generation(const ServerNetSession *s)
{
	return (s != (const ServerNetSession *) NULL) ? s->generation : 0;
}

void
server_net_session_begin_epoch(ServerNetSession *s, uint32_t epoch,
			       int first_tick)
{
	if (s == (ServerNetSession *) NULL || !s->used)
		return;
	s->epoch = epoch;
	s->have_epoch = 1;
	s->first_tick = first_tick;
	s->have_packet_seq = 0;
	s->last_packet_seq = 0;
	s->inflight_count = 0;
	s->outack_count = 0;
	s->quota_tick = net_tick;
	s->quota_intents = 0;
	s->quota_releases = 0;
	s->quota_events = 0;
}

uint32_t
server_net_session_epoch(const ServerNetSession *s)
{
	return (s != (const ServerNetSession *) NULL) ? s->epoch : 0;
}

/* ------------------------------------------------------------------ */
/* Deduplication and acknowledgment bookkeeping                        */
/* ------------------------------------------------------------------ */

static int
dedup_contains(const ServerNetSession *s, uint32_t id)
{
	int i;

	for (i = 0; i < s->dedup_count; i++) {
		int idx = (s->dedup_head + i) % XTK_CMD_DEDUP_WINDOW;

		if (s->dedup[idx] == id)
			return 1;
	}
	return 0;
}

static void
dedup_insert(ServerNetSession *s, uint32_t id)
{
	if (dedup_contains(s, id))
		return;
	if (s->dedup_count < (int) XTK_CMD_DEDUP_WINDOW) {
		int idx = (s->dedup_head + s->dedup_count) % XTK_CMD_DEDUP_WINDOW;

		s->dedup[idx] = id;
		s->dedup_count++;
	} else {
		s->dedup[s->dedup_head] = id;
		s->dedup_head = (s->dedup_head + 1) % XTK_CMD_DEDUP_WINDOW;
	}
}

static void
record_ack(ServerNetSession *s, uint32_t id, int tick, uint8_t status,
	   uint8_t reason)
{
	NetAck *a;
	int idx;

	if (s->acklog_count < (int) SERVER_NET_ACK_LOG) {
		idx = (s->acklog_head + s->acklog_count) % SERVER_NET_ACK_LOG;
		s->acklog_count++;
	} else {
		idx = s->acklog_head;
		s->acklog_head = (s->acklog_head + 1) % SERVER_NET_ACK_LOG;
	}
	a = &s->acklog[idx];
	a->command_id = id;
	a->assigned_tick = tick;
	a->status = status;
	a->reason = reason;

	if (s->outack_count < (int) SERVER_NET_OUT_ACK) {
		NetAck *o = &s->outack[s->outack_count++];

		o->command_id = id;
		o->assigned_tick = tick;
		o->status = status;
		o->reason = reason;
	}
}

int
server_net_session_ack_lookup(const ServerNetSession *s, uint32_t command_id,
			      uint32_t *assigned_tick, uint8_t *status,
			      uint8_t *reason)
{
	int i;

	if (s == (const ServerNetSession *) NULL || !s->used)
		return 0;
	for (i = s->acklog_count - 1; i >= 0; i--) {
		int idx = (s->acklog_head + i) % SERVER_NET_ACK_LOG;
		const NetAck *a = &s->acklog[idx];

		if (a->command_id != command_id)
			continue;
		if (assigned_tick != (uint32_t *) NULL)
			*assigned_tick = (uint32_t) a->assigned_tick;
		if (status != (uint8_t *) NULL)
			*status = a->status;
		if (reason != (uint8_t *) NULL)
			*reason = a->reason;
		return 1;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* Command validation and mapping                                      */
/* ------------------------------------------------------------------ */

/* 0 when the command is within the frozen release op ranges, else the
 * XtkErrorCode that rejects it. */
static uint16_t
command_reason(const XtkCommand *c)
{
	if (!XTK_OP_RELEASE_AUTHORIZED(c->op))
		return XTK_ERR_UNAUTHORIZED_OP;

	switch (c->op) {
	  case XTK_OP_NONE:
	  case XTK_OP_HEARTBEAT:
	  case XTK_OP_FIRE:
		return XTK_ERR_NONE;
	  case XTK_OP_DRIVE_REL:
		if (!xtk_finite_f64(c->a))
			return XTK_ERR_NONFINITE;
		if (c->a != floor(c->a) || c->a < -9.0 || c->a > 9.0)
			return XTK_ERR_RANGE;
		return XTK_ERR_NONE;
	  case XTK_OP_DRIVE_ABS:
		if (!xtk_finite_f64(c->a))
			return XTK_ERR_NONFINITE;
		if (c->a != floor(c->a) || c->a < 0.0 || c->a > 9.0)
			return XTK_ERR_RANGE;
		return XTK_ERR_NONE;
	  case XTK_OP_HEADING:
		if (!xtk_finite_f64(c->a))
			return XTK_ERR_NONFINITE;
		return XTK_ERR_NONE;
	  case XTK_OP_AIM_WORLD:
		if (!xtk_finite_f64(c->a) || !xtk_finite_f64(c->b))
			return XTK_ERR_NONFINITE;
		return XTK_ERR_NONE;
	  case XTK_OP_FIRE_HOLD:
		if (!xtk_finite_f64(c->a))
			return XTK_ERR_NONFINITE;
		if (c->a != 0.0 && c->a != 1.0)
			return XTK_ERR_RANGE;
		return XTK_ERR_NONE;
	  case XTK_OP_WEAPON:
		if (!xtk_finite_f64(c->a) || !xtk_finite_f64(c->b))
			return XTK_ERR_NONFINITE;
		if (c->a != floor(c->a) || c->a < 0.0 || c->a > 5.0)
			return XTK_ERR_RANGE;
		if (c->b != floor(c->b) || c->b < 0.0 || c->b > 2.0)
			return XTK_ERR_RANGE;
		return XTK_ERR_NONE;
	  default:
		return XTK_ERR_UNAUTHORIZED_OP;
	}
}

/* Release / neutralising commands keep the reserved queue room so a saturated
 * session can always stop driving and firing. */
static int
command_is_release(const XtkCommand *c)
{
	switch (c->op) {
	  case XTK_OP_NONE:
	  case XTK_OP_HEARTBEAT:
		return 1;
	  case XTK_OP_DRIVE_REL:
	  case XTK_OP_DRIVE_ABS:
	  case XTK_OP_FIRE_HOLD:
		return c->a == 0.0;
	  case XTK_OP_WEAPON:
		return c->b == 0.0;	/* wire 0 = off */
	  default:
		return 0;
	}
}

/* Map a validated wire command to an engine intent.  Returns 0 when the
 * command has no engine effect (NONE / HEARTBEAT). */
static int
wire_to_intent(const XtkCommand *c, Intent *in)
{
	(void) memset(in, 0, sizeof(*in));
	in->a = c->a;
	in->b = c->b;

	switch (c->op) {
	  case XTK_OP_DRIVE_REL:
		in->op = INTENT_DRIVE_REL;
		return 1;
	  case XTK_OP_DRIVE_ABS:
		in->op = INTENT_DRIVE_ABS;
		return 1;
	  case XTK_OP_HEADING:
		in->op = INTENT_HEADING;
		return 1;
	  case XTK_OP_FIRE:
		in->op = INTENT_FIRE;
		return 1;
	  case XTK_OP_WEAPON:
		in->op = INTENT_WEAPON;
		in->a = (double) (int) c->a;
		/* wire b: 0 off, 1 on, 2 fire -> engine ON/OFF/FIRE. */
		if ((int) c->b == 0)
			in->b = INTENT_WEAPON_OFF;
		else if ((int) c->b == 1)
			in->b = INTENT_WEAPON_ON;
		else
			in->b = INTENT_WEAPON_FIRE;
		return 1;
	  case XTK_OP_AIM_WORLD:
		in->op = INTENT_AIM_WORLD;
		return 1;
	  case XTK_OP_FIRE_HOLD:
		in->op = INTENT_FIRE_HOLD;
		return 1;
	  default:
		return 0;
	}
}

static void
reject(ServerNetSession *s, const XtkCommand *c, int tick, uint16_t reason)
{
	record_ack(s, c->command_id, tick, XTK_ACK_REJECTED, (uint8_t) reason);
	stats.cmds_rejected++;
	switch (reason) {
	  case XTK_ERR_UNAUTHORIZED_OP:	stats.cmds_unauthorized++; break;
	  case XTK_ERR_NONFINITE:	stats.cmds_nonfinite++; break;
	  case XTK_ERR_RANGE:		stats.cmds_range++; break;
	  case XTK_ERR_DUPLICATE:	stats.cmds_duplicate++; break;
	  case XTK_ERR_STALE_EPOCH:	stats.cmds_stale_epoch++; break;
	  case XTK_ERR_STALE_SEQUENCE:	stats.cmds_stale_epoch++; break;
	  case XTK_ERR_FOREIGN_OWNER:	stats.cmds_foreign++; break;
	  case XTK_ERR_QUEUE_SATURATED:	stats.cmds_saturated++; stats.quota_rejects++; break;
	  case XTK_ERR_BAD_CHANNEL:	stats.cmds_bad_channel++; break;
	  default:			break;
	}
}

static int
admit_one(ServerNetSession *s, const XtkCommand *c, int assign_tick,
	  int reliable)
{
	uint16_t reason;
	Intent in;
	int rc;

	(void) reliable;	/* dedup is shared across channels */

	/* Reset the per-peer per-tick quotas when the tick changes. */
	note_tick(assign_tick);

	/* Ownership: the session's binding, never a peer-supplied player. */
	if (!s->bound ||
	    !server_players_validate(s->controller, s->slot, s->generation)) {
		reject(s, c, assign_tick, XTK_ERR_FOREIGN_OWNER);
		return 0;
	}

	/* Cross-packet / cross-channel dedup of the command id. */
	if (dedup_contains(s, c->command_id)) {
		reject(s, c, assign_tick, XTK_ERR_DUPLICATE);
		return 0;
	}

	/* A delayed edge older than the expiry window must not fire after
	 * respawn/reset; the wire tick_hint is only a freshness hint. */
	if ((int) c->tick_hint + (int) XTK_CMD_EXPIRY_TICKS < server_frame()) {
		dedup_insert(s, c->command_id);
		reject(s, c, assign_tick, XTK_ERR_STALE_SEQUENCE);
		return 0;
	}

	reason = command_reason(c);
	if (reason != XTK_ERR_NONE) {
		dedup_insert(s, c->command_id);
		reject(s, c, assign_tick, reason);
		return 0;
	}

	/* Heartbeat / NONE are accepted liveness traffic with no engine effect. */
	if (c->op == XTK_OP_HEARTBEAT || c->op == XTK_OP_NONE) {
		dedup_insert(s, c->command_id);
		record_ack(s, c->command_id, assign_tick, XTK_ACK_ACCEPTED,
			   XTK_ERR_NONE);
		stats.cmds_accepted++;
		if (c->op == XTK_OP_HEARTBEAT)
			stats.heartbeats++;
		return 1;
	}

	/*
	 * Entity-life stamp (P3.11).  A gameplay command carries the controlled
	 * entity-life generation it was produced under; a delayed edge from a
	 * previous life must not fire after a respawn/reset even inside the
	 * expiry window.  A zero stamp is unstamped (accept); a non-zero stamp
	 * must match the session's current controlled life generation.
	 */
	if (c->life_generation != 0) {
		uint32_t cur = server_events_generation(XTK_ENTITY_VEHICLE,
							(uint16_t) s->slot);

		if (c->life_generation != cur) {
			dedup_insert(s, c->command_id);
			reject(s, c, assign_tick, XTK_ERR_STALE_LIFE);
			return 0;
		}
	}

	/* D3-E per-peer per-tick fairness quota.  A flooding peer never gets
	 * more than its own share of this tick, so the healthy peer's
	 * next-tick admission and either peer's drive/fire release stay
	 * available.  Release commands have their own small quota so a peer
	 * can always neutralise. */
	if (command_is_release(c)) {
		if (s->quota_releases >= SERVER_NET_PEER_RELEASE_QUOTA) {
			reject(s, c, assign_tick, XTK_ERR_QUEUE_SATURATED);
			return 0;
		}
	} else if (s->quota_intents >= SERVER_NET_PEER_INTENT_QUOTA) {
		reject(s, c, assign_tick, XTK_ERR_QUEUE_SATURATED);
		return 0;
	}

	/* Keep SERVER_NET_RELEASE_RESERVE engine slots free for the two
	 * server-side neutralisations.  Transient: do not remember it, so a
	 * later batch may retry. */
	if (intent_queued() >= INTENT_QUEUE_MAX - SERVER_NET_RELEASE_RESERVE) {
		reject(s, c, assign_tick, XTK_ERR_QUEUE_SATURATED);
		return 0;
	}

	if (!wire_to_intent(c, &in)) {
		dedup_insert(s, c->command_id);
		reject(s, c, assign_tick, XTK_ERR_UNAUTHORIZED_OP);
		return 0;
	}
	in.tick = assign_tick;
	in.player = s->slot;		/* authoritative owner */
	in.sequence = s->admit_seq++;

	rc = intent_push(&in);
	if (rc == INTENT_OK) {
		if (command_is_release(c))
			s->quota_releases++;
		else
			s->quota_intents++;
		dedup_insert(s, c->command_id);
		record_ack(s, c->command_id, assign_tick, XTK_ACK_ACCEPTED,
			   XTK_ERR_NONE);
		if (s->inflight_count < (int) SERVER_NET_INFLIGHT) {
			s->inflight[s->inflight_count].command_id = c->command_id;
			s->inflight[s->inflight_count].assigned_tick = assign_tick;
			s->inflight_count++;
		}
		stats.cmds_accepted++;
		stats.intents_pushed++;
		return 1;
	}

	if (rc == INTENT_REJECT_FULL) {
		reject(s, c, assign_tick, XTK_ERR_QUEUE_SATURATED);
		return 0;
	}
	if (rc == INTENT_REJECT_DUPLICATE) {
		reject(s, c, assign_tick, XTK_ERR_DUPLICATE);
		return 0;
	}
	if (rc == INTENT_REJECT_FOREIGN || rc == INTENT_REJECT_BADPLAYER) {
		dedup_insert(s, c->command_id);
		reject(s, c, assign_tick, XTK_ERR_FOREIGN_OWNER);
		return 0;
	}
	dedup_insert(s, c->command_id);
	reject(s, c, assign_tick, XTK_ERR_UNAUTHORIZED_OP);
	return 0;
}

int
server_net_admit_commands(ServerNetSession *s, const XtkCommand *cmds,
			  int count, int reliable, int assign_tick)
{
	int i, accepted = 0;

	if (s == (ServerNetSession *) NULL || !s->used ||
	    cmds == (const XtkCommand *) NULL)
		return 0;
	if (count < 0)
		return 0;
	if (count > (int) XTK_MAX_INPUT_COMMANDS_PER_BATCH)
		count = (int) XTK_MAX_INPUT_COMMANDS_PER_BATCH;

	for (i = 0; i < count; i++)
		accepted += admit_one(s, &cmds[i], assign_tick, reliable);
	return accepted;
}

static int
stale_batch(ServerNetSession *s, const XtkCommand *cmds, int count,
	    int assign_tick)
{
	int i;

	for (i = 0; i < count; i++) {
		record_ack(s, cmds[i].command_id, assign_tick, XTK_ACK_REJECTED,
			   XTK_ERR_STALE_EPOCH);
		stats.cmds_rejected++;
		stats.cmds_stale_epoch++;
	}
	return 0;			/* nothing accepted */
}

static int
admit_message_internal(ServerNetSession *s, const XtkMessage *msg,
		       int assign_tick)
{
	int count;

	switch (msg->env.message_type) {
	  case XTK_MSG_INPUT_BATCH:
		count = (int) msg->body.input_batch.command_count;
		if (!s->have_epoch || msg->env.epoch != s->epoch)
			return stale_batch(s, msg->body.input_batch.command,
					   count, assign_tick);
		/* Newer-wins window: ignore an older/duplicate datagram. */
		if (s->have_packet_seq &&
		    (int32_t) (msg->body.input_batch.packet_sequence -
			       s->last_packet_seq) <= 0)
			return 0;
		s->last_packet_seq = msg->body.input_batch.packet_sequence;
		s->have_packet_seq = 1;
		return server_net_admit_commands(s,
						 msg->body.input_batch.command,
						 count, 0, assign_tick);
	  case XTK_MSG_RELIABLE_CMD:
		count = (int) msg->body.reliable_cmd.command_count;
		if (!s->have_epoch || msg->env.epoch != s->epoch)
			return stale_batch(s, msg->body.reliable_cmd.command,
					   count, assign_tick);
		return server_net_admit_commands(s,
						 msg->body.reliable_cmd.command,
						 count, 1, assign_tick);
	  default:
		return 0;
	}
}

int
server_net_admit_message(ServerNetSession *s, const XtkMessage *msg,
			 int assign_tick)
{
	if (s == (ServerNetSession *) NULL || !s->used ||
	    msg == (const XtkMessage *) NULL)
		return 0;
	note_tick(assign_tick);
	return admit_message_internal(s, msg, assign_tick);
}

int
server_net_admit_message_on(ServerNetSession *s, const XtkMessage *msg,
			    uint8_t channel, int assign_tick)
{
	const XtkCommand *cmds = (const XtkCommand *) NULL;
	int count = 0;
	uint8_t want;

	if (s == (ServerNetSession *) NULL || !s->used ||
	    msg == (const XtkMessage *) NULL)
		return 0;

	note_tick(assign_tick);

	/* The actual transport channel must match the message's class before
	 * any command is considered (wire.h XTK_CH_*). */
	want = xtk_next_channel(msg->env.message_type);
	if (want == 0xFFu || channel != want) {
		if (msg->env.message_type == XTK_MSG_INPUT_BATCH) {
			cmds = msg->body.input_batch.command;
			count = (int) msg->body.input_batch.command_count;
		} else if (msg->env.message_type == XTK_MSG_RELIABLE_CMD) {
			cmds = msg->body.reliable_cmd.command;
			count = (int) msg->body.reliable_cmd.command_count;
		}
		for (; count > 0; count--, cmds++) {
			record_ack(s, cmds->command_id, assign_tick,
				   XTK_ACK_REJECTED, XTK_ERR_BAD_CHANNEL);
			stats.cmds_rejected++;
			stats.cmds_bad_channel++;
		}
		return 0;
	}
	return admit_message_internal(s, msg, assign_tick);
}

/* ------------------------------------------------------------------ */
/* Bounded transport service and post-step flush                       */
/* ------------------------------------------------------------------ */

static void
flush_acks(ServerNetSession *s)
{
	int off = 0;

	while (off < s->outack_count) {
		XtkMessage msg;
		int n = s->outack_count - off;
		int i;

		if (n > (int) XTK_ACK_MAX_ENTRIES)
			n = (int) XTK_ACK_MAX_ENTRIES;

		(void) memset(&msg, 0, sizeof(msg));
		msg.env.message_type = XTK_MSG_CMD_ACK;
		msg.env.epoch = s->epoch;
		msg.body.cmd_ack.count = (uint16_t) n;
		for (i = 0; i < n; i++) {
			msg.body.cmd_ack.entry[i].command_id =
				s->outack[off + i].command_id;
			msg.body.cmd_ack.entry[i].assigned_tick =
				(uint32_t) s->outack[off + i].assigned_tick;
			msg.body.cmd_ack.entry[i].status =
				s->outack[off + i].status;
			msg.body.cmd_ack.entry[i].reason =
				s->outack[off + i].reason;
		}
		if (s->t != (Transport *) NULL &&
		    s->peer != (TransportPeer *) NULL) {
			size_t len;

			if (xtk_encode(txbuf, sizeof(txbuf), &msg, &len) ==
			    XTK_ERR_NONE)
				(void) transport_send(s->t, s->peer,
						      XTK_CH_CONTROL, txbuf, len);
		}
		stats.acks_sent += (unsigned long) n;
		off += n;
	}
	s->outack_count = 0;
}

int
server_net_session_backpressure(ServerNetSession *s)
{
	if (s == (ServerNetSession *) NULL || !s->used ||
	    s->t == (Transport *) NULL || s->peer == (TransportPeer *) NULL)
		return 0;
	if (transport_peer_reliable_bytes(s->t, s->peer) >
	    XTK_MAX_RELIABLE_QUEUE_BYTES) {
		stats.sessions_aborted++;
		server_net_session_destroy(s);
		return 1;
	}
	return 0;
}

/*
 * P3.9: the single caller of transport_service().  The remaining per-tick
 * budget is what is left of XTK_MAX_SERVICE_WORK_PER_TICK after every earlier
 * service call for the same tick, so the aggregate never exceeds the frozen
 * bound regardless of how many peers or paths service the host.
 */
int
server_net_service(int assign_tick)
{
	int remaining, events;

	if (net_transport == (Transport *) NULL)
		return 0;

	note_tick(assign_tick);
	if (assign_tick != service_tick) {
		service_tick = assign_tick;
		service_used = 0;
	}
	remaining = (int) XTK_MAX_SERVICE_WORK_PER_TICK - service_used;
	if (remaining <= 0)
		return 0;

	events = transport_service(net_transport, remaining);
	if (events < 0)
		events = 0;
	service_used += events;

	stats.service_events += (unsigned long) events;
	if ((unsigned long) events > stats.service_max_events)
		stats.service_max_events = (unsigned long) events;
	return events;
}

/*
 * P3.9: the single caller of transport_receive().  Each dequeued datagram is
 * decoded once and routed through `fn` (the session coordinator) or the
 * internal admission path (the admission-only pre-step).  Returns the number
 * of datagrams drained; bounded by SERVER_NET_SERVICE_BUDGET.
 */
int
server_net_drain(int assign_tick, ServerNetRxFn fn, void *ctx)
{
	int served = 0;

	if (net_transport == (Transport *) NULL)
		return 0;

	note_tick(assign_tick);
	if (assign_tick != drain_tick) {
		drain_tick = assign_tick;
		drain_used = 0;
	}
	while (served < (int) SERVER_NET_SERVICE_BUDGET) {
		size_t len;
		uint8_t ch;
		TransportPeer *p;
		int r = transport_receive(net_transport, &p, rxbuf,
					  sizeof(rxbuf), &len, &ch);

		if (r <= 0)
			break;
		served++;
		if (xtk_decode(rxbuf, len, &rxmsg) != XTK_ERR_NONE)
			continue;

		if (fn != (ServerNetRxFn) NULL) {
			fn(ctx, p, &rxmsg, ch, assign_tick);
			continue;
		}

		/* Default admission-only routing for one connected peer. */
		{
			ServerNetSession *s = session_for_peer(p);

			if (s == (ServerNetSession *) NULL)
				continue;
			/* Fair per-peer event budget: a flooding peer cannot
			 * consume the whole tick, so the healthy peer is still
			 * serviced. */
			if (!server_net_session_allow_event(s))
				continue;
			stats.packets_rx++;
			(void) server_net_admit_message_on(s, &rxmsg, ch,
							   assign_tick);
		}
	}
	drain_used += served;
	if ((unsigned long) drain_used > stats.drain_max_per_tick)
		stats.drain_max_per_tick = (unsigned long) drain_used;
	return served;
}

int
server_net_pre_step(int assign_tick)
{
	int events;
	int i;
	TransportPeer *p;

	if (nsessions <= 0 && net_transport == (Transport *) NULL)
		return 0;
	if (net_transport == (Transport *) NULL)
		return 0;

	note_tick(assign_tick);

	events = server_net_service(assign_tick);

	while (transport_poll_connect(net_transport, &p)) {
		if (session_for_peer(p) == (ServerNetSession *) NULL)
			(void) server_net_session_create(p);
	}
	while (transport_poll_disconnect(net_transport, &p)) {
		ServerNetSession *s = session_for_peer(p);

		if (s != (ServerNetSession *) NULL)
			server_net_session_destroy(s);
	}

	(void) server_net_drain(assign_tick, (ServerNetRxFn) NULL,
				(void *) NULL);

	for (i = 0; i < SERVER_NET_MAX_SESSIONS; i++)
		if (sessions[i].used)
			(void) server_net_session_backpressure(&sessions[i]);

	return events;
}

void
server_net_post_step(int tick)
{
	int i;

	if (nsessions <= 0)
		return;

	for (i = 0; i < SERVER_NET_MAX_SESSIONS; i++) {
		ServerNetSession *s = &sessions[i];
		int j, w = 0;

		if (!s->used)
			continue;
		for (j = 0; j < s->inflight_count; j++) {
			if (s->inflight[j].assigned_tick <= tick) {
				record_ack(s, s->inflight[j].command_id,
					   s->inflight[j].assigned_tick,
					   XTK_ACK_APPLIED, XTK_ERR_NONE);
				stats.applied_marks++;
			} else {
				s->inflight[w++] = s->inflight[j];
			}
		}
		s->inflight_count = w;
		flush_acks(s);
	}
}

void
server_net_stats(ServerNetStats *out)
{
	if (out == (ServerNetStats *) NULL)
		return;
	*out = stats;
}
