/*-
 * Copyright (c) 1988 Terry Donahue
 * All rights reserved.
 *
 * transport_fixture.c -- P3.9 transport isolation, fragmentation and
 * connection-reuse acceptance.
 *
 * Two modes:
 *
 *   transport  exercises the real pinned ENet v1.3.18 transport over loopback
 *              with two peers and the shared admission layer:
 *                - the aggregate 64-service-event budget per tick holds across
 *                  repeated service calls and both peers (one receive/router
 *                  owner: server_net_service/server_net_drain);
 *                - the application receive queue and the reliable backlog have
 *                  recorded caps; a saturated peer is aborted alone;
 *                - a maximum application snapshot is fragmented unreliably,
 *                  reassembled atomically, and never partially applied under
 *                  simulated fragment loss or fragment reordering; a
 *                  syscall-level packet capture proves the approved
 *                  reliability choice;
 *                - 100 connect/disconnect/rejoin cycles reuse wrappers with no
 *                  stale queued packet, slot exhaustion or growing resources;
 *                - a frozen reader and a flooding peer each depart/are bounded
 *                  alone while the healthy peer keeps progressing.
 *
 *   session    drives the shipping coordinator through a real ENet client,
 *              forcing snapshot sends to fail and checking that at most the
 *              newest unsent snapshot is retained (and that a recovered link
 *              sends the full snapshot).
 *
 * A `TRANSPORT-FIXTURE PASS` means every check passed.
 *
 * Sandbox adaptation (test harness only): this sandbox denies sendmsg()/
 * sendto() with an explicit destination address.  __wrap_sendmsg() connects
 * the socket to the destination, sends, then disconnects it again, which is an
 * equivalent kernel path and still lets one process serve several peers.  The
 * same wrappers capture the ENet protocol commands actually handed to the
 * kernel (the packet-capture evidence) and can simulate unreliable-fragment
 * loss or reorder on the receive side.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <time.h>
#include <unistd.h>

#include "xtank.h"
#include "bullet.h"
#include "clfkr.h"
#include "object.h"
#include "thread.h"
#include "vehicle.h"
#include "team.h"
#include "globals.h"
#include "server.h"
#include "server_events.h"
#include "server_session.h"
#include "server_net.h"
#include "intent.h"
#include "proto.h"

#include "../../Protocol/content-manifest.h"

/* Globals normally owned by server_main.c / main.c (both excluded here). */
char team_char[] = "NROYGBV";

Eset actual_eset, *eset;
Bset actual_bset, *bset;

Maze maze;
Map real_map;
Settings settings;
Boolean game_paused;
char *executable_name;

int num_veh;
int num_veh_alive;
int num_veh_dead;
Vehicle actual_vehicles[MAX_VEHICLES];
Vehicle *live_vehicles[MAX_VEHICLES];
Vehicle *dead_vehicles[MAX_VEHICLES];
CLFkr command_options;

extern char username[MAX_STRING];
extern Mdesc *mdesc;
extern Vdesc *vdesc;
extern int num_combatants;
extern Combatant combatant[];
extern int frame;
extern TeamData teamdata[];
extern int num_teams;

int
InitConfigStruct(CLFkr *ConfigRunning)
{
	ConfigRunning->AutoExit = FALSE;
	ConfigRunning->AutoStart = FALSE;
	ConfigRunning->UseSetting = FALSE;
	ConfigRunning->PrintScores = FALSE;
	ConfigRunning->Settings = NULL;
	ConfigRunning->NoDelay = FALSE;
	ConfigRunning->NoIO = FALSE;
	return 0;
}

/* ---- sandbox send/recv wrap + wire capture ---------------------------- */

ssize_t __real_sendmsg(int fd, const struct msghdr *msg, int flags);
ssize_t __real_recvmsg(int fd, struct msghdr *msg, int flags);

static unsigned long cap_cmd[256];	/* datagrams by first ENet command */
static unsigned long cap_datagrams;
static int cap_enabled;

/* Simulated fragment loss: drop fragment 2 of every other packet, so half the
   packets lose one fragment (and must be discarded whole) while the rest
   reassemble.  Deterministic so the three candidate repeats stay identical. */
static int drop_enable;
static unsigned long drop_frag_count;
static unsigned long recv_pkt_index;

/* Simulated fragment reorder: hold one chosen unreliable fragment and release
   it only after the socket has drained, so ENet sees fragment 1 last and must
   still reassemble the whole snapshot (atomically).  Deterministic. */
static int reorder_enable;
static unsigned char reorder_hold[80000];
static size_t reorder_hold_len;
static struct sockaddr_storage reorder_hold_from;
static socklen_t reorder_hold_fromlen;

static unsigned char scratch_send[80000];
static unsigned char scratch_recv[80000];

/*
 * ENet protocol command bytes (include/enet/protocol.h, pinned v1.3.18):
 * SEND_RELIABLE=6, SEND_UNRELIABLE=7, SEND_FRAGMENT=8, SEND_UNRELIABLE_FRAGMENT=12;
 * the acknowledge flag is 0x80.
 */
#define ENET_CMD_SEND_FRAGMENT_ACK		0x88u
#define ENET_CMD_SEND_UNRELIABLE_FRAGMENT	0x0Cu

/*
 * First ENet command in a datagram.  The 16-bit peerID header carries flags
 * in its high byte (network order): SENT_TIME (0x8000) adds two more header
 * bytes.  No checksum/compression is configured, so the first command begins
 * at offset 2 or 4.
 */
static int
first_command(const unsigned char *p, size_t n, unsigned char *cmd, size_t *off)
{
	size_t o = (p[0] & 0x80u) ? 4u : 2u;

	if (n <= o)
		return 0;
	*off = o;
	*cmd = p[o];
	return 1;
}

static void
capture_record(const unsigned char *p, size_t n)
{
	unsigned char cmd;
	size_t off;

	if (!cap_enabled)
		return;
	cap_datagrams++;
	if (first_command(p, n, &cmd, &off))
		cap_cmd[cmd]++;
}

static void
capture_reset(void)
{
	(void) memset(cap_cmd, 0, sizeof(cap_cmd));
	cap_datagrams = 0;
	cap_enabled = 1;
}

