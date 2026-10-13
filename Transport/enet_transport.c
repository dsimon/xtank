/*
 * Transport/enet_transport.c -- ENet-backed transport implementation (P2.7).
 *
 * The only translation unit in the tree that includes ENet.  ENet is pinned
 * at v1.3.18 (P2.1-REPORT.md, D2-B); no reliability is implemented over raw
 * UDP here.
 *
 * All calls run on the simulation thread.  transport_service() uses a zero
 * timeout, so a paused or slow reader can never block the simulation: the
 * worst it can do is fill this module's bounded receive queue (packets are
 * dropped and counted) or grow the peer's reliable backlog, which the caller
 * watches with transport_peer_reliable_bytes() and aborts.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>
#include <string.h>

#include <enet/enet.h>

#include "transport.h"

/* Three protocol channels: 0 reliable ordered, 1/2 unreliable sequenced. */
#define TRANSPORT_CHANNELS	3

struct TransportPeer {
	ENetPeer *enet;
	int connected;
	int used_once;		/* pool slot has already carried a wrapper */
};

typedef struct {
	TransportPeer *peer;
	uint8_t channel;
	size_t len;
	uint8_t *data;
} RxEntry;

struct Transport {
	ENetHost *host;
	TransportPeer *pool;
	int pool_cap;

	TransportPeer **conn_q;
	int conn_head, conn_count;
	TransportPeer **disc_q;
	int disc_head, disc_count;

	RxEntry rx[TRANSPORT_RX_QUEUE_MAX];
	int rx_head, rx_count;

	TransportStats stats;
};

static int enet_ready;

/*
 * Claim a free pool slot for `ep` (used by inbound CONNECT and by an explicit
 * outgoing connect).  A slot that already carried a wrapper is counted as a
 * reuse, which is the P3.9 evidence that connect/disconnect cycles recycle
 * wrappers instead of exhausting the pool.
 */
static TransportPeer *
pool_claim(Transport *t, ENetPeer *ep)
{
	int i;

	for (i = 0; i < t->pool_cap; i++) {
		if (t->pool[i].enet == (ENetPeer *) NULL) {
			if (t->pool[i].used_once)
				t->stats.wrappers_reused++;
			t->pool[i].enet = ep;
			t->pool[i].connected = 0;
			t->pool[i].used_once = 1;
			ep->data = &t->pool[i];
			return &t->pool[i];
		}
	}
	return (TransportPeer *) NULL;
}

/* Locate (or, for a connect, create) the wrapper for an ENet peer. */
static TransportPeer *
peer_wrap(Transport *t, ENetPeer *ep, int create)
{
	if (ep == (ENetPeer *) NULL)
		return (TransportPeer *) NULL;
	if (ep->data != NULL)
		return (TransportPeer *) ep->data;
	if (!create)
		return (TransportPeer *) NULL;
	return pool_claim(t, ep);
}

/*
 * Release a wrapper back to the pool.  The ENet peer is unbound first so a
 * later ENet peer that reuses the same address can never resolve to this
 * wrapper's old connection; the wrapper pointer itself stays valid until the
 * pool slot is handed out again, so a caller still holding it can map it to
 * its own peer record.
 */
static void
peer_release(Transport *t, TransportPeer *p)
{
	if (p == (TransportPeer *) NULL)
		return;
	if (p->enet != (ENetPeer *) NULL)
		p->enet->data = NULL;
	p->enet = (ENetPeer *) NULL;
	p->connected = 0;
	t->stats.wrappers_released++;
}

/*
 * Drop every not-yet-delivered received copy that belongs to `p`.  Called when
 * the wrapper is released: the connection is gone, so its queued traffic must
 * not be delivered later (which would let a reused wrapper deliver a previous
 * connection's bytes).
 */
static void
rx_purge_peer(Transport *t, TransportPeer *p)
{
	int i;

	for (i = 0; i < TRANSPORT_RX_QUEUE_MAX; i++) {
		RxEntry *e = &t->rx[i];

		if (e->data != NULL && e->peer == p) {
			free(e->data);
			e->data = NULL;
			e->len = 0;
			e->peer = (TransportPeer *) NULL;
			t->stats.dropped_rx++;
		}
	}
}

