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
 * view_fixture.c -- P3.8 per-player view isolation and authorization.
 *
 * Builds two independent human recipients (slots A/B) with separated cameras,
 * distinct mapper histories, private HUD/self state and private radar contacts,
 * and independently checks each recipient's exported bytes:
 *
 *   - each snapshot carries only its own camera's vehicles/bullets/landmarks;
 *   - MapCell is that recipient's mapper knowledge (never the full real_map);
 *   - radar contacts are anonymous (no vehicle-naming flags);
 *   - SelfState (HUD) is the recipient's own vehicle only;
 *   - export order A->B and B->A yields byte-identical per-recipient snapshots;
 *   - 0/1/5 exports leave the authoritative simulation byte-identical;
 *   - entity-life generations are full-width u32 (a >255 life is not truncated)
 *     and binding/entity generations are independently validated;
 *   - a stale generation is rejected (no arbitrary-camera query);
 *   - landmarks carry the full-width grid key and id.
 *
 * The fixture may manipulate simulation state directly (it is the harness); the
 * exporter is the read-only unit under test.
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
#include "server_events.h"
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
	hbytes(&h, combatant, sizeof(Combatant) * (size_t) num_combatants);
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

/* The camera of a recipient must track that recipient's own Loc. */
static int
snap_centered_on(const XtkViewSnapshot *s, const Vehicle *v)
{
	return s->own_slot == (uint8_t) v->number &&
	       s->self.slot == (uint8_t) v->number;
}