static size_t
gather(const struct msghdr *msg, unsigned char *dst, size_t cap)
{
	size_t off = 0;
	size_t k;

	for (k = 0; k < msg->msg_iovlen && off < cap; k++) {
		size_t c = msg->msg_iov[k].iov_len;

		if (off + c > cap)
			c = cap - off;
		(void) memcpy(dst + off, msg->msg_iov[k].iov_base, c);
		off += c;
	}
	return off;
}

ssize_t
__wrap_sendmsg(int fd, const struct msghdr *msg, int flags)
{
	if (msg == NULL)
		return __real_sendmsg(fd, msg, flags);

	if (cap_enabled) {
		size_t n = gather(msg, scratch_send, sizeof(scratch_send));

		capture_record(scratch_send, n);
	}

	if (msg->msg_name != NULL) {
		if (connect(fd, (struct sockaddr *) msg->msg_name,
			    msg->msg_namelen) == 0) {
			struct sockaddr_storage local;
			socklen_t ll = sizeof(local);
			struct msghdr m = *msg;
			struct sockaddr unspec;
			ssize_t r;
			int had;

			/* Capture the (possibly auto-assigned) local address
			   AFTER the connect, then restore it once disconnected so
			   the socket can again receive from any peer. */
			had = getsockname(fd, (struct sockaddr *) &local, &ll) == 0 &&
			      local.ss_family == AF_INET;
			m.msg_name = NULL;
			m.msg_namelen = 0;
			r = __real_sendmsg(fd, &m, flags);
			(void) memset(&unspec, 0, sizeof(unspec));
			unspec.sa_family = AF_UNSPEC;
			(void) connect(fd, &unspec, sizeof(unspec));
			if (had) {
				int on = 1;

				(void) setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
						  &on, sizeof(on));
				(void) bind(fd, (struct sockaddr *) &local, ll);
			}
			return r;
		}
	}
	return __real_sendmsg(fd, msg, flags);
}

/* Return 1 to consume-and-drop the datagram (simulated fragment loss). */
static int
recv_should_drop(const unsigned char *p, size_t n)
{
	unsigned char cmd;
	size_t off;
	unsigned long frag;

	if (!drop_enable)
		return 0;
	if (!first_command(p, n, &cmd, &off))
		return 0;
	if (cmd != ENET_CMD_SEND_UNRELIABLE_FRAGMENT)
		return 0;
	/* After the 4-byte command header: startSequenceNumber (2),
	   dataLength (2), fragmentCount (4), fragmentNumber (4) -> off+12. */
	if (n < off + 16)
		return 0;
	frag = ((unsigned long) p[off + 12] << 24) |
	       ((unsigned long) p[off + 13] << 16) |
	       ((unsigned long) p[off + 14] << 8) |
	       (unsigned long) p[off + 15];
	if (frag == 0)
		recv_pkt_index++;
	if ((recv_pkt_index & 1UL) == 0 && frag == 2) {
		drop_frag_count++;
		return 1;
	}
	return 0;
}

/* Return 1 if this datagram is fragment 1 of an unreliable fragmented packet
   chosen for the deterministic reorder test. */
static int
recv_is_reorder_frag1(const unsigned char *p, size_t n)
{
	unsigned char cmd;
	size_t off;
	unsigned long frag;

	if (!reorder_enable || reorder_hold_len > 0)
		return 0;
	if (!first_command(p, n, &cmd, &off))
		return 0;
	if (cmd != ENET_CMD_SEND_UNRELIABLE_FRAGMENT)
		return 0;
	if (n < off + 16)
		return 0;
	frag = ((unsigned long) p[off + 12] << 24) |
	       ((unsigned long) p[off + 13] << 16) |
	       ((unsigned long) p[off + 14] << 8) |
	       (unsigned long) p[off + 15];
	return frag == 1;
}

ssize_t
__wrap_recvmsg(int fd, struct msghdr *msg, int flags)
{
	struct msghdr m;
	struct sockaddr_storage from;
	struct iovec iov;
	ssize_t n;

	(void) memset(&m, 0, sizeof(m));
	(void) memset(&from, 0, sizeof(from));
	m.msg_name = &from;
	m.msg_namelen = sizeof(from);
	iov.iov_base = scratch_recv;
	iov.iov_len = sizeof(scratch_recv);
	m.msg_iov = &iov;
	m.msg_iovlen = 1;

	n = __real_recvmsg(fd, &m, flags);
	if (n <= 0) {
		/* Socket drained: release a held reordered fragment last. */
		if (reorder_hold_len > 0) {
			size_t hl = reorder_hold_len;
			size_t off = 0, left = hl, k;

			reorder_hold_len = 0;
			for (k = 0; k < msg->msg_iovlen && left > 0; k++) {
				size_t c = msg->msg_iov[k].iov_len;

				if (c > left)
					c = left;
				(void) memcpy(msg->msg_iov[k].iov_base,
					      reorder_hold + off, c);
				off += c;
				left -= c;
			}
			if (msg->msg_name != NULL && msg->msg_namelen > 0) {
				size_t c = msg->msg_namelen;

				if (c > reorder_hold_fromlen)
					c = reorder_hold_fromlen;
				(void) memcpy(msg->msg_name,
					      &reorder_hold_from, c);
			}
			msg->msg_flags = 0;
			return (ssize_t) hl;
		}
		return n;
	}

	capture_record(scratch_recv, (size_t) n);
	if (recv_should_drop(scratch_recv, (size_t) n))
		return 0;	/* datagram consumed but not delivered */
	if (recv_is_reorder_frag1(scratch_recv, (size_t) n)) {
		/* Hold this fragment; deliver it once the socket has drained
		   so the peer sees it after the later fragments. */
		(void) memcpy(reorder_hold, scratch_recv, (size_t) n);
		reorder_hold_len = (size_t) n;
		reorder_hold_from = from;
		reorder_hold_fromlen = sizeof(from);
		return 0;
	}

	{
		size_t off = 0, left = (size_t) n, k;

		for (k = 0; k < msg->msg_iovlen && left > 0; k++) {
			size_t c = msg->msg_iov[k].iov_len;

			if (c > left)
				c = left;
			(void) memcpy(msg->msg_iov[k].iov_base,
				      scratch_recv + off, c);
			off += c;
			left -= c;
		}
	}
	if (msg->msg_name != NULL && msg->msg_namelen > 0) {
		size_t c = msg->msg_namelen;

		if (c > sizeof(from))
			c = sizeof(from);
		(void) memcpy(msg->msg_name, &from, c);
	}
	msg->msg_flags = 0;
	return n;
}