static void
q_push(TransportPeer **q, int *head, int *count, int cap, TransportPeer *p)
{
	if (*count >= cap)
		return;			/* bounded: drop an unread event */
	q[(*head + *count) % cap] = p;
	(*count)++;
}

static int
q_pop(TransportPeer **q, int *head, int *count, int cap, TransportPeer **out)
{
	if (*count <= 0)
		return 0;
	*out = q[*head];
	*head = (*head + 1) % cap;
	(*count)--;
	return 1;
}

Transport *
transport_create(uint16_t port, int max_peers)
{
	Transport *t;
	ENetAddress addr;
	int cap;

	if (!enet_ready) {
		if (enet_initialize() != 0)
			return (Transport *) NULL;
		enet_ready = 1;
	}
	if (max_peers < 1)
		max_peers = 1;

	t = (Transport *) calloc(1, sizeof(Transport));
	if (t == (Transport *) NULL)
		return (Transport *) NULL;

	addr.host = ENET_HOST_ANY;
	addr.port = port;
	t->host = enet_host_create(&addr, (size_t) max_peers,
				   TRANSPORT_CHANNELS, 0, 0);
	if (t->host == (ENetHost *) NULL) {
		free(t);
		return (Transport *) NULL;
	}

	cap = max_peers + 1;
	t->pool_cap = cap;
	t->pool = (TransportPeer *) calloc((size_t) cap, sizeof(TransportPeer));
	t->conn_q = (TransportPeer **) calloc((size_t) cap, sizeof(TransportPeer *));
	t->disc_q = (TransportPeer **) calloc((size_t) cap, sizeof(TransportPeer *));
	if (t->pool == NULL || t->conn_q == NULL || t->disc_q == NULL) {
		transport_destroy(t);
		return (Transport *) NULL;
	}
	return t;
}

void
transport_destroy(Transport *t)
{
	int i;

	if (t == (Transport *) NULL)
		return;

	for (i = 0; i < TRANSPORT_RX_QUEUE_MAX; i++) {
		if (t->rx[i].data != NULL)
			free(t->rx[i].data);
		t->rx[i].data = NULL;
	}
	if (t->host != (ENetHost *) NULL)
		enet_host_destroy(t->host);
	free(t->pool);
	free(t->conn_q);
	free(t->disc_q);
	free(t);
}

uint16_t
transport_port(Transport *t)
{
	if (t == (Transport *) NULL || t->host == (ENetHost *) NULL)
		return 0;
	return t->host->address.port;
}

TransportPeer *
transport_connect(Transport *t, const char *host, uint16_t port)
{
	TransportPeer *p;
	ENetPeer *ep;
	ENetAddress addr;

	if (t == (Transport *) NULL || host == (const char *) NULL)
		return (TransportPeer *) NULL;
	if (enet_address_set_host(&addr, host) != 0)
		return (TransportPeer *) NULL;
	addr.port = port;

	ep = enet_host_connect(t->host, &addr, TRANSPORT_CHANNELS, 0);
	if (ep == (ENetPeer *) NULL)
		return (TransportPeer *) NULL;
	p = pool_claim(t, ep);
	if (p == (TransportPeer *) NULL) {
		/* No wrapper slot: never leave a half-tracked ENet peer. */
		enet_peer_disconnect_now(ep, 0);
		return (TransportPeer *) NULL;
	}
	p->connected = 0;
	return p;
}

