/*
 * impairment_relay.c -- P3.14 real-UDP impairment middlebox (userspace netem).
 *
 * A real UDP proxy on the loopback path between one authoritative ENet server
 * and N ENet clients.  Every application datagram is forwarded over real UDP
 * sockets with configurable symmetric delay, loss, reorder, duplication and a
 * probabilistic extra-delay term, so the shipped ENet/transport path is
 * exercised on a genuinely impaired link (P3.14).
 *
 * Topology per client i (port P_i chosen by the relay):
 *
 *   client i --(real UDP to P_i)--> cfd[i] (bound P_i, connected to client)
 *   cfd[i] --(real UDP from P_i)--> server    [c2s]
 *   server --(real UDP to Q_i)--> sfd[i] (bound Q_i, connected to server)
 *   sfd[i] --(real UDP to client)--> client   [s2c]
 *
 * The client-facing socket cfd is bound to P_i so each client sees replies from
 * the exact port it sent to (ENet matches peers by address).  The server-facing
 * socket sfd uses a *distinct* ephemeral port Q_i: the two must not share a
 * port, because SO_REUSEPORT hashing can then deliver a client datagram to the
 * connected server-facing socket, which silently drops non-server sources and
 * would stall the handshake (observed as an intermittent bring-up failure).
 * The server's replies to Q_i are received on sfd and forwarded to the client
 * from cfd (source P_i).  Connected sends are required because this sandbox
 * forbids unconnected sendto/sendmsg (the fixtures link -Wl,--wrap=sendmsg for
 * the same reason).
 *
 * The relay writes a per-datagram text trace and a framed binary capture of
 * every delivered server->client payload per client, so the harness can compute
 * the *measured* impairment and audit recipient authorization.
 *
 * Build: see Tests/phase3/impairment.sh.  Usage: --help.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define MAX_CLIENTS 8
#define QMAX 131072
#define MAXPKT 65536

/* ---- deterministic PRNG (xorshift64; no libc rand) -------------------- */
static uint64_t g_rng;
static uint64_t rng_next(void)
{
	uint64_t x = g_rng;
	x ^= x << 13; x ^= x >> 7; x ^= x << 17;
	g_rng = x;
	return x;
}
static double rng_unit(void) { return (double) (rng_next() >> 11) / 9007199254740992.0; }

static long long now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long) ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

typedef struct {
	int used;
	int port;
	int sfd;			/* bound Q_i, connected to server (s2c recv / c2s send) */
	int cfd;			/* bound P_i, connected to client (c2s recv / s2c send) */
	int cfd_connected;
	struct sockaddr_in addr;	/* client address */
	unsigned long long scheduled, delivered, dropped, dup, reordered, extra;
	long long delay_sum, delay_max;
	int delay_n;
	FILE *cap;			/* framed delivered s2c payloads */
} Client;

typedef struct {
	long long rel;			/* release time */
	int dir;			/* 0 = c2s, 1 = s2c */
	int ci;				/* client index */
	int len;
	int extra;			/* extra-delay applied */
	int reordered;
	unsigned char *data;
} Pkt;

/* config */
static int g_listen_port, g_server_port;
static long long g_delay_ns, g_extra_ns, g_reorder_ns;
static double g_loss, g_reorder, g_dup, g_extra_prob;
static long long g_duration_ns;
static const char *g_trace_path, *g_capture_dir, *g_ready_file, *g_stats_path;
static const char *g_label = "relay";
static unsigned long long g_seed_val;
static int g_nclients = 2;
static FILE *g_trace;

static Client g_clients[MAX_CLIENTS];
static int g_nclients_init;

static void
trace_pkt(const char *dir, int ci, int len, const char *action,
	  int extra, int reorder)
{
	if (g_trace == NULL)
		return;
	fprintf(g_trace, "%lld %s ci=%d len=%d action=%s extra=%d reorder=%d\n",
		now_ns(), dir, ci, len, action, extra, reorder);
}

static void
apply(Pkt *p)
{
	if (g_loss > 0.0 && rng_unit() < g_loss) {
		p->len = 0;	/* mark dropped */
		p->extra = -1;
		return;
	}
	p->rel = now_ns() + g_delay_ns;
	if (g_reorder > 0.0 && rng_unit() < g_reorder) {
		p->reordered = 1;
		p->rel += g_reorder_ns;
	}
	if (g_extra_prob > 0.0 && rng_unit() < g_extra_prob) {
		p->extra = 1;
		p->rel += g_extra_ns;
	}
}

static Pkt g_q[QMAX];
static int g_qn;

