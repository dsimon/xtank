/*-
 * Copyright (c) 1988 Terry Donahue
 * All rights reserved.
 *
 * P2.15 network-path parity fixture.
 *
 * Runs the same match and the same tick-indexed command log three ways and
 * compares the authoritative per-tick state byte-for-byte:
 *
 *   leg X       the frozen pinned X reference (built and run by parity.sh)
 *               driven by the legacy controls through Tests/phase1's
 *               reference_input.c trace adapter;
 *   leg direct  the shipped terminal-free server core + P2.8 session, with the
 *               command log injected straight into the bounded intent queue
 *               (no transport), driven by this fixture;
 *   leg network the same core/session, but with the command log encoded by the
 *               P2.3 codec, carried over the pinned P2.7 ENet transport to a
 *               real P2.14 client session, admitted by P2.7 and stepped by
 *               P2.8; the client model (Client/model.c) consumes the authorized
 *               VIEW_SNAPSHOT stream the server exports.
 *
 * The fixture emits the F2/V/B/E subset of the frozen v2 STATE_DUMP format that
 * Tests/phase1/animate.c writes, so the direct and network dumps can be cmp'd
 * against the frozen-X and direct-intent oracle dumps by parity.sh.
 *
 * It is a test harness: it links the simulation so it can *host* the
 * authoritative server.  The shipped client and server never do this.  It
 * changes no gameplay rule; the network path must not change an outcome.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "xtank.h"
#include "bullet.h"
#include "clfkr.h"
#include "object.h"
#include "thread.h"
#include "vehicle.h"
#include "server.h"
#include "server_view.h"
#include "server_events.h"
#include "server_session.h"
#include "intent.h"
#include "proto.h"
#include "gr.h"

#include "transport.h"

#include "session_client.h"
#include "model.h"

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

/* ---- sandbox sendmsg shim (test-harness only; see P2.7/P2.14) --------- */
ssize_t __real_sendmsg(int fd, const struct msghdr *msg, int flags);