/* ---- acceptance bookkeeping ------------------------------------------- */

static int failures;
static int checks;

static void
check(const char *what, int ok)
{
	checks++;
	if (!ok) {
		fprintf(stderr, "  [FAIL] %s\n", what);
		failures++;
	} else {
		fprintf(stderr, "  [ok]   %s\n", what);
	}
}

static uint64_t
now_ms(void)
{
	struct timespec ts;

	(void) clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t) ts.tv_sec * 1000ULL + (uint64_t) ts.tv_nsec / 1000000ULL;
}

/* ---- shared setup ----------------------------------------------------- */

static const char *g_assets;

static int
server_bringup(void)
{
	ServerConfig *cfg;

	bset = &actual_bset;
	eset = &actual_eset;
	InitConfigStruct(&command_options);
	command_options.NoDelay = TRUE;
	command_options.AutoExit = TRUE;
	(void) strncpy(username, "p3.9", MAX_STRING - 1);
	username[MAX_STRING - 1] = '\0';

	if (content_set_root(g_assets) != 0) {
		fprintf(stderr, "transport_fixture: %s\n", content_last_error());
		return -1;
	}
	cfg = server_config();
	server_config_init(cfg);
	server_config_release(cfg);
	cfg->seed = 114336;
	cfg->seed_set = 1;
	server_init(cfg);

	init_prog_descs();
	init_settings();
	server_release_settings(&settings);
	init_threader();
	init_msg_sys();
	init_box_names();
	geometry_order_objects();
	geometry_rotate_objects();
	return 0;
}

/* ---- generic ENet helpers --------------------------------------------- */

static int
service_pair(Transport *a, Transport *b, int rounds)
{
	int i;

	for (i = 0; i < rounds; i++) {
		(void) transport_service(a, 64);
		(void) transport_service(b, 64);
	}
	return i;
}

static int
wait_server_peers(Transport *server, Transport **clients, int nclients,
		  TransportPeer **sps, int want)
{
	int i, got = 0;

	for (i = 0; i < 20000 && got < want; i++) {
		int c;
		TransportPeer *tp;

		(void) transport_service(server, 64);
		while (got < want && transport_poll_connect(server, &tp))
			sps[got++] = tp;
		for (c = 0; c < nclients; c++)
			(void) transport_service(clients[c], 64);
	}
	return got;
}

static int
recv_until(Transport *t, TransportPeer **peer, uint8_t *buf, size_t cap,
	   size_t *len, uint8_t *ch, int rounds)
{
	int i;

	for (i = 0; i < rounds; i++) {
		int r = transport_receive(t, peer, buf, cap, len, ch);

		if (r != 0)
			return r;
		(void) transport_service(t, 64);
	}
	return 0;
}

/* Drain and discard every queued application receive. */
static int
drain_rx(Transport *t)
{
	uint8_t buf[TRANSPORT_MAX_PACKET];
	size_t len;
	uint8_t ch;
	TransportPeer *p;
	int n = 0;

	while (transport_receive(t, &p, buf, sizeof(buf), &len, &ch) == 1)
		n++;
	return n;
}

/* Service and discard until the transport has no queued work left, so a
   later sub-test cannot observe an earlier sub-test's packets. */
static void
flush_rx(Transport *t)
{
	int i;

	for (i = 0; i < 400; i++) {
		(void) transport_service(t, 64);
		(void) drain_rx(t);
	}
	(void) drain_rx(t);
}

/* ---- transport mode --------------------------------------------------- */