int
transport_service(Transport *t, int budget)
{
	int n = 0;

	if (t == (Transport *) NULL || t->host == (ENetHost *) NULL)
		return -1;
	if (budget <= 0)
		budget = 1;

	t->stats.service_calls++;
	while (n < budget) {
		ENetEvent ev;
		int r = enet_host_service(t->host, &ev, 0);

		if (r <= 0)
			break;
		n++;
		switch (ev.type) {
		  case ENET_EVENT_TYPE_CONNECT: {
			TransportPeer *p = peer_wrap(t, ev.peer, 1);

			if (p != (TransportPeer *) NULL) {
				p->connected = 1;
				q_push(t->conn_q, &t->conn_head, &t->conn_count,
				       t->pool_cap, p);
				t->stats.connects++;
			}
			break;
		  }
		  case ENET_EVENT_TYPE_DISCONNECT: {
			TransportPeer *p = peer_wrap(t, ev.peer, 0);

			if (p != (TransportPeer *) NULL) {
				p->connected = 0;
				q_push(t->disc_q, &t->disc_head, &t->disc_count,
				       t->pool_cap, p);
				t->stats.disconnects++;
			}
			break;
		  }
		  case ENET_EVENT_TYPE_RECEIVE: {
			TransportPeer *p = peer_wrap(t, ev.peer, 0);
			size_t len = ev.packet->dataLength;

			if (p != (TransportPeer *) NULL &&
			    len <= TRANSPORT_MAX_PACKET &&
			    t->rx_count < TRANSPORT_RX_QUEUE_MAX) {
				uint8_t *copy = (uint8_t *) malloc(len ? len : 1);

				if (copy != (uint8_t *) NULL) {
					int idx = (t->rx_head + t->rx_count) %
						  TRANSPORT_RX_QUEUE_MAX;

					if (len)
						(void) memcpy(copy, ev.packet->data,
							      len);
					t->rx[idx].peer = p;
					t->rx[idx].channel = ev.channelID;
					t->rx[idx].len = len;
					t->rx[idx].data = copy;
					t->rx_count++;
					if ((unsigned long) t->rx_count >
					    t->stats.rx_queue_max)
						t->stats.rx_queue_max =
							(unsigned long) t->rx_count;
					t->stats.packets_rx++;
					t->stats.bytes_rx += len;
				} else {
					t->stats.dropped_rx++;
				}
			} else {
				t->stats.dropped_rx++;
			}
			enet_packet_destroy(ev.packet);
			break;
		  }
		  default:
			break;
		}
	}

	t->stats.service_events += (unsigned long) n;
	if ((unsigned long) n > t->stats.service_max_events)
		t->stats.service_max_events = (unsigned long) n;
	return n;
}

int
transport_poll_connect(Transport *t, TransportPeer **peer)
{
	if (t == (Transport *) NULL)
		return 0;
	return q_pop(t->conn_q, &t->conn_head, &t->conn_count, t->pool_cap,
		     peer);
}

int
transport_poll_disconnect(Transport *t, TransportPeer **peer)
{
	int got;

	if (t == (Transport *) NULL)
		return 0;
	got = q_pop(t->disc_q, &t->disc_head, &t->disc_count, t->pool_cap,
		    peer);
	if (got) {
		/*
		 * The connection is over: purge any undelivered copy of its
		 * traffic and return the wrapper to the pool.  The returned
		 * pointer stays valid for the caller's peer lookup until the
		 * slot is handed out again, but no longer resolves to the dead
		 * ENet peer.
		 */
		rx_purge_peer(t, *peer);
		peer_release(t, *peer);
	}
	return got;
}

int
transport_receive(Transport *t, TransportPeer **peer, uint8_t *out,
		  size_t cap, size_t *out_len, uint8_t *out_channel)
{
	RxEntry *e;

	if (t == (Transport *) NULL || t->rx_count <= 0)
		return 0;

	e = &t->rx[t->rx_head];
	if (e->len > cap) {
		free(e->data);
		e->data = NULL;
		t->rx_head = (t->rx_head + 1) % TRANSPORT_RX_QUEUE_MAX;
		t->rx_count--;
		t->stats.dropped_rx++;
		return -1;
	}
	if (e->len)
		(void) memcpy(out, e->data, e->len);
	*out_len = e->len;
	*out_channel = e->channel;
	*peer = e->peer;

	free(e->data);
	e->data = NULL;
	t->rx_head = (t->rx_head + 1) % TRANSPORT_RX_QUEUE_MAX;
	t->rx_count--;
	return 1;
}