ssize_t
__wrap_sendmsg(int fd, const struct msghdr *msg, int flags)
{
	if (msg != NULL && msg->msg_name != NULL) {
		if (connect(fd, (struct sockaddr *) msg->msg_name,
			    msg->msg_namelen) == 0) {
			struct msghdr m = *msg;

			m.msg_name = NULL;
			m.msg_namelen = 0;
			return __real_sendmsg(fd, &m, flags);
		}
	}
	return __real_sendmsg(fd, msg, flags);
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

/* ---- scenario command log --------------------------------------------- */

#define MAX_CMDS 4096

typedef struct {
	int tick;
	char op[24];
	double a, b;
	char b_str[16];
	int has_a, has_b;
} Cmd;

static Cmd g_cmds[MAX_CMDS];
static int g_ncmds;

static void
load_trace(const char *path)
{
	FILE *fp = fopen(path, "r");
	char line[256];

	if (fp == NULL) {
		fprintf(stderr, "parity_fixture: cannot open trace %s\n", path);
		exit(2);
	}
	while (fgets(line, sizeof(line), fp) != NULL && g_ncmds < MAX_CMDS) {
		char *p = line;
		char *tok, *save;
		Cmd c;

		while (*p == ' ' || *p == '\t')
			p++;
		if (*p == '#' || *p == '\n' || *p == '\0')
			continue;
		memset(&c, 0, sizeof(c));
		tok = strtok_r(p, " \t\n", &save);
		if (tok == NULL)
			continue;
		c.tick = atoi(tok);
		tok = strtok_r(NULL, " \t\n", &save);	/* player (must be 0) */
		if (tok == NULL || atoi(tok) != 0)
			continue;
		tok = strtok_r(NULL, " \t\n", &save);
		if (tok == NULL)
			continue;
		strncpy(c.op, tok, sizeof(c.op) - 1);
		tok = strtok_r(NULL, " \t\n", &save);
		if (tok != NULL) {
			c.has_a = 1;
			if (strcmp(c.op, "firehold") == 0)
				c.a = (strcmp(tok, "on") == 0) ? 1.0 : 0.0;
			else
				c.a = strtod(tok, NULL);
		}
		tok = strtok_r(NULL, " \t\n", &save);
		if (tok != NULL) {
			c.has_b = 1;
			c.b = strtod(tok, NULL);
			strncpy(c.b_str, tok, sizeof(c.b_str) - 1);
		}
		g_cmds[g_ncmds++] = c;
	}
	fclose(fp);
}

/* Map a trace op to the wire op/operands (release-authorized subset). */
static uint8_t
wire_op(const char *op, double *a, double *b, const char *bstr, uint8_t *flags)
{
	*flags = 0;
	if (strcmp(op, "wep") == 0) {
		*flags = XTK_FLAG_EDGE;
		*b = (strcmp(bstr, "on") == 0) ? 1.0 :
		     (strcmp(bstr, "off") == 0) ? 0.0 : 2.0;
		return XTK_OP_WEAPON;
	}
	if (strcmp(op, "drive") == 0) {
		*a = (double) (int) *a;
		*flags = XTK_FLAG_HELD;
		return XTK_OP_DRIVE_REL;
	}
	if (strcmp(op, "driveabs") == 0) {
		*a = (double) (int) *a;
		*flags = XTK_FLAG_HELD;
		return XTK_OP_DRIVE_ABS;
	}
	if (strcmp(op, "turn") == 0) {
		*flags = XTK_FLAG_EDGE;
		return XTK_OP_HEADING;
	}
	if (strcmp(op, "aim") == 0) {
		*flags = XTK_FLAG_EDGE;
		return XTK_OP_AIM_WORLD;
	}
	if (strcmp(op, "fire") == 0) {
		*flags = XTK_FLAG_EDGE;
		return XTK_OP_FIRE;
	}
	if (strcmp(op, "firehold") == 0) {
		*flags = XTK_FLAG_HELD;
		return XTK_OP_FIRE_HOLD;
	}
	return XTK_OP_NONE;
}

/* ---- v2 dump subset (F2/V/B/E) ---------------------------------------- */

static FILE *g_dump;

static void
dump_tick(unsigned int status)
{
	int i, w;

	if (g_dump == NULL)
		return;
	fprintf(g_dump,
		"F2 %d epoch 0 status %u alive %d dead %d nb %d ne %d nv %d\n",
		frame, status, num_veh_alive, num_veh_dead,
		bset->number, eset->number, num_veh);
	for (i = 0; i < num_veh; i++) {
		Vehicle *v = &actual_vehicles[i];
		Combatant *o = v->owner;

		fprintf(g_dump,
			"V %d team %d status %x grid %d %d box %d %d pos %.4f %.4f "
			"z %.4f heading %.6f oldheading %.6f desired %.6f drive %.4f "
			"speed %.4f xspeed %.4f yspeed %.4f rot %d oldrot %d fuel %.4f "
			"heat %d armor %d %d %d %d %d %d discs %d deathtimer %d "
			"nweapons %d nturrets %d score %d kills %d deaths %d\n",
			v->number, v->team, (unsigned) v->status,
			v->loc->grid_x, v->loc->grid_y,
			(int) v->loc->box_x, (int) v->loc->box_y,
			v->loc->x, v->loc->y, v->loc->z,
			v->vector.heading, v->vector.old_heading,
			v->vector.desired_heading, v->vector.drive,
			v->vector.speed, v->vector.xspeed, v->vector.yspeed,
			v->vector.rot, v->vector.old_rot, v->fuel, v->heat,
			v->armor.side[0], v->armor.side[1], v->armor.side[2],
			v->armor.side[3], v->armor.side[4], v->armor.side[5],
			v->num_discs, v->death_timer,
			v->num_weapons, v->num_turrets,
			o ? o->score : 0, o ? o->kills : 0, o ? o->deaths : 0);
	}
	for (i = 0; i < bset->number; i++) {
		Bullet *b = bset->list[i];
		int own = b->owner ? (int) b->owner->number : -1;

		fprintf(g_dump,
			"B %d owner %d type %d life %d thrower %d safety %d hurt %d "
			"state %d rot %d grid %d %d box %d %d pos %.4f %.4f z %.4f "
			"xspeed %.4f yspeed %.4f\n",
			i, own, (int) b->type, b->life, b->thrower, b->safety,
			b->hurt_owner, b->state, b->rot,
			b->loc->grid_x, b->loc->grid_y,
			(int) b->loc->box_x, (int) b->loc->box_y,
			b->loc->x, b->loc->y, b->loc->z, b->xspeed, b->yspeed);
	}
	for (i = 0; i < eset->number; i++) {
		Exp *e = eset->list[i];

		fprintf(g_dump, "E %d x %d y %d z %d life %d color %d\n",
			i, e->x, e->y, e->z, e->life, e->color);
	}
	(void) w;
	fflush(g_dump);
}

/* ---- bring-up ---------------------------------------------------------- */

static const char *g_settings_path;

static int
server_bringup(const char *assets)
{
	ServerConfig *cfg;
	int idx = -1;

	bset = &actual_bset;
	eset = &actual_eset;
	InitConfigStruct(&command_options);
	command_options.NoDelay = TRUE;
	command_options.AutoExit = TRUE;
	command_options.PrintScores = FALSE;
	strncpy(username, "p2.15", MAX_STRING - 1);
	username[MAX_STRING - 1] = '\0';

	if (content_set_root(assets) != 0) {
		fprintf(stderr, "parity_fixture: %s\n", content_last_error());
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
	/* Register the maze so the settings file's "Maze:" line resolves before
	 * the session rebuilds it; the shipped server does the same in P2.8. */
	if (make_mdesc((char *) "Arena", &idx) != DESC_LOADED) {
		fprintf(stderr, "parity_fixture: cannot load Arena\n");
		return -1;
	}
	if (g_settings_path != NULL &&
	    content_load_settings(g_settings_path) != 0) {
		fprintf(stderr, "parity_fixture: %s\n", content_last_error());
		return -1;
	}
	init_threader();
	init_msg_sys();
	init_box_names();
	geometry_order_objects();
	geometry_rotate_objects();
	return 0;
}

/* Build the lobby messages that reach a Running epoch, without a transport. */
static void
direct_lobby(ServerSession *s)
{
	XtkMessage m;

	memset(&m, 0, sizeof(m));
	m.env.message_type = XTK_MSG_HELLO;
	m.body.hello.proto_major = XTK_PROTOCOL_MAJOR;
	m.body.hello.proto_minor = XTK_PROTOCOL_MINOR;
	memcpy(m.body.hello.content_set_digest, xtk_content_set_digest,
	       XTK_SHA256_BYTES);
	strcpy(m.body.hello.name, "pilot");
	(void) server_session_message(s, &m, 1000);

	memset(&m, 0, sizeof(m));
	m.env.message_type = XTK_MSG_CONFIG_SELECT;
	m.body.config_select.maze_id = XTK_CONTENT_ID_ARENA;
	m.body.config_select.vehicle_id = XTK_CONTENT_ID_VANGUARD;
	(void) server_session_message(s, &m, 1000);

	memset(&m, 0, sizeof(m));
	m.env.message_type = XTK_MSG_READY;
	/* D3-B revision rule: readiness is expressed against the accepted
	 * config revision. */
	m.body.ready.ready = 1;
	m.body.ready.config_revision = server_session_config_revision(s);
	(void) server_session_message(s, &m, 1000);
}

/* Push this tick's trace commands straight into the bounded intent queue. */
static int
push_direct(int tick, unsigned int *seq)
{
	int i, n = 0;

	for (i = 0; i < g_ncmds; i++) {
		Intent in;
		Cmd *c = &g_cmds[i];

		if (c->tick != tick)
			continue;
		memset(&in, 0, sizeof(in));
		in.tick = tick;
		in.player = 0;
		in.sequence = (*seq)++;
		if (strcmp(c->op, "drive") == 0) {
			in.op = INTENT_DRIVE_REL;
			in.a = c->a;
		} else if (strcmp(c->op, "driveabs") == 0) {
			in.op = INTENT_DRIVE_ABS;
			in.a = c->a;
		} else if (strcmp(c->op, "turn") == 0) {
			in.op = INTENT_HEADING;
			in.a = c->a;
		} else if (strcmp(c->op, "aim") == 0) {
			in.op = INTENT_AIM_WORLD;
			in.a = c->a;
			in.b = c->b;
		} else if (strcmp(c->op, "fire") == 0) {
			in.op = INTENT_FIRE;
		} else if (strcmp(c->op, "firehold") == 0) {
			in.op = INTENT_FIRE_HOLD;
			in.a = c->a;
		} else if (strcmp(c->op, "wep") == 0) {
			in.op = INTENT_WEAPON;
			in.a = c->a;
			if (strcmp(c->b_str, "on") == 0)
				in.b = INTENT_WEAPON_ON;
			else if (strcmp(c->b_str, "off") == 0)
				in.b = INTENT_WEAPON_OFF;
			else
				in.b = INTENT_WEAPON_FIRE;
		} else {
			continue;
		}
		if (intent_push(&in) == INTENT_OK)
			n++;
	}
	return n;
}

/* ---- shared run loop --------------------------------------------------- */

#define P2_15_TICKS 800

static int
run_direct(const char *dump_path)
{
	ServerSession *s;
	unsigned int seq = 0;
	int t, status = GAME_RUNNING;

	g_dump = fopen(dump_path, "w");
	if (g_dump == NULL)
		return -1;

	server_session_init();
	s = server_session_accept(NULL);
	check("direct: session accepted (transport-free)", s != NULL);
	if (s == NULL)
		return -1;
	direct_lobby(s);
	check("direct: admin + config + ready",
	      server_session_can_start(s) == 1);
	if (server_session_begin(s, 1, 1000) != SERVER_SESSION_OK)
		return -1;
	check("direct: epoch 1 running",
	      server_session_state(s) == SERVER_SESSION_RUNNING);

	for (t = 1; t <= P2_15_TICKS; t++) {
		(void) push_direct(t, &seq);
		status = server_session_step(s, 1000);
		dump_tick((unsigned int) status);
		if (status != GAME_RUNNING)
			break;
	}

	/* Force a match result deterministically after the compared window so
	 * the RESULTS/win path is exercised identically on both legs. */
	if (server_session_state(s) == SERVER_SESSION_RUNNING) {
		combatant[0].score = settings.si.winning_score;
		for (t = 0; t < 200; t++) {
			status = server_session_step(s, 1000);
			dump_tick((unsigned int) status);
			if (status != GAME_RUNNING)
				break;
		}
	}
	fprintf(stderr, "direct: final status %d frame %d scores %d/%d\n",
		status, frame, combatant[0].score, combatant[1].score);
	fclose(g_dump);
	server_session_destroy(s);
	return 0;
}

/* ---- network leg ------------------------------------------------------- */

typedef struct {
	FILE *snap;
	int have_snap;
	uint32_t last_tick;
	uint64_t recv_ns;
	XtkClientModel *model;
} NetCtx;

static int
net_callback(void *user, const XtkMessage *m)
{
	NetCtx *ctx = (NetCtx *) user;

	if (m->env.message_type == XTK_MSG_VIEW_SNAPSHOT) {
		const XtkViewSnapshot *snap = &m->body.view_snapshot;
		int i;
		int own = snap->own_slot;

		(void) model_observe(ctx->model, m->env.epoch, snap,
				     ctx->recv_ns);
		ctx->have_snap = 1;
		ctx->last_tick = snap->server_tick;
		for (i = 0; i < snap->vehicle_count; i++) {
			if (snap->vehicle[i].slot == own) {
				fprintf(ctx->snap, "S %u %d %.4f %.4f %u\n",
					snap->server_tick,
					snap->vehicle[i].slot,
					snap->vehicle[i].x,
					snap->vehicle[i].y,
					snap->self.score);
			}
		}
	}
	return 0;
}

static int
run_network(const char *dump_path, const char *snap_path)
{
	Transport *host;
	uint16_t port;
	ServerSession *s = NULL;
	XtkSessionClient *sc;
	XtkClientModel *model;
	NetCtx ctx;
	uint64_t ns = 1000000000ULL;
	uint32_t pkt_seq = 0;
	int t, i, status = GAME_RUNNING;

	g_dump = fopen(dump_path, "w");
	if (g_dump == NULL)
		return -1;
	memset(&ctx, 0, sizeof(ctx));
	ctx.snap = fopen(snap_path, "w");
	model = (XtkClientModel *) calloc(1, sizeof(*model));
	model_init(model);
	ctx.model = model;

	server_session_init();
	host = transport_create(0, 1);
	check("network: listen transport created", host != NULL);
	if (host == NULL)
		return -1;
	port = transport_port(host);
	server_session_set_transport(host);

	sc = session_client_create();
	check("network: client created", sc != NULL);
	check("network: connect initiated",
	      session_client_connect(sc, "127.0.0.1", port, "pilot") == 0);

	for (i = 0; i < 4000 && session_client_state(sc) != XTK_SC_LOBBY; i++) {
		(void) server_session_service(1000);
		(void) session_client_pump(sc, ns, net_callback, &ctx);
		ns += 66666667ULL;
	}
	check("network: lobby reached",
	      session_client_state(sc) == XTK_SC_LOBBY);
	check("network: session id assigned", session_client_id(sc) != 0);
	check("network: client admin", session_client_is_admin(sc) == 1);

	(void) session_client_send_config(sc, XTK_CONTENT_ID_ARENA,
					  XTK_CONTENT_ID_VANGUARD, 0);
	/* Wait for the server's CONFIG_STATUS before expressing readiness
	 * against the resulting revision (P3.10 revision rule). */
	for (i = 0; i < 4000 && session_client_config_revision(sc) == 0; i++) {
		(void) server_session_service(1000);
		(void) session_client_pump(sc, ns, net_callback, &ctx);
		ns += 66666667ULL;
	}
	check("network: config revision known",
	      session_client_config_revision(sc) != 0);
	(void) session_client_send_ready(sc, 1);
	for (i = 0; i < 4000 && !(s != NULL && server_session_state(s) ==
				  SERVER_SESSION_RUNNING); i++) {
		(void) server_session_service(1000);
		(void) session_client_pump(sc, ns, net_callback, &ctx);
		ns += 66666667ULL;
		s = server_session_current();
	}
	check("network: match running", s != NULL &&
	      server_session_state(s) == SERVER_SESSION_RUNNING);
	check("network: epoch 1", session_client_epoch(sc) == 1);

	for (t = 1; t <= P2_15_TICKS; t++) {
		XtkMessage batch;
		int n = 0;

		memset(&batch, 0, sizeof(batch));
		batch.env.message_type = XTK_MSG_INPUT_BATCH;
		batch.env.epoch = session_client_epoch(sc);
		batch.body.input_batch.packet_sequence = ++pkt_seq;
		batch.body.input_batch.last_observed_server_tick =
			(uint32_t) frame;
		for (i = 0; i < g_ncmds; i++) {
			XtkCommand c;
			Cmd *cmd = &g_cmds[i];

			if (cmd->tick != t)
				continue;
			memset(&c, 0, sizeof(c));
			c.a = cmd->a;
			c.b = cmd->b;
			c.command_id = (uint32_t) (t * 64 + n + 1);
			c.tick_hint = (uint32_t) t;
			c.op = wire_op(cmd->op, &c.a, &c.b, cmd->b_str, &c.flags);
			batch.body.input_batch.command[n++] = c;
		}
		batch.body.input_batch.command_count = (uint16_t) n;
		if (n > 0)
			(void) session_client_send_input(sc, &batch);

		(void) server_session_service(1000);
		(void) session_client_pump(sc, ns, net_callback, &ctx);
		ns += 66666667ULL;

		s = server_session_current();
		if (s == NULL || server_session_state(s) !=
		    SERVER_SESSION_RUNNING)
			break;
		status = server_session_step(s, 1000);
		dump_tick((unsigned int) status);
		if (status != GAME_RUNNING)
			break;
	}

	if (s != NULL && server_session_state(s) == SERVER_SESSION_RUNNING) {
		combatant[0].score = settings.si.winning_score;
		for (t = 0; t < 200; t++) {
			status = server_session_step(s, 1000);
			dump_tick((unsigned int) status);
			if (status != GAME_RUNNING)
				break;
		}
	}
	/* Drain the last in-flight snapshots so the client model sees every
	 * authorized view the server exported for the compared window. */
	for (i = 0; i < 20; i++) {
		(void) server_session_service(1000);
		(void) session_client_pump(sc, ns, net_callback, &ctx);
		ns += 66666667ULL;
	}
	fprintf(stderr, "network: final status %d frame %d snapshots %u\n",
		status, frame,
		(unsigned) (s ? server_session_snapshot_seq(s) : 0));
	fclose(g_dump);
	if (ctx.snap != NULL)
		fclose(ctx.snap);
	free(model);
	session_client_destroy(sc);
	transport_destroy(host);
	return 0;
}

int
main(int argc, char *argv[])
{
	const char *mode, *assets, *trace, *dump, *snap = NULL;

	if (argc < 6) {
		fprintf(stderr,
			"usage: %s <direct|network> <assets> <settings> <trace> "
			"<dump> [snap]\n", argv[0]);
		return 2;
	}
	mode = argv[1];
	assets = argv[2];
	g_settings_path = argv[3];
	trace = argv[4];
	dump = argv[5];
	if (argc > 6)
		snap = argv[6];

	if (server_bringup(assets) != 0)
		return 2;
	load_trace(trace);

	if (strcmp(mode, "direct") == 0)
		(void) run_direct(dump);
	else if (strcmp(mode, "network") == 0)
		(void) run_network(dump, snap);
	else
		return 2;

	if (failures != 0) {
		printf("PARITY FIXTURE FAIL (%d checks, %d failures)\n",
		       checks, failures);
		return 1;
	}
	printf("PARITY FIXTURE PASS (%d checks, mode=%s)\n", checks, mode);
	return 0;
}