/* A. aggregate service budget, application caps, single saturated peer. */
static void
audit_budgets_and_caps(Transport *server, Transport *ca, TransportPeer *cpa,
		       Transport *cb, TransportPeer *cpb,
		       TransportPeer *spa, TransportPeer *spb)
{
	(void) cpb;
	TransportStats st;

	fprintf(stderr, "== aggregate transport budget and queue caps\n");

	/* Flood the server's receive queue without draining it: the bounded
	   application queue must cap and count drops. */
	{
		uint8_t small[64];
		int i;

		(void) memset(small, 0x5A, sizeof(small));
		for (i = 0; i < 200; i++) {
			(void) transport_send(ca, cpa, XTK_CH_INPUT, small,
					      sizeof(small));
			(void) transport_service(server, 64);
			(void) transport_service(ca, 64);
		}
		transport_stats(server, &st);
		check("application rx queue depth capped",
		      st.rx_queue_max <= TRANSPORT_RX_QUEUE_MAX);
		check("excess received packets were dropped and counted",
		      st.dropped_rx > 0);
		/* Drain the queue. */
		{
			uint8_t buf[TRANSPORT_MAX_PACKET];
			size_t len;
			uint8_t ch;
			TransportPeer *p;

			while (transport_receive(server, &p, buf,
						 sizeof(buf), &len, &ch) == 1)
				;
			(void) p;
		}
	}

	/* Aggregate service budget: with both peers flooding, the sum of
	   events serviced in one tick never exceeds the frozen 64 bound, and a
	   fresh tick restores the budget. */
	{
		uint8_t small[64];
		int tick, i;

		(void) memset(small, 0x33, sizeof(small));
		server_net_init();
		server_net_set_transport(server);
		(void) server_net_session_create(spa);
		(void) server_net_session_create(spb);

		for (tick = 1; tick <= 2; tick++) {
			int sum = 0, r;

			for (i = 0; i < 300; i++) {
				(void) transport_send(ca, cpa, XTK_CH_INPUT,
						      small, sizeof(small));
				(void) transport_send(cb, cpb, XTK_CH_INPUT,
						      small, sizeof(small));
			}
			do {
				r = server_net_service(tick);
				sum += (r > 0) ? r : 0;
			} while (r > 0);
			check("aggregate service events per tick <= 64",
			      sum <= (int) XTK_MAX_SERVICE_WORK_PER_TICK);
		}
		{
			ServerNetStats ns;

			server_net_stats(&ns);
			check("measured service-work maximum bounded",
			      ns.service_max_events <=
				      XTK_MAX_SERVICE_WORK_PER_TICK);
		}
	}

	/* Reliable output cap: fill one peer's backlog, abort only it. */
	{
		ServerNetSession *sa, *sb;
		uint8_t big[4096];
		size_t max_backlog = 0;
		int i;

		(void) memset(big, 0xCD, sizeof(big));
		server_net_init();
		server_net_set_transport(server);
		sa = server_net_session_create(spa);
		sb = server_net_session_create(spb);
		check("two admission sessions exist",
		      sa != NULL && sb != NULL &&
		      server_net_session_count() == 2);

		for (i = 0; i < 400; i++) {
			size_t b;

			(void) transport_send(server, spb, XTK_CH_CONTROL,
					      big, sizeof(big));
			b = transport_peer_reliable_bytes(server, spb);
			if (b > max_backlog)
				max_backlog = b;
			if (b > XTK_MAX_RELIABLE_QUEUE_BYTES)
				break;
		}
		check("reliable backlog respects 256 KiB/peer (bounded overshoot)",
		      max_backlog <= XTK_MAX_RELIABLE_QUEUE_BYTES + sizeof(big));
		check("saturated peer is aborted alone",
		      server_net_session_backpressure(sb) == 1);
		check("healthy peer session survives the abort",
		      server_net_session_count() == 1 && sa != NULL);
		flush_rx(server);
		/* Healthy peer round-trips. */
		{
			uint8_t buf[64], rbuf[64];
			size_t len, rlen;
			uint8_t ch, rch;
			TransportPeer *rp;
			int ok = 0;

			(void) memset(buf, 0x11, sizeof(buf));
			(void) transport_send(ca, cpa, XTK_CH_INPUT,
					      "ping", 4);
			(void) transport_send(ca, cpa, XTK_CH_INPUT,
					      "pong", 4);
			for (i = 0; i < 5000 && !ok; i++) {
				(void) transport_service(server, 64);
				(void) transport_service(ca, 64);
				if (transport_receive(server, &rp, rbuf,
						      sizeof(rbuf), &rlen,
						      &rch) == 1)
					ok = (rlen >= 4);
			}
			check("healthy peer still round-trips after isolation",
			      ok == 1);
		}
	}
}

/* B. fragmentation, capture and atomic reassembly. */
static void
audit_fragmentation(Transport *server, Transport *ca, TransportPeer *cpa)
{
	static uint8_t big[60000];
	uint8_t buf[TRANSPORT_MAX_PACKET];
	size_t len, rlen;
	uint8_t ch, rch;
	TransportPeer *rp;
	int i;

	fprintf(stderr, "== unreliable fragmentation, capture and reassembly\n");
	flush_rx(server);
	for (i = 0; i < (int) sizeof(big); i++)
		big[i] = (uint8_t) (i * 7 + 3);

	/* Oversized channel-1 snapshot must be an unreliable fragment
	   (command 0x0C), never a reliable fragment/ACK (0x88). */
	capture_reset();
	drop_enable = 0;
	check("maximum snapshot send accepted",
	      transport_send(ca, cpa, XTK_CH_SNAPSHOT, big, sizeof(big)) == 0);
	{
		TransportStats st;

		transport_stats(ca, &st);
		check("oversized channel-1 send is counted as a fragment",
		      st.unreliable_fragments > 0);
	}
	fprintf(stderr, "  capture: datagrams=%lu cmd0x0C=%lu cmd0x88=%lu\n",
		cap_datagrams, cap_cmd[ENET_CMD_SEND_UNRELIABLE_FRAGMENT],
		cap_cmd[ENET_CMD_SEND_FRAGMENT_ACK]);
	check("wire capture shows an unreliable fragment, no reliable fragment",
	      cap_cmd[ENET_CMD_SEND_UNRELIABLE_FRAGMENT] > 0 &&
	      cap_cmd[ENET_CMD_SEND_FRAGMENT_ACK] == 0);
	rlen = 0;
	{
		int rr = recv_until(server, &rp, buf, sizeof(buf), &rlen, &rch,
				    5000);

		fprintf(stderr, "  snapshot recv rr=%d rlen=%zu (want %zu) "
			"ch=%u\n", rr, rlen, sizeof(big), rch);
		check("maximum snapshot reassembled whole",
		      rr == 1 && rlen == sizeof(big) &&
		      memcmp(buf, big, sizeof(big)) == 0);
		check("snapshot arrived on the snapshot channel",
		      rch == XTK_CH_SNAPSHOT);
	}

	/* Reliable channel 0 uses SEND_FRAGMENT+ACK. */
	capture_reset();
	check("maximum reliable send accepted",
	      transport_send(ca, cpa, XTK_CH_CONTROL, big, sizeof(big)) == 0);
	check("wire capture shows a reliable fragment/ACK on channel 0",
	      cap_cmd[ENET_CMD_SEND_FRAGMENT_ACK] > 0);
	rlen = 0;
	check("reliable fragment reassembled whole",
	      recv_until(server, &rp, buf, sizeof(buf), &rlen, &rch, 5000) == 1 &&
	      rlen == sizeof(big) && memcmp(buf, big, sizeof(big)) == 0);

	/* Atomicity under simulated fragment loss: every delivered snapshot is
	   complete and byte-identical; nothing partial is ever delivered. */
	capture_reset();
	drop_enable = 1;
	recv_pkt_index = 0;
	drop_frag_count = 0;
	{
		int sent = 0, delivered = 0, partial = 0;
		int j;

		for (j = 0; j < 40; j++) {
			uint8_t tagbuf[60000];
			size_t got = 0;

			(void) memcpy(tagbuf, big, sizeof(tagbuf));
			/* tag the last bytes so a partial copy is detectable */
			tagbuf[0] = (uint8_t) j;
			tagbuf[sizeof(tagbuf) - 1] = (uint8_t) (0xA0 + j);
			(void) transport_send(ca, cpa, XTK_CH_SNAPSHOT,
					      tagbuf, sizeof(tagbuf));
			sent++;
			for (i = 0; i < 400; i++) {
				(void) transport_service(server, 64);
				(void) transport_service(ca, 64);
				if (transport_receive(server, &rp, buf,
						      sizeof(buf), &got,
						      &rch) == 1)
					break;
			}
			if (got > 0) {
				delivered++;
				if (got != sizeof(tagbuf) ||
				    buf[0] != (uint8_t) j ||
				    buf[got - 1] !=
					    (uint8_t) (0xA0 + j))
					partial++;
			}
		}
		drop_enable = 0;
		fprintf(stderr, "  fragmentation loss: sent=%d delivered=%d "
			"partial=%d dropped-fragments=%lu\n",
			sent, delivered, partial, drop_frag_count);
		check("simulated fragment loss actually dropped fragments",
		      drop_frag_count > 0);
		check("no partially reassembled snapshot was ever delivered",
		      partial == 0);
		check("complete snapshots still arrive under loss",
		      delivered > 0);
	}

	/* Atomicity under fragment reorder: hold one fragment until the socket
	   drains so ENet sees the packet's fragments out of order; the snapshot
	   must still be reassembled whole (never partially applied). */
	capture_reset();
	drop_enable = 0;
	reorder_enable = 1;
	reorder_hold_len = 0;
	{
		uint8_t tagbuf[60000];
		size_t got = 0;
		int rr;

		(void) memcpy(tagbuf, big, sizeof(tagbuf));
		tagbuf[0] = 0x5A;
		tagbuf[sizeof(tagbuf) - 1] = 0xA5;
		check("reordered-snapshot send accepted",
		      transport_send(ca, cpa, XTK_CH_SNAPSHOT, tagbuf,
				     sizeof(tagbuf)) == 0);
		rr = recv_until(server, &rp, buf, sizeof(buf), &got, &rch, 5000);
		reorder_enable = 0;
		fprintf(stderr, "  fragmentation reorder: rr=%d rlen=%zu "
			"(want %zu) held-last-fragment=1\n", rr, got,
			sizeof(tagbuf));
		check("snapshot reassembled whole under fragment reorder",
		      rr == 1 && got == sizeof(tagbuf) &&
		      memcmp(buf, tagbuf, sizeof(tagbuf)) == 0);
		check("reordered snapshot arrived on the snapshot channel",
		      rch == XTK_CH_SNAPSHOT);
	}
}

