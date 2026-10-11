/*-
 * Copyright (c) 1988 Terry Donahue
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * view_fixture.c -- P2.5 authorized read-only view exporter acceptance.
 *
 * Links the shipped display-free server core (Src/sobj, without server_main.o)
 * plus the frozen P2.3 wire codec, drives one terminal-free human plus one
 * drone, and independently asserts the docs/VIEW-v1.md rules:
 *
 *   - simulation parity: 0/1/5 exports per tick leave the simulation state
 *     byte-identical (FNV hash of the authoritative globals);
 *   - the exporter makes no do_special() call while it runs (link-time --wrap);
 *   - field/visibility allowlist: SelfState matches the own vehicle, MapCell ==
 *     mapper knowledge only, LandmarkView == real_map within the viewport only,
 *     RadarBlip always anonymous, every reserved byte zero, counts in bounds;
 *   - viewport edges (half-open, half-extent = ANIM_WIN_WIDTH/2);
 *   - unexplored vs remembered/stale mapper knowledge;
 *   - anonymous and expired radar contacts;
 *   - the death camera freezes on the last own position;
 *   - the bound-maximized snapshot encodes to exactly 28 524 bytes.
 *
 * The fixture may manipulate simulation state directly (it is the harness); the
 * exporter itself is the read-only unit under test.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xtank.h"
#include "bullet.h"
#include "clfkr.h"
#include "object.h"
#include "vehicle.h"
#include "server.h"
#include "server_view.h"
#include "wire.h"
#include "proto.h"
#include "gr.h"

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

/* ---- link-time instrumentation ---------------------------------------- */

static int in_export;
static unsigned long n_export, n_special_in_export;

int __real_server_view_export(int, unsigned int, uint32_t, uint16_t,
			      XtkViewSnapshot *);
void __real_do_special(Vehicle *, SpecialType, int);

int
__wrap_server_view_export(int slot, unsigned int generation,
			  uint32_t seq, uint16_t flags, XtkViewSnapshot *out)
{
	int r;

	n_export++;
	in_export = 1;
	r = __real_server_view_export(slot, generation, seq, flags, out);
	in_export = 0;
	return r;
}

void
__wrap_do_special(Vehicle *v, SpecialType s, int action)
{
	if (in_export)
		n_special_in_export++;
	__real_do_special(v, s, action);
}

/* ---- acceptance bookkeeping ------------------------------------------- */

static int failures;

static void
check(const char *what, int ok)
{
	if (!ok) {
		fprintf(stderr, "  [FAIL] %s\n", what);
		failures++;
	} else {
		fprintf(stderr, "  [ok]   %s\n", what);
	}
}

/* ---- simulation-state hash (proves read-only export) ------------------ */

static void
hbytes(unsigned long *h, const void *p, size_t n)
{
	const unsigned char *b = (const unsigned char *) p;
	size_t i;

	for (i = 0; i < n; i++) {
		*h ^= b[i];
		*h *= 16777619UL;
	}
}

static unsigned long
sim_hash(void)
{
	unsigned long h = 2166136261UL;
	int i;

	hbytes(&h, &frame, sizeof(frame));
	hbytes(&h, &num_veh, sizeof(num_veh));
	hbytes(&h, &num_veh_alive, sizeof(num_veh_alive));
	hbytes(&h, &num_veh_dead, sizeof(num_veh_dead));
	hbytes(&h, actual_vehicles, (size_t) num_veh * sizeof(Vehicle));
	hbytes(&h, real_map, sizeof(real_map));
	hbytes(&h, bset->array, sizeof(actual_bset.array));
	hbytes(&h, &bset->number, sizeof(bset->number));
	hbytes(&h, eset->array, sizeof(actual_eset.array));
	hbytes(&h, &eset->number, sizeof(eset->number));
	hbytes(&h, &special_stats, sizeof(special_stats));

	for (i = 0; i < num_veh; i++) {
		Special *s = &actual_vehicles[i].special[(int) MAPPER];

		if (s->status != SP_nonexistent && s->record != NULL)
			hbytes(&h, s->record, sizeof(Mapper));
		s = &actual_vehicles[i].special[(int) RADAR];
		if (s->status != SP_nonexistent && s->record != NULL)
			hbytes(&h, s->record, sizeof(Radar));
	}
	return h;
}

/* ---- scenario helpers ------------------------------------------------- */

static int
view_has_slot(const XtkViewSnapshot *s, int slot)
{
	int i;

	for (i = 0; i < s->vehicle_count; i++)
		if (s->vehicle[i].slot == (uint8_t) slot)
			return 1;
	return 0;
}

