/*-
 * Copyright (c) 1988 Terry Donahue
 * All rights reserved.
 *
 * P3.14 approved impairment / slow-client fixture (adapted from the P3.13
 * latency fixture).  See impairment.sh and P3.14-REPORT.md.
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
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>

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

/* ---- P3.14 impairment-relay plumbing (test harness only) ------------- */
#define IMP_MAX_CLIENTS 4
static uint16_t g_client_ports[IMP_MAX_CLIENTS]; /* per-client relay ports */
static int g_client_port_count;
static pid_t g_relay_pid = -1;
static char g_workdir[1024];
static const char *g_relay_bin = "impairment_relay";

/* P3.13 bounded read-only instrumentation (server_loop.c / server_session.c). */
extern void server_timing_enable(int on);
extern void server_timing_reset(void);
extern void server_timing_report(const char *path);
extern uint64_t server_session_tx_bytes(const ServerSession *s);
extern uint64_t server_session_tx_messages(const ServerSession *s);

/* ---- impairment configuration + real-UDP relay child ----------------- */
typedef struct {
	double delay_ms, loss, reorder, dup, extra_ms, extra_prob;
	unsigned long long seed;
	int freeze;              /* 1 = freeze a client for freeze_dur_ticks */
	int freeze_client;       /* 0 = A, 1 = B */
	long freeze_at;          /* tick to start the freeze */
	long freeze_dur_ticks;   /* freeze length in ticks (10 s @15 Hz = 150) */
	long reset_at;           /* 0 = no mid-run reset (backpressure stage) */
} ImpairCfg;

static ImpairCfg g_imp;

/* P3.14 freeze diagnostics (recorded at run end). */
static long g_freeze_removed_tick = -1;
static int g_freeze_announced = 0;
static long g_reset_tick_done = -1;
static unsigned long g_reset_epoch_before = 0, g_reset_epoch_after = 0;
/*
 * The last reliable LOBBY_STATE's readiness, observed from either client's
 * callback.  Bring-up waits for all peers ready before sending START, so it
 * never sends START before READY(B) is processed (the two travel on
 * independent ordered streams) and never floods the per-tick admission quota.
 */
static volatile int g_lobby_all_ready;
static volatile int g_lobby_human_count;

/* P3.14 applied-command capture (for the exact-replay check). */
static const char *g_applied_path = NULL;
static FILE *g_applied = NULL;
static int g_healthy_admin_index = -1;
static int g_healthy_self_index = -1;
static int g_frozen_state_at_end = -1;

/*
 * Launch the real-UDP impairment relay as a child process and wait for it to
 * report its chosen listen port.  Returns the port, or 0 on failure.  The relay
 * is a separate process reached only over real loopback UDP; the child stderr
 * is inherited so relay diagnostics land in the fixture log.
 */
static int
relay_launch(const char *label, uint16_t server_port, int nclients)
{
	char ready[1200];
	char trace[1200];
	char capdir[1200];
	char stats[1200];
	char sp[32], dp[32], el[32], lp[32], ep[32], rp[32], up[32], sd[32];
	char nc[32];
	pid_t pid;
	int i;
	FILE *rf;

	snprintf(ready, sizeof(ready), "%s/relay-%s.ready", g_workdir, label);
	snprintf(trace, sizeof(trace), "%s/relay-%s.trace", g_workdir, label);
	snprintf(capdir, sizeof(capdir), "%s", g_workdir);
	snprintf(stats, sizeof(stats), "%s/relay-%s.stats", g_workdir, label);
	(void) unlink(ready);

	snprintf(sp, sizeof(sp), "%u", (unsigned) server_port);
	snprintf(nc, sizeof(nc), "%d", nclients);
	snprintf(dp, sizeof(dp), "%.3f", g_imp.delay_ms);
	snprintf(el, sizeof(el), "%.3f", g_imp.loss);
	snprintf(lp, sizeof(lp), "%.3f", g_imp.reorder);
	snprintf(up, sizeof(up), "%.3f", g_imp.dup);
	snprintf(ep, sizeof(ep), "%.3f", g_imp.extra_ms);
	snprintf(rp, sizeof(rp), "%.4f", g_imp.extra_prob);
	snprintf(sd, sizeof(sd), "%llu", g_imp.seed);

	pid = fork();
	if (pid < 0) {
		perror("relay fork");
		return 0;
	}
	if (pid == 0) {
		char *av[64];
		int n = 0;
		av[n++] = (char *) g_relay_bin;
		av[n++] = "--server"; av[n++] = sp;
		av[n++] = "--clients"; av[n++] = nc;
		av[n++] = "--listen"; av[n++] = (char *) "0";
		av[n++] = "--delay-ms"; av[n++] = dp;
		av[n++] = "--loss"; av[n++] = el;
		av[n++] = "--reorder"; av[n++] = lp;
		av[n++] = "--dup"; av[n++] = up;
		av[n++] = "--extra-delay-ms"; av[n++] = ep;
		av[n++] = "--extra-prob"; av[n++] = rp;
		av[n++] = "--seed"; av[n++] = sd;
		av[n++] = "--duration-ms"; av[n++] = (char *) "0";
		av[n++] = "--label"; av[n++] = (char *) label;
		av[n++] = "--trace"; av[n++] = trace;
		av[n++] = "--capture-dir"; av[n++] = capdir;
		av[n++] = "--stats"; av[n++] = stats;
		av[n++] = "--ready-file"; av[n++] = ready;
		av[n] = NULL;
		execvp(g_relay_bin, av);
		perror("relay execvp");
		_exit(127);
	}
	g_relay_pid = pid;
	g_client_port_count = 0;
	for (i = 0; i < 500; i++) {
		rf = fopen(ready, "r");
		if (rf != NULL) {
			int p;
			g_client_port_count = 0;
			while (g_client_port_count < nclients &&
			       g_client_port_count < IMP_MAX_CLIENTS &&
			       fscanf(rf, "%d", &p) == 1)
				g_client_ports[g_client_port_count++] = (uint16_t) p;
			fclose(rf);
			break;
		}
		usleep(10000);
	}
	if (g_client_port_count != nclients) {
		fprintf(stderr, "impairment_fixture: relay did not become ready (%d/%d)\n",
			g_client_port_count, nclients);
		return 0;
	}
	return g_client_port_count;
}