/* C. 100 connect/disconnect/rejoin cycles reuse wrappers. */
static void
audit_reuse(Transport *server, uint16_t port)
{
	int cycle, ok = 1, stale_bad = 0;
	TransportPeer *sp;
	unsigned long rss0 = 0, rss1 = 0;
	TransportStats st0, st1;

	fprintf(stderr, "== 100 connect/disconnect/rejoin cycles\n");
	transport_stats(server, &st0);
	{
		FILE *f = fopen("/proc/self/statm", "r");
		long pages = 0;

		if (f != NULL) {
			if (fscanf(f, "%ld", &pages) == 1)
				rss0 = (unsigned long) pages;
			fclose(f);
		}
	}

	for (cycle = 0; cycle < 100 && ok; cycle++) {
		Transport *client = transport_create(0, 1);
		TransportPeer *cp;
		uint8_t sbuf[XTK_MAX_MESSAGE_BYTES], rbuf[XTK_MAX_MESSAGE_BYTES];
		char msg[32], got[TRANSPORT_MAX_PACKET];
		size_t len, rlen;
		uint8_t ch;
		int i, connected = 0;

		if (client == NULL) {
			ok = 0;
			break;
		}
		cp = transport_connect(client, "127.0.0.1", port);
		sp = (TransportPeer *) NULL;
		for (i = 0; i < 20000 && sp == (TransportPeer *) NULL; i++) {
			(void) transport_service(server, 64);
			(void) transport_service(client, 64);
			while (transport_poll_connect(server, &sp))
				connected = 1;
		}
		if (!connected || sp == (TransportPeer *) NULL) {
			ok = 0;
			transport_destroy(client);
			break;
		}

		/* Tagged reliable message must arrive intact. */
		(void) snprintf(msg, sizeof(msg), "cycle-%03d", cycle);
		(void) memcpy(sbuf, msg, strlen(msg) + 1);
		(void) transport_send(client, cp, XTK_CH_CONTROL, sbuf,
				      strlen(msg) + 1);
		rlen = 0;
		if (recv_until(server, &sp, (uint8_t *) got, sizeof(got), &rlen,
			       &ch, 5000) != 1 ||
		    rlen != strlen(msg) + 1 || memcmp(got, msg, rlen) != 0)
			ok = 0;

		/* A message sent but never drained, then a disconnect: the
		   queued copy must be purged, not delivered to the next peer. */
		if (cycle == 0) {
			(void) transport_send(client, cp, XTK_CH_CONTROL,
					      "stale-probe", 12);
			(void) transport_service(server, 64);
			/* do not drain */
		}

		/* Graceful close so the server observes the disconnect. */
		transport_disconnect(client, cp);
		for (i = 0; i < 20000; i++) {
			TransportPeer *dp;

			(void) transport_service(server, 64);
			(void) transport_service(client, 64);
			if (transport_poll_disconnect(server, &dp))
				break;
		}
		transport_destroy(client);
		if (cycle == 0) {
			size_t l2;
			uint8_t c2;
			TransportPeer *rp2;

			/* The undrained stale-probe copy must be gone. */
			if (transport_receive(server, &rp2, (uint8_t *) got,
					      sizeof(got), &l2, &c2) == 1 &&
			    l2 == 12 && memcmp(got, "stale-probe", 11) == 0)
				stale_bad = 1;
		}
	}

	check("100 connect/disconnect/rejoin cycles all succeeded", ok == 1);
	check("no stale queued packet survived a wrapper release",
	      stale_bad == 0);
	transport_stats(server, &st1);
	fprintf(stderr, "  reuse: connects=%lu wrappers_released=%lu "
		"wrappers_reused=%lu\n", st1.connects, st1.wrappers_released,
		st1.wrappers_reused);
	check("inbound connects counted", st1.connects >= 100);
	check("disconnect wrappers were released", st1.wrappers_released >= 100);
	check("pool slots were reused, not exhausted",
	      st1.wrappers_reused + 3 >= st1.connects);

	{
		FILE *f = fopen("/proc/self/statm", "r");
		long pages = 0;

		if (f != NULL) {
			if (fscanf(f, "%ld", &pages) == 1)
				rss1 = (unsigned long) pages;
			fclose(f);
		}
	}
	check("resources did not grow without bound across the cycles",
	      rss1 <= rss0 + 2048);	/* <= 8 MiB growth */
}