static void
enqueue(const unsigned char *d, int len, int dir, int ci)
{
	Pkt *p;
	int copies = 1;

	if (g_dup > 0.0 && rng_unit() < g_dup)
		copies = 2;
	while (copies-- > 0) {
		if (g_qn >= QMAX)
			return;
		p = &g_q[g_qn++];
		memset(p, 0, sizeof(*p));
		p->dir = dir;
		p->ci = ci;
		p->len = len;
		p->data = (unsigned char *) malloc((size_t) len);
		if (p->data != NULL)
			memcpy(p->data, d, (size_t) len);
		apply(p);
		if (g_clients[ci].used)
			g_clients[ci].scheduled++;
	}
}

static void
deliver(Pkt *p)
{
	int fd;
	Client *c = (p->ci >= 0 && p->ci < MAX_CLIENTS) ? &g_clients[p->ci] : NULL;

	if (c == NULL || !c->used)
		return;
	if (p->len == 0) {
		c->dropped++;
		trace_pkt(p->dir ? "s2c" : "c2s", p->ci, 0, "drop", 0, 0);
		return;
	}
	fd = (p->dir == 0) ? c->sfd : c->cfd;
	if (fd < 0 || (p->dir == 1 && !c->cfd_connected)) {
		c->dropped++;
		return;
	}
	if (send(fd, p->data, p->len, 0) < 0) {
		c->dropped++;
		trace_pkt(p->dir ? "s2c" : "c2s", p->ci, p->len, "sendfail",
			  p->extra, p->reordered);
		return;
	}
	c->delivered++;
	if (p->reordered)
		c->reordered++;
	if (p->extra == 1)
		c->extra++;
	{
		long long lat = now_ns() - (p->rel - g_delay_ns -
			(p->reordered ? g_reorder_ns : 0) -
			(p->extra == 1 ? g_extra_ns : 0));
		if (lat < 0) lat = 0;
		c->delay_sum += lat; c->delay_n++;
		if (lat > c->delay_max) c->delay_max = lat;
	}
	trace_pkt(p->dir ? "s2c" : "c2s", p->ci, p->len,
		  p->reordered ? "reorder" : "fwd", p->extra, p->reordered);
	if (p->dir == 1 && c->cap != NULL) {
		uint32_t n = (uint32_t) p->len;
		unsigned char hdr[4] = { (unsigned char) (n >> 24),
			(unsigned char) (n >> 16), (unsigned char) (n >> 8),
			(unsigned char) n };
		fwrite(hdr, 1, 4, c->cap);
		fwrite(p->data, 1, (size_t) p->len, c->cap);
	}
}

static void
pump_queue(void)
{
	long long t = now_ns();
	int i;
	for (i = 0; i < g_qn;) {
		if (g_q[i].rel <= t) {
			deliver(&g_q[i]);
			if (g_q[i].data != NULL)
				free(g_q[i].data);
			g_q[i] = g_q[--g_qn];
		} else
			i++;
	}
}

static void
write_stats(long long elapsed_ns)
{
	FILE *f = fopen(g_stats_path, "w");
	int i;
	if (f == NULL)
		return;
	fprintf(f, "label=%s\n", g_label);
	fprintf(f, "listen_port=%d server_port=%d clients=%d\n",
		g_listen_port, g_server_port, g_nclients);
	fprintf(f, "seed=%llu\n", g_seed_val);
	fprintf(f, "delay_ms=%.3f extra_delay_ms=%.3f extra_prob=%.4f loss=%.4f "
		"reorder=%.4f reorder_ms=%.3f dup=%.4f\n", g_delay_ns / 1e6,
		g_extra_ns / 1e6, g_extra_prob, g_loss, g_reorder,
		g_reorder_ns / 1e6, g_dup);
	fprintf(f, "elapsed_ms=%.1f nclients=%d\n", elapsed_ns / 1e6,
		g_nclients_init);
	for (i = 0; i < g_nclients_init; i++) {
		Client *c = &g_clients[i];
		fprintf(f, "client%d scheduled=%llu delivered=%llu dropped=%llu "
			"reordered=%llu extra=%llu dup=%llu delay_avg_ms=%.3f "
			"delay_max_ms=%.3f\n", i, c->scheduled, c->delivered,
			c->dropped, c->reordered, c->extra, c->dup,
			c->delay_n ? (double) c->delay_sum / c->delay_n / 1e6 : 0.0,
			(double) c->delay_max / 1e6);
	}
	fclose(f);
}

static volatile sig_atomic_t g_stop;

static void
on_signal(int s)
{
	(void) s;
	g_stop = 1;
}

