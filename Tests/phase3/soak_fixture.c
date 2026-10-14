/*-
 * Copyright (c) 1988 Terry Donahue
 * All rights reserved.
 *
 * P3.15 paced four-vehicle bot/reset soak fixture.
 *
 * Drives the *shipped* coordinator + admission + pinned ENet transport + the
 * frozen codec and two real P3.10 client sessions through a single continuous,
 * real-time paced, two-human / two-drone match with many RESET transactions.
 * Unlike the P3.13/P3.14 fixtures it survives a 60-minute run and records its
 * progress to disk incrementally, so a supervisor can poll it.
 *
 *   run <assets> <maze> <settings> <resets> <workdir> <label>
 *         [--epoch-ticks N] [--rejoin-at i,j,...]
 *
 * The shipped absolute-deadline pacer (server_loop_pace) runs each epoch at
 * 15 Hz.  A RESET returns to the shared lobby barrier; a fresh ready/start
 * begins the next epoch.  On selected resets a mid-epoch admin departure is
 * exercised first (2 s valid-input removal, administrator transfer) and the
 * departed client rejoins with a *fresh* connection/ownership in the new
 * lobby (connection reuse).
 *
 * The frozen simulation is never modified: only read-only engine globals are
 * sampled for bot evidence, and all input/profile generation is test-only.
 * Files are written under <workdir> (outside the checkout):
 *
 *   <workdir>/progress.txt   one line every ~15 s + at every reset (fflushed)
 *   <workdir>/resets.txt     one detailed line per completed reset
 *   <workdir>/cycles.txt     one line per admin/rejoin cycle
 *   <workdir>/<label>.summary  final key=value summary
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
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

/* InitConfigStruct is normally owned by main.c (excluded from this build). */
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

/* ---- sandbox sendmsg shim (test-harness only) ------------------------- */
ssize_t __real_sendmsg(int fd, const struct msghdr *msg, int flags);

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

/* ---- harness state ---------------------------------------------------- */
typedef struct {
	XtkSessionClient *sc;
	uint32_t pkt_seq;
	int fps;
	uint64_t next_render_ns;
	int present;
	/* integrity metrics (P3.14 style) */
	unsigned long snapshots;
	unsigned long results_msgs, lobby_msgs, welcome_msgs, epoch_msgs;
	unsigned long events_total, events_death, events_respawn, events_score,
		      events_match_end;
	unsigned long dup_death, gen_regression, bad_slot;
	uint32_t death_gen[XTK_MAX_VEHICLES];
	uint8_t  death_seen[XTK_MAX_VEHICLES];
	uint32_t veh_gen[XTK_MAX_VEHICLES];
	uint32_t max_snap_age_ticks, last_snap_tick;
	int bound_slot;
	const char *name;
} NetPeer;

/* Drone (bot) evidence accumulated during the current epoch. */
typedef struct {
	double move;			/* sum |dpos| over bot vehicles   */
	double heading_change;		/* sum |dheading| (radians)       */
	double turret_change;		/* sum |dturret angle|            */
	unsigned long bullets_owned_max;	/* max drone-owned bullets */
	unsigned long fire_samples;	/* samples with >=1 drone bullet  */
	unsigned long bot_samples;
	int have_prev;
	double prev_x[XTK_MAX_VEHICLES], prev_y[XTK_MAX_VEHICLES];
	double prev_head[XTK_MAX_VEHICLES], prev_turr[XTK_MAX_VEHICLES];
	uint8_t prev_seen[XTK_MAX_VEHICLES];
} DroneStats;

typedef struct {
	uint64_t pace_t0;
	long late_ticks;
	long ticks_total;
	unsigned long max_reliable, max_outgoing;
	int max_intent_queue;
	int max_vehicles, max_bullets, max_explosions;
	unsigned long rss_warm_kb;	/* RSS after warm-up (reset #2)   */
	unsigned long rss_max_kb, rss_min_after_warm_kb;
	unsigned long fds_max, threads_max;
	int resets_done;
} RunStats;

static char g_workdir[1024];
static uint16_t g_server_port;
static FILE *g_progress, *g_resets, *g_cycles;
static int g_verbose;
static uint64_t g_run_t0;

static uint64_t
mono_ns(void)
{
	struct timespec ts;

	(void) clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t) ts.tv_sec * 1000000000ULL + (uint64_t) ts.tv_nsec;
}

static void
kv_read(const char *file, const char *key, unsigned long *out)
{
	FILE *fp = fopen(file, "r");
	char line[256];

	*out = 0;
	if (fp == NULL)
		return;
	while (fgets(line, sizeof(line), fp) != NULL) {
		if (strncmp(line, key, strlen(key)) == 0) {
			(void) sscanf(line + strlen(key), "%lu", out);
			break;
		}
	}
	fclose(fp);
}

static unsigned long
rss_kb(void)
{
	unsigned long v = 0;

	kv_read("/proc/self/status", "VmRSS:", &v);
	return v;
}

static unsigned long
rss_hwm_kb(void)
{
	unsigned long v = 0;

	kv_read("/proc/self/status", "VmHWM:", &v);
	return v;
}

static unsigned long
thread_count(void)
{
	unsigned long v = 0;

	kv_read("/proc/self/status", "Threads:", &v);
	return v;
}