/* D. frozen reader and flooding peer are isolated.
 *
 * Under the sandbox's explicit-destination send restriction the harness
 * connects/disconnects a shared UDP socket per send (see __wrap_sendmsg); a
 * single in-process host still serves both peers through that path.  A fresh
 * host and two clients are used so no earlier sub-test can contaminate the
 * isolation evidence. */
static void
audit_isolation(void)
{
	Transport *server, *ca, *cb;
	TransportPeer *cpa = (TransportPeer *) NULL;
	TransportPeer *cpb = (TransportPeer *) NULL;
	TransportPeer *spa = (TransportPeer *) NULL;
	TransportPeer *spb = (TransportPeer *) NULL;
	Transport *clients[2];
	TransportPeer *sps[2] = { NULL, NULL };

	fprintf(stderr, "== frozen-reader and flood isolation\n");
	server = transport_create(0, 2);
	ca = transport_create(0, 1);
	cb = transport_create(0, 1);
	if (server == NULL || ca == NULL || cb == NULL) {
		check("isolation host and clients created", 0);
		return;
	}
	cpa = transport_connect(ca, "127.0.0.1", transport_port(server));
	cpb = transport_connect(cb, "127.0.0.1", transport_port(server));
	clients[0] = ca;
	clients[1] = cb;
	(void) cpa;
	(void) cpb;
	check("isolation peers connected",
	      wait_server_peers(server, clients, 2, sps, 2) == 2);
	spa = sps[0];
	spb = sps[1];
	if (spa == NULL || spb == NULL)
		return;

	/* Flooding peer: its per-peer event quota is bounded while the healthy
	   peer's heartbeat is still admitted. */
	{
		ServerNetSession *sa, *sb;
		uint8_t buf[XTK_MAX_MESSAGE_BYTES];
		XtkMessage hb;
		size_t len;
		int i, admitted = 0, hb0;
		ServerNetStats ns;

		server_net_init();
		server_net_set_transport(server);
		server_players_init();
		hb0 = server_players_add("healthy", 1);
		(void) server_players_bind_slot(hb0, 0);
		sa = server_net_session_create(spa);
		sb = server_net_session_create(spb);
		(void) server_net_session_bind(sa, 1, 0,
			      server_players_binding(hb0)->generation);
		server_net_session_begin_epoch(sa, 1, 0);

		/* Healthy peer heartbeat, sent first so it is at the front. */
		(void) memset(&hb, 0, sizeof(hb));
		hb.env.message_type = XTK_MSG_INPUT_BATCH;
		hb.env.epoch = 1;
		hb.body.input_batch.packet_sequence = 1;
		hb.body.input_batch.command_count = 1;
		hb.body.input_batch.command[0].command_id = 0xABCD;
		hb.body.input_batch.command[0].tick_hint = 1;
		hb.body.input_batch.command[0].op = XTK_OP_HEARTBEAT;
		(void) xtk_encode(buf, sizeof(buf), &hb, &len);
		(void) transport_send(ca, cpa, XTK_CH_INPUT, buf, len);

		/* Flooding peer: many batches. */
		for (i = 0; i < 500; i++) {
			hb.body.input_batch.packet_sequence = (uint32_t) (i + 2);
			hb.body.input_batch.command[0].command_id =
				(uint32_t) (0x1000 + i);
			(void) xtk_encode(buf, sizeof(buf), &hb, &len);
			(void) transport_send(cb, cpb, XTK_CH_INPUT, buf, len);
		}

		for (i = 0; i < 200 && !admitted; i++) {
			(void) server_net_pre_step(i + 1);
			server_net_stats(&ns);
			if (ns.heartbeats > 0)
				admitted = 1;
		}
		server_net_stats(&ns);
		check("healthy peer is not starved by a flooding peer",
		      admitted == 1);
		check("per-peer event quota bounded the flooder",
		      ns.events_skipped > 0);
		check("measured decode budget bounded per tick",
		      ns.drain_max_per_tick <= SERVER_NET_SERVICE_BUDGET);
	}

	/* Frozen reader: one peer never reads; its reliable backlog is capped
	   and it is aborted alone while the healthy peer round-trips. */
	{
		ServerNetSession *sa, *sb;
		uint8_t big[4096];
		int i, ok = 0;

		(void) memset(big, 0xEE, sizeof(big));
		flush_rx(server);
		server_net_init();
		server_net_set_transport(server);
		sa = server_net_session_create(spa);
		sb = server_net_session_create(spb);
		(void) sa;

		for (i = 0; i < 400 &&
		     transport_peer_reliable_bytes(server, spb) <=
			     XTK_MAX_RELIABLE_QUEUE_BYTES; i++)
			(void) transport_send(server, spb, XTK_CH_CONTROL,
					      big, sizeof(big));
		check("frozen reader's reliable backlog reached the cap",
		      transport_peer_reliable_bytes(server, spb) >
			      XTK_MAX_RELIABLE_QUEUE_BYTES);
		check("frozen reader is aborted alone",
		      server_net_session_backpressure(sb) == 1 &&
		      server_net_session_count() == 1);

		/* Healthy peer still round-trips reliable bytes. */
		(void) transport_send(ca, cpa, XTK_CH_CONTROL, "healthy", 8);
		for (i = 0; i < 5000 && !ok; i++) {
			uint8_t rb[64];
			size_t rl;
			uint8_t rc;
			TransportPeer *rp;

			(void) transport_service(server, 64);
			(void) transport_service(ca, 64);
			if (transport_receive(server, &rp, rb, sizeof(rb),
					      &rl, &rc) == 1)
				ok = (rl == 8 && memcmp(rb, "healthy", 8) == 0);
		}
		check("healthy peer progresses while the frozen reader is gone",
		      ok == 1);
	}

	transport_destroy(cb);
	transport_destroy(ca);
	transport_destroy(server);
}

