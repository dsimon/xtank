/*-
 * Copyright (c) 1988 Terry Donahue
 * All rights reserved.
 *
 * P3.12 two-client integration and independent network parity fixture.
 *
 * Two bot-free humans, driven by the frozen release-authorized command trace
 * generated from the reference replay (Tests/phase3/fixtures/parity/*.trace),
 * are run two ways and the full frozen v2 STATE_DUMP record set is compared
 * byte-for-byte:
 *
 *   direct  the shipped terminal-free server core + P3.4 coordinator, with
 *           each human's commands injected straight into the bounded intent
 *           queue (no transport);
 *   network the same core/coordinator, with the commands encoded by the
 *           frozen v1.1 codec and carried over the pinned ENet transport to two
 *           real P3.10 client sessions, admitted by P3.6 and exported by P3.8.
 *
 * The emitted dump is the complete v2 format Tests/phase1/animate.c writes
 * (F2/V/VW/VT/VC/B/E + maze MX/MP/ML + radar RNH/RB/NRH/NRB), so parity.sh can
 * cmp it against the frozen X reference dump.  Both legs replay the *same*
 * applied release log, so the network path must not change an outcome.
 *
 * The fixture links the simulation only to *host* the authoritative server;
 * the shipped client and server never do.  No gameplay rule changes.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

/* ---- sandbox sendmsg shim (test-harness only; see P2.7/P3.9) --------- */
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
#define MAX_CMDS 32768

typedef struct {
	int tick;
	int player;
	char op[24];
	double a, b;
	char b_str[16];
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
		tok = strtok_r(NULL, " \t\n", &save);
		if (tok == NULL)
			continue;
		c.player = atoi(tok);
		tok = strtok_r(NULL, " \t\n", &save);
		if (tok == NULL)
			continue;
		strncpy(c.op, tok, sizeof(c.op) - 1);
		tok = strtok_r(NULL, " \t\n", &save);
		if (tok != NULL) {
			if (strcmp(c.op, "firehold") == 0)
				c.a = (strcmp(tok, "on") == 0) ? 1.0 : 0.0;
			else
				c.a = strtod(tok, NULL);
		}
		tok = strtok_r(NULL, " \t\n", &save);
		if (tok != NULL) {
			c.b = strtod(tok, NULL);
			strncpy(c.b_str, tok, sizeof(c.b_str) - 1);
		}
		g_cmds[g_ncmds++] = c;
	}
	fclose(fp);
}

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

/* Map a trace op to the bounded intent-queue op (direct leg). */
static IntentOp
intent_op(const char *op, double *a, double *b, const char *bstr)
{
	if (strcmp(op, "drive") == 0)
		return INTENT_DRIVE_REL;
	if (strcmp(op, "driveabs") == 0)
		return INTENT_DRIVE_ABS;
	if (strcmp(op, "turn") == 0)
		return INTENT_HEADING;
	if (strcmp(op, "aim") == 0)
		return INTENT_AIM_WORLD;
	if (strcmp(op, "fire") == 0)
		return INTENT_FIRE;
	if (strcmp(op, "firehold") == 0) {
		/* The trace stores on/off in *a; tolerate an explicit token too. */
		if (bstr != NULL && bstr[0] != '\0')
			*a = (strcmp(bstr, "on") == 0) ? 1.0 : 0.0;
		else
			*a = (*a != 0.0) ? 1.0 : 0.0;
		return INTENT_FIRE_HOLD;
	}
	if (strcmp(op, "wep") == 0) {
		*b = (strcmp(bstr, "on") == 0) ? INTENT_WEAPON_ON :
		     (strcmp(bstr, "off") == 0) ? INTENT_WEAPON_OFF :
		     INTENT_WEAPON_FIRE;
		return INTENT_WEAPON;
	}
	return INTENT_NONE;
}

/* ---- full frozen v2 dump (F2/V/VW/VT/VC/B/E + MX/MP/ML + radar) ------- */

typedef struct {
	int present, flags, type, team, strength;
} V2Box;

static V2Box v2_prev_maze[GRID_WIDTH][GRID_HEIGHT];
static V2Box v2_prev_mapper[MAX_VEHICLES][GRID_WIDTH][GRID_HEIGHT];
static int v2_first = 1;
static int v2_mapper_prev_valid[MAX_VEHICLES];
static FILE *g_dump;
static int g_epoch;