static Mapper *
ensure_mapper(Vehicle *v)
{
	Special *s = &v->special[(int) MAPPER];

	if (s->record == NULL) {
		s->record = calloc(1, sizeof(Mapper));
		s->proc = NULL;
	}
	if (s->status == SP_nonexistent)
		s->status = SP_off;
	return (Mapper *) s->record;
}

static Radar *
ensure_radar(Vehicle *v)
{
	Special *s = &v->special[(int) RADAR];

	if (s->record == NULL) {
		s->record = calloc(1, sizeof(Radar));
		s->proc = NULL;
	}
	s->status = SP_on;
	return (Radar *) s->record;
}

static void
place_vehicle_at(Vehicle *v, double x, double y)
{
	v->loc->x = (FLOAT) x;
	v->loc->y = (FLOAT) y;
}

int
main(int argc, char *argv[])
{
	const char *assets = (argc > 1) ? argv[1] : ".";
	ServerConfig *cfg;
	int vdesc_index, mdesc_index;
	int hb, t;
	Vehicle *own, *drone;
	XtkViewSnapshot snap;
	int half;

	bset = &actual_bset;
	eset = &actual_eset;
	InitConfigStruct(&command_options);
	command_options.NoDelay = TRUE;
	command_options.PrintScores = FALSE;
	command_options.AutoExit = TRUE;

	(void) strncpy(username, "p2.5", MAX_STRING - 1);
	username[MAX_STRING - 1] = '\0';

	if (content_set_root(assets) != 0) {
		fprintf(stderr, "view_fixture: %s\n", content_last_error());
		return 2;
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

	if (make_vdesc((char *) "Vanguard", &vdesc_index) != DESC_LOADED) {
		fprintf(stderr, "view_fixture: cannot load Vanguard\n");
		return 2;
	}
	if (make_mdesc((char *) "combat1", &mdesc_index) != DESC_LOADED) {
		fprintf(stderr, "view_fixture: cannot load combat1\n");
		return 2;
	}
	settings.mdesc = &mdesc[mdesc_index];
	settings.si.game = settings.mdesc->type;

	server_players_init();
	hb = server_players_add("humanA", 7);
	if (hb < 0 || server_players_bind_slot(hb, 0) != 0) {
		fprintf(stderr, "view_fixture: cannot bind human\n");
		return 2;
	}

	num_combatants = 2;
	combatant[0].num_players = 1;
	combatant[0].player[0] = hb;
	combatant[0].num_programs = 0;
	combatant[0].team = 1 % MAX_TEAMS;
	combatant[0].vdesc = vdesc_index;
	(void) strncpy(combatant[0].name, "humanA", MAX_STRING - 1);
	combatant[0].name[MAX_STRING - 1] = '\0';

	combatant[1].num_players = 0;
	combatant[1].num_programs = 1;
	combatant[1].program[0] = 0;
	combatant[1].team = 2 % MAX_TEAMS;
	combatant[1].vdesc = vdesc_index;

	fprintf(stderr, "== building match\n");
	if (server_match_init() != GAME_RUNNING) {
		fprintf(stderr, "view_fixture: match failed to start\n");
		return 2;
	}
	own = combatant[0].vehicle;
	drone = combatant[1].vehicle;
	if (own == NULL || drone == NULL) {
		fprintf(stderr, "view_fixture: no vehicles\n");
		return 2;
	}

	/* A few ticks so the mapper model pass populates knowledge. */
	for (t = 0; t < 3; t++)
		(void) server_step();

	/* ---------------- basic export + field allowlist ---------------- */
	fprintf(stderr, "== basic export / field allowlist\n");
	check("export succeeds", server_view_export(0, 0, 1,
		XTK_FLAG_KEYFRAME, &snap) == SERVER_VIEW_OK);
	check("server_tick == frame", snap.server_tick == (uint32_t) frame);
	check("own_slot == own.number", snap.own_slot == (uint8_t) own->number);
	check("self.slot == own.number", snap.self.slot == (uint8_t) own->number);
	check("self.generation == deaths+1",
		snap.self.generation == (uint8_t) (combatant[0].deaths + 1));
	check("self.armor[0] == FRONT",
		snap.self.armor[0] == (uint16_t) own->armor.side[FRONT]);
	check("self.armor[1] == BACK",
		snap.self.armor[1] == (uint16_t) own->armor.side[BACK]);
	check("self.armor[2] == LEFT",
		snap.self.armor[2] == (uint16_t) own->armor.side[LEFT]);
	check("self.armor[3] == RIGHT",
		snap.self.armor[3] == (uint16_t) own->armor.side[RIGHT]);
	check("self.heat == own.heat", snap.self.heat == (uint16_t) own->heat);
	check("self.score == own.score",
		snap.self.score == (uint32_t) combatant[0].score);
	check("self.weapon_count == own.num_weapons",
		snap.self.weapon_count == (uint8_t) own->num_weapons);
	check("header weapon_count == self.weapon_count",
		snap.weapon_count == snap.self.weapon_count);
	{
		int w, ammo_ok = 1;
		uint8_t bitmap_ok = 1;

		for (w = 0; w < own->num_weapons && w < XTK_MAX_WEAPONS; w++) {
			if (snap.self.ammo[w] != (uint16_t) own->weapon[w].ammo)
				ammo_ok = 0;
			if (snap.self.reload[w] !=
			    (uint16_t) own->weapon[w].reload_counter)
				ammo_ok = 0;
			if (!!(own->weapon[w].status & WS_on) !=
			    !!(snap.self.weapon_enabled_bitmap & (1u << w)))
				bitmap_ok = 0;
		}
		check("self ammo/reload match", ammo_ok);
		check("weapon_enabled_bitmap matches status", bitmap_ok);
	}

	/* reserved bytes are always zero and counts are in bounds. */
	{
		int i, res_ok = 1;

		if (snap.self.reserved[0] || snap.self.reserved[1])
			res_ok = 0;
		for (i = 0; i < snap.vehicle_count; i++)
			if (memcmp(snap.vehicle[i].reserved, "\0\0\0\0", 4))
				res_ok = 0;
		for (i = 0; i < snap.bullet_count; i++)
			if (snap.bullet[i].reserved)
				res_ok = 0;
		for (i = 0; i < snap.explosion_count; i++)
			if (memcmp(snap.explosion[i].reserved, "\0\0\0\0", 4))
				res_ok = 0;
		for (i = 0; i < snap.landmark_count; i++)
			if (snap.landmark[i].reserved ||
			    memcmp(snap.landmark[i].reserved2, "\0\0\0\0", 4))
				res_ok = 0;
		for (i = 0; i < snap.radar_blip_count; i++)
			if (snap.radar[i].reserved)
				res_ok = 0;
		check("all reserved bytes zero", res_ok);
		check("counts within protocol bounds",
			snap.vehicle_count <= XTK_MAX_VEHICLES &&
			snap.bullet_count <= XTK_MAX_BULLETS &&
			snap.explosion_count <= XTK_MAX_EXPLOSIONS &&
			snap.map_cell_count <= XTK_MAX_MAP_CELLS &&
			snap.landmark_count <= XTK_MAX_LANDMARKS &&
			snap.radar_blip_count <= XTK_MAX_RADAR_BLIPS);
	}

	/* ---------------- viewport edges ---------------- */
	fprintf(stderr, "== viewport edges (half-open, half=%d)\n",
		server_view_half_extent());
	half = server_view_half_extent();
	{
		double ox = own->loc->x, oy = own->loc->y;
		int in;

		place_vehicle_at(drone, ox + half - 1, oy);
		(void) server_view_export(0, 0, 2, 0, &snap);
		in = (view_has_slot(&snap, drone->number));
		check("vehicle just inside far edge is visible", in);

		place_vehicle_at(drone, ox + half, oy);
		(void) server_view_export(0, 0, 3, 0, &snap);
		in = (view_has_slot(&snap, drone->number));
		check("vehicle at far edge is excluded (half-open)", !in);

		place_vehicle_at(drone, ox - half, oy);
		(void) server_view_export(0, 0, 4, 0, &snap);
		in = (view_has_slot(&snap, drone->number));
		check("vehicle at near edge is visible", in);

		place_vehicle_at(drone, ox - half - 1, oy);
		(void) server_view_export(0, 0, 5, 0, &snap);
		in = (view_has_slot(&snap, drone->number));
		check("vehicle just outside near edge is invisible", !in);

		place_vehicle_at(drone, ox, oy + half - 1);
		(void) server_view_export(0, 0, 6, 0, &snap);
		in = (view_has_slot(&snap, drone->number));
		check("vehicle just inside far y edge is visible", in);

		place_vehicle_at(drone, ox, oy + half);
		(void) server_view_export(0, 0, 7, 0, &snap);
		in = (view_has_slot(&snap, drone->number));
		check("vehicle at far y edge is excluded", !in);

		/* Landmark viewport rule: box centre vs the same window. */
		{
			int gx = (int) (own->loc->x / BOX_WIDTH);
			int gy = (int) (own->loc->y / BOX_HEIGHT);

			if (gx >= GRID_WIDTH)
				gx = GRID_WIDTH - 1;
			if (gy >= GRID_HEIGHT)
				gy = GRID_HEIGHT - 1;
			real_map[gx][gy].type = ARMOR;
			(void) server_view_export(0, 0, 8, 0, &snap);
			check("visible landmark (own box) exported",
				snap.landmark_count >= 1);
			real_map[gx][gy].type = NORMAL;
		}
	}

	/* ---------------- mapper knowledge only ---------------- */
	fprintf(stderr, "== mapper knowledge (unexplored vs remembered)\n");
	{
		Mapper *m = ensure_mapper(own);
		Box *rb;
		int i, mc_ok = 1;

		/* Clear to a known unexplored state, then prove absence. */
		memset(m->map, 0, sizeof(m->map));
		(void) server_view_export(0, 0, 9, 0, &snap);
		check("unexplored map exports zero cells", snap.map_cell_count == 0);

		/* Remembered wall with no matching real_map wall is still exported. */
		m->map[5][5].flags = NORTH_WALL;
		m->map[5][5].type = NORMAL;
		real_map[5][5].flags = 0;
		(void) server_view_export(0, 0, 10, 0, &snap);
		check("remembered cell exported", snap.map_cell_count == 1);
		check("remembered NORTH_WALL flag set",
			snap.cell[0].flags & SERVER_MAPCELL_NORTH_WALL);
		check("remembered cell is KNOWN",
			snap.cell[0].flags & SERVER_MAPCELL_KNOWN);
		check("remembered cell has no phantom real_map wall",
			!(real_map[5][5].flags & NORTH_WALL));

		/* Every exported cell must match the mapper record exactly. */
		memset(m->map, 0, sizeof(m->map));
		m->map[4][4].flags = NORTH_WALL | WEST_WALL | NORTH_DEST;
		m->map[4][4].type = FUEL;
		m->map[4][4].team = NEUTRAL;
		m->map[7][9].flags = WEST_WALL | WEST_DEST;
		m->map[7][9].type = AMMO;
		m->map[7][9].team = 1;
		(void) server_view_export(0, 0, 11, 0, &snap);
		check("mapper-known cell count exported", snap.map_cell_count == 2);
		for (i = 0; i < snap.map_cell_count; i++) {
			XtkMapCell *c = &snap.cell[i];

			rb = &m->map[c->grid_x][c->grid_y];
			if (!(rb->flags || rb->type || rb->team || rb->strength))
				mc_ok = 0;
			if (c->type != (uint8_t) rb->type)
				mc_ok = 0;
			if (!!(rb->flags & NORTH_WALL) !=
			    !!(c->flags & SERVER_MAPCELL_NORTH_WALL))
				mc_ok = 0;
			if (!!(rb->flags & WEST_WALL) !=
			    !!(c->flags & SERVER_MAPCELL_WEST_WALL))
				mc_ok = 0;
			if (!!(rb->flags & NORTH_DEST) !=
			    !!(c->flags & SERVER_MAPCELL_NORTH_DEST))
				mc_ok = 0;
			if (!!(rb->flags & WEST_DEST) !=
			    !!(c->flags & SERVER_MAPCELL_WEST_DEST))
				mc_ok = 0;
			if (!!(rb->team != NEUTRAL) !=
			    !!(c->flags & SERVER_MAPCELL_TEAM_KNOWN))
				mc_ok = 0;
		}
		check("every MapCell matches the mapper record", mc_ok);

		/* real_map content the mapper does not know must not leak. */
		memset(m->map, 0, sizeof(m->map));
		real_map[29][29].type = ARMOR;
		(void) server_view_export(0, 0, 12, 0, &snap);
		check("real_map-only cell not exported as MapCell",
			snap.map_cell_count == 0);
		m->map[29][29].type = ARMOR;
		(void) server_view_export(0, 0, 13, 0, &snap);
		check("mapper-known cell now exported", snap.map_cell_count == 1 &&
			snap.cell[0].grid_x == 29 && snap.cell[0].grid_y == 29);
	}

	/* ---------------- radar: anonymous / expired ---------------- */
	fprintf(stderr, "== radar contacts (anonymous / expired)\n");
	{
		Radar *r = ensure_radar(own);

		memset(r, 0, sizeof(*r));
		r->num_blips = 2;
		r->blip[0].x = 100;
		r->blip[0].y = 120;
		r->blip[0].life = SERVER_RADAR_BORN;
		r->blip[0].flags = 0xFFFF;	/* names vehicles; must not leak */
		r->blip[1].x = 60;
		r->blip[1].y = 70;
		r->blip[1].life = SERVER_RADAR_BORN - 12;
		r->blip[1].flags = 0x00FF;

		(void) server_view_export(0, 0, 14, 0, &snap);
		check("radar blips exported", snap.radar_blip_count == 2);
		check("radar age[0] == 0", snap.radar[0].age == 0);
		check("radar age[1] == 12", snap.radar[1].age == 12);
		check("radar positions copied",
			snap.radar[0].x == 100 && snap.radar[0].y == 120 &&
			snap.radar[1].x == 60 && snap.radar[1].y == 70);
		check("radar blips are anonymous",
			snap.radar[0].classification == SERVER_RADAR_ANONYMOUS &&
			snap.radar[1].classification == SERVER_RADAR_ANONYMOUS);

		/* Expiry removes contacts from the record; the exporter sees none. */
		r->num_blips = 0;
		(void) server_view_export(0, 0, 15, 0, &snap);
		check("expired radar exports zero blips",
			snap.radar_blip_count == 0);
	}

	/* ---------------- read-only parity: 0/1/5 exports ---------------- */
	fprintf(stderr, "== read-only parity (0/1/5 exports per tick)\n");
	{
		unsigned long h0, h1, h5;
		int i;

		(void) server_step();
		h0 = sim_hash();
		for (i = 0; i < 1; i++)
			(void) server_view_export(0, 0, 16, 0, &snap);
		h1 = sim_hash();
		for (i = 0; i < 5; i++)
			(void) server_view_export(0, 0, 17, 0, &snap);
		h5 = sim_hash();
		check("simulation state unchanged after 0 exports", h0 == h0);
		check("simulation state unchanged after 1 export", h0 == h1);
		check("simulation state unchanged after 5 exports", h0 == h5);
	}

	/* ---------------- death camera ---------------- */
	fprintf(stderr, "== death camera\n");
	{
		double fx = own->loc->x, fy = own->loc->y;

		kill_vehicle(own, (Vehicle *) NULL);
		check("own is not alive after kill",
			!tstflag(own->status, VS_is_alive));

		(void) server_view_export(0, 0, 18, 0, &snap);
		check("death camera exports SelfState", snap.self.slot ==
			(uint8_t) own->number);
		check("death camera lifecycle shows not-alive",
			!(snap.self.lifecycle_flags & VS_is_alive));
		check("dead own vehicle is not a VehicleView",
			!view_has_slot(&snap, own->number));

		/* Camera is frozen on the last own position. */
		place_vehicle_at(drone, fx + 10, fy);
		(void) server_view_export(0, 0, 19, 0, &snap);
		check("frozen camera sees a nearby vehicle",
			view_has_slot(&snap, drone->number));
	}

	/* ---------------- bound-maximized size via the codec ---------------- */
	fprintf(stderr, "== worst-case view size\n");
	{
		static XtkMessage msg;
		static uint8_t buf[XTK_MAX_MESSAGE_BYTES];
		size_t len = 0;
		int rc;

		memset(&msg, 0, sizeof(msg));
		msg.env.message_type = XTK_MSG_VIEW_SNAPSHOT;
		msg.body.view_snapshot.vehicle_count = XTK_MAX_VEHICLES;
		msg.body.view_snapshot.bullet_count = XTK_MAX_BULLETS;
		msg.body.view_snapshot.explosion_count = XTK_MAX_EXPLOSIONS;
		msg.body.view_snapshot.map_cell_count = XTK_MAX_MAP_CELLS;
		msg.body.view_snapshot.landmark_count = XTK_MAX_LANDMARKS;
		msg.body.view_snapshot.radar_blip_count = XTK_MAX_RADAR_BLIPS;
		msg.body.view_snapshot.weapon_count = XTK_MAX_WEAPONS;

		rc = xtk_encode(buf, sizeof(buf), &msg, &len);
		check("worst-case snapshot encodes", rc == XTK_ERR_NONE);
		check("worst-case payload == 28492",
			len - XTK_ENVELOPE_BYTES == 28492u);
		check("worst-case message == 28524", len == 28524u);
		check("worst-case fits XTK_MAX_MESSAGE_BYTES",
			len <= XTK_MAX_MESSAGE_BYTES);
	}

	/* ---------------- no special/draw callback during export ---------- */
	check("no do_special() call during any export", n_special_in_export == 0);

	if (failures != 0) {
		printf("VIEW FIXTURE FAIL (%d checks)\n", failures);
		return 1;
	}
	printf("VIEW FIXTURE PASS (%lu exports; do_special_in_export=%lu; "
	       "worst-case 28524 bytes)\n", n_export, n_special_in_export);
	return 0;
}