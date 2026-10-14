/*-
 * Copyright (c) 1988 Terry Donahue
 * All rights reserved.
 *
 * P3.13 timing / bandwidth / latency fixture.
 *
 * Drives the *shipped* coordinator + admission + pinned ENet transport + the
 * frozen codec and two real P3.10 client sessions through a live two-human /
 * two-drone match.  Unlike the P3.12 playable/parity drivers this fixture
 * *measures* the shipped loop:
 *
 *   pace <assets> <maze> <settings> <fpsA> <fpsB> <target_ticks> <out>
 *       Real-time 15 Hz paced run using the shipped absolute-deadline pacer
 *       (server_loop_pace).  The two clients pump/render at fpsA/fpsB while
 *       input refreshes at 15 Hz (P3.11).  Records, per tick: ingress/service/
 *       admission, sim, export and encode wall time (from the bounded read-only
 *       instrumentation in Src/server_loop.c + Src/server_session.c), the total
 *       per-tick work excluding pace sleep, late ticks, per-peer and aggregate
 *       bytes, the actual ENet wire totals (headers+fragments), entity/view
 *       maxima, queue/RSS maxima and same-client input-to-visible latency.
 *
 *   stress <assets> <maze> <settings> <ticks> <out>
 *       Fast (unpaced) maximum-density run: both humans hold fire while driving
 *       and two drones fight, to push the entity/view bounds as far as the
 *       release configuration reaches.  Records the maxima reached separately.
 *
 * The frozen simulation is never modified: the instrumentation only reads
 * clocks and counters, and the input/profile generation here is test-only.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/resource.h>
#include <sys/socket.h>

#include "xtank.h"
#include "bullet.h"
#include "clfkr.h"
#include "object.h"
#include "special.h"
#include "thread.h"
#include "vehicle.h"
#include "server.h"
#include "server_view.h"
#include "server_events.h"
#include "server_session.h"
#include "server_net.h"
#include "intent.h"
#include "proto.h"
#include "gr.h"

#include "transport.h"

#include "session_client.h"

#include "../../Protocol/content-manifest.h"
#include "../../Protocol/limits.h"

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

/* P3.13 bounded read-only instrumentation (server_loop.c / server_session.c). */
extern void server_timing_enable(int on);
extern void server_timing_reset(void);
extern void server_timing_report(const char *path);
extern uint64_t server_session_tx_bytes(const ServerSession *s);
extern uint64_t server_session_tx_messages(const ServerSession *s);

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

/* ---- sandbox sendmsg shim + wire accounting (test-harness only) ------- */
ssize_t __real_sendmsg(int fd, const struct msghdr *msg, int flags);

static unsigned long long g_wire_bytes;
static unsigned long g_wire_sends;

ssize_t
__wrap_sendmsg(int fd, const struct msghdr *msg, int flags)
{
	if (msg != NULL && msg->msg_name != NULL) {
		struct sockaddr_storage local;
		socklen_t ll = sizeof(local);
		struct msghdr m = *msg;
		struct sockaddr unspec;
		ssize_t r;
		int had;

		if (connect(fd, (struct sockaddr *) msg->msg_name,
			    msg->msg_namelen) != 0)
			return __real_sendmsg(fd, msg, flags);
		had = getsockname(fd, (struct sockaddr *) &local, &ll) == 0 &&
		      local.ss_family == AF_INET;
		m.msg_name = NULL;
		m.msg_namelen = 0;
		r = __real_sendmsg(fd, &m, flags);
		if (r > 0) {
			g_wire_bytes += (unsigned long long) r;
			g_wire_sends++;
		}
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
	return __real_sendmsg(fd, msg, flags);
}

/* ---- monotonic clock -------------------------------------------------- */
static uint64_t
mono_ns(void)
{
	struct timespec ts;

	(void) clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t) ts.tv_sec * 1000000000ULL + (uint64_t) ts.tv_nsec;
}

/* ---- bounded histograms (bounded memory, online p50/p95/p99) ---------- */
typedef struct {
	long long bucket_ns;
	long nbuckets;
	unsigned long *count;	/* [nbuckets] */
	long long max_ns;
	long long sum_ns;
	long samples;
} Hist;

static int
hist_init(Hist *h, long long bucket_ns, long nbuckets)
{
	h->bucket_ns = bucket_ns;
	h->nbuckets = nbuckets;
	h->count = (unsigned long *) calloc((size_t) nbuckets, sizeof(unsigned long));
	h->max_ns = h->sum_ns = 0;
	h->samples = 0;
	return h->count != NULL;
}

static void
hist_add(Hist *h, long long ns)
{
	long b;

	if (ns < 0)
		ns = 0;
	h->samples++;
	h->sum_ns += ns;
	if (ns > h->max_ns)
		h->max_ns = ns;
	b = (long) (ns / h->bucket_ns);
	if (b >= h->nbuckets)
		b = h->nbuckets - 1;
	h->count[b]++;
}