static void
relay_stop(void)
{
	if (g_relay_pid > 0) {
		int st;
		(void) kill(g_relay_pid, SIGTERM);
		(void) waitpid(g_relay_pid, &st, 0);
		g_relay_pid = -1;
	}
}

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
	uint64_t ack_ns;
} LatPing;

typedef struct {
	XtkSessionClient *sc;
	LatPing ping[LAT_SLOTS];
	Hist lat;
	Hist ack;
	int tracked;
	/* P3.14 entity/event integrity metrics */
	unsigned long events_death, events_respawn, events_score, events_match_end;
	unsigned long dup_death;              /* same (slot,generation) death twice */
	uint32_t death_gen[XTK_MAX_VEHICLES];
	uint8_t  death_seen[XTK_MAX_VEHICLES];
	uint32_t veh_gen[XTK_MAX_VEHICLES];   /* last seen per-slot generation */
	unsigned long gen_regression;         /* slot generation went backwards  */
	unsigned long snapshots;
	uint32_t max_snap_age_ticks;          /* max(server frame - snapshot tick) */
	uint32_t last_snap_tick;
	/* P3.14 recipient-authorization audit (decoded per-recipient stream) */
	FILE *authz;                          /* one line per delivered message   */
	int bound_slot;                       /* slot the server bound to this peer */
	unsigned long view_msgs;
	unsigned long bad_slot;               /* own_slot != bound_slot           */
	unsigned long events_total;
	unsigned long lobby_msgs, results_msgs, ack_msgs;
} LatTracker;

static const char *
msg_type_name(uint8_t t)
{
	switch (t) {
	case XTK_MSG_CMD_ACK: return "CMD_ACK";
	case XTK_MSG_VIEW_SNAPSHOT: return "VIEW";
	case XTK_MSG_EVENT: return "EVENT";
	case XTK_MSG_LOBBY_STATE: return "LOBBY";
	case XTK_MSG_RESULTS: return "RESULTS";
	case XTK_MSG_WELCOME: return "WELCOME";
	case XTK_MSG_START: return "START";
	case XTK_MSG_PING: return "PING";
	case XTK_MSG_PONG: return "PONG";
	default: return "OTHER";
	}
}