static int
run_transport(void)
{
	Transport *server;
	Transport *ca, *cb;
	TransportPeer *cpa = (TransportPeer *) NULL;
	TransportPeer *cpb = (TransportPeer *) NULL;
	TransportPeer *spa = (TransportPeer *) NULL;
	TransportPeer *spb = (TransportPeer *) NULL;
	uint16_t port;
	Transport *clients[2];
	TransportPeer *sps[2];

	fprintf(stderr, "== bringing up a two-peer ENet host\n");
	server = transport_create(0, 2);
	check("server transport created", server != NULL);
	if (server == NULL)
		return failures ? 1 : 0;
	port = transport_port(server);
	check("server bound to an ephemeral port", port != 0);

	ca = transport_create(0, 1);
	cb = transport_create(0, 1);
	check("two client transports created", ca != NULL && cb != NULL);
	if (ca == NULL || cb == NULL)
		return failures ? 1 : 0;
	cpa = transport_connect(ca, "127.0.0.1", port);
	cpb = transport_connect(cb, "127.0.0.1", port);
	check("client A connect initiated", cpa != NULL);
	check("client B connect initiated", cpb != NULL);

	clients[0] = ca;
	clients[1] = cb;
	check("both peers connected",
	      wait_server_peers(server, clients, 2, sps, 2) == 2);
	spa = sps[0];
	spb = sps[1];
	if (spa == NULL || spb == NULL)
		return failures ? 1 : 0;

	audit_budgets_and_caps(server, ca, cpa, cb, cpb, spa, spb);
	audit_fragmentation(server, ca, cpa);
	audit_isolation();

	transport_destroy(cb);
	transport_destroy(ca);
	transport_destroy(server);

	/* Reuse cycles need a clean host. */
	{
		Transport *host = transport_create(0, 2);

		check("reuse host created", host != NULL);
		if (host != NULL) {
			audit_reuse(host, transport_port(host));
			transport_destroy(host);
		}
	}
	return failures ? 1 : 0;
}

/* ---- session mode: newest unsent snapshot retention ------------------- */

/* Force transport_send to fail on one channel/peer to exercise retention. */
static uint8_t g_fail_channel = 0xFF;
static TransportPeer *g_fail_peer;

int
__real_transport_send(Transport *t, TransportPeer *peer, uint8_t channel,
		      const void *data, size_t len);

int
__wrap_transport_send(Transport *t, TransportPeer *peer, uint8_t channel,
		      const void *data, size_t len)
{
	if (channel == g_fail_channel && (g_fail_peer == NULL ||
					  peer == g_fail_peer))
		return -1;
	return __real_transport_send(t, peer, channel, data, len);
}

static void
make_hello(XtkMessage *m, const char *name)
{
	(void) memset(m, 0, sizeof(*m));
	m->env.message_type = XTK_MSG_HELLO;
	m->body.hello.client_nonce = 0x11223344u;
	m->body.hello.proto_major = XTK_PROTOCOL_MAJOR;
	m->body.hello.proto_minor = XTK_PROTOCOL_MINOR;
	m->body.hello.client_build = 1;
	(void) strncpy(m->body.hello.name, name, XTK_MAX_NAME_BYTES);
	(void) memcpy(m->body.hello.content_set_digest,
		      xtk_content_set_digest, XTK_SHA256_BYTES);
}

static int
tf_send_msg(Transport *client, TransportPeer *cp, const XtkMessage *m)
{
	uint8_t buf[XTK_MAX_MESSAGE_BYTES];
	size_t len = 0;

	if (xtk_encode(buf, sizeof(buf), m, &len) != XTK_ERR_NONE)
		return -1;
	return transport_send(client, cp, xtk_next_channel(m->env.message_type),
			      buf, len);
}

/* One valid INPUT_BATCH heartbeat keeps the peer's liveness clock fresh. */
static void
client_heartbeat(Transport *client, TransportPeer *cp, uint32_t epoch,
		 uint32_t seq, uint32_t id)
{
	XtkMessage hb;

	(void) memset(&hb, 0, sizeof(hb));
	hb.env.message_type = XTK_MSG_INPUT_BATCH;
	hb.env.epoch = epoch;
	hb.body.input_batch.packet_sequence = seq;
	hb.body.input_batch.command_count = 1;
	hb.body.input_batch.command[0].command_id = id;
	hb.body.input_batch.command[0].tick_hint = (uint32_t) frame + 1;
	hb.body.input_batch.command[0].op = XTK_OP_HEARTBEAT;
	(void) tf_send_msg(client, cp, &hb);
}

/* Service the server coordinator and the client until `want` decodes. */
static int
client_wait(Transport *host, Transport *client, int want, uint64_t *nowp)
{
	int i;

	for (i = 0; i < 8000; i++) {
		uint8_t buf[XTK_MAX_MESSAGE_BYTES];
		size_t len;
		uint8_t ch;
		TransportPeer *p;

		(void) server_session_service(*nowp);
		(void) transport_service(client, 64);
		if (transport_receive(client, &p, buf, sizeof(buf), &len,
				      &ch) == 1) {
			XtkMessage m;

			if (xtk_decode(buf, len, &m) == XTK_ERR_NONE &&
			    m.env.message_type == want)
				return want;
		}
		*nowp += 16;
	}
	return -1;
}