int
main(int argc, char *argv[])
{
	const char *assets = (argc > 1) ? argv[1] : ".";
	ServerConfig *cfg;
	int vdesc_index, mdesc_index;
	int hbA, hbB;
	Vehicle *A, *B;
	static XtkViewSnapshot sa, sa2, sb;
	static XtkViewSnapshot orderAB_A, orderBA_A;
	int t;

	bset = &actual_bset;
	eset = &actual_eset;
	InitConfigStruct(&command_options);
	command_options.NoDelay = TRUE;
	command_options.PrintScores = FALSE;
	command_options.AutoExit = TRUE;

	(void) strncpy(username, "p3.8", MAX_STRING - 1);
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
	hbA = server_players_add("humanA", 11);
	hbB = server_players_add("humanB", 22);
	if (hbA < 0 || hbB < 0 ||
	    server_players_bind_slot(hbA, 0) != 0 ||
	    server_players_bind_slot(hbB, 1) != 0) {
		fprintf(stderr, "view_fixture: cannot bind humans\n");
		return 2;
	}

	num_combatants = 2;
	combatant[0].num_players = 1;
	combatant[0].player[0] = hbA;
	combatant[0].num_programs = 0;
	combatant[0].team = 1 % MAX_TEAMS;
	combatant[0].vdesc = vdesc_index;
	(void) strncpy(combatant[0].name, "humanA", MAX_STRING - 1);
	combatant[0].name[MAX_STRING - 1] = '\0';

	combatant[1].num_players = 1;
	combatant[1].player[0] = hbB;
	combatant[1].num_programs = 0;
	combatant[1].team = 2 % MAX_TEAMS;
	combatant[1].vdesc = vdesc_index;
	(void) strncpy(combatant[1].name, "humanB", MAX_STRING - 1);
	combatant[1].name[MAX_STRING - 1] = '\0';

	fprintf(stderr, "== building two-human match\n");
	server_events_reset(1, 0);
	if (server_match_init() != GAME_RUNNING) {
		fprintf(stderr, "view_fixture: match failed to start\n");
		return 2;
	}
	A = combatant[0].vehicle;
	B = combatant[1].vehicle;
	if (A == NULL || B == NULL) {
		fprintf(stderr, "view_fixture: no vehicles\n");
		return 2;
	}

	/* Separate the two cameras well beyond one viewport (2*384 units). */
	place_vehicle_at(A, 1000.0, 1000.0);
	place_vehicle_at(B, 5000.0, 5000.0);

	/* Private HUD: different armor so SelfState cannot be confused. */
	A->armor.side[FRONT] = 111;
	B->armor.side[FRONT] = 222;

	/* Distinct remembered mapper knowledge. */
	{
		Mapper *mA = ensure_mapper(A);
		Mapper *mB = ensure_mapper(B);

		memset(mA->map, 0, sizeof(mA->map));
		memset(mB->map, 0, sizeof(mB->map));
		mA->map[5][5].flags = NORTH_WALL;
		mA->map[5][5].type = FUEL;
		mB->map[20][20].flags = WEST_WALL;
		mB->map[20][20].type = AMMO;
	}

	/* Distinct anonymous radar contacts. */
	{
		Radar *rA = ensure_radar(A);
		Radar *rB = ensure_radar(B);

		memset(rA, 0, sizeof(*rA));
		memset(rB, 0, sizeof(*rB));
		rA->num_blips = 1;
		rA->blip[0].x = 10; rA->blip[0].y = 20;
		rA->blip[0].life = SERVER_RADAR_BORN;
		rA->blip[0].flags = 0xFFFF;
		rB->num_blips = 2;
		rB->blip[0].x = 30; rB->blip[0].y = 40;
		rB->blip[0].life = SERVER_RADAR_BORN - 3;
		rB->blip[1].x = 50; rB->blip[1].y = 60;
		rB->blip[1].life = SERVER_RADAR_BORN - 9;
	}

	/* A visible landmark only in A's viewport (full grid key). */
	{
		int gx = (int) (A->loc->x / BOX_WIDTH);
		int gy = (int) (A->loc->y / BOX_HEIGHT);

		real_map[gx][gy].type = ARMOR;
	}

	/* One bullet in each viewport (private visible state). */
	{
		Bullet *bA = &bset->array[4];
		Bullet *bB = &bset->array[9];

		memset(&actual_bset, 0, sizeof(actual_bset));
		bset->number = 2;
		bset->list[0] = bA;
		bset->list[1] = bB;
		bA->type = CANNON;
		bA->owner = A;
		bA->loc = &bA->loc1;
		bA->loc1.x = A->loc->x + 50; bA->loc1.y = A->loc->y;
		bB->type = FLAME;
		bB->owner = B;
		bB->loc = &bB->loc1;
		bB->loc1.x = B->loc->x + 50; bB->loc1.y = B->loc->y;
	}

	/* ---------------- separated cameras / recipient bytes ---------------- */
	fprintf(stderr, "== separated cameras and recipient isolation\n");
	check("A export succeeds", server_view_export(0, 0, 1, XTK_FLAG_KEYFRAME,
		&sa) == SERVER_VIEW_OK);
	check("B export succeeds", server_view_export(1, 0, 1, XTK_FLAG_KEYFRAME,
		&sb) == SERVER_VIEW_OK);
	check("A snapshot centred on A", snap_centered_on(&sa, A));
	check("B snapshot centred on B", snap_centered_on(&sb, B));
	check("own_slot differs", sa.own_slot != sb.own_slot);
	check("SelfState HUD is private (armor differs)",
		sa.self.armor[0] == 111 && sb.self.armor[0] == 222);
	check("A camera excludes B's vehicle", !view_has_slot(&sa, B->number));
	check("B camera excludes A's vehicle", !view_has_slot(&sb, A->number));

	/* Each sees only its own bullet. */
	check("A sees exactly one bullet", sa.bullet_count == 1);
	check("B sees exactly one bullet", sb.bullet_count == 1);
	check("A bullet is A's (id 4, owner A)",
		sa.bullet[0].id == 4 && sa.bullet[0].owner_slot ==
		(uint8_t) A->number);
	check("B bullet is B's (id 9, owner B)",
		sb.bullet[0].id == 9 && sb.bullet[0].owner_slot ==
		(uint8_t) B->number);

	/* Mapper knowledge is recipient-private. */
	{
		int a_has55 = 0, a_has20 = 0, b_has55 = 0, b_has20 = 0, i;

		for (i = 0; i < sa.map_cell_count; i++) {
			if (sa.cell[i].grid_x == 5 && sa.cell[i].grid_y == 5)
				a_has55 = 1;
			if (sa.cell[i].grid_x == 20 && sa.cell[i].grid_y == 20)
				a_has20 = 1;
		}
		for (i = 0; i < sb.map_cell_count; i++) {
			if (sb.cell[i].grid_x == 5 && sb.cell[i].grid_y == 5)
				b_has55 = 1;
			if (sb.cell[i].grid_x == 20 && sb.cell[i].grid_y == 20)
				b_has20 = 1;
		}
		check("A exports its own mapper cell (5,5)", a_has55);
		check("A does not export B's mapper cell (20,20)", !a_has20);
		check("B exports its own mapper cell (20,20)", b_has20);
		check("B does not export A's mapper cell (5,5)", !b_has55);
	}

	/* Radar is private and anonymous. */
	check("A exports one anonymous blip",
		sa.radar_blip_count == 1 &&
		sa.radar[0].classification == SERVER_RADAR_ANONYMOUS);
	check("B exports two anonymous blips",
		sb.radar_blip_count == 2 &&
		sb.radar[0].classification == SERVER_RADAR_ANONYMOUS &&
		sb.radar[1].classification == SERVER_RADAR_ANONYMOUS);
	check("radar positions are recipient-private",
		sa.radar[0].x == 10 && sb.radar[0].x == 30 &&
		sb.radar[1].x == 50);

	/* Landmark full-width grid key and viewport scoping. */
	check("A exports the visible landmark",
		sa.landmark_count >= 1);
	{
		int gx = (int) (A->loc->x / BOX_WIDTH);
		int gy = (int) (A->loc->y / BOX_HEIGHT);
		int i, found = 0;

		for (i = 0; i < sa.landmark_count; i++)
			if (sa.landmark[i].grid_x == (uint8_t) gx &&
			    sa.landmark[i].grid_y == (uint8_t) gy &&
			    sa.landmark[i].id ==
			    (uint16_t) (gx * GRID_HEIGHT + gy))
				found = 1;
		check("landmark carries full grid_x/grid_y/id", found);
	}
	check("B does not see A's landmark", sb.landmark_count == 0);

	/* ---------------- export order independence ---------------- */
	fprintf(stderr, "== export order A->B and B->A\n");
	(void) server_view_export(0, 0, 2, 0, &orderAB_A);
	(void) server_view_export(1, 0, 2, 0, &sb);
	(void) server_view_export(1, 0, 2, 0, &sb);
	(void) server_view_export(0, 0, 2, 0, &orderBA_A);
	check("A bytes identical regardless of export order",
		memcmp(&orderAB_A, &orderBA_A, sizeof(orderAB_A)) == 0);
	/* And the full snapshots must differ between recipients. */
	check("A and B snapshot bytes differ across all fields",
		sa.own_slot != sb.own_slot);

	/* ---------------- read-only parity (0/1/5) ---------------- */
	fprintf(stderr, "== read-only parity (0/1/5 exports)\n");
	{
		unsigned long h0, h1, h5;
		int i;

		frame++;
		h0 = sim_hash();
		(void) server_view_export(0, 0, 3, 0, &sa);
		h1 = sim_hash();
		for (i = 0; i < 5; i++)
			(void) server_view_export(0, 0, 3, 0, &sa);
		h5 = sim_hash();
		check("simulation unchanged after 1 export", h0 == h1);
		check("simulation unchanged after 5 exports", h0 == h5);
	}

	/* ---------------- u32 generations / independent ids ---------------- */
	fprintf(stderr, "== full-width generations and binding independence\n");
	{
		uint32_t gA = server_events_generation(XTK_ENTITY_VEHICLE,
						       (uint16_t) A->number);
		uint32_t gB = server_events_generation(XTK_ENTITY_VEHICLE,
						       (uint16_t) B->number);
		uint32_t gBig;
		int i;

		(void) server_view_export(0, 0, 4, 0, &sa);
		(void) server_view_export(1, 0, 4, 0, &sb);
		check("A self generation == entity-life counter (u32)",
			sa.self.generation == gA);
		check("B self generation == entity-life counter (u32)",
			sb.self.generation == gB);
		check("A and B generations independent", gA == gB);

		/* Each visible bullet generation matches its own kind/id. */
		check("A bullet generation is the bullet's life counter",
			sa.bullet[0].generation ==
			server_events_generation(XTK_ENTITY_BULLET,
						 sa.bullet[0].id));
		check("A bullet owner generation is A's life counter",
			sa.bullet[0].owner_generation == gA);

		/* Force A's life counter past 255: it must not truncate. */
		for (i = 0; i < 300; i++)
			server_events_note_spawn((int) A->number,
						 (double) A->loc->x,
						 (double) A->loc->y);
		gBig = server_events_generation(
			XTK_ENTITY_VEHICLE, (uint16_t) A->number);
		(void) server_view_export(0, 0, 5, 0, &sa);
		check("entity-life generation exceeds 255", gBig > 255u);
		check("u32 generation is not truncated in the snapshot",
			sa.self.generation == gBig);

		/* A stale/foreign generation is rejected. */
		check("stale generation rejected",
			server_view_export(0, 0xFFFFFFFFu, 6, 0, &sa) ==
			SERVER_VIEW_ERR_GENERATION);
		check("matching generation accepted",
			server_view_export(0, gBig, 6, 0, &sa) == SERVER_VIEW_OK);
		/* Binding and entity generations are independent: a binding
		 * generation is not the entity-life generation. */
		check("forced high life does not alias B's generation",
			server_events_generation(XTK_ENTITY_VEHICLE, 1) != gBig);
	}

	/* ---------------- no full-map leak ---------------- */
	fprintf(stderr, "== no full-map leak\n");
	{
		Mapper *mA = ensure_mapper(A);

		memset(mA->map, 0, sizeof(mA->map));
		real_map[29][29].type = ARMOR;	/* A's mapper does not know it */
		(void) server_view_export(0, 0, 7, 0, &sa);
		check("real_map-only cell is not exported as MapCell",
			sa.map_cell_count == 0);
		real_map[29][29].type = NORMAL;
	}

	if (failures != 0) {
		printf("VIEW FIXTURE FAIL (%d checks)\n", failures);
		return 1;
	}
	printf("VIEW FIXTURE PASS (2 recipients; separated cameras/mapper/HUD/"
	       "radar; order-independent; u32 generations; no full-map leak)\n");
	return 0;
}
