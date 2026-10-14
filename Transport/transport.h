/*
 * Transport/transport.h -- bounded, nonblocking datagram transport seam (P2.7).
 *
 * The server simulation never touches a socket.  This module is the only
 * place ENet (pinned v1.3.18, D2-B) appears; the admission layer and the
 * session speak this neutral interface.  It is deliberately independent of
 * the engine, the wire protocol and SDL: it moves opaque byte strings on the
 * three protocol channels (0 reliable ordered, 1/2 unreliable sequenced).
 *
 * Design rules (docs/PHASE2-PLAN.md section 3, docs/PROTOCOL-v1.md):
 *
 *   - Every call is nonblocking.  transport_service() polls with a zero
 *     timeout and does at most `budget` events of work; it never waits.
 *   - Connection setup (DNS/name resolution and connect) happens outside the
 *     simulation tick, never in transport_service().
 *   - Received packets are copied into a bounded internal queue.  A full
 *     queue drops the newest packet and is counted; nothing grows without
 *     bound.
 *   - Reliable output queued for a peer is reported so the caller can abort a
 *     saturated peer instead of buffering forever.
 *
 * P3.9 additions (docs/PHASE3-DECISIONS.md D3-F):
 *
 *   - Channel 0 is reliable ordered.  Channels 1 and 2 are unreliable
 *     *sequenced* and are sent with ENet's unreliable-fragment flag, so an
 *     oversized snapshot is fragmented and reassembled by ENet without ever
 *     being silently downgraded to the reliable backlog (a packet that does
 *     not fully reassemble is discarded, never partially applied).
 *   - A peer's receive wrapper is released (and any undelivered queued copy of
 *     its traffic purged) when its disconnect is observed, so repeated
 *     connect/disconnect cycles reuse pool slots instead of exhausting them
 *     and a reused wrapper never aliases the previous connection's traffic.
 *   - The total queued ENet output for a peer is measurable
 *     (transport_peer_outgoing_bytes) so the caller can keep the actual ENet
 *     per-peer queue inside the approved bound.
 *
 * All functions must be called from one thread (the simulation thread).
 */

#ifndef XTK_TRANSPORT_H
#define XTK_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>

/* Largest application packet the transport will carry or deliver. */
#define TRANSPORT_MAX_PACKET	65536u

/* Maximum received packets held before transport_service() starts dropping. */
#define TRANSPORT_RX_QUEUE_MAX	64

typedef struct Transport Transport;
typedef struct TransportPeer TransportPeer;

typedef struct {
	unsigned long packets_rx;
	unsigned long packets_tx;
	unsigned long bytes_rx;
	unsigned long bytes_tx;
	unsigned long connects;
	unsigned long disconnects;
	unsigned long dropped_rx;	/* rx queue full / oversized packet   */
	unsigned long service_calls;
	unsigned long service_events;
	unsigned long service_max_events;
	/* P3.9 bounded-work/lifetime evidence. */
	unsigned long wrappers_released;	/* disconnect wrappers freed  */
	unsigned long wrappers_reused;		/* pool slots handed out again */
	unsigned long rx_queue_max;		/* high-water rx queue depth  */
	unsigned long unreliable_fragments;	/* ch1/2 packets over one MTU */
	/*
	 * P3.13 bounded read-only ENet service/output timing (nanoseconds).
	 * Only accumulated when the measurement harness sets XTANK_TIMING;
	 * otherwise both stay 0 and no clock is read.
	 */
	unsigned long long ns_service;		/* inside transport_service */
	unsigned long long ns_send;		/* inside transport_send     */
	/*
	 * P3.13 read-only ENet wire totals from the host counters: UDP payload
	 * bytes/packets ENet actually sent (including ENet command headers and
	 * fragmentation).  Compared against bytes_tx they expose the real wire
	 * fragment/header overhead, not the theoretical packet size.
	 */
	unsigned long long tx_wire_bytes;
	unsigned long long tx_wire_packets;
} TransportStats;

/*
 * Create a listening host bound to `port` (0 selects an ephemeral port) that
 * accepts at most `max_peers` simultaneous peers.  Returns NULL on failure
 * (including ENet initialisation failure).  `channels` is fixed at 3 by the
 * protocol; callers do not choose it.
 */
Transport *transport_create(uint16_t port, int max_peers);

/* Tear the host and every peer down; frees queued receive copies. */
void transport_destroy(Transport *t);

/* The local port the host is bound to (valid after transport_create). */
uint16_t transport_port(Transport *t);

/*
 * Begin an outgoing connection to `host`:`port`.  This resolves the address
 * and must be called outside a simulation tick (it is not part of
 * transport_service()).  Returns the new peer handle, or NULL on failure.
 * The peer is not usable until a CONNECT event is observed.
 */
TransportPeer *transport_connect(Transport *t, const char *host, uint16_t port);

/*
 * Service the host without blocking.  Polls with a zero timeout and processes
 * at most `budget` events (connect, disconnect or receive), copying received
 * payloads into the bounded queue.  Returns the number of events processed
 * (>= 0) or -1 on a fatal host error.  Never sleeps and never waits on I/O.
 */
int transport_service(Transport *t, int budget);

/*
 * Pop one pending connection/disconnection event, or return 0 when none.
 * The peer handle stays valid until the peer is destroyed or the transport is
 * torn down.
 */
int transport_poll_connect(Transport *t, TransportPeer **peer);
int transport_poll_disconnect(Transport *t, TransportPeer **peer);

/*
 * Copy the oldest queued received packet into out[0..cap).  Returns 1 and
 * sets out_len and out_channel on success, 0 when the queue is empty, or -1
 * if the packet does not fit `cap` (it is discarded).  Packets are delivered
 * in arrival order.
 */
int transport_receive(Transport *t, TransportPeer **peer, uint8_t *out,
		      size_t cap, size_t *out_len, uint8_t *out_channel);

/*
 * Queue `len` bytes on `channel` for `peer`.  Channel 0 is reliable ordered;
 * channels 1 and 2 are unreliable sequenced (D3-F: an oversized packet is
 * fragmented with ENet's unreliable-fragment flag and never downgraded to the
 * reliable backlog).  Returns 0 on success, -1 on a bad argument or a send
 * failure.  Data is copied by the transport.
 */
int transport_send(Transport *t, TransportPeer *peer, uint8_t channel,
		   const void *data, size_t len);

/*
 * Bytes of reliable (channel 0) output still queued or in flight for `peer`.
 * The caller aborts a peer whose reliable backlog exceeds the approved bound
 * (XTK_MAX_RELIABLE_QUEUE_BYTES) rather than buffering without bound.
 */
size_t transport_peer_reliable_bytes(Transport *t, TransportPeer *peer);

/*
 * Bytes of ENet output still queued or in flight for `peer` across every
 * channel (reliable + unreliable).  This is the actual per-peer ENet queue
 * size, independent of the application layer; callers use it to record and
 * bound the real transport memory footprint (P3.9).
 */
size_t transport_peer_outgoing_bytes(Transport *t, TransportPeer *peer);

/* 1 while `peer` is connected (a CONNECT was seen and no DISCONNECT yet). */
int transport_peer_connected(Transport *t, TransportPeer *peer);

/* Close `peer` now and release it. */
void transport_disconnect(Transport *t, TransportPeer *peer);

/* Cumulative transport counters (for bounded-work evidence). */
void transport_stats(Transport *t, TransportStats *out);

#endif /* XTK_TRANSPORT_H */