static unsigned long
fd_count(void)
{
	DIR *d = opendir("/proc/self/fd");
	struct dirent *e;
	unsigned long n = 0;

	if (d == NULL)
		return 0;
	while ((e = readdir(d)) != NULL)
		if (e->d_name[0] != '.')
			n++;
	closedir(d);
	return n;
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
	strncpy(username, "p3.15", MAX_STRING - 1);
	username[MAX_STRING - 1] = '\0';

	if (content_set_root(assets) != 0) {
		fprintf(stderr, "soak_fixture: %s\n", content_last_error());
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
		fprintf(stderr, "soak_fixture: cannot load %s\n", maze_name);
		return -1;
	}
	if (g_settings_path != NULL &&
	    content_load_settings(g_settings_path) != 0) {
		fprintf(stderr, "soak_fixture: %s\n", content_last_error());
		return -1;
	}
	init_threader();
	init_msg_sys();
	init_box_names();
	geometry_order_objects();
	geometry_rotate_objects();
	return 0;
}

/* ---- inbound callback: integrity accounting + lobby readiness --------- */
static volatile int g_lobby_all_ready;

static int
soak_callback(void *user, const XtkMessage *m)
{
	NetPeer *p = (NetPeer *) user;
	int i;

	if (m == (const XtkMessage *) NULL)
		return 0;
	switch (m->env.message_type) {
	case XTK_MSG_VIEW_SNAPSHOT: {
		uint32_t t = m->body.view_snapshot.server_tick;
		int v;

		p->snapshots++;
		if (p->bound_slot >= 0 &&
		    (int) m->body.view_snapshot.own_slot != p->bound_slot)
			p->bad_slot++;
		p->last_snap_tick = t;
		if ((uint32_t) frame >= t) {
			uint32_t age = (uint32_t) frame - t;

			if (age > p->max_snap_age_ticks)
				p->max_snap_age_ticks = age;
		}
		for (v = 0; v < (int) m->body.view_snapshot.vehicle_count &&
		     v < XTK_MAX_VEHICLES; v++) {
			const XtkVehicleView *vv =
				&m->body.view_snapshot.vehicle[v];
			uint8_t slot = vv->slot;

			if (slot < XTK_MAX_VEHICLES) {
				if (p->veh_gen[slot] != 0 &&
				    vv->generation < p->veh_gen[slot])
					p->gen_regression++;
				p->veh_gen[slot] = vv->generation;
			}
		}
		break;
	}
	case XTK_MSG_EVENT: {
		const XtkEvent *e = &m->body.event;

		p->events_total++;
		switch (e->event_type) {
		case XTK_EV_DEATH: {
			uint16_t id = e->entity_id;

			p->events_death++;
			if (id < XTK_MAX_VEHICLES) {
				if (p->death_seen[id] &&
				    p->death_gen[id] == e->entity_generation)
					p->dup_death++;
				p->death_seen[id] = 1;
				p->death_gen[id] = e->entity_generation;
			}
			break;
		}
		case XTK_EV_RESPAWN:
			p->events_respawn++;
			break;
		case XTK_EV_SCORE:
			p->events_score++;
			break;
		case XTK_EV_MATCH_END:
			p->events_match_end++;
			break;
		default:
			break;
		}
		break;
	}
	case XTK_MSG_RESULTS:
		p->results_msgs++;
		break;
	case XTK_MSG_LOBBY_STATE: {
		const XtkLobbyStatus *ls = &m->body.lobby_state;
		int all = ls->human_count >= 2;
		int j;

		p->lobby_msgs++;
		for (j = 0; j < (int) ls->human_count &&
		     j < XTK_LOBBY_ROSTER_MAX; j++) {
			if (!ls->entry[j].ready)
				all = 0;
		}
		/* Only a *lobby* roster gates START; during a running admin
		 * transfer human_count may be 1 and must not set all-ready. */
		if (ls->session_state == XTK_SESSION_LOBBY)
			g_lobby_all_ready = all;
		break;
	}
	case XTK_MSG_WELCOME:
		p->welcome_msgs++;
		break;
	case XTK_MSG_EPOCH_BEGIN:
		p->epoch_msgs++;
		break;
	default:
		break;
	}
	(void) i;
	return 0;
}

static void
net_render(NetPeer *p, uint64_t now_ns)
{
	int k;

	if (p->fps <= 0)
		p->fps = 15;
	if (p->next_render_ns == 0)
		p->next_render_ns = now_ns;
	for (k = 0; k < 1000 && now_ns >= p->next_render_ns; k++) {
		(void) session_client_pump(p->sc, now_ns, soak_callback, p);
		p->next_render_ns += 1000000000ULL / (uint64_t) p->fps;
	}
	if (now_ns > p->next_render_ns + 1000000000ULL)
		p->next_render_ns = now_ns + 1000000000ULL / (uint64_t) p->fps;
}

/* Monotonic across epochs: the engine `frame` (and therefore any frame-derived
 * command id) resets at each epoch start, while the shipped per-session dedup
 * ring persists across a reset.  A unique id avoids a spurious cross-epoch
 * duplicate rejection. */
static uint32_t g_cmd_id = 1;

static void
send_tick_input(NetPeer *p, int player, int fire)
{
	XtkMessage batch;
	XtkCommand c;
	uint32_t tick = (uint32_t) (frame + 1);
	uint32_t id = g_cmd_id;

	g_cmd_id += 2;

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
}