static int
run_session(void)
{
	Transport *host, *client;
	TransportPeer *cp;
	ServerSession *s;
	uint64_t now = now_ms();
	XtkMessage m;
	int i;

	server_session_init();
	server_session_set_capacity(1);
	host = transport_create(0, SERVER_SESSION_MAX_PEERS);
	check("session host created", host != NULL);
	if (host == NULL)
		return failures ? 1 : 0;
	server_session_set_transport(host);
	client = transport_create(0, 1);
	cp = transport_connect(client, "127.0.0.1", transport_port(host));
	check("session client connect initiated", cp != NULL);

	for (i = 0; i < 20000 && server_session_count() == 0; i++) {
		(void) server_session_service(now);
		(void) transport_service(client, 64);
		now += 8;
	}
	check("session peer admitted", server_session_count() == 1);
	s = server_session_current();
	if (s == (ServerSession *) NULL)
		return failures ? 1 : 0;

	make_hello(&m, "alpha");
	check("hello sent", tf_send_msg(client, cp, &m) == 0);
	check("welcome received",
	      client_wait(host, client, XTK_MSG_WELCOME, &now) ==
	      XTK_MSG_WELCOME);

	(void) memset(&m, 0, sizeof(m));
	m.env.message_type = XTK_MSG_CONFIG_SELECT;
	m.body.config_select.maze_id = XTK_CONTENT_ID_COMBAT1;
	m.body.config_select.vehicle_id = XTK_CONTENT_ID_VANGUARD;
	m.body.config_select.drone_count = 1;
	check("config sent", tf_send_msg(client, cp, &m) == 0);
	check("config status received",
	      client_wait(host, client, XTK_MSG_CONFIG_STATUS, &now) ==
	      XTK_MSG_CONFIG_STATUS);

	(void) memset(&m, 0, sizeof(m));
	m.env.message_type = XTK_MSG_READY;
	m.body.ready.ready = 1;
	m.body.ready.config_revision = server_session_config_revision(s);
	check("ready sent", tf_send_msg(client, cp, &m) == 0);

	for (i = 0; i < 20000 &&
	     server_session_state(s) != SERVER_SESSION_RUNNING; i++) {
		(void) server_session_service(now);
		(void) transport_service(client, 64);
		now += 16;
	}
	check("solo match auto-started", server_session_state(s) ==
	      SERVER_SESSION_RUNNING);
	if (server_session_state(s) != SERVER_SESSION_RUNNING)
		return failures ? 1 : 0;

	/* Forced snapshot send failure: at most one newest snapshot retained,
	   the peer is not dropped, and the sequence does not advance. */
	g_fail_channel = XTK_CH_SNAPSHOT;
	g_fail_peer = server_session_peer(s);
	{
		uint32_t seq0 = server_session_snapshot_seq(s);
		uint32_t hbseq = 1;
		int pending_seen = 0, over_one = 0;

		for (i = 0; i < 12; i++) {
			client_heartbeat(client, cp,
					 server_session_epoch(NULL), hbseq++,
					 0x2000u + (uint32_t) i);
			(void) server_session_service(now);
			(void) transport_service(client, 64);
			(void) server_session_step(s, now);
			now += 66;
			if (server_session_pending_snapshot(s) > 0)
				pending_seen = 1;
			if (server_session_pending_snapshot(s) > 1)
				over_one = 1;
		}
		check("newest unsent snapshot retained on send failure",
		      pending_seen == 1);
		check("at most one unsent snapshot ever retained",
		      over_one == 0);
		check("retention did not advance the sent sequence",
		      server_session_snapshot_seq(s) == seq0);
		check("healthy peer survives a transient snapshot failure",
		      server_session_state(s) == SERVER_SESSION_RUNNING &&
		      server_session_count() == 1);
	}

	/* Recovery: with the link healthy the retained snapshot is flushed and
	   a fresh one advances the sequence. */
	g_fail_channel = 0xFF;
	g_fail_peer = NULL;
	{
		int got_snap = 0;
		uint32_t seq0 = server_session_snapshot_seq(s);
		uint32_t hbseq = 100;

		for (i = 0; i < 90 && !got_snap; i++) {
			uint8_t buf[XTK_MAX_MESSAGE_BYTES];
			size_t len;
			uint8_t ch;
			TransportPeer *p;

			client_heartbeat(client, cp,
					 server_session_epoch(NULL), hbseq++,
					 0x3000u + (uint32_t) i);
			(void) server_session_service(now);
			(void) transport_service(client, 64);
			(void) server_session_step(s, now);
			now += 66;
			while (transport_receive(client, &p, buf, sizeof(buf),
						 &len, &ch) == 1) {
				XtkMessage d;

				if (xtk_decode(buf, len, &d) == XTK_ERR_NONE &&
				    d.env.message_type == XTK_MSG_VIEW_SNAPSHOT)
					got_snap = 1;
			}
		}
		check("client receives a full snapshot after recovery",
		      got_snap == 1);
		check("no snapshot remains retained after recovery",
		      server_session_pending_snapshot(s) == 0);
		check("snapshot sequence advanced after recovery",
		      server_session_snapshot_seq(s) > seq0);
	}

	server_session_destroy(s);
	transport_destroy(client);
	transport_destroy(host);
	return failures ? 1 : 0;
}

int
main(int argc, char *argv[])
{
	const char *mode = (argc > 2) ? argv[2] : "transport";

	g_assets = (argc > 1) ? argv[1] : ".";
	if (server_bringup() != 0)
		return 2;

	if (!strcmp(mode, "session")) {
		(void) run_session();
		if (failures != 0) {
			printf("TRANSPORT-FIXTURE FAIL (%d checks, session)\n",
			       failures);
			return 1;
		}
		printf("TRANSPORT-FIXTURE PASS (session: newest unsent snapshot "
		       "retained on forced send failure, recovered full)\n");
		return 0;
	}

	(void) run_transport();
	if (failures != 0) {
		printf("TRANSPORT-FIXTURE FAIL (%d checks, transport)\n",
		       failures);
		return 1;
	}
	printf("TRANSPORT-FIXTURE PASS (transport: aggregate 64-event budget, "
	       "application/ENet caps, unreliable fragmentation capture + "
	       "atomic reassembly under loss, 100 reuse cycles, frozen/flood "
	       "isolation)\n");
	return 0;
}