static long long
hist_pct(const Hist *h, int pct)
{
	unsigned long target, cum = 0;
	long i;

	if (h->samples == 0)
		return 0;
	target = (unsigned long) ((h->samples * (long) pct + 99) / 100);
	for (i = 0; i < h->nbuckets; i++) {
		cum += h->count[i];
		if (cum >= target)
			return (long long) (i + 1) * h->bucket_ns;
	}
	return h->max_ns;
}

/* ---- same-client input-to-visible latency tracker --------------------- */
/*
 * Measurement method: the fixture stamps CLOCK_MONOTONIC (ns) when it sends an
 * input command, observes the server CMD_ACK for that command (which carries the
 * authoritative assigned tick) and then the first VIEW_SNAPSHOT whose
 * server_tick >= assigned_tick.  Latency = snapshot-arrival time - send time on
 * the *same process monotonic clock* (the clients and server share it).  This
 * covers input -> server admission -> tick -> export -> client receipt; it is a
 * lower bound on a healthy LAN (loopback, no propagation delay) and is reported
 * as such.
 */
#define LAT_SLOTS 256
typedef struct {
	int active;
	int acked;
	uint32_t command_id;
	uint32_t assigned_tick;
	uint64_t send_ns;
} LatPing;

typedef struct {
	XtkSessionClient *sc;
	LatPing ping[LAT_SLOTS];
	Hist lat;
	int tracked;
} LatTracker;

static int
lat_callback(void *user, const XtkMessage *m)
{
	LatTracker *lt = (LatTracker *) user;
	uint64_t now = mono_ns();
	int i;

	if (m == (const XtkMessage *) NULL)
		return 0;
	if (m->env.message_type == XTK_MSG_CMD_ACK) {
		for (i = 0; i < (int) m->body.cmd_ack.count; i++) {
			uint32_t id = m->body.cmd_ack.entry[i].command_id;
			LatPing *p = &lt->ping[id & (LAT_SLOTS - 1)];

			if (p->active && p->command_id == id) {
				p->acked = 1;
				p->assigned_tick =
					m->body.cmd_ack.entry[i].assigned_tick;
			}
		}
	} else if (m->env.message_type == XTK_MSG_VIEW_SNAPSHOT) {
		uint32_t t = m->body.view_snapshot.server_tick;

		for (i = 0; i < LAT_SLOTS; i++) {
			LatPing *p = &lt->ping[i];

			if (p->active && p->acked && p->assigned_tick <= t) {
				uint64_t d = now - p->send_ns;

				if (lt->lat.count != NULL)
					hist_add(&lt->lat, (long long) d);
				p->active = 0;
			}
		}
	}
	return 0;
}

static void
lat_track(LatTracker *lt, uint32_t id)
{
	LatPing *p = &lt->ping[id & (LAT_SLOTS - 1)];

	memset(p, 0, sizeof(*p));
	p->active = 1;
	p->command_id = id;
	p->send_ns = mono_ns();
}

/* ---- bring-up --------------------------------------------------------- */
static const char *g_settings_path;
static int g_maze_id = XTK_CONTENT_ID_ARENA;