/* Service the server and pump the present clients until `deadline` (ns). */
static void
pump_until(uint64_t deadline_ns)
{
	for (;;) {
		uint64_t t = mono_ns();

		(void) server_session_service(t / 1000000ULL);
		if (g_workdir[0] != '\0') {
			/* clients are pumped by callers with their own peers */
		}
		if (t >= deadline_ns)
			break;
		usleep(500);
	}
}

/* Pump present peers and service the server until both are in `want` state. */
static int
wait_states(NetPeer *A, NetPeer *B, int want_a, int want_b, uint64_t timeout_ns)
{
	uint64_t dl = mono_ns() + timeout_ns;

	while (mono_ns() < dl) {
		uint64_t t = mono_ns();

		(void) server_session_service(t / 1000000ULL);
		if (A->present)
			(void) session_client_pump(A->sc, t, soak_callback, A);
		if (B->present)
			(void) session_client_pump(B->sc, t, soak_callback, B);
		if ((!A->present || session_client_state(A->sc) == want_a) &&
		    (!B->present || session_client_state(B->sc) == want_b))
			return 0;
		usleep(500);
	}
	return -1;
}

/* Connect both humans, configure maze/Vanguard/`drones` and start one epoch. */
static int
match_start(Transport *host, NetPeer *A, NetPeer *B, int drones)
{
	uint64_t deadline = mono_ns() + 20000000000ULL;
	int i;

	A->present = B->present = 1;
	for (i = 0; i < 20000 && mono_ns() < deadline &&
	     session_client_state(A->sc) != XTK_SC_LOBBY; i++) {
		uint64_t now = mono_ns();

		(void) server_session_service(now / 1000000ULL);
		(void) session_client_pump(A->sc, now, soak_callback, A);
		usleep(1000);
	}
	(void) session_client_connect(B->sc, "127.0.0.1", g_server_port, B->name);
	for (i = 0; i < 20000 && mono_ns() < deadline &&
	     (session_client_state(A->sc) != XTK_SC_LOBBY ||
	      session_client_state(B->sc) != XTK_SC_LOBBY); i++) {
		uint64_t now = mono_ns();

		(void) server_session_service(now / 1000000ULL);
		(void) session_client_pump(A->sc, now, soak_callback, A);
		(void) session_client_pump(B->sc, now, soak_callback, B);
		usleep(1000);
	}
	if (session_client_state(A->sc) != XTK_SC_LOBBY ||
	    session_client_state(B->sc) != XTK_SC_LOBBY) {
		fprintf(stderr, "soak_fixture: lobby timeout A=%d B=%d\n",
			(int) session_client_state(A->sc),
			(int) session_client_state(B->sc));
		return -1;
	}

	(void) session_client_send_config(A->sc, (uint16_t) g_maze_id,
					  XTK_CONTENT_ID_VANGUARD,
					  (uint8_t) drones);
	for (i = 0; i < 20000 && mono_ns() < deadline &&
	     session_client_config_revision(A->sc) == 0; i++) {
		uint64_t now = mono_ns();

		(void) server_session_service(now / 1000000ULL);
		(void) session_client_pump(A->sc, now, soak_callback, A);
		(void) session_client_pump(B->sc, now, soak_callback, B);
		usleep(1000);
	}
	g_lobby_all_ready = 0;
	(void) session_client_send_ready(A->sc, 1);
	(void) session_client_send_ready(B->sc, 1);
	server_seed_random(114336);
	{
		uint64_t phase_deadline = mono_ns() + 15000000000ULL;
		uint64_t next_start = 0;
		int start_attempts = 0;

		for (i = 0; i < 400000 && mono_ns() < phase_deadline; i++) {
			ServerSession *s;
			uint64_t now = mono_ns();

			(void) server_session_service(now / 1000000ULL);
			(void) session_client_pump(A->sc, now, soak_callback, A);
			(void) session_client_pump(B->sc, now, soak_callback, B);
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

		fprintf(stderr, "soak_fixture: start timeout A=%d B=%d srv=%d\n",
			(int) session_client_state(A->sc),
			(int) session_client_state(B->sc),
			s != NULL ? (int) server_session_state(s) : -1);
	}
	return -1;
}

/* Fresh-client rejoin after a departure (connection reuse). */
static int
rejoin(NetPeer *dp, NetPeer *other)
{
	uint64_t dl;

	dp->bound_slot = -1;
	session_client_reconnect(dp->sc);
	if (session_client_connect(dp->sc, "127.0.0.1", g_server_port,
				   dp->name) != 0)
		return -1;
	dl = mono_ns() + 10000000000ULL;
	while (mono_ns() < dl &&
	       session_client_state(dp->sc) != XTK_SC_LOBBY) {
		uint64_t t = mono_ns();

		(void) server_session_service(t / 1000000ULL);
		(void) session_client_pump(dp->sc, t, soak_callback, dp);
		if (other->present)
			(void) session_client_pump(other->sc, t, soak_callback,
						   other);
		usleep(500);
	}
	return session_client_state(dp->sc) == XTK_SC_LOBBY ? 0 : -1;
}

/* ---- progress / records ---------------------------------------------- */
static void
progress_line(RunStats *rs, int status)
{
	if (g_progress == NULL)
		return;
	fprintf(g_progress,
		"wall_s=%.1f ticks=%ld resets=%d status=%d late=%ld rss_kb=%lu "
		"rss_hwm_kb=%lu fds=%lu threads=%lu intent_q=%d events=%d "
		"veh=%d bul=%d exp=%d\n",
		(double) (mono_ns() - g_run_t0) / 1e9, rs->ticks_total,
		rs->resets_done, status, rs->late_ticks, rss_kb(), rss_hwm_kb(),
		fd_count(), thread_count(), intent_queued(),
		server_events_count(), num_veh, bset->number, eset->number);
	fflush(g_progress);
}

static void
record_reset(RunStats *rs, int idx, int kind, unsigned long epoch_before,
	     unsigned long epoch_after, const DroneStats *ds, NetPeer *A,
	     NetPeer *B)
{
	if (g_resets != NULL) {
		fprintf(g_resets,
			"reset=%d tick=%ld epoch_before=%lu epoch_after=%lu "
			"kind=%s wall_s=%.1f rss_kb=%lu rss_hwm_kb=%lu fds=%lu "
			"threads=%lu intent_q=%d events=%d veh=%d bul=%d exp=%d "
			"reliable_max=%lu outgoing_max=%lu "
			"drone_move=%.2f heading_change=%.2f turret_change=%.2f "
			"bullets_owned_max=%lu fire_samples=%lu "
			"peerA_snap=%lu peerB_snap=%lu "
			"peerA_bad=%lu peerB_bad=%lu peerA_genreg=%lu peerB_genreg=%lu "
			"peerA_dupdeath=%lu peerB_dupdeath=%lu "
			"peerA_results=%lu peerB_results=%lu "
			"peerA_lobby=%lu peerB_lobby=%lu "
			"peerA_matchend=%lu peerB_matchend=%lu\n",
			idx, rs->ticks_total, epoch_before, epoch_after,
			kind == 1 ? "win" : "abort",
			(double) (mono_ns() - g_run_t0) / 1e9, rss_kb(),
			rss_hwm_kb(), fd_count(), thread_count(), intent_queued(),
			server_events_count(), num_veh, bset->number, eset->number,
			rs->max_reliable, rs->max_outgoing, ds->move,
			ds->heading_change, ds->turret_change,
			ds->bullets_owned_max, ds->fire_samples,
			A->snapshots, B->snapshots, A->bad_slot, B->bad_slot,
			A->gen_regression, B->gen_regression, A->dup_death,
			B->dup_death, A->results_msgs, B->results_msgs,
			A->lobby_msgs, B->lobby_msgs, A->events_match_end,
			B->events_match_end);
		fflush(g_resets);
	}
}

static void
record_cycle(int n, int dep_idx, unsigned long admin_before,
	     unsigned long admin_after, int transfer_ok, int rejoin_ok,
	     double removal_ms)
{
	if (g_cycles != NULL) {
		fprintf(g_cycles,
			"cycle=%d departed=%c admin_before=%lu admin_after=%lu "
			"transfer_ok=%d rejoin_ok=%d removal_ms=%.1f wall_s=%.1f\n",
			n, dep_idx == 0 ? 'A' : 'B', admin_before, admin_after,
			transfer_ok, rejoin_ok, removal_ms,
			(double) (mono_ns() - g_run_t0) / 1e9);
		fflush(g_cycles);
	}
}

/* ---- drone evidence sampling ------------------------------------------ */
static void
sample_drones(DroneStats *ds)
{
	int i;

	for (i = 0; i < num_combatants && i < MAX_VEHICLES; i++) {
		Combatant *c = &combatant[i];
		Vehicle *v;
		int slot;

		if (c->num_programs <= 0)		/* human */
			continue;
		v = c->vehicle;
		if (v == NULL || v->loc == NULL || v->number >= MAX_VEHICLES)
			continue;
		slot = v->number;
		ds->bot_samples++;
		if (ds->prev_seen[slot]) {
			double dx = v->loc->x - ds->prev_x[slot];
			double dy = v->loc->y - ds->prev_y[slot];
			double dh = v->vector.heading - ds->prev_head[slot];
			double dtu = v->turret[0].angle - ds->prev_turr[slot];

			if (dx < 0) dx = -dx;
			if (dy < 0) dy = -dy;
			if (dh < 0) dh = -dh;
			if (dtu < 0) dtu = -dtu;
			ds->move += dx + dy;
			ds->heading_change += dh;
			ds->turret_change += dtu;
		}
		ds->prev_seen[slot] = 1;
		ds->prev_x[slot] = v->loc->x;
		ds->prev_y[slot] = v->loc->y;
		ds->prev_head[slot] = v->vector.heading;
		ds->prev_turr[slot] = v->turret[0].angle;
	}
	{
		unsigned long owned = 0;
		int b;

		for (b = 0; b < bset->number && b < MAX_BULLETS; b++) {
			Bullet *bl = bset->list[b];
			int j;

			if (bl == NULL || bl->owner == NULL)
				continue;
			for (j = 0; j < num_combatants && j < MAX_VEHICLES; j++) {
				if (combatant[j].num_programs > 0 &&
				    combatant[j].vehicle == bl->owner) {
					owned++;
					break;
				}
			}
		}
		if (owned > ds->bullets_owned_max)
			ds->bullets_owned_max = owned;
		if (owned > 0)
			ds->fire_samples++;
	}
}

/* ---- one epoch -------------------------------------------------------- */
/*
 * Runs one paced epoch of at most `epoch_ticks` ticks.  Returns the engine
 * status; *natural_win is set when the match ended by ordinary rules.
 */
static int
run_epoch(Transport *host, NetPeer *A, NetPeer *B, int epoch_ticks,
	  RunStats *rs, DroneStats *ds, uint64_t *next_progress_ns,
	  int reset_idx, int rejoin_at, int *departed_idx,
	  unsigned long *admin_before, int *transfer_ok, double *removal_ms)
{
	ServerSession *s = server_session_current();
	uint64_t epoch_t0;
	int et, status = GAME_RUNNING;
	int departed = 0;

	server_loop_configure(15);
	server_loop_epoch_begin();
	epoch_t0 = mono_ns();

	for (et = 0; et < epoch_ticks; et++) {
		uint64_t now = mono_ns();
		long long a, b, svc, stp;
		int fire = ((et % 30) == 0);

		/* Mid-epoch admin departure (once) for a rejoin cycle. */
		if (rejoin_at >= 0 && !departed && et == epoch_ticks / 2) {
			NetPeer *dp, *sp;
			int dep_is_A;

			/* Depart whichever client is currently the admin. */
			dep_is_A = (session_client_admin_index(A->sc) ==
				    session_client_self_index(A->sc));
			dp = dep_is_A ? A : B;
			sp = dep_is_A ? B : A;
			*admin_before = session_client_admin_index(dp->sc);
			*departed_idx = dep_is_A ? 0 : 1;
			fprintf(stderr, "soak: DEPART client %c at epoch-tick %d "
				"(admin %lu)\n", dep_is_A ? 'A' : 'B', et,
				*admin_before);
			(void) session_client_send_quit(dp->sc);
			{
				uint64_t dl = mono_ns() + 5000000000ULL;
				uint64_t dep_t0 = mono_ns();
				int ok = 0;

				while (mono_ns() < dl) {
					uint64_t t = mono_ns();

					(void) server_session_service(t / 1000000ULL);
					(void) session_client_pump(A->sc, t,
						soak_callback, A);
					(void) session_client_pump(B->sc, t,
						soak_callback, B);
					if (sp->present &&
					    session_client_admin_index(sp->sc) ==
					    session_client_self_index(sp->sc) &&
					    server_session_count() == 1) {
						ok = 1;
						break;
					}
					usleep(500);
				}
				*transfer_ok = ok ? 1 : 0;
				*removal_ms = (double) (mono_ns() - dep_t0) / 1e6;
				dp->present = 0;
			}
			departed = 1;
			fprintf(stderr, "soak: transfer_ok=%d remaining_admin="
				"%d\n", *transfer_ok,
				(int) session_client_admin_index(sp->sc));
		}

		if (A->present)
			net_render(A, now);
		if (B->present)
			net_render(B, now);
		if (A->present)
			send_tick_input(A, 0, fire);
		if (B->present)
			send_tick_input(B, 1, fire);

		a = (long long) mono_ns();
		(void) server_session_service(mono_ns() / 1000000ULL);
		b = (long long) mono_ns();
		svc = b - a;

		{
			long long deadline = (long long) epoch_t0 +
				(long long) (et + 1) * 1000000000LL / 15LL;

			if (b > deadline)
				rs->late_ticks++;
		}
		server_loop_pace(et + 1);
		{
			long long pdeadline = (long long) epoch_t0 +
				(long long) (et + 1) * 1000000000LL / 15LL;

			for (;;) {
				uint64_t t = mono_ns();

				if (A->present)
					(void) session_client_pump(A->sc, t,
						soak_callback, A);
				if (B->present)
					(void) session_client_pump(B->sc, t,
						soak_callback, B);
				if ((long long) t >= pdeadline)
					break;
				{
					struct timespec ts;

					ts.tv_sec = 0;
					ts.tv_nsec = 1000000;
					(void) nanosleep(&ts, NULL);
				}
			}
		}
		a = (long long) mono_ns();
		(void) server_session_service(mono_ns() / 1000000ULL);
		b = (long long) mono_ns();
		svc += b - a;

		a = (long long) mono_ns();
		s = server_session_current();
		status = server_session_step(s, mono_ns() / 1000000ULL);
		b = (long long) mono_ns();
		stp = b - a;

		if (A->present)
			(void) session_client_pump(A->sc, mono_ns(), soak_callback, A);
		if (B->present)
			(void) session_client_pump(B->sc, mono_ns(), soak_callback, B);

		rs->ticks_total++;
		if ((et % 15) == 0) {
			sample_drones(ds);
			{
				int q = intent_queued();

				if (q > rs->max_intent_queue)
					rs->max_intent_queue = q;
			}
			if (num_veh > rs->max_vehicles)
				rs->max_vehicles = num_veh;
			if (bset->number > rs->max_bullets)
				rs->max_bullets = bset->number;
			if (eset->number > rs->max_explosions)
				rs->max_explosions = eset->number;
		}
		if (mono_ns() >= *next_progress_ns) {
			progress_line(rs, status);
			*next_progress_ns = mono_ns() + 15000000000ULL;
		}
		if (status != GAME_RUNNING) {
			fprintf(stderr, "soak: epoch end et=%d status=%d count=%d "
				"A=%d B=%d frame=%d\n", et, status,
				server_session_count(),
				A->present ? (int) session_client_state(A->sc) : -9,
				B->present ? (int) session_client_state(B->sc) : -9,
				frame);
			break;
		}
	}
	(void) host;
	return status;
}

/* ---- reset + restart transaction -------------------------------------- */
/*
 * Returns 0 and sets epoch_after when the new epoch is running.  The caller
 * reconnects a departed peer (if any) before calling this with `dp_to_join`
 * non-NULL.
 */
static int
reset_and_restart(Transport *host, NetPeer *A, NetPeer *B, int dp_to_join,
		  unsigned long *epoch_before, unsigned long *epoch_after)
{
	NetPeer *admin = NULL, *other = NULL, *dp = NULL;
	uint64_t dl;
	int attempt;

	(void) host;
	/* The admin is whichever present client the server names admin. */
	if (A->present && session_client_admin_index(A->sc) ==
	    session_client_self_index(A->sc))
		admin = A;
	else if (B->present && session_client_admin_index(B->sc) ==
		 session_client_self_index(B->sc))
		admin = B;
	else if (A->present)
		admin = A;
	else
		admin = B;
	other = (admin == A) ? B : A;

	*epoch_before = session_client_epoch(admin->sc);
	(void) session_client_send_reset(admin->sc);
	dl = mono_ns() + 10000000000ULL;
	while (mono_ns() < dl && admin->present &&
	       session_client_state(admin->sc) != XTK_SC_LOBBY)
		wait_states(A, B, XTK_SC_LOBBY, XTK_SC_LOBBY, 1000000ULL);
	if (session_client_state(admin->sc) != XTK_SC_LOBBY)
		fprintf(stderr, "soak_fixture: reset did not reach lobby "
			"(admin=%d)\n", (int) session_client_state(admin->sc));

	/* Rejoin the departed peer in the fresh lobby (connection reuse). */
	dp = (dp_to_join == 0) ? A : B;
	if (dp_to_join >= 0) {
		if (rejoin(dp, (dp == A) ? B : A) != 0) {
			fprintf(stderr, "soak_fixture: rejoin failed\n");
			return -1;
		}
		dp->present = 1;
	}
	g_lobby_all_ready = 0;
	if (A->present)
		(void) session_client_send_ready(A->sc, 1);
	if (B->present)
		(void) session_client_send_ready(B->sc, 1);
	server_seed_random(114336);
	dl = mono_ns() + 15000000000ULL;
	for (attempt = 0; attempt < 4; attempt++) {
		uint64_t sdl = mono_ns() + 2000000000ULL;

		while (mono_ns() < sdl) {
			ServerSession *s;
			uint64_t t = mono_ns();

			(void) server_session_service(t / 1000000ULL);
			if (A->present)
				(void) session_client_pump(A->sc, t, soak_callback, A);
			if (B->present)
				(void) session_client_pump(B->sc, t, soak_callback, B);
			s = server_session_current();
			if (s != NULL &&
			    server_session_state(s) == SERVER_SESSION_RUNNING)
				goto running;
			if (g_lobby_all_ready)
				(void) session_client_send_start(admin->sc);
			usleep(500);
		}
		(void) dl;
	}
running:
	{
		ServerSession *cs = server_session_current();

		fprintf(stderr, "soak: restart srv=%d A=%d B=%d epA=%u epB=%u "
			"lobA=%d lobB=%d all_ready=%d admin=%c\n",
			cs != NULL ? (int) server_session_state(cs) : -9,
			A->present ? (int) session_client_state(A->sc) : -9,
			B->present ? (int) session_client_state(B->sc) : -9,
			A->present ? (unsigned) session_client_epoch(A->sc) : 0,
			B->present ? (unsigned) session_client_epoch(B->sc) : 0,
			A->present ? session_client_have_lobby_state(A->sc) : -1,
			B->present ? session_client_have_lobby_state(B->sc) : -1,
			g_lobby_all_ready, admin == A ? 'A' : 'B');
	}
	/* Settle both clients on the new epoch before reading it. */
	dl = mono_ns() + 5000000000ULL;
	while (mono_ns() < dl &&
	       ((A->present && session_client_state(A->sc) != XTK_SC_RUNNING) ||
		(B->present && session_client_state(B->sc) != XTK_SC_RUNNING))) {
		uint64_t t = mono_ns();

		(void) server_session_service(t / 1000000ULL);
		if (A->present)
			(void) session_client_pump(A->sc, t, soak_callback, A);
		if (B->present)
			(void) session_client_pump(B->sc, t, soak_callback, B);
		usleep(500);
	}
	(void) other;
	*epoch_after = session_client_epoch(admin->sc);
	return 0;
}

/* ---- summary ---------------------------------------------------------- */
static void
write_summary(const char *out, const char *maze, NetPeer *A, NetPeer *B,
	      RunStats *rs, int resets, int epoch_ticks, int cycles_done)
{
	FILE *sum;
	char path[1200];
	int i;

	snprintf(path, sizeof(path), "%s.summary", out);
	sum = fopen(path, "w");
	if (sum == NULL)
		return;
	fprintf(sum, "mode=soak\n");
	fprintf(sum, "maze=%s\n", maze);
	fprintf(sum, "drones=2\n");
	fprintf(sum, "resets_target=%d resets_done=%d epoch_ticks=%d\n",
		resets, rs->resets_done, epoch_ticks);
	fprintf(sum, "ticks_total=%ld wall_s=%.1f\n", rs->ticks_total,
		(double) (mono_ns() - g_run_t0) / 1e9);
	fprintf(sum, "late_ticks=%ld\n", rs->late_ticks);
	fprintf(sum, "max_reliable_bytes=%lu max_outgoing_bytes=%lu\n",
		rs->max_reliable, rs->max_outgoing);
	fprintf(sum, "max_intent_queue=%d\n", rs->max_intent_queue);
	fprintf(sum, "max_vehicles=%d max_bullets=%d max_explosions=%d\n",
		rs->max_vehicles, rs->max_bullets, rs->max_explosions);
	fprintf(sum, "rss_warm_kb=%lu rss_max_kb=%lu rss_min_after_warm_kb=%lu "
		"fds_max=%lu threads_max=%lu\n", rs->rss_warm_kb, rs->rss_max_kb,
		rs->rss_min_after_warm_kb, rs->fds_max, rs->threads_max);
	fprintf(sum, "rejoin_cycles=%d\n", cycles_done);
	for (i = 0; i < 2; i++) {
		NetPeer *p = (i == 0) ? A : B;
		const char *c = (i == 0) ? "A" : "B";

		fprintf(sum, "peer%s_snapshots=%lu peer%s_bad_slot=%lu "
			"peer%s_gen_regression=%lu peer%s_dup_death=%lu "
			"peer%s_results=%lu peer%s_lobby=%lu peer%s_welcome=%lu "
			"peer%s_epoch_begin=%lu peer%s_events=%lu peer%s_death=%lu "
			"peer%s_respawn=%lu peer%s_score=%lu peer%s_match_end=%lu "
			"peer%s_max_snap_age_ticks=%u\n",
			c, p->snapshots, c, p->bad_slot, c, p->gen_regression,
			c, p->dup_death, c, p->results_msgs, c, p->lobby_msgs,
			c, p->welcome_msgs, c, p->epoch_msgs, c, p->events_total,
			c, p->events_death, c, p->events_respawn, c, p->events_score,
			c, p->events_match_end, c, p->max_snap_age_ticks);
	}
	fclose(sum);
}

/* ---- arg plumbing ----------------------------------------------------- */
typedef struct {
	int epoch_ticks;
	int rejoin_at[32];
	int n_rejoin;
} SoakCfg;

static int
is_rejoin_reset(SoakCfg *cfg, int idx)
{
	int i;

	for (i = 0; i < cfg->n_rejoin; i++)
		if (cfg->rejoin_at[i] == idx)
			return 1;
	return 0;
}

static int
run_soak(const char *assets, const char *maze, int drones, SoakCfg *cfg,
	 int resets, const char *out, const char *label)
{
	Transport *host;
	NetPeer A, B;
	RunStats rs;
	DroneStats ds;
	uint64_t next_progress_ns;
	long r;

	memset(&A, 0, sizeof(A));
	memset(&B, 0, sizeof(B));
	memset(&rs, 0, sizeof(rs));
	A.fps = B.fps = 15;
	A.bound_slot = B.bound_slot = -1;
	A.name = "playerA";
	B.name = "playerB";

	(void) setenv("XTANK_TIMING", "1", 1);
	server_timing_enable(1);

	server_session_init();
	server_session_set_capacity(XTK_MAX_HUMANS);
	host = transport_create(0, SERVER_SESSION_MAX_PEERS);
	if (host == NULL) {
		fprintf(stderr, "soak_fixture: transport_create failed\n");
		return -1;
	}
	server_session_set_transport(host);
	g_server_port = transport_port(host);

	A.sc = session_client_create();
	B.sc = session_client_create();
	if (A.sc == NULL || B.sc == NULL)
		return -1;
	if (session_client_connect(A.sc, "127.0.0.1", g_server_port, A.name) != 0)
		return -1;
	if (match_start(host, &A, &B, drones) != 0) {
		fprintf(stderr, "soak_fixture: handshake/start failed\n");
		return -1;
	}
	{
		ServerSession *sa = server_session_peer_at(0);
		ServerSession *sb = server_session_peer_at(1);

		if (sa != NULL)
			A.bound_slot = server_session_bound_slot(sa);
		if (sb != NULL)
			B.bound_slot = server_session_bound_slot(sb);
	}

	command_options.NoDelay = FALSE;
	g_run_t0 = mono_ns();
	rs.pace_t0 = g_run_t0;
	next_progress_ns = g_run_t0 + 15000000000ULL;

	progress_line(&rs, GAME_RUNNING);
	for (r = 0; r < resets; r++) {
		int status, natural_win = 0;
		int dep_idx = -1, transfer_ok = -1, rejoin_ok = -1;
		unsigned long admin_before = 0, epoch_before = 0, epoch_after = 0;
		double removal_ms = -1.0;
		int do_rejoin = is_rejoin_reset(cfg, (int) r);

		memset(&ds, 0, sizeof(ds));
		status = run_epoch(host, &A, &B, cfg->epoch_ticks, &rs, &ds,
				   &next_progress_ns, (int) r,
				   do_rejoin ? 1 : -1, &dep_idx,
				   &admin_before, &transfer_ok, &removal_ms);
		if (status != GAME_RUNNING)
			natural_win = 1;

		if (reset_and_restart(host, &A, &B, dep_idx, &epoch_before,
				      &epoch_after) != 0)
			return -1;
		rs.resets_done++;

		/* Refresh bound slots after the new binding. */
		{
			ServerSession *sa = server_session_peer_at(0);
			ServerSession *sb = server_session_peer_at(1);

			A.bound_slot = -1;
			B.bound_slot = -1;
			if (sa != NULL)
				A.bound_slot = server_session_bound_slot(sa);
			if (sb != NULL)
				B.bound_slot = server_session_bound_slot(sb);
		}
		if (do_rejoin) {
			rejoin_ok = (dep_idx >= 0) ? 1 : 0;
			record_cycle((int) r, dep_idx, admin_before,
				     (unsigned long) session_client_admin_index(
					(dep_idx == 0 ? &B : &A)->sc),
				     transfer_ok, rejoin_ok, removal_ms);
		}

		/* Warm-up window: ignore the first two resets for the RSS floor. */
		if (rs.resets_done == 2)
			rs.rss_warm_kb = rss_kb();
		if (rs.resets_done >= 2) {
			unsigned long rr = rss_kb();

			if (rr > rs.rss_max_kb)
				rs.rss_max_kb = rr;
			if (rs.rss_min_after_warm_kb == 0 ||
			    rr < rs.rss_min_after_warm_kb)
				rs.rss_min_after_warm_kb = rr;
		}
		{
			unsigned long f = fd_count(), t = thread_count();

			if (f > rs.fds_max)
				rs.fds_max = f;
			if (t > rs.threads_max)
				rs.threads_max = t;
		}

		record_reset(&rs, (int) r, natural_win, epoch_before,
			     epoch_after, &ds, &A, &B);
		progress_line(&rs, GAME_RUNNING);
		fprintf(stderr, "soak: reset %ld/%d done epoch %lu->%lu %s "
			"wall=%.1fs\n", r + 1, resets, epoch_before, epoch_after,
			natural_win ? "WIN" : "abort",
			(double) (mono_ns() - g_run_t0) / 1e9);
	}

	write_summary(out, maze, &A, &B, &rs, resets, cfg->epoch_ticks,
		      cfg->n_rejoin);
	{
		char path[1200];

		snprintf(path, sizeof(path), "%s.timing", out);
		server_timing_report(path);
	}
	printf("SOAK FIXTURE PASS (resets=%d ticks=%ld wall=%.1fs late=%ld "
	       "rss_warm=%lu rss_max=%lu)\n", rs.resets_done, rs.ticks_total,
	       (double) (mono_ns() - g_run_t0) / 1e9, rs.late_ticks,
	       rs.rss_warm_kb, rs.rss_max_kb);

	session_client_destroy(A.sc);
	session_client_destroy(B.sc);
	transport_destroy(host);
	return 0;
}

int
main(int argc, char *argv[])
{
	const char *mode, *assets, *maze, *out, *label;
	SoakCfg cfg;
	int resets, i;
	char outbuf[1200];

	memset(&cfg, 0, sizeof(cfg));
	cfg.epoch_ticks = 2700;
	cfg.n_rejoin = 0;

	if (argc < 2) {
		fprintf(stderr,
		  "usage: %s run <assets> <maze> <settings> <resets> "
		  "<workdir> <label> [--epoch-ticks N] [--rejoin-at i,j,..]\n",
		  argv[0]);
		return 2;
	}
	mode = argv[1];
	if (strcmp(mode, "run") != 0) {
		fprintf(stderr, "soak_fixture: unknown mode %s\n", mode);
		return 2;
	}
	if (argc < 8) {
		fprintf(stderr, "usage: %s run <assets> <maze> <settings> "
			"<resets> <workdir> <label> [--epoch-ticks N] "
			"[--rejoin-at i,j,..]\n", argv[0]);
		return 2;
	}
	assets = argv[2];
	maze = argv[3];
	g_settings_path = argv[4];
	resets = atoi(argv[5]);
	snprintf(g_workdir, sizeof(g_workdir), "%s", argv[6]);
	label = argv[7];
	for (i = 8; i + 1 < argc; i += 2) {
		if (strcmp(argv[i], "--epoch-ticks") == 0)
			cfg.epoch_ticks = atoi(argv[i + 1]);
		else if (strcmp(argv[i], "--rejoin-at") == 0) {
			char *tok, *save = NULL;
			char buf[256];

			snprintf(buf, sizeof(buf), "%s", argv[i + 1]);
			for (tok = strtok_r(buf, ",", &save); tok != NULL;
			     tok = strtok_r(NULL, ",", &save)) {
				if (cfg.n_rejoin < 32)
					cfg.rejoin_at[cfg.n_rejoin++] = atoi(tok);
			}
		} else if (strcmp(argv[i], "--verbose") == 0)
			g_verbose = 1;
	}
	g_verbose = g_verbose;	/* silence unused warning when disabled */

	out = (snprintf(outbuf, sizeof(outbuf), "%s/%s", g_workdir, label),
	       outbuf);
	if (strcmp(maze, "combat1") == 0)
		g_maze_id = XTK_CONTENT_ID_COMBAT1;
	else
		g_maze_id = XTK_CONTENT_ID_ARENA;
	if (server_bringup(assets, maze) != 0)
		return 2;
	{
		char p[1200];

		snprintf(p, sizeof(p), "%s/progress.txt", g_workdir);
		g_progress = fopen(p, "a");
		snprintf(p, sizeof(p), "%s/resets.txt", g_workdir);
		g_resets = fopen(p, "a");
		snprintf(p, sizeof(p), "%s/cycles.txt", g_workdir);
		g_cycles = fopen(p, "a");
	}
	if (run_soak(assets, maze, 2, &cfg, resets, out, label) != 0)
		return 1;
	return 0;
}