int
transport_send(Transport *t, TransportPeer *peer, uint8_t channel,
	       const void *data, size_t len)
{
	ENetPacket *pkt;
	enet_uint32 flags;

	if (t == (Transport *) NULL || peer == (TransportPeer *) NULL ||
	    peer->enet == (ENetPeer *) NULL || channel >= TRANSPORT_CHANNELS)
		return -1;
	if (len > TRANSPORT_MAX_PACKET)
		return -1;

	/*
	 * D3-F: channel 0 is reliable ordered; channels 1 and 2 are unreliable
	 * sequenced and use ENet's unreliable-fragment flag.  Without the flag
	 * ENet would send an oversized unreliable packet as reliable
	 * SEND_FRAGMENT+ACK commands, silently turning a dropped view into a
	 * retransmitted reliable backlog.  With it, a snapshot that does not
	 * fully reassemble is discarded whole -- never partially applied.
	 */
	if (channel == 0)
		flags = ENET_PACKET_FLAG_RELIABLE;
	else
		flags = ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT;
	if (channel != 0 && peer->enet->mtu > 0 &&
	    len > (size_t) peer->enet->mtu)
		t->stats.unreliable_fragments++;
	pkt = enet_packet_create(data, len, flags);
	if (pkt == (ENetPacket *) NULL)
		return -1;
	if (enet_peer_send(peer->enet, channel, pkt) != 0) {
		enet_packet_destroy(pkt);
		return -1;
	}
	enet_host_flush(t->host);
	t->stats.packets_tx++;
	t->stats.bytes_tx += len;
	return 0;
}

static size_t
list_bytes(const ENetList *list)
{
	ENetListNode *it;
	size_t total = 0;

	for (it = enet_list_begin(list); it != enet_list_end(list);
	     it = enet_list_next(it)) {
		ENetOutgoingCommand *c = (ENetOutgoingCommand *) it;

		/* fragmentLength is the wire bytes for this command; a fragmented
		 * packet appears once per fragment, so never sum packet->dataLength. */
		total += c->fragmentLength;
	}
	return total;
}

size_t
transport_peer_reliable_bytes(Transport *t, TransportPeer *peer)
{
	ENetPeer *p;

	if (t == (Transport *) NULL || peer == (TransportPeer *) NULL ||
	    peer->enet == (ENetPeer *) NULL)
		return 0;
	p = peer->enet;
	/* Reliable commands with a packet wait in outgoingSendReliableCommands;
	 * other queued commands wait in outgoingCommands; reliableDataInTransit
	 * counts the fragments already on the wire awaiting acknowledgement. */
	return (size_t) p->reliableDataInTransit +
	       list_bytes(&p->outgoingSendReliableCommands) +
	       list_bytes(&p->outgoingCommands);
}

size_t
transport_peer_outgoing_bytes(Transport *t, TransportPeer *peer)
{
	ENetPeer *p;

	if (t == (Transport *) NULL || peer == (TransportPeer *) NULL ||
	    peer->enet == (ENetPeer *) NULL)
		return 0;
	p = peer->enet;
	/* Every not-yet-removed command for this peer, reliable or not, plus
	 * the reliable bytes already on the wire awaiting acknowledgement.  An
	 * unreliable command is removed as soon as it is written to the socket,
	 * so this reflects the true per-peer ENet queue footprint. */
	return (size_t) p->reliableDataInTransit +
	       list_bytes(&p->sentReliableCommands) +
	       list_bytes(&p->outgoingSendReliableCommands) +
	       list_bytes(&p->outgoingCommands);
}

int
transport_peer_connected(Transport *t, TransportPeer *peer)
{
	if (t == (Transport *) NULL || peer == (TransportPeer *) NULL)
		return 0;
	return peer->connected;
}

void
transport_disconnect(Transport *t, TransportPeer *peer)
{
	if (t == (Transport *) NULL || peer == (TransportPeer *) NULL ||
	    peer->enet == (ENetPeer *) NULL)
		return;
	enet_peer_disconnect(peer->enet, 0);
	enet_host_flush(t->host);
}

void
transport_stats(Transport *t, TransportStats *out)
{
	if (t == (Transport *) NULL || out == (TransportStats *) NULL)
		return;
	*out = t->stats;
}