static void
v2_box_fields(int *present, int *flags, int *type, int *team, int *strength, Box *b)
{
	*present = (b->flags || b->type || b->team || b->strength);
	*flags = (int) b->flags;
	*type = (int) b->type;
	*team = (int) b->team;
	*strength = (int) b->strength;
}

static void
v2_reset_baseline(void)
{
	int i, x, y;

	v2_first = 1;
	for (i = 0; i < MAX_VEHICLES; i++)
		v2_mapper_prev_valid[i] = 0;
	for (x = 0; x < GRID_WIDTH; x++)
		for (y = 0; y < GRID_HEIGHT; y++)
			v2_prev_maze[x][y].present = 0;
}

/*
 * The shipped session finalizes a finished match (game_results ->
 * game_finalize_scores) inside server_session_step, after the tick snapshots
 * and events have been exported, and resets the per-match combatant score
 * then.  The frozen X reference dumps its authoritative end-of-tick state at
 * the same game_rules seam but before that post-match zeroing, so a dump read
 * after server_session_step would otherwise differ on the terminal tick only.
 * These are per-tick authoritative dumps, so recover the captured pre-reset
 * score (the results are copied while the combatant scores still exist) before
 * emitting the terminal tick.  Read-only: no simulation state or tick is
 * changed, and scores are re-zeroed when the epoch restarts.
 */
static void
restore_final_scores(void)
{
	XtkResults r;
	int i, k;

	if (server_events_results(&r) != SERVER_EVENTS_OK)
		return;
	for (i = 0; i < num_veh; i++) {
		Combatant *o = actual_vehicles[i].owner;

		if (o == NULL)
			continue;
		for (k = 0; k < r.vehicle_count; k++) {
			if (r.entry[k].slot == (uint8_t) o->number) {
				o->score = (int) r.entry[k].score;
				break;
			}
		}
	}
}