static int
server_bringup(const char *assets, const char *maze_name)
{
	ServerConfig *cfg;
	int idx = -1;

	bset = &actual_bset;
	eset = &actual_eset;
	InitConfigStruct(&command_options);
	command_options.NoDelay = TRUE;
	command_options.AutoExit = TRUE;
	command_options.PrintScores = FALSE;
	strncpy(username, "p3.13", MAX_STRING - 1);
	username[MAX_STRING - 1] = '\0';

	if (content_set_root(assets) != 0) {
		fprintf(stderr, "latency_fixture: %s\n", content_last_error());
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
	if (make_mdesc((char *) maze_name, &idx) != DESC_LOADED) {
		fprintf(stderr, "latency_fixture: cannot load %s\n", maze_name);
		return -1;
	}
	if (g_settings_path != NULL &&
	    content_load_settings(g_settings_path) != 0) {
		fprintf(stderr, "latency_fixture: %s\n", content_last_error());
		return -1;
	}
	init_threader();
	init_msg_sys();
	init_box_names();
	geometry_order_objects();
	geometry_rotate_objects();
	return 0;
}

/* ---- live two-client match ------------------------------------------- */
typedef struct {
	XtkSessionClient *sc;
	uint32_t pkt_seq;
	int fps;
	uint64_t next_render_ns;
	LatTracker lat;
} NetPeer;

static void
net_render(NetPeer *p, uint64_t now_ns)
{
	int k;

	if (p->fps <= 0)
		p->fps = 15;
	if (p->next_render_ns == 0)
		p->next_render_ns = now_ns;
	for (k = 0; k < 1000 && now_ns >= p->next_render_ns; k++) {
		(void) session_client_pump(p->sc, now_ns, lat_callback, &p->lat);
		p->next_render_ns += 1000000000ULL / (uint64_t) p->fps;
	}
	/* Avoid unbounded catch-up if the host stalls. */
	if (now_ns > p->next_render_ns + 1000000000ULL)
		p->next_render_ns = now_ns + 1000000000ULL / (uint64_t) p->fps;
}

static uint32_t
send_tick_input(NetPeer *p, uint32_t tick, int player, int fire)
{
	XtkMessage batch;
	XtkCommand c;
	uint32_t id = tick * 64u + (uint32_t) player * 16u + 1u;

	memset(&batch, 0, sizeof(batch));
	batch.env.message_type = XTK_MSG_INPUT_BATCH;
	batch.env.epoch = session_client_epoch(p->sc);
	batch.body.input_batch.packet_sequence = ++p->pkt_seq;
	batch.body.input_batch.last_observed_server_tick = (uint32_t) frame;

	memset(&c, 0, sizeof(c));
	c.op = XTK_OP_DRIVE_REL;
	c.flags = XTK_FLAG_HELD;
	c.a = (player == 0) ? 9.0 : -9.0;
	c.command_id = id;
	c.tick_hint = tick;
	batch.body.input_batch.command[0] = c;

	if (fire) {
		XtkCommand f;

		memset(&f, 0, sizeof(f));
		f.op = XTK_OP_FIRE;
		f.flags = XTK_FLAG_EDGE;
		f.command_id = id + 1u;
		f.tick_hint = tick;
		batch.body.input_batch.command[1] = f;
		batch.body.input_batch.command_count = 2;
	} else {
		batch.body.input_batch.command_count = 1;
	}
	(void) session_client_send_input(p->sc, &batch);
	lat_track(&p->lat, id);
	return id;
}

/* Connect both humans, configure maze/Vanguard/2 drones and start one epoch. */
static int
match_start(Transport *host, NetPeer *A, NetPeer *B, int drones)
{
	uint64_t ns = 1000000000ULL;
	int i;

	for (i = 0; i < 3000 && session_client_state(A->sc) != XTK_SC_LOBBY; i++) {
		(void) server_session_service(ns / 1000000ULL);
		(void) session_client_pump(A->sc, ns, lat_callback, &A->lat);
		ns += 66666667ULL;
	}
	(void) session_client_connect(B->sc, "127.0.0.1", transport_port(host),
				      "playerB");
	for (i = 0; i < 3000 &&
	     (session_client_state(A->sc) != XTK_SC_LOBBY ||
	      session_client_state(B->sc) != XTK_SC_LOBBY); i++) {
		(void) server_session_service(ns / 1000000ULL);
		(void) session_client_pump(A->sc, ns, lat_callback, &A->lat);
		(void) session_client_pump(B->sc, ns, lat_callback, &B->lat);
		ns += 66666667ULL;
	}
	if (session_client_state(A->sc) != XTK_SC_LOBBY ||
	    session_client_state(B->sc) != XTK_SC_LOBBY)
		return -1;

	(void) session_client_send_config(A->sc, (uint16_t) g_maze_id,
					  XTK_CONTENT_ID_VANGUARD,
					  (uint8_t) drones);
	for (i = 0; i < 2000 && session_client_config_revision(A->sc) == 0; i++) {
		(void) server_session_service(ns / 1000000ULL);
		(void) session_client_pump(A->sc, ns, lat_callback, &A->lat);
		(void) session_client_pump(B->sc, ns, lat_callback, &B->lat);
		ns += 66666667ULL;
	}
	(void) session_client_send_ready(A->sc, 1);
	(void) session_client_send_ready(B->sc, 1);
	server_seed_random(114336);
	(void) session_client_send_start(A->sc);
	for (i = 0; i < 3000; i++) {
		ServerSession *s;

		(void) server_session_service(ns / 1000000ULL);
		(void) session_client_pump(A->sc, ns, lat_callback, &A->lat);
		(void) session_client_pump(B->sc, ns, lat_callback, &B->lat);
		ns += 66666667ULL;
		s = server_session_current();
		if (s != NULL &&
		    server_session_state(s) == SERVER_SESSION_RUNNING)
			return 0;
	}
	return -1;
}

/* ---- summary accumulation -------------------------------------------- */
typedef struct {
	Hist total;
	Hist service;
	Hist step;
	uint64_t pace_t0;
	long late_ticks;
	unsigned long max_reliable;
	unsigned long max_outgoing;
	int max_intent_queue;
	int max_vehicles;
	int max_bullets;
	int max_explosions;
	int max_cells;
	int max_landmarks;
	int max_radar;
	unsigned long max_view_bytes;
} RunStats;

static void
sample_entities(ServerSession *sa, ServerSession *sb, RunStats *rs)
{
	int slot;
	static XtkViewSnapshot snap;

	for (slot = 0; slot < 2; slot++) {
		ServerSession *p = (slot == 0) ? sa : sb;
		uint8_t *buf;
		size_t len;
		XtkMessage m;
		int bs = server_session_bound_slot(p);

		if (bs < 0)
			continue;
		if (server_view_export(bs, 0, 0, 0, &snap) != SERVER_VIEW_OK)
			continue;
		if ((int) snap.vehicle_count > rs->max_vehicles)
			rs->max_vehicles = snap.vehicle_count;
		if ((int) snap.bullet_count > rs->max_bullets)
			rs->max_bullets = snap.bullet_count;
		if ((int) snap.explosion_count > rs->max_explosions)
			rs->max_explosions = snap.explosion_count;
		if ((int) snap.map_cell_count > rs->max_cells)
			rs->max_cells = snap.map_cell_count;
		if ((int) snap.landmark_count > rs->max_landmarks)
			rs->max_landmarks = snap.landmark_count;
		if ((int) snap.radar_blip_count > rs->max_radar)
			rs->max_radar = snap.radar_blip_count;
		buf = (uint8_t *) malloc(XTK_MAX_MESSAGE_BYTES);
		if (buf != NULL) {
			memset(&m, 0, sizeof(m));
			m.env.message_type = XTK_MSG_VIEW_SNAPSHOT;
			m.body.view_snapshot = snap;
			if (xtk_encode(buf, XTK_MAX_MESSAGE_BYTES, &m, &len) ==
			    XTK_ERR_NONE && len > rs->max_view_bytes)
				rs->max_view_bytes = (unsigned long) len;
			free(buf);
		}
	}
}

static unsigned long
rss_hwm_kb(void)
{
	FILE *fp = fopen("/proc/self/status", "r");
	char line[256];
	unsigned long kb = 0;

	if (fp == NULL)
		return 0;
	while (fgets(line, sizeof(line), fp) != NULL) {
		if (strncmp(line, "VmHWM:", 6) == 0) {
			(void) sscanf(line + 6, "%lu", &kb);
			break;
		}
	}
	fclose(fp);
	return kb;
}

/* Runs the paced two-human / two-drone match.  Returns 0 on success. */
static int
run_pace(const char *assets, const char *maze, NetPeer *A, NetPeer *B,
	 int drones, int target_ticks, const char *out)
{
	Transport *host;
	ServerSession *s = NULL, *sa = NULL, *sb = NULL;
	RunStats rs;
	TransportStats tstats;
	uint64_t dur_ns, elapsed;
	long ticks = 0;
	long long last_tx[2] = { 0, 0 };
	int i, status = GAME_RUNNING;
	FILE *sum;
	char path[1200];

	memset(&rs, 0, sizeof(rs));
	if (!hist_init(&rs.total, 1000, 100001))	/* 1 us .. 100 ms */
		return -1;
	if (!hist_init(&rs.service, 1000, 100001) ||
	    !hist_init(&rs.step, 1000, 100001) ||
	    !hist_init(&A->lat.lat, 100000, 50001) ||	/* 100 us .. 5 s */
	    !hist_init(&B->lat.lat, 100000, 50001)) {
		fprintf(stderr, "latency_fixture: histogram alloc failed\n");
		return -1;
	}

	(void) setenv("XTANK_TIMING", "1", 1);
	server_timing_enable(1);

	server_session_init();
	server_session_set_capacity(XTK_MAX_HUMANS);
	host = transport_create(0, SERVER_SESSION_MAX_PEERS);
	if (host == NULL) {
		fprintf(stderr, "latency_fixture: transport_create failed\n");
		return -1;
	}
	server_session_set_transport(host);

	A->sc = session_client_create();
	B->sc = session_client_create();
	if (A->sc == NULL || B->sc == NULL)
		return -1;
	if (session_client_connect(A->sc, "127.0.0.1", transport_port(host),
				   "playerA") != 0)
		return -1;
	if (match_start(host, A, B, drones) != 0) {
		fprintf(stderr, "latency_fixture: handshake/start failed\n");
		return -1;
	}
	s = server_session_current();
	if (s == NULL || server_session_state(s) != SERVER_SESSION_RUNNING) {
		fprintf(stderr, "latency_fixture: match did not start\n");
		return -1;
	}
	sa = server_session_peer_at(0);
	sb = server_session_peer_at(1);

	/* Measure the shipped pacer: NoDelay off so server_loop_pace sleeps. */
	command_options.NoDelay = FALSE;
	server_loop_configure(15);
	server_loop_epoch_begin();
	rs.pace_t0 = mono_ns();
	server_timing_reset();

	dur_ns = (uint64_t) target_ticks * 1000000000ULL / 15ULL;

	while (ticks < target_ticks) {
		uint64_t now = mono_ns();
		long long a, b, svc, stp;

		net_render(A, now);
		net_render(B, now);

		/* Input refresh at 15 Hz, independent of render FPS (P3.11). */
		{
			int fire = ((ticks % 30) == 0);

			(void) send_tick_input(A, (uint32_t) (frame + 1), 0, fire);
			(void) send_tick_input(B, (uint32_t) (frame + 1), 1, fire);
		}

		a = (long long) mono_ns();
		(void) server_session_service(mono_ns() / 1000000ULL);
		b = (long long) mono_ns();
		svc = b - a;

		/* Late-deadline check before waiting (mirrors the shipped loop). */
		{
			long long deadline = (long long) rs.pace_t0 +
				(long long) (ticks + 1) * 1000000000LL / 15LL;

			if (b > deadline)
				rs.late_ticks++;
		}
		server_loop_pace(frame + 1);

		a = (long long) mono_ns();
		status = server_session_step(s, mono_ns() / 1000000ULL);
		b = (long long) mono_ns();
		stp = b - a;

		hist_add(&rs.service, svc);
		hist_add(&rs.step, stp);
		hist_add(&rs.total, svc + stp);
		ticks++;

		if ((ticks % 15) == 0) {
			(void) sample_entities(sa, sb, &rs);
			{
				int q = intent_queued();

				if (q > rs.max_intent_queue)
					rs.max_intent_queue = q;
			}
			for (i = 0; i < 2; i++) {
				ServerSession *p = (i == 0) ? sa : sb;
				TransportPeer *tp;

				if (p == NULL)
					continue;
				tp = server_session_peer(p);
				if (tp == NULL)
					continue;
				{
					unsigned long rb =
						(unsigned long)
						 transport_peer_reliable_bytes(host, tp);
					unsigned long ob =
						(unsigned long)
						 transport_peer_outgoing_bytes(host, tp);

					if (rb > rs.max_reliable)
						rs.max_reliable = rb;
					if (ob > rs.max_outgoing)
						rs.max_outgoing = ob;
				}
			}
		}
		if (status != GAME_RUNNING) {
			fprintf(stderr,
				"latency_fixture: match ended early at tick %ld status %d\n",
				ticks, status);
			break;
		}
	}

	elapsed = mono_ns() - rs.pace_t0;
	server_timing_report((snprintf(path, sizeof(path), "%s.timing", out),
			      path));
	transport_stats(host, &tstats);

	last_tx[0] = (long long) server_session_tx_bytes(sa);
	last_tx[1] = (long long) server_session_tx_bytes(sb);

	sum = fopen((snprintf(path, sizeof(path), "%s.summary", out), path), "w");
	if (sum == NULL)
		return -1;
	fprintf(sum, "mode=pace\n");
	fprintf(sum, "maze=%s\n", maze);
	fprintf(sum, "drones=%d\n", drones);
	fprintf(sum, "fpsA=%d\n", A->fps);
	fprintf(sum, "fpsB=%d\n", B->fps);
	fprintf(sum, "target_ticks=%d\n", target_ticks);
	fprintf(sum, "ticks=%ld\n", ticks);
	fprintf(sum, "elapsed_ms=%.1f\n", (double) elapsed / 1e6);
	fprintf(sum, "late_ticks=%ld\n", rs.late_ticks);
	fprintf(sum, "status=%d\n", status);
	fprintf(sum, "service_samples=%ld service_p50_us=%.1f service_p95_us=%.1f "
		"service_p99_us=%.1f service_max_us=%.1f\n", rs.service.samples,
		(double) hist_pct(&rs.service, 50) / 1000.0,
		(double) hist_pct(&rs.service, 95) / 1000.0,
		(double) hist_pct(&rs.service, 99) / 1000.0,
		(double) rs.service.max_ns / 1000.0);
	fprintf(sum, "total_samples=%ld total_p50_us=%.1f total_p95_us=%.1f "
		"total_p99_us=%.1f total_max_us=%.1f\n", rs.total.samples,
		(double) hist_pct(&rs.total, 50) / 1000.0,
		(double) hist_pct(&rs.total, 95) / 1000.0,
		(double) hist_pct(&rs.total, 99) / 1000.0,
		(double) rs.total.max_ns / 1000.0);
	fprintf(sum, "tx_bytes_peer0=%lld\n", last_tx[0]);
	fprintf(sum, "tx_bytes_peer1=%lld\n", last_tx[1]);
	fprintf(sum, "tx_bytes_agg=%llu\n", tstats.bytes_tx);
	fprintf(sum, "tx_packets_agg=%lu\n", tstats.packets_tx);
	fprintf(sum, "wire_bytes=%llu\n", tstats.tx_wire_bytes);
	fprintf(sum, "wire_packets=%llu\n", tstats.tx_wire_packets);
	fprintf(sum, "sendmsg_bytes=%llu\n", g_wire_bytes);
	fprintf(sum, "sendmsg_calls=%lu\n", g_wire_sends);
	fprintf(sum, "unreliable_fragments=%lu\n", tstats.unreliable_fragments);
	fprintf(sum, "rx_queue_max=%lu\n", tstats.rx_queue_max);
	fprintf(sum, "max_reliable_bytes=%lu\n", rs.max_reliable);
	fprintf(sum, "max_outgoing_bytes=%lu\n", rs.max_outgoing);
	fprintf(sum, "max_intent_queue=%d\n", rs.max_intent_queue);
	fprintf(sum, "max_vehicles=%d\n", rs.max_vehicles);
	fprintf(sum, "max_bullets=%d\n", rs.max_bullets);
	fprintf(sum, "max_explosions=%d\n", rs.max_explosions);
	fprintf(sum, "max_map_cells=%d\n", rs.max_cells);
	fprintf(sum, "max_landmarks=%d\n", rs.max_landmarks);
	fprintf(sum, "max_radar=%d\n", rs.max_radar);
	fprintf(sum, "max_view_bytes=%lu\n", rs.max_view_bytes);
	fprintf(sum, "lat_samples_A=%ld lat_p50_ms_A=%.2f lat_p95_ms_A=%.2f "
		"lat_p99_ms_A=%.2f lat_max_ms_A=%.2f\n", A->lat.lat.samples,
		(double) hist_pct(&A->lat.lat, 50) / 1e6,
		(double) hist_pct(&A->lat.lat, 95) / 1e6,
		(double) hist_pct(&A->lat.lat, 99) / 1e6,
		(double) A->lat.lat.max_ns / 1e6);
	fprintf(sum, "lat_samples_B=%ld lat_p50_ms_B=%.2f lat_p95_ms_B=%.2f "
		"lat_p99_ms_B=%.2f lat_max_ms_B=%.2f\n", B->lat.lat.samples,
		(double) hist_pct(&B->lat.lat, 50) / 1e6,
		(double) hist_pct(&B->lat.lat, 95) / 1e6,
		(double) hist_pct(&B->lat.lat, 99) / 1e6,
		(double) B->lat.lat.max_ns / 1e6);
	fprintf(sum, "rss_hwm_kb=%lu\n", rss_hwm_kb());
	fprintf(sum, "wire_frame=loopback-monotonic-same-process\n");
	fclose(sum);

	(void) i;
	session_client_destroy(A->sc);
	session_client_destroy(B->sc);
	transport_destroy(host);
	printf("LATENCY FIXTURE pace ticks=%ld late=%ld total_p99_us=%.1f "
	       "lat_p95_ms_A=%.2f lat_p95_ms_B=%.2f\n", ticks, rs.late_ticks,
	       (double) hist_pct(&rs.total, 99) / 1000.0,
	       (double) hist_pct(&A->lat.lat, 95) / 1e6,
	       (double) hist_pct(&B->lat.lat, 95) / 1e6);
	return 0;
}

/* Fast maximum-density stress run: push the entity/view bounds. */
static int
run_stress(const char *assets, const char *maze, NetPeer *A, NetPeer *B,
	   int ticks_target, const char *out)
{
	Transport *host;
	ServerSession *s = NULL, *sa = NULL, *sb = NULL;
	RunStats rs;
	TransportStats tstats;
	uint64_t ns = 1000000000ULL;
	long ticks = 0;
	int i, status = GAME_RUNNING;
	FILE *sum;
	char path[1200];

	memset(&rs, 0, sizeof(rs));
	(void) setenv("XTANK_TIMING", "1", 1);
	server_timing_enable(1);

	server_session_init();
	server_session_set_capacity(XTK_MAX_HUMANS);
	host = transport_create(0, SERVER_SESSION_MAX_PEERS);
	if (host == NULL)
		return -1;
	server_session_set_transport(host);
	A->sc = session_client_create();
	B->sc = session_client_create();
	if (A->sc == NULL || B->sc == NULL)
		return -1;
	if (session_client_connect(A->sc, "127.0.0.1", transport_port(host),
				   "playerA") != 0)
		return -1;
	command_options.NoDelay = TRUE;
	if (match_start(host, A, B, 2) != 0)
		return -1;
	s = server_session_current();
	sa = server_session_peer_at(0);
	sb = server_session_peer_at(1);

	while (ticks < ticks_target) {
		XtkMessage batch;
		XtkCommand c[2];
		uint32_t id = (uint32_t) (frame + 1) * 64u + 1u;

		net_render(A, ns);
		net_render(B, ns);

		/* Hold fire continuously to maximise live entities. */
		memset(&batch, 0, sizeof(batch));
		batch.env.message_type = XTK_MSG_INPUT_BATCH;
		batch.env.epoch = session_client_epoch(A->sc);
		batch.body.input_batch.packet_sequence = ++A->pkt_seq;
		batch.body.input_batch.last_observed_server_tick = (uint32_t) frame;
		memset(&c[0], 0, sizeof(c[0]));
		c[0].op = XTK_OP_FIRE_HOLD;
		c[0].flags = XTK_FLAG_HELD;
		c[0].a = 1.0;
		c[0].command_id = id;
		c[0].tick_hint = (uint32_t) (frame + 1);
		c[1] = c[0];
		c[1].op = XTK_OP_DRIVE_REL;
		c[1].a = 9.0;
		c[1].command_id = id + 1u;
		batch.body.input_batch.command[0] = c[0];
		batch.body.input_batch.command[1] = c[1];
		batch.body.input_batch.command_count = 2;
		(void) session_client_send_input(A->sc, &batch);

		id = (uint32_t) (frame + 1) * 64u + 17u;
		batch.env.epoch = session_client_epoch(B->sc);
		batch.body.input_batch.packet_sequence = ++B->pkt_seq;
		memset(&c[0], 0, sizeof(c[0]));
		c[0].op = XTK_OP_FIRE_HOLD;
		c[0].flags = XTK_FLAG_HELD;
		c[0].a = 1.0;
		c[0].command_id = id;
		c[0].tick_hint = (uint32_t) (frame + 1);
		c[1] = c[0];
		c[1].op = XTK_OP_DRIVE_REL;
		c[1].a = -9.0;
		c[1].command_id = id + 1u;
		batch.body.input_batch.command[0] = c[0];
		batch.body.input_batch.command[1] = c[1];
		batch.body.input_batch.command_count = 2;
		(void) session_client_send_input(B->sc, &batch);

		(void) server_session_service(ns / 1000000ULL);
		(void) session_client_pump(A->sc, ns, lat_callback, &A->lat);
		(void) session_client_pump(B->sc, ns, lat_callback, &B->lat);
		ns += 66666667ULL;

		status = server_session_step(s, ns / 1000000ULL);
		ticks++;
		sample_entities(sa, sb, &rs);
		{
			int q = intent_queued();

			if (q > rs.max_intent_queue)
				rs.max_intent_queue = q;
		}
		for (i = 0; i < 2; i++) {
			ServerSession *p = (i == 0) ? sa : sb;
			TransportPeer *tp = (p != NULL) ? server_session_peer(p) : NULL;
			unsigned long rb, ob;

			if (tp == NULL)
				continue;
			rb = (unsigned long) transport_peer_reliable_bytes(host, tp);
			ob = (unsigned long) transport_peer_outgoing_bytes(host, tp);
			if (rb > rs.max_reliable)
				rs.max_reliable = rb;
			if (ob > rs.max_outgoing)
				rs.max_outgoing = ob;
		}
		if (status != GAME_RUNNING)
			break;
	}

	transport_stats(host, &tstats);

	/*
	 * Independent maximum authorized-message bound check: encode the largest
	 * possible VIEW_SNAPSHOT the frozen schema allows (every record array at
	 * its engine maximum) and record its size and ENet fragment estimate.
	 * This is a codec/transport bound proof, separate from the live stress
	 * maxima above; it never touches the simulation.
	 */
	{
		static XtkViewSnapshot big;
		uint8_t *buf = (uint8_t *) malloc(XTK_MAX_MESSAGE_BYTES);
		size_t len = 0;
		int enc_ok = 0;
		int frag = 0;

		if (buf != NULL) {
			XtkMessage m;

			memset(&big, 0, sizeof(big));
			big.vehicle_count = XTK_MAX_VEHICLES;
			big.bullet_count = XTK_MAX_BULLETS;
			big.explosion_count = XTK_MAX_EXPLOSIONS;
			big.map_cell_count = XTK_MAX_MAP_CELLS;
			big.landmark_count = XTK_MAX_LANDMARKS;
			big.radar_blip_count = XTK_MAX_RADAR_BLIPS;
			memset(&m, 0, sizeof(m));
			m.env.message_type = XTK_MSG_VIEW_SNAPSHOT;
			m.body.view_snapshot = big;
			if (xtk_encode(buf, XTK_MAX_MESSAGE_BYTES, &m, &len) ==
			    XTK_ERR_NONE) {
				XtkMessage back;

				enc_ok = 1;
				frag = (int) ((len + 1399u) / 1400u);
				if (xtk_decode(buf, len, &back) == XTK_ERR_NONE &&
				    back.body.view_snapshot.bullet_count ==
					XTK_MAX_BULLETS)
					enc_ok = 2;
			}
			free(buf);
		}
		sum = fopen((snprintf(path, sizeof(path), "%s.summary", out), path),
			    "w");
		if (sum == NULL)
			return -1;
		fprintf(sum, "bound_view_bytes=%zu\n", len);
		fprintf(sum, "bound_encode_ok=%d\n", enc_ok);
		fprintf(sum, "bound_fragment_est=%d\n", frag);
		fprintf(sum, "bound_vehicle_max=%u\n", (unsigned) XTK_MAX_VEHICLES);
		fprintf(sum, "bound_bullet_max=%u\n", (unsigned) XTK_MAX_BULLETS);
		fprintf(sum, "bound_explosion_max=%u\n",
			(unsigned) XTK_MAX_EXPLOSIONS);
		fprintf(sum, "bound_cell_max=%u\n", (unsigned) XTK_MAX_MAP_CELLS);
		fprintf(sum, "mode=stress\n");
	}
	if (sum == NULL)
		return -1;
	fprintf(sum, "maze=%s\n", maze);
	fprintf(sum, "ticks=%ld\n", ticks);
	fprintf(sum, "max_vehicles=%d\n", rs.max_vehicles);
	fprintf(sum, "max_bullets=%d\n", rs.max_bullets);
	fprintf(sum, "max_explosions=%d\n", rs.max_explosions);
	fprintf(sum, "max_map_cells=%d\n", rs.max_cells);
	fprintf(sum, "max_landmarks=%d\n", rs.max_landmarks);
	fprintf(sum, "max_radar=%d\n", rs.max_radar);
	fprintf(sum, "max_view_bytes=%lu\n", rs.max_view_bytes);
	fprintf(sum, "max_reliable_bytes=%lu\n", rs.max_reliable);
	fprintf(sum, "max_outgoing_bytes=%lu\n", rs.max_outgoing);
	fprintf(sum, "max_intent_queue=%d\n", rs.max_intent_queue);
	fprintf(sum, "unreliable_fragments=%lu\n", tstats.unreliable_fragments);
	fprintf(sum, "wire_bytes=%llu\n", tstats.tx_wire_bytes);
	fprintf(sum, "tx_bytes_agg=%llu\n", tstats.bytes_tx);
	fprintf(sum, "engine_limit_vehicles=%u\n", (unsigned) XTK_MAX_VEHICLES);
	fprintf(sum, "engine_limit_bullets=%u\n", (unsigned) XTK_MAX_BULLETS);
	fprintf(sum, "engine_limit_explosions=%u\n",
		(unsigned) XTK_MAX_EXPLOSIONS);
	fprintf(sum, "rss_hwm_kb=%lu\n", rss_hwm_kb());
	fclose(sum);

	printf("LATENCY FIXTURE stress ticks=%ld max_veh=%d max_bul=%d "
	       "max_exp=%d max_view=%lu\n", ticks, rs.max_vehicles,
	       rs.max_bullets, rs.max_explosions, rs.max_view_bytes);
	return 0;
}

int
main(int argc, char *argv[])
{
	const char *mode, *assets, *maze, *out;
	NetPeer A, B;

	if (argc < 2) {
		fprintf(stderr, "usage:\n");
		fprintf(stderr, "  %s pace <assets> <maze> <settings> <fpsA> "
			"<fpsB> <ticks> <out>\n", argv[0]);
		fprintf(stderr, "  %s stress <assets> <maze> <settings> <ticks> "
			"<out>\n", argv[0]);
		return 2;
	}
	mode = argv[1];
	memset(&A, 0, sizeof(A));
	memset(&B, 0, sizeof(B));

	if (strcmp(mode, "pace") == 0) {
		if (argc < 9) {
			fprintf(stderr, "usage: %s pace <assets> <maze> "
				"<settings> <fpsA> <fpsB> <ticks> <out>\n",
				argv[0]);
			return 2;
		}
		assets = argv[2];
		maze = argv[3];
		g_settings_path = argv[4];
		A.fps = atoi(argv[5]);
		B.fps = atoi(argv[6]);
		out = argv[8];
		if (strcmp(maze, "combat1") == 0)
			g_maze_id = XTK_CONTENT_ID_COMBAT1;
		else
			g_maze_id = XTK_CONTENT_ID_ARENA;
		if (server_bringup(assets, maze) != 0)
			return 2;
		if (run_pace(assets, maze, &A, &B, 2, atoi(argv[7]), out) != 0)
			return 1;
		printf("LATENCY FIXTURE PASS (mode=pace)\n");
		return 0;
	}
	if (strcmp(mode, "stress") == 0) {
		if (argc < 7) {
			fprintf(stderr, "usage: %s stress <assets> <maze> "
				"<settings> <ticks> <out>\n", argv[0]);
			return 2;
		}
		assets = argv[2];
		maze = argv[3];
		g_settings_path = argv[4];
		out = argv[6];
		if (strcmp(maze, "combat1") == 0)
			g_maze_id = XTK_CONTENT_ID_COMBAT1;
		else
			g_maze_id = XTK_CONTENT_ID_ARENA;
		if (server_bringup(assets, maze) != 0)
			return 2;
		if (run_stress(assets, maze, &A, &B, atoi(argv[5]), out) != 0)
			return 1;
		printf("LATENCY FIXTURE PASS (mode=stress)\n");
		return 0;
	}
	fprintf(stderr, "latency_fixture: unknown mode %s\n", mode);
	return 2;
}