static int
lat_callback(void *user, const XtkMessage *m)
{
	LatTracker *lt = (LatTracker *) user;
	uint64_t now = mono_ns();
	int i;

	if (m == (const XtkMessage *) NULL)
		return 0;
	if (lt->authz != NULL) {
		switch (m->env.message_type) {
		case XTK_MSG_VIEW_SNAPSHOT:
			fprintf(lt->authz, "VIEW epoch=%u slot=%u tick=%u\n",
				m->env.epoch, m->body.view_snapshot.own_slot,
				m->body.view_snapshot.server_tick);
			break;
		case XTK_MSG_EVENT:
			fprintf(lt->authz, "EVENT epoch=%u type=%u id=%u gen=%u\n",
				m->env.epoch, m->body.event.event_type,
				m->body.event.entity_id,
				m->body.event.entity_generation);
			break;
		case XTK_MSG_LOBBY_STATE:
			fprintf(lt->authz, "LOBBY epoch=%u\n", m->env.epoch);
			break;
		case XTK_MSG_RESULTS:
			fprintf(lt->authz, "RESULTS epoch=%u\n", m->env.epoch);
			break;
		default:
			fprintf(lt->authz, "%s epoch=%u\n",
				msg_type_name(m->env.message_type), m->env.epoch);
			break;
		}
	}
	if (m->env.message_type == XTK_MSG_LOBBY_STATE) {
		const XtkLobbyStatus *ls = &m->body.lobby_state;
		int all = ls->human_count >= 2;
		int j;

		for (j = 0; j < (int) ls->human_count &&
		     j < XTK_LOBBY_ROSTER_MAX; j++) {
			if (!ls->entry[j].ready)
				all = 0;
		}
		g_lobby_human_count = (int) ls->human_count;
		g_lobby_all_ready = all;
	}
	if (m->env.message_type == XTK_MSG_CMD_ACK) {
		lt->ack_msgs++;
		for (i = 0; i < (int) m->body.cmd_ack.count; i++) {
			uint32_t id = m->body.cmd_ack.entry[i].command_id;
			LatPing *p = &lt->ping[id & (LAT_SLOTS - 1)];

			if (g_applied != NULL &&
			    m->body.cmd_ack.entry[i].status == XTK_ACK_APPLIED) {
				uint32_t at = m->body.cmd_ack.entry[i].assigned_tick;
				int sub = (int) (id % 64u) % 16;
				int pl = (int) ((id % 64u) / 16);

				if (sub == 1)
					fprintf(g_applied, "%u %d drive %d\n", at,
						pl, pl == 0 ? 9 : -9);
				else if (sub == 2)
					fprintf(g_applied, "%u %d fire\n", at, pl);
			}
			if (p->active && p->command_id == id) {
				p->acked = 1;
				p->ack_ns = now;
				p->assigned_tick =
					m->body.cmd_ack.entry[i].assigned_tick;
			}
		}
	} else if (m->env.message_type == XTK_MSG_VIEW_SNAPSHOT) {
		uint32_t t = m->body.view_snapshot.server_tick;
		int v;

		lt->snapshots++;
		lt->view_msgs++;
		if (lt->bound_slot >= 0 &&
		    (int) m->body.view_snapshot.own_slot != lt->bound_slot)
			lt->bad_slot++;
		lt->last_snap_tick = t;
		if ((uint32_t) frame >= t) {
			uint32_t age = (uint32_t) frame - t;
			if (age > lt->max_snap_age_ticks)
				lt->max_snap_age_ticks = age;
		}
		for (v = 0; v < (int) m->body.view_snapshot.vehicle_count &&
		     v < XTK_MAX_VEHICLES; v++) {
			const XtkVehicleView *vv = &m->body.view_snapshot.vehicle[v];
			uint8_t slot = vv->slot;

			if (slot < XTK_MAX_VEHICLES) {
				if (lt->veh_gen[slot] != 0 &&
				    vv->generation < lt->veh_gen[slot])
					lt->gen_regression++;
				lt->veh_gen[slot] = vv->generation;
			}
		}
		for (i = 0; i < LAT_SLOTS; i++) {
			LatPing *p = &lt->ping[i];

			if (p->active && p->acked && p->assigned_tick <= t) {
				uint64_t d = now - p->send_ns;

				if (lt->lat.count != NULL)
					hist_add(&lt->lat, (long long) d);
				if (lt->ack.count != NULL && p->ack_ns >= p->send_ns)
					hist_add(&lt->ack,
						 (long long) (p->ack_ns - p->send_ns));
				p->active = 0;
			}
		}
	} else if (m->env.message_type == XTK_MSG_EVENT) {
		const XtkEvent *e = &m->body.event;

		lt->events_total++;
		switch (e->event_type) {
		case XTK_EV_DEATH: {
			uint16_t id = e->entity_id;
			lt->events_death++;
			if (id < XTK_MAX_VEHICLES) {
				if (lt->death_seen[id] &&
				    lt->death_gen[id] == e->entity_generation)
					lt->dup_death++;
				lt->death_seen[id] = 1;
				lt->death_gen[id] = e->entity_generation;
			}
			break;
		}
		case XTK_EV_RESPAWN:
			lt->events_respawn++;
			break;
		case XTK_EV_SCORE:
			lt->events_score++;
			break;
		case XTK_EV_MATCH_END:
			lt->events_match_end++;
			break;
		default:
			break;
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
	/*
	 * P3.14: use the *real* monotonic clock throughout bring-up so the
	 * server's valid-input timeout (2 s) is measured against the same time
	 * base as run_pace().  (The P3.13 fixture used an artificial counter
	 * because its clients were in-process; through the real-UDP relay a
	 * mismatched base makes the first step look like a 2 s timeout.)
	 */
	uint64_t deadline;
	int i;

	deadline = mono_ns() + 20000000000ULL;	/* 20 s wall-clock */
	for (i = 0; i < 20000 && mono_ns() < deadline &&
	     session_client_state(A->sc) != XTK_SC_LOBBY; i++) {
		uint64_t now = mono_ns();

		(void) server_session_service(now / 1000000ULL);
		(void) session_client_pump(A->sc, now, lat_callback, &A->lat);
		usleep(1000);
	}
	(void) session_client_connect(B->sc, "127.0.0.1", g_client_ports[1],
				      "playerB");
	for (i = 0; i < 20000 && mono_ns() < deadline &&
	     (session_client_state(A->sc) != XTK_SC_LOBBY ||
	      session_client_state(B->sc) != XTK_SC_LOBBY); i++) {
		uint64_t now = mono_ns();

		(void) server_session_service(now / 1000000ULL);
		(void) session_client_pump(A->sc, now, lat_callback, &A->lat);
		(void) session_client_pump(B->sc, now, lat_callback, &B->lat);
		usleep(1000);
	}
	if (session_client_state(A->sc) != XTK_SC_LOBBY ||
	    session_client_state(B->sc) != XTK_SC_LOBBY) {
		fprintf(stderr, "latency_fixture: lobby timeout A=%d B=%d "
			"elapsed_ms=%lld\n", (int) session_client_state(A->sc),
			(int) session_client_state(B->sc),
			(long long) ((mono_ns() - (deadline - 20000000000ULL)) / 1000000ULL));
		return -1;
	}

	(void) session_client_send_config(A->sc, (uint16_t) g_maze_id,
					  XTK_CONTENT_ID_VANGUARD,
					  (uint8_t) drones);
	for (i = 0; i < 20000 && mono_ns() < deadline &&
	     session_client_config_revision(A->sc) == 0; i++) {
		uint64_t now = mono_ns();

		(void) server_session_service(now / 1000000ULL);
		(void) session_client_pump(A->sc, now, lat_callback, &A->lat);
		(void) session_client_pump(B->sc, now, lat_callback, &B->lat);
		usleep(1000);
	}
	g_lobby_all_ready = 0;
	(void) session_client_send_ready(A->sc, 1);
	(void) session_client_send_ready(B->sc, 1);
	server_seed_random(114336);
	/*
	 * READY(A), READY(B) and START(A) travel on two *independent* ordered
	 * ENet streams (one per peer), so under loss/reorder START(A) can reach
	 * the server before READY(B) and be rejected NOT_READY.  Wait for the
	 * server's reliable LOBBY_STATE to show both peers ready before asking
	 * to start; then send START once with only a few spaced retries.  This
	 * mirrors the real UI (Start unlocks only when everyone is ready) and
	 * avoids flooding the fixed-tick per-peer admission quota, which would
	 * otherwise ack-then-drop reliable control messages during bring-up.
	 */
	{
		uint64_t phase_deadline = mono_ns() + 15000000000ULL;
		uint64_t next_start = 0;
		int start_attempts = 0;

		for (i = 0; i < 200000 && mono_ns() < phase_deadline; i++) {
			ServerSession *s;
			uint64_t now = mono_ns();

			(void) server_session_service(now / 1000000ULL);
			(void) session_client_pump(A->sc, now, lat_callback, &A->lat);
			(void) session_client_pump(B->sc, now, lat_callback, &B->lat);
			s = server_session_current();
			if (s != NULL &&
			    server_session_state(s) == SERVER_SESSION_RUNNING)
				return 0;
			if (g_lobby_all_ready && start_attempts < 4 &&
			    now >= next_start) {
				(void) session_client_send_start(A->sc);
				start_attempts++;
				next_start = now + 2000000000ULL;
			}
			usleep(1000);
		}
	}
	{
		ServerSession *s = server_session_current();
		fprintf(stderr, "latency_fixture: start timeout A=%d B=%d "
			"cfgA=%u cfgB=%u srv=%d\n", (int) session_client_state(A->sc),
			(int) session_client_state(B->sc),
			(unsigned) session_client_config_revision(A->sc),
			(unsigned) session_client_config_revision(B->sc),
			s != NULL ? (int) server_session_state(s) : -1);
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
	 int drones, int target_ticks, const char *out, const char *label)
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
	    !hist_init(&B->lat.lat, 100000, 50001) ||
	    !hist_init(&A->lat.ack, 1000, 100001) ||
	    !hist_init(&B->lat.ack, 1000, 100001)) {
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

	if (relay_launch(label, transport_port(host), 2) != 2)
		return -1;

	A->sc = session_client_create();
	B->sc = session_client_create();
	if (A->sc == NULL || B->sc == NULL)
		return -1;
	if (session_client_connect(A->sc, "127.0.0.1", g_client_ports[0],
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
	A->lat.bound_slot = -1;
	B->lat.bound_slot = -1;
	sa = server_session_peer_at(0);
	sb = server_session_peer_at(1);
	if (sa != NULL)
		A->lat.bound_slot = server_session_bound_slot(sa);
	if (sb != NULL)
		B->lat.bound_slot = server_session_bound_slot(sb);

	/* P3.14 recipient-authorization audit logs (decoded per-recipient). */
	{
		char ap[1200];

		snprintf(ap, sizeof(ap), "%s.authz.0", out);
		A->lat.authz = fopen(ap, "w");
		snprintf(ap, sizeof(ap), "%s.authz.1", out);
		B->lat.authz = fopen(ap, "w");
	}

	if (g_applied_path != NULL) {
		g_applied = fopen(g_applied_path, "w");
		if (g_applied == NULL) {
			fprintf(stderr, "impairment_fixture: cannot open applied log %s\n",
				g_applied_path);
			return -1;
		}
	}

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
		int frozen = g_imp.freeze && ticks >= g_imp.freeze_at &&
			ticks < g_imp.freeze_at + g_imp.freeze_dur_ticks;
		int freezeA = frozen && g_imp.freeze_client == 0;
		int freezeB = frozen && g_imp.freeze_client == 1;

		/* P3.14 backpressure: reset packets across channels, then a fresh
		 * start transaction on a new epoch, all through the impaired link. */
		if (g_imp.reset_at > 0 && ticks == g_imp.reset_at) {
			int it;
			uint64_t dl;

			g_reset_epoch_before = session_client_epoch(A->sc);
			fprintf(stderr, "backpressure: RESET at tick %ld epoch %u\n",
				ticks, g_reset_epoch_before);
			(void) session_client_send_reset(A->sc);
			dl = mono_ns() + 10000000000ULL;
			for (it = 0; it < 40000 && mono_ns() < dl &&
			     session_client_state(A->sc) != XTK_SC_LOBBY; it++) {
				uint64_t t = mono_ns();

				(void) server_session_service(t / 1000000ULL);
				(void) session_client_pump(A->sc, t, lat_callback,
							   &A->lat);
				(void) session_client_pump(B->sc, t, lat_callback,
							   &B->lat);
				usleep(500);
			}
			fprintf(stderr, "backpressure: lobby state A=%d B=%d\n",
				session_client_state(A->sc),
				session_client_state(B->sc));
			/*
			 * D3-B: a reset preserves the configured maze/vehicle/
			 * drone count but requires fresh readiness.  The server
			 * keeps config_valid/config_revision across the return to
			 * lobby, so only READY + START are re-sent.  Retry START
			 * because READY(B) and START(A) are independent ordered
			 * ENet streams (same reason as match_start), and under
			 * heavy impairment a single early START would be rejected
			 * NOT_READY and strand the lobby.
			 */
			g_lobby_all_ready = 0;
			(void) session_client_send_ready(A->sc, 1);
			(void) session_client_send_ready(B->sc, 1);
			server_seed_random(114336);
			{
				uint64_t next_start = 0;
				int start_attempts = 0;

				dl = mono_ns() + 15000000000ULL;
				for (it = 0; it < 400000 && mono_ns() < dl; it++) {
					uint64_t t = mono_ns();

					(void) server_session_service(t / 1000000ULL);
					(void) session_client_pump(A->sc, t,
								   lat_callback,
								   &A->lat);
					(void) session_client_pump(B->sc, t,
								   lat_callback,
								   &B->lat);
					s = server_session_current();
					if (s != NULL && server_session_state(s) ==
					    SERVER_SESSION_RUNNING)
						break;
					if (g_lobby_all_ready &&
					    start_attempts < 4 && t >= next_start) {
						(void) session_client_send_start(A->sc);
						start_attempts++;
						next_start = t + 2000000000ULL;
					}
					usleep(500);
				}
			}
			/*
			 * The server commits the new epoch as soon as it processes
			 * START, but each client only observes it when it receives
			 * the reliable START/EPOCH_BEGIN on its own stream (which
			 * may be retransmitted under impairment).  Settle both
			 * clients before reading the new epoch.
			 */
			dl = mono_ns() + 5000000000ULL;
			for (it = 0;
			     it < 100000 && mono_ns() < dl &&
			     (session_client_state(A->sc) != XTK_SC_RUNNING ||
			      session_client_state(B->sc) != XTK_SC_RUNNING);
			     it++) {
				uint64_t t = mono_ns();

				(void) server_session_service(t / 1000000ULL);
				(void) session_client_pump(A->sc, t, lat_callback,
							   &A->lat);
				(void) session_client_pump(B->sc, t, lat_callback,
							   &B->lat);
				usleep(500);
			}
			{
				ServerSession *ss = server_session_current();
				ServerSession *pa = server_session_peer_at(0);
				ServerSession *pb = server_session_peer_at(1);

				fprintf(stderr, "backpressure: restart done srv=%d "
					"A=%d B=%d cfgA=%u cfgB=%u epA=%u epB=%u "
					"cfgvalid=%d readyA=%d readyB=%d adminA=%d\n",
					ss != NULL ? (int) server_session_state(ss) : -1,
					(int) session_client_state(A->sc),
					(int) session_client_state(B->sc),
					(unsigned) session_client_config_revision(A->sc),
					(unsigned) session_client_config_revision(B->sc),
					(unsigned) session_client_epoch(A->sc),
					(unsigned) session_client_epoch(B->sc),
					ss != NULL ? server_session_config_valid(ss) : -1,
					pa != NULL ? server_session_ready(pa) : -1,
					pb != NULL ? server_session_ready(pb) : -1,
					pa != NULL ? server_session_is_admin(pa) : -1);
			}
			sa = server_session_peer_at(0);
			sb = server_session_peer_at(1);
			if (sa != NULL)
				A->lat.bound_slot = server_session_bound_slot(sa);
			if (sb != NULL)
				B->lat.bound_slot = server_session_bound_slot(sb);
			g_reset_epoch_after = session_client_epoch(A->sc);
			g_reset_tick_done = ticks;
			fprintf(stderr, "backpressure: restarted epoch %u\n",
				g_reset_epoch_after);
		}

		if (freezeA && !g_freeze_announced) {
			fprintf(stderr, "impairment_fixture: FREEZE client A at tick %ld\n", ticks);
			g_freeze_announced = 1;
		}
		if (freezeB && !g_freeze_announced) {
			fprintf(stderr, "impairment_fixture: FREEZE client B at tick %ld\n", ticks);
			g_freeze_announced = 1;
		}

		/* Input refresh at 15 Hz, independent of render FPS (P3.11). */
		{
			int fire = ((ticks % 30) == 0);

			if (!freezeA) {
				net_render(A, now);
				(void) send_tick_input(A, (uint32_t) (frame + 1), 0, fire);
			}
			if (!freezeB) {
				net_render(B, now);
				(void) send_tick_input(B, (uint32_t) (frame + 1), 1, fire);
			}
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
		/*
		 * Pace to the tick deadline while pumping the clients at ~1 kHz,
		 * as a continuously-rendering client does.  This avoids adding a
		 * whole fixture tick to the measured input-to-visible latency
		 * (the shipped server loop sleeps the same interval).
		 */
		{
			long long pdeadline = (long long) rs.pace_t0 +
				(long long) (ticks + 1) * 1000000000LL / 15LL;

			for (;;) {
				uint64_t t = mono_ns();

				if (!freezeA)
					(void) session_client_pump(A->sc, t,
						lat_callback, &A->lat);
				if (!freezeB)
					(void) session_client_pump(B->sc, t,
						lat_callback, &B->lat);
				if ((long long) t >= pdeadline)
					break;
				{
					struct timespec ts;

					ts.tv_sec = 0;
					ts.tv_nsec = 1000000;	/* 1 ms */
					(void) nanosleep(&ts, NULL);
				}
			}
		}

		/*
		 * A real server receives continuously; service again after the pace
		 * wait so input that arrived during the sleep is admitted into the
		 * tick we are about to run.  This matches the shipped receive path
		 * (its socket buffer is drained at the tick boundary) without
		 * inventing an extra tick of delay.
		 */
		a = (long long) mono_ns();
		(void) server_session_service(mono_ns() / 1000000ULL);
		b = (long long) mono_ns();
		svc += b - a;

		a = (long long) mono_ns();
		status = server_session_step(s, mono_ns() / 1000000ULL);
		b = (long long) mono_ns();
		stp = b - a;

		/*
		 * A real client receives/renders continuously rather than once per
		 * server tick; pump both here with a fresh timestamp so the
		 * input-to-visible measurement reflects the shipped client rather
		 * than the fixture's tick granularity.  A frozen client is left
		 * unserviced (no send, no receive).
		 */
		if (!freezeA)
			(void) session_client_pump(A->sc, mono_ns(), lat_callback,
						   &A->lat);
		if (!freezeB)
			(void) session_client_pump(B->sc, mono_ns(), lat_callback,
						   &B->lat);

		hist_add(&rs.service, svc);
		hist_add(&rs.step, stp);
		hist_add(&rs.total, svc + stp);
		ticks++;

		/* P3.14 freeze: record when the server removes the frozen peer. */
		if (g_imp.freeze && g_freeze_removed_tick < 0 &&
		    server_session_peer_at(g_imp.freeze_client) == NULL)
			g_freeze_removed_tick = ticks;

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
	fprintf(sum, "ack_p50_ms_A=%.2f ack_p95_ms_A=%.2f ack_p50_ms_B=%.2f "
		"ack_p95_ms_B=%.2f\n",
		(double) hist_pct(&A->lat.ack, 50) / 1e6,
		(double) hist_pct(&A->lat.ack, 95) / 1e6,
		(double) hist_pct(&B->lat.ack, 50) / 1e6,
		(double) hist_pct(&B->lat.ack, 95) / 1e6);
	fprintf(sum, "rss_hwm_kb=%lu\n", rss_hwm_kb());
	fprintf(sum, "wire_frame=loopback-monotonic-same-process\n");
	fprintf(sum, "label=%s\n", label);
	fprintf(sum, "impair_delay_ms=%.3f impair_loss=%.3f impair_reorder=%.3f "
		"impair_dup=%.3f impair_extra_ms=%.3f impair_extra_prob=%.4f "
		"impair_seed=%llu\n", g_imp.delay_ms, g_imp.loss, g_imp.reorder,
		g_imp.dup, g_imp.extra_ms, g_imp.extra_prob, g_imp.seed);
	for (i = 0; i < 2; i++) {
		LatTracker *l = (i == 0) ? &A->lat : &B->lat;
		fprintf(sum, "peer%d_snapshots=%lu peer%d_max_snap_age_ticks=%u "
			"peer%d_deaths=%lu peer%d_respawns=%lu peer%d_dup_death=%lu "
			"peer%d_gen_regression=%lu peer%d_score_events=%lu "
			"peer%d_match_end=%lu\n", i, l->snapshots, i,
			l->max_snap_age_ticks, i, l->events_death, i,
			l->events_respawn, i, l->dup_death, i, l->gen_regression,
			i, l->events_score, i, l->events_match_end);
		fprintf(sum, "peer%d_bound_slot=%d peer%d_view_msgs=%lu "
			"peer%d_bad_slot=%lu peer%d_events=%lu peer%d_ack_msgs=%lu\n",
			i, l->bound_slot, i, l->view_msgs, i, l->bad_slot,
			i, l->events_total, i, l->ack_msgs);
	}
	if (g_imp.freeze) {
		XtkSessionClient *healthy = (g_imp.freeze_client == 0)
			? B->sc : A->sc;
		XtkSessionClient *frozen = (g_imp.freeze_client == 0)
			? A->sc : B->sc;

		g_healthy_admin_index = session_client_admin_index(healthy);
		g_healthy_self_index = session_client_self_index(healthy);
		g_frozen_state_at_end = (int) session_client_state(frozen);
	}
	fprintf(sum, "freeze_on=%d freeze_client=%d freeze_at=%ld freeze_dur=%ld "
		"freeze_removed_tick=%ld healthy_admin_index=%d "
		"healthy_self_index=%d frozen_state_at_end=%d\n",
		g_imp.freeze, g_imp.freeze_client, g_imp.freeze_at,
		g_imp.freeze_dur_ticks, g_freeze_removed_tick,
		g_healthy_admin_index, g_healthy_self_index,
		g_frozen_state_at_end);
	fprintf(sum, "reset_at=%ld reset_tick_done=%ld reset_epoch_before=%lu "
		"reset_epoch_after=%lu\n", g_imp.reset_at, g_reset_tick_done,
		g_reset_epoch_before, g_reset_epoch_after);
	fclose(sum);
	for (i = 0; i < 2; i++) {
		FILE *af = (i == 0) ? A->lat.authz : B->lat.authz;

		if (af != NULL) {
			fclose(af);
			if (i == 0)
				A->lat.authz = NULL;
			else
				B->lat.authz = NULL;
		}
	}
	if (g_applied != NULL) {
		fclose(g_applied);
		g_applied = NULL;
	}

	(void) i;
	relay_stop();
	session_client_destroy(A->sc);
	session_client_destroy(B->sc);
	transport_destroy(host);
	printf("IMPAIRMENT FIXTURE run ticks=%ld late=%ld total_p99_us=%.1f "
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
	g_client_ports[0] = transport_port(host);
	A->sc = session_client_create();
	B->sc = session_client_create();
	if (A->sc == NULL || B->sc == NULL)
		return -1;
	if (session_client_connect(A->sc, "127.0.0.1", g_client_ports[0],
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

static void
parse_imp_flags(int argc, char **argv, int start)
{
	int i;
	for (i = start; i + 1 < argc; i += 2) {
		const char *k = argv[i];
		const char *v = argv[i + 1];
		if (strcmp(k, "--delay-ms") == 0) g_imp.delay_ms = atof(v);
		else if (strcmp(k, "--loss") == 0) g_imp.loss = atof(v);
		else if (strcmp(k, "--reorder") == 0) g_imp.reorder = atof(v);
		else if (strcmp(k, "--dup") == 0) g_imp.dup = atof(v);
		else if (strcmp(k, "--extra-delay-ms") == 0) g_imp.extra_ms = atof(v);
		else if (strcmp(k, "--extra-prob") == 0) g_imp.extra_prob = atof(v);
		else if (strcmp(k, "--seed") == 0) g_imp.seed = strtoull(v, NULL, 0);
		else if (strcmp(k, "--freeze") == 0) g_imp.freeze = atoi(v);
		else if (strcmp(k, "--freeze-client") == 0) g_imp.freeze_client = atoi(v);
		else if (strcmp(k, "--freeze-at") == 0) g_imp.freeze_at = atol(v);
		else if (strcmp(k, "--freeze-dur") == 0) g_imp.freeze_dur_ticks = atol(v);
		else if (strcmp(k, "--reset-at") == 0) g_imp.reset_at = atol(v);
		else if (strcmp(k, "--applied-log") == 0) g_applied_path = v;
	}
}

int
main(int argc, char *argv[])
{
	const char *mode, *assets, *maze, *out, *label;
	NetPeer A, B;
	long ticks_target;
	char outbuf[1200];
	const char *wd, *rb;

	if (argc < 2) {
		fprintf(stderr,
		  "usage:\n"
		  "  %s run <assets> <maze> <settings> <ticks> <workdir> <label>"
		  " [--delay-ms N --loss P --reorder P --dup P --extra-delay-ms N\n"
		  "         --extra-prob P --seed N --freeze 0|1 --freeze-client 0|1"
		  " --freeze-at T --freeze-dur T]\n"
		  "  %s stress <assets> <maze> <settings> <ticks> <out>\n",
		  argv[0], argv[0]);
		return 2;
	}
	mode = argv[1];
	memset(&g_imp, 0, sizeof(g_imp));
	g_imp.seed = 114336;
	memset(&A, 0, sizeof(A));
	memset(&B, 0, sizeof(B));
	A.fps = B.fps = 15;

	wd = getenv("XTANK_P3_WORKDIR");
	snprintf(g_workdir, sizeof(g_workdir), "%s", wd ? wd : ".");
	rb = getenv("XTANK_P3_RELAY_BIN");
	if (rb != NULL)
		g_relay_bin = rb;

	if (strcmp(mode, "run") == 0) {
		if (argc < 8) {
			fprintf(stderr, "usage: %s run <assets> <maze> "
				"<settings> <ticks> <workdir> <label> [flags]\n",
				argv[0]);
			return 2;
		}
		assets = argv[2];
		maze = argv[3];
		g_settings_path = argv[4];
		ticks_target = atol(argv[5]);
		snprintf(g_workdir, sizeof(g_workdir), "%s", argv[6]);
		label = argv[7];
		parse_imp_flags(argc, argv, 8);
		out = (snprintf(outbuf, sizeof(outbuf), "%s/%s", g_workdir, label),
		       outbuf);
		if (strcmp(maze, "combat1") == 0)
			g_maze_id = XTK_CONTENT_ID_COMBAT1;
		else
			g_maze_id = XTK_CONTENT_ID_ARENA;
		if (server_bringup(assets, maze) != 0)
			return 2;
		if (run_pace(assets, maze, &A, &B, 2, (int) ticks_target, out,
			     label) != 0)
			return 1;
		printf("IMPAIRMENT FIXTURE PASS (mode=run label=%s)\n", label);
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
		printf("IMPAIRMENT FIXTURE PASS (mode=stress)\n");
		return 0;
	}
	fprintf(stderr, "impairment_fixture: unknown mode %s\n", mode);
	return 2;
}