static void
dump_tick(unsigned int status)
{
	int i, j, w, x, y;

	if (g_dump == NULL)
		return;

	if (status != GAME_RUNNING)
		restore_final_scores();

	fprintf(g_dump,
		"F2 %d epoch %d status %u alive %d dead %d nb %d ne %d nv %d\n",
		frame, g_epoch, status, num_veh_alive, num_veh_dead,
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

		for (w = 0; w < v->num_weapons; w++)
			fprintf(g_dump,
				"VW %d %d type %d status %x ammo %d reload %d hits %d\n",
				v->number, w, (int) v->weapon[w].type,
				(unsigned) v->weapon[w].status, v->weapon[w].ammo,
				v->weapon[w].reload_counter, v->weapon[w].hits);

		for (w = 0; w < v->num_turrets; w++)
			fprintf(g_dump,
				"VT %d %d angle %.6f desired %.6f flag %d\n",
				v->number, w, v->turret[w].angle,
				v->turret[w].desired_angle,
				(int) v->turret[w].angle_flag);

		fprintf(g_dump,
			"VC %d timetocamo %d camod %d oldcamod %d rcs %.4f "
			"stealthy %.4f normal %.4f\n",
			v->number, v->time_to_camo, v->camod, v->old_camod,
			v->rcs, v->stealthy_rcs, v->normal_rcs);
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

	for (x = 0; x < GRID_WIDTH; x++)
		for (y = 0; y < GRID_HEIGHT; y++) {
			Box *b = &real_map[x][y];
			V2Box *p = &v2_prev_maze[x][y];
			int present, flags, type, team, strength;

			v2_box_fields(&present, &flags, &type, &team, &strength, b);
			if (v2_first || p->present != present || p->flags != flags ||
			    p->type != type || p->team != team ||
			    p->strength != strength) {
				if (present)
					fprintf(g_dump,
						"MX %d %d flags %x type %d team %d "
						"code %d strength %d\n",
						x, y, (unsigned) b->flags, (int) b->type,
						(int) b->team, (int) b->teleport_code,
						(int) b->strength);
				p->present = present; p->flags = flags;
				p->type = type; p->team = team;
				p->strength = strength;
			}
		}

	for (i = 0; i < num_veh; i++) {
		Vehicle *v = &actual_vehicles[i];
		Special *s;

		s = &v->special[(int) MAPPER];
		if (s->status != SP_nonexistent && s->record) {
			Mapper *m = (Mapper *) s->record;

			fprintf(g_dump,
				"MPH %d need_redisplay %d map_invalid %d "
				"num_symbols %d num_landmarks %d\n",
				v->number, m->need_redisplay, m->map_invalid,
				m->num_symbols, m->num_landmarks);

			for (x = 0; x < GRID_WIDTH; x++)
				for (y = 0; y < GRID_HEIGHT; y++) {
					Box *b = &m->map[x][y];
					V2Box *p = &v2_prev_mapper[i][x][y];
					int present, flags, type, team, strength;

					v2_box_fields(&present, &flags, &type, &team,
						      &strength, b);
					if (!v2_mapper_prev_valid[i] ||
					    p->present != present || p->flags != flags ||
					    p->type != type || p->team != team ||
					    p->strength != strength) {
						if (present)
							fprintf(g_dump,
								"MP %d %d %d flags %x type %d "
								"team %d strength %d\n",
								v->number, x, y,
								(unsigned) b->flags,
								(int) b->type,
								(int) b->team,
								(int) b->strength);
						p->present = present; p->flags = flags;
						p->type = type; p->team = team;
						p->strength = strength;
					}
				}
			v2_mapper_prev_valid[i] = 1;

			for (j = 0; j < m->num_landmarks; j++)
				fprintf(g_dump,
					"ML %d %d %d type %d team %d\n",
					v->number, m->landmark[j].x, m->landmark[j].y,
					(int) m->landmark[j].type,
					(int) m->landmark[j].team);
		}

		s = &v->special[(int) RADAR];
		if (s->status != SP_nonexistent && s->record) {
			Radar *r = (Radar *) s->record;

			fprintf(g_dump,
				"RNH %d num_blips %d pos %d start %d %d end %d %d\n",
				v->number, r->num_blips, r->pos, r->start_x,
				r->start_y, r->end_x, r->end_y);
			for (j = 0; j < r->num_blips; j++)
				fprintf(g_dump,
					"RB %d %d x %d y %d life %d view %d "
					"oldview %d flags %d\n",
					v->number, j, r->blip[j].x, r->blip[j].y,
					r->blip[j].life, r->blip[j].view,
					r->blip[j].old_view, r->blip[j].flags);
		}

		s = &v->special[(int) NEW_RADAR];
		if (s->status != SP_nonexistent && s->record) {
			newRadar *nr = (newRadar *) s->record;

			fprintf(g_dump, "NRH %d frame_updated %d need_redisplay %d\n",
				v->number, nr->frame_updated, nr->need_redisplay);
			for (j = 0; j < num_veh; j++)
				fprintf(g_dump,
					"NRB %d %d draw %d %d %d %d %d\n",
					v->number, j, nr->blip[j].draw_loc.x,
					nr->blip[j].draw_loc.y,
					nr->blip[j].draw_radar,
					nr->blip[j].draw_tactical,
					nr->blip[j].draw_friend);
		}
	}

	v2_first = 0;
	fflush(g_dump);
}

/* ---- bring-up ---------------------------------------------------------- */
static const char *g_settings_path;
static int g_maze_id = XTK_CONTENT_ID_ARENA;
static int g_epochs = 2;
static int g_ticks = 4000;
/* P3.13 replay-invariance: client render FPS per human (default 15 = P3.12). */
static int g_fps_a = 15;
static int g_fps_b = 15;

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
	strncpy(username, "p3.12", MAX_STRING - 1);
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
	/* Register the selected maze so the settings file's "Maze:" line
	 * resolves before the session rebuilds it. */
	if (make_mdesc((char *) maze_name, &idx) != DESC_LOADED) {
		fprintf(stderr, "parity_fixture: cannot load %s\n", maze_name);
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

/* ---- direct leg -------------------------------------------------------- */

static void
direct_lobby(ServerSession *s, const char *name, uint16_t maze, int drones)
{
	XtkMessage m;

	memset(&m, 0, sizeof(m));
	m.env.message_type = XTK_MSG_HELLO;
	m.body.hello.proto_major = XTK_PROTOCOL_MAJOR;
	m.body.hello.proto_minor = XTK_PROTOCOL_MINOR;
	memcpy(m.body.hello.content_set_digest, xtk_content_set_digest,
	       XTK_SHA256_BYTES);
	strncpy(m.body.hello.name, name, XTK_MAX_NAME_BYTES);
	(void) server_session_message(s, &m, 1000);

	memset(&m, 0, sizeof(m));
	m.env.message_type = XTK_MSG_CONFIG_SELECT;
	m.body.config_select.maze_id = maze;
	m.body.config_select.vehicle_id = XTK_CONTENT_ID_VANGUARD;
	m.body.config_select.drone_count = (uint8_t) drones;
	(void) server_session_message(s, &m, 1000);
}

static void
direct_ready(ServerSession *s)
{
	XtkMessage m;

	memset(&m, 0, sizeof(m));
	m.env.message_type = XTK_MSG_READY;
	m.body.ready.ready = 1;
	m.body.ready.config_revision = server_session_config_revision(s);
	(void) server_session_message(s, &m, 1000);
}

static void
push_direct(int tick, unsigned int *seq)
{
	int i;

	for (i = 0; i < g_ncmds; i++) {
		Intent in;
		Cmd *c = &g_cmds[i];

		if (c->tick != tick)
			continue;
		memset(&in, 0, sizeof(in));
		in.tick = tick;
		in.player = c->player;
		in.sequence = (*seq)++;
		in.a = c->a;
		in.b = c->b;
		in.op = intent_op(c->op, &in.a, &in.b, c->b_str);
		if (in.op == INTENT_NONE)
			continue;
		(void) intent_push(&in);
	}
}

static int
run_direct(const char *dump_path)
{
	ServerSession *sa, *sb, *s;
	unsigned int seq = 0;
	int e, t, status = GAME_RUNNING;

	server_session_init();
	server_session_set_capacity(XTK_MAX_HUMANS);
	sa = server_session_accept(NULL);
	check("direct: session A accepted", sa != NULL);
	sb = server_session_accept(NULL);
	check("direct: session B accepted", sb != NULL);
	if (sa == NULL || sb == NULL)
		return -1;
	direct_lobby(sa, "playerA", g_maze_id, 0);
	direct_lobby(sb, "playerB", g_maze_id, 0);
	direct_ready(sa);
	direct_ready(sb);
	check("direct: config + two ready", server_session_can_start(sa) == 1);

	for (e = 0; e < g_epochs; e++) {
		char epath[1024];

		snprintf(epath, sizeof(epath), "%s.e%d", dump_path, e);
		g_dump = fopen(epath, "w");
		if (g_dump == NULL)
			return -1;
		g_epoch = e;
		v2_reset_baseline();
		if (e > 0) {
			server_session_return_to_lobby(sa);
			direct_lobby(sa, "playerA", g_maze_id, 0);
			direct_lobby(sb, "playerB", g_maze_id, 0);
			direct_ready(sa);
			direct_ready(sb);
		}
		server_seed_random(114336);
		seq = 0;
		if (server_session_begin(sa, 1, 1000) != SERVER_SESSION_OK) {
			fclose(g_dump);
			return -1;
		}
		check("direct: epoch running", server_session_state(sa) ==
		      SERVER_SESSION_RUNNING);
		for (t = 1; t <= g_ticks; t++) {
			push_direct(t, &seq);
			s = server_session_current();
			if (s == NULL || server_session_state(s) !=
			    SERVER_SESSION_RUNNING)
				break;
			status = server_session_step(s, 1000);
			dump_tick((unsigned int) status);
			if (status != GAME_RUNNING)
				break;
		}
		fprintf(stderr, "direct: epoch %d final status %d frame %d\n",
			e, status, frame);
		fclose(g_dump);
		g_dump = NULL;
	}
	server_session_destroy(sa);
	server_session_destroy(sb);
	return 0;
}

/* ---- network leg ------------------------------------------------------- */

typedef struct {
	XtkSessionClient *sc;
	uint32_t pkt_seq;
	int render_fps;		/* P3.13: client render rate (default 15) */
} NetPeer;

static int
net_callback(void *user, const XtkMessage *m)
{
	(void) user;
	(void) m;
	return 0;
}

/*
 * P3.13 replay-invariance knob.  The default render rate is 15 FPS, which
 * makes `pumps` 1 and the code path byte-for-byte the P3.12 network leg.  A
 * higher rate pumps the client extra times within the same 15 Hz server tick,
 * modelling a 30/144 FPS renderer; it never changes what the server applies.
 */
static void
net_step(NetPeer *p, uint64_t *ns)
{
	int fps = (p->render_fps > 0) ? p->render_fps : 15;
	int pumps = (fps + 14) / 15;
	int k;

	if (pumps < 1)
		pumps = 1;
	(void) server_session_service((*ns) / 1000000ULL);
	for (k = 0; k < pumps; k++)
		(void) session_client_pump(p->sc, *ns, net_callback, NULL);
	*ns += 66666667ULL;
}

static int
run_network(const char *dump_path)
{
	Transport *host;
	uint16_t port;
	ServerSession *s = NULL;
	NetPeer A, B;
	uint64_t ns = 1000000000ULL;
	int e, t, i, status = GAME_RUNNING;

	server_session_init();
	server_session_set_capacity(XTK_MAX_HUMANS);
	host = transport_create(0, SERVER_SESSION_MAX_PEERS);
	check("network: listen transport created", host != NULL);
	if (host == NULL)
		return -1;
	port = transport_port(host);
	server_session_set_transport(host);

	memset(&A, 0, sizeof(A));
	memset(&B, 0, sizeof(B));
	A.render_fps = g_fps_a;
	B.render_fps = g_fps_b;
	A.sc = session_client_create();
	B.sc = session_client_create();
	check("network: clients created", A.sc != NULL && B.sc != NULL);
	check("network: A connect",
	      session_client_connect(A.sc, "127.0.0.1", port, "playerA") == 0);
	for (i = 0; i < 4000 && session_client_state(A.sc) != XTK_SC_LOBBY; i++)
		net_step(&A, &ns);
	check("network: B connect",
	      session_client_connect(B.sc, "127.0.0.1", port, "playerB") == 0);
	for (i = 0; i < 4000 &&
	     (session_client_state(A.sc) != XTK_SC_LOBBY ||
	      session_client_state(B.sc) != XTK_SC_LOBBY); i++) {
		net_step(&A, &ns);
		net_step(&B, &ns);
	}
	check("network: both in lobby",
	      session_client_state(A.sc) == XTK_SC_LOBBY &&
	      session_client_state(B.sc) == XTK_SC_LOBBY);
	check("network: A admin", session_client_is_admin(A.sc) == 1);

	(void) session_client_send_config(A.sc, (uint16_t) g_maze_id,
					  XTK_CONTENT_ID_VANGUARD, 0);
	for (i = 0; i < 4000 && session_client_config_revision(A.sc) == 0; i++) {
		net_step(&A, &ns);
		net_step(&B, &ns);
	}
	(void) session_client_send_ready(A.sc, 1);
	(void) session_client_send_ready(B.sc, 1);

	for (e = 0; e < g_epochs; e++) {
		char epath[1024];

		snprintf(epath, sizeof(epath), "%s.e%d", dump_path, e);
		g_dump = fopen(epath, "w");
		if (g_dump == NULL)
			return -1;
		g_epoch = e;
		v2_reset_baseline();

		if (e > 0) {
			/* The prior epoch ended (natural win -> RESULTS, or a
			 * tick-limited abort from RUNNING).  The admin client
			 * requests a reset, both peers return to the lobby, and
			 * then re-ready against the (unchanged) revision. */
			(void) session_client_send_reset(A.sc);
			for (i = 0; i < 4000 &&
			     (session_client_state(A.sc) != XTK_SC_LOBBY ||
			      session_client_state(B.sc) != XTK_SC_LOBBY); i++) {
				net_step(&A, &ns);
				net_step(&B, &ns);
			}
			(void) session_client_send_ready(A.sc, 1);
			(void) session_client_send_ready(B.sc, 1);
			for (i = 0; i < 200; i++) {
				net_step(&A, &ns);
				net_step(&B, &ns);
			}
		}
		server_seed_random(114336);
		A.pkt_seq = 0;
		B.pkt_seq = 0;
		(void) session_client_send_start(A.sc);
		for (i = 0; i < 4000 && !(s != NULL &&
		     server_session_state(s) == SERVER_SESSION_RUNNING); i++) {
			net_step(&A, &ns);
			net_step(&B, &ns);
			s = server_session_current();
		}
		check("network: match running", s != NULL &&
		      server_session_state(s) == SERVER_SESSION_RUNNING);

		for (t = 1; t <= g_ticks; t++) {
			NetPeer *peers[2];
			int pi;

			peers[0] = &A;
			peers[1] = &B;
			for (pi = 0; pi < 2; pi++) {
				NetPeer *p = peers[pi];
				XtkMessage batch;
				int n = 0;

				memset(&batch, 0, sizeof(batch));
				batch.env.message_type = XTK_MSG_INPUT_BATCH;
				batch.env.epoch = session_client_epoch(p->sc);
				batch.body.input_batch.packet_sequence = ++p->pkt_seq;
				batch.body.input_batch.last_observed_server_tick =
					(uint32_t) frame;
				for (i = 0; i < g_ncmds; i++) {
					XtkCommand c;
					Cmd *cmd = &g_cmds[i];

					if (cmd->tick != t || cmd->player != pi)
						continue;
					memset(&c, 0, sizeof(c));
					c.a = cmd->a;
					c.b = cmd->b;
					c.command_id = (uint32_t)
						(t * 64 + pi * 16 + n + 1);
					c.tick_hint = (uint32_t) t;
					c.op = wire_op(cmd->op, &c.a, &c.b,
						       cmd->b_str, &c.flags);
					batch.body.input_batch.command[n++] = c;
				}
				if (n == 0) {
					/* Idle tick keeps the session alive: the
					 * shipped client emits a heartbeat with no
					 * engine effect (P3.11). */
					XtkCommand hb;

					memset(&hb, 0, sizeof(hb));
					hb.op = XTK_OP_HEARTBEAT;
					hb.command_id = (uint32_t)
						(t * 64 + pi * 16 + 1);
					hb.tick_hint = (uint32_t) t;
					batch.body.input_batch.command[0] = hb;
					n = 1;
				}
				batch.body.input_batch.command_count =
					(uint16_t) n;
				(void) session_client_send_input(p->sc,
								 &batch);
			}

			net_step(&A, &ns);
			net_step(&B, &ns);

			s = server_session_current();
			if (s == NULL || server_session_state(s) !=
			    SERVER_SESSION_RUNNING)
				break;
			/* Same monotonic clock as server_session_service() above
			 * (the shipped loop uses one now_ms() for both), so the
			 * neutral-input timeout never sees a non-monotonic t. */
			status = server_session_step(s, ns / 1000000ULL);
			dump_tick((unsigned int) status);
			if (status != GAME_RUNNING)
				break;
		}
		fprintf(stderr, "network: epoch %d final status %d frame %d\n",
			e, status, frame);
		fclose(g_dump);
		g_dump = NULL;
	}
	session_client_destroy(A.sc);
	session_client_destroy(B.sc);
	transport_destroy(host);
	return 0;
}

int
main(int argc, char *argv[])
{
	const char *mode, *assets, *maze, *settings, *trace, *dump;

	if (argc < 7) {
		fprintf(stderr, "usage: %s <direct|network> <assets> <maze> "
			"<settings> <trace> <dump> [epochs]\n", argv[0]);
		return 2;
	}
	mode = argv[1];
	assets = argv[2];
	maze = argv[3];
	g_settings_path = argv[4];
	trace = argv[5];
	dump = argv[6];
	if (argc > 7)
		g_epochs = atoi(argv[7]);
	{
		const char *tk = getenv("XTANK_P3_TICKS");

		if (tk != NULL && atoi(tk) > 0)
			g_ticks = atoi(tk);
	}
	/* P3.13: XTANK_P3_FPS="30" or "30,144" (mixed-rate pair). */
	{
		const char *fp = getenv("XTANK_P3_FPS");

		if (fp != NULL) {
			int a = atoi(fp);
			const char *comma = strchr(fp, ',');

			g_fps_a = (a > 0) ? a : 15;
			g_fps_b = g_fps_a;
			if (comma != NULL) {
				int b = atoi(comma + 1);

				if (b > 0)
					g_fps_b = b;
			}
		}
	}

	if (strcmp(maze, "combat1") == 0)
		g_maze_id = XTK_CONTENT_ID_COMBAT1;
	else
		g_maze_id = XTK_CONTENT_ID_ARENA;

	if (server_bringup(assets, maze) != 0)
		return 2;
	load_trace(trace);

	if (strcmp(mode, "direct") == 0)
		(void) run_direct(dump);
	else if (strcmp(mode, "network") == 0)
		(void) run_network(dump);
	else
		return 2;

	if (failures != 0) {
		printf("PARITY FIXTURE FAIL (%d checks, %d failures)\n",
		       checks, failures);
		return 1;
	}
	printf("PARITY FIXTURE PASS (%d checks, mode=%s, epochs=%d)\n",
	       checks, mode, g_epochs);
	return 0;
}