static void
usage(void)
{
	fprintf(stderr,
	  "usage: impairment_relay --server PORT [--clients N] [--delay-ms N]\n"
	  "  [--loss P] [--reorder P] [--reorder-ms N] [--dup P]\n"
	  "  [--extra-delay-ms N] [--extra-prob P] [--seed N] [--duration-ms N]\n"
	  "  [--trace FILE] [--capture-dir DIR] [--stats FILE] [--ready-file FILE]\n"
	  "  [--label NAME]\n");
}

/* Create client i's two sockets, both bound to the same port P_i. */
static int
setup_client(int i)
{
	Client *c = &g_clients[i];
	struct sockaddr_in a, sv;
	socklen_t al;
	int on = 1;

	memset(c, 0, sizeof(*c));
	c->sfd = c->cfd = -1;

	/* cfd: client-facing, bound to P_i, connected once the client's address
	 * is learned from its first datagram. */
	c->cfd = socket(AF_INET, SOCK_DGRAM, 0);
	if (c->cfd < 0)
		return -1;
	setsockopt(c->cfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
	setsockopt(c->cfd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));

	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	a.sin_port = htons((uint16_t) (i == 0 ? g_listen_port : 0));
	if (bind(c->cfd, (struct sockaddr *) &a, sizeof(a)) != 0) {
		perror("relay bind cfd");
		return -1;
	}
	al = sizeof(a);
	if (getsockname(c->cfd, (struct sockaddr *) &a, &al) != 0)
		return -1;
	c->port = ntohs(a.sin_port);

	/* sfd: server-facing on a distinct ephemeral port Q_i, connected to the
	 * server.  See the topology note above: sharing P_i would let the kernel
	 * hash client datagrams into this connected socket and drop them. */
	c->sfd = socket(AF_INET, SOCK_DGRAM, 0);
	if (c->sfd < 0)
		return -1;
	setsockopt(c->sfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
	setsockopt(c->sfd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	a.sin_port = 0;
	if (bind(c->sfd, (struct sockaddr *) &a, sizeof(a)) != 0) {
		perror("relay bind sfd");
		return -1;
	}

	memset(&sv, 0, sizeof(sv));
	sv.sin_family = AF_INET;
	sv.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	sv.sin_port = htons((uint16_t) g_server_port);
	if (connect(c->sfd, (struct sockaddr *) &sv, sizeof(sv)) != 0) {
		perror("relay connect sfd");
		return -1;
	}

	if (g_capture_dir != NULL) {
		char p[1200];
		snprintf(p, sizeof(p), "%s/cap_client%d_s2c.bin", g_capture_dir, i);
		c->cap = fopen(p, "wb");
	}
	c->used = 1;
	return 0;
}

int
main(int argc, char **argv)
{
	struct sockaddr_in la, server_addr;
	int i, ready_written = 0;
	long long t0;
	int on = 1;

	setvbuf(stderr, NULL, _IONBF, 0);
	for (i = 1; i < argc; i++) {
		const char *a = argv[i];
		const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;
		if (strcmp(a, "--help") == 0) { usage(); return 0; }
		else if (strcmp(a, "--listen") == 0 && v) { g_listen_port = atoi(v); i++; }
		else if (strcmp(a, "--server") == 0 && v) { g_server_port = atoi(v); i++; }
		else if (strcmp(a, "--clients") == 0 && v) { g_nclients = atoi(v); i++; }
		else if (strcmp(a, "--delay-ms") == 0 && v) { g_delay_ns = (long long) (atof(v) * 1e6); i++; }
		else if (strcmp(a, "--extra-delay-ms") == 0 && v) { g_extra_ns = (long long) (atof(v) * 1e6); i++; }
		else if (strcmp(a, "--extra-prob") == 0 && v) { g_extra_prob = atof(v); i++; }
		else if (strcmp(a, "--loss") == 0 && v) { g_loss = atof(v); i++; }
		else if (strcmp(a, "--reorder") == 0 && v) { g_reorder = atof(v); i++; }
		else if (strcmp(a, "--reorder-ms") == 0 && v) { g_reorder_ns = (long long) (atof(v) * 1e6); i++; }
		else if (strcmp(a, "--dup") == 0 && v) { g_dup = atof(v); i++; }
		else if (strcmp(a, "--seed") == 0 && v) { g_seed_val = strtoull(v, NULL, 0); i++; }
		else if (strcmp(a, "--duration-ms") == 0 && v) { g_duration_ns = (long long) (atoll(v) * 1000000LL); i++; }
		else if (strcmp(a, "--trace") == 0 && v) { g_trace_path = v; i++; }
		else if (strcmp(a, "--capture-dir") == 0 && v) { g_capture_dir = v; i++; }
		else if (strcmp(a, "--stats") == 0 && v) { g_stats_path = v; i++; }
		else if (strcmp(a, "--ready-file") == 0 && v) { g_ready_file = v; i++; }
		else if (strcmp(a, "--label") == 0 && v) { g_label = v; i++; }
		else { fprintf(stderr, "relay: bad arg %s\n", a); usage(); return 2; }
	}
	if (g_server_port <= 0) { usage(); return 2; }
	if (g_nclients < 1) g_nclients = 1;
	if (g_nclients > MAX_CLIENTS) g_nclients = MAX_CLIENTS;
	if (g_reorder_ns == 0) g_reorder_ns = 3000000LL + (long long) (3.0 * 1e6);
	g_rng = g_seed_val ? g_seed_val : 114336ULL;

	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);

	memset(&server_addr, 0, sizeof(server_addr));
	server_addr.sin_family = AF_INET;
	server_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	server_addr.sin_port = htons((uint16_t) g_server_port);

	for (i = 0; i < g_nclients; i++) {
		if (setup_client(i) != 0)
			return 1;
	}
	g_nclients_init = g_nclients;

	if (g_ready_file != NULL) {
		FILE *rf = fopen(g_ready_file, "w");
		if (rf != NULL) {
			for (i = 0; i < g_nclients; i++)
				fprintf(rf, "%d\n", g_clients[i].port);
			fclose(rf);
		}
		ready_written = 1;
	}
	if (g_trace_path != NULL) {
		g_trace = fopen(g_trace_path, "w");
		if (g_trace != NULL)
			setvbuf(g_trace, NULL, _IOLBF, 0);
	}
	fprintf(stderr, "relay: %d clients on ports", g_nclients);
	for (i = 0; i < g_nclients; i++)
		fprintf(stderr, " %d", g_clients[i].port);
	fprintf(stderr, " -> server %d (%s)\n", g_server_port, g_label);
	(void) ready_written;

	t0 = now_ns();
	while (!g_stop) {
		fd_set rf;
		int maxfd = -1;
		struct timeval tv = { 0, 2000 };

		/* Exit if the owning fixture died without stopping us, so a failed
		 * stage cannot leave an orphaned relay holding sockets. */
		if (getppid() == 1)
			break;
		FD_ZERO(&rf);
		for (i = 0; i < g_nclients; i++) {
			if (g_clients[i].sfd >= 0) { FD_SET(g_clients[i].sfd, &rf); if (g_clients[i].sfd > maxfd) maxfd = g_clients[i].sfd; }
			if (g_clients[i].cfd >= 0) { FD_SET(g_clients[i].cfd, &rf); if (g_clients[i].cfd > maxfd) maxfd = g_clients[i].cfd; }
		}
		if (select(maxfd + 1, &rf, NULL, NULL, &tv) > 0) {
			for (i = 0; i < g_nclients; i++) {
				Client *c = &g_clients[i];
				if (c->sfd >= 0 && FD_ISSET(c->sfd, &rf)) {
					unsigned char b[MAXPKT];
					ssize_t r = recv(c->sfd, b, sizeof(b), 0);
					if (r > 0)
						enqueue(b, (int) r, 1, i);
				}
				if (c->cfd >= 0 && FD_ISSET(c->cfd, &rf)) {
					unsigned char b[MAXPKT];
					struct sockaddr_in from;
					socklen_t fl = sizeof(from);
					ssize_t r = recvfrom(c->cfd, b, sizeof(b), 0,
							     (struct sockaddr *) &from, &fl);
					if (r > 0) {
						if (!c->cfd_connected &&
						    from.sin_addr.s_addr == htonl(INADDR_LOOPBACK)) {
							c->addr = from;
							if (connect(c->cfd, (struct sockaddr *) &from, sizeof(from)) == 0)
								c->cfd_connected = 1;
							fprintf(stderr, "relay: client %d = %s:%d\n", i,
								inet_ntoa(from.sin_addr), ntohs(from.sin_port));
						}
						if (c->cfd_connected)
							enqueue(b, (int) r, 0, i);
					}
				}
			}
		}
		pump_queue();
		if (g_duration_ns > 0 && now_ns() - t0 >= g_duration_ns)
			break;
	}
	pump_queue();

	if (g_trace != NULL) fclose(g_trace);
	for (i = 0; i < g_nclients_init; i++)
		if (g_clients[i].cap != NULL) { fclose(g_clients[i].cap); g_clients[i].cap = NULL; }
	if (g_stats_path != NULL)
		write_stats(now_ns() - t0);
	fprintf(stderr, "relay: stopped\n");
	(void) la;
	(void) on;
	return 0;
}
