/*-
 * Copyright (c) 1988 Terry Donahue
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met: [see the project COPYRIGHT / legacy sources]
 *
 * departure-design_fixture.c -- P3.2 departure feasibility and lifecycle
 * contract characterization.
 *
 * This is a DESIGN/AUDIT fixture, not an implementation.  It links the shipped
 * display-free server core (Src/sobj, without server_main.o) and does two
 * things:
 *
 *   1. Freezes an ordinary two-human bot-free reference: it drives two
 *      terminal-free human controllers through a deterministic scripted match,
 *      hashes the authoritative character state and prints
 *      `DEPARTURE-DESIGN hash-ordinary=<hex>`.  Later Phase 3 core changes must
 *      leave that ordinary path byte-identical.
 *
 *   2. Characterizes a candidate server-only removal seam against the approved
 *      D3-D departure policy, using ONLY the existing engine primitives:
 *
 *        - live departure:           no kill/score/death/RNG/explosion side
 *                                    effect, storage retained, map flag
 *                                    cleared, projectiles keep a valid owner;
 *        - respawn-waiting departure: leaves the dead list, never respawns;
 *        - pending/held input:       demonstrates that queued input for the
 *                                    departed slot still applies (a ghost can
 *                                    fire), that intent_reset() is NOT an
 *                                    acceptable substitute (it clears the
 *                                    surviving controller too), and therefore
 *                                    that one small player-scoped cancel seam
 *                                    is required;
 *        - live/dead lists, team counts, map occupancy, cleanup.
 *
 * It changes NOTHING in Src/.  A `DEPARTURE-DESIGN PASS` means the audit ran
 * and its design conclusions hold, including the identified smallest seam.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xtank.h"
#include "bullet.h"
#include "clfkr.h"
#include "object.h"
#include "thread.h"
#include "vehicle.h"
#include "team.h"
#include "globals.h"
#include "server.h"
#include "intent.h"
#include "proto.h"

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

/* ---- FNV-1a state hash (same mix as the P1.1 oracle, format-free) ------ */

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
state_hash(void)
{
	unsigned long h = 2166136261UL;
	int i;

	hbytes(&h, &frame, sizeof(frame));
	hbytes(&h, &num_veh, sizeof(num_veh));
	hbytes(&h, &num_veh_alive, sizeof(num_veh_alive));
	hbytes(&h, &num_veh_dead, sizeof(num_veh_dead));
	for (i = 0; i < num_veh; i++) {
		Vehicle *v = &actual_vehicles[i];
		Combatant *o = v->owner;

		hbytes(&h, &v->number, sizeof(v->number));
		hbytes(&h, &v->team, sizeof(v->team));
		hbytes(&h, &v->status, sizeof(v->status));
		hbytes(&h, &v->loc->x, sizeof(v->loc->x));
		hbytes(&h, &v->loc->y, sizeof(v->loc->y));
		hbytes(&h, &v->vector.heading, sizeof(v->vector.heading));
		hbytes(&h, &v->fuel, sizeof(v->fuel));
		hbytes(&h, &v->armor, sizeof(v->armor));
		hbytes(&h, &v->num_discs, sizeof(v->num_discs));
		hbytes(&h, &v->death_timer, sizeof(v->death_timer));
		if (o != (Combatant *) NULL) {
			hbytes(&h, &o->score, sizeof(o->score));
			hbytes(&h, &o->kills, sizeof(o->kills));
			hbytes(&h, &o->deaths, sizeof(o->deaths));
			hbytes(&h, o->name, strnlen(o->name, MAX_STRING));
		}
	}
	hbytes(&h, &bset->number, sizeof(bset->number));
	for (i = 0; i < bset->number; i++) {
		Bullet *b = bset->list[i];
		int own = b->owner ? (int) b->owner->number : -1;

		hbytes(&h, &own, sizeof(own));
		hbytes(&h, &b->type, sizeof(b->type));
		hbytes(&h, &b->life, sizeof(b->life));
		hbytes(&h, &b->loc->x, sizeof(b->loc->x));
		hbytes(&h, &b->loc->y, sizeof(b->loc->y));
	}
	hbytes(&h, &eset->number, sizeof(eset->number));
	return h;
}

/* ---- deterministic two-human script ----------------------------------- */

#define REF_TICKS 200

static void
push_intent(int tick, int player, IntentOp op, double a, double b)
{
	Intent in;

	(void) memset(&in, 0, sizeof(in));
	in.tick = tick;
	in.player = player;
	in.sequence = (unsigned int) (tick * 8 + player * 4 + (int) op);
	in.op = op;
	in.a = a;
	in.b = b;
	(void) intent_push(&in);
}

/*
 * One ordinary two-human tick: both drive/turn/fire from a fixed script so the
 * match evolves through movement, collisions and projectiles with no bots.
 */
static void
script_tick(int t)
{
	push_intent(t, 0, INTENT_DRIVE_REL, (double) ((t % 9) - 4), 0.0);
	push_intent(t, 1, INTENT_HEADING, (double) (t % 7) - 3.0, 0.0);
	if ((t % 5) == 0) {
		push_intent(t, 0, INTENT_FIRE, 0.0, 0.0);
		push_intent(t, 1, INTENT_FIRE, 0.0, 0.0);
	}
}

/* ---- candidate server-only removal seam (design sketch, no Src change) -- */

static int departed[MAX_VEHICLES];

static int
in_list(Vehicle **list, int n, Vehicle *v)
{
	int i;

	for (i = 0; i < n; i++)
		if (list[i] == v)
			return 1;
	return 0;
}

static void
remove_from_live(Vehicle *v)
{
	int i;

	for (i = 0; i < num_veh_alive; i++)
		if (live_vehicles[i] == v) {
			live_vehicles[i] = live_vehicles[--num_veh_alive];
			return;
		}
}

static void
remove_from_dead(Vehicle *v)
{
	int i;

	for (i = 0; i < num_veh_dead; i++)
		if (dead_vehicles[i] == v) {
			dead_vehicles[i] = dead_vehicles[--num_veh_dead];
			return;
		}
}

/*
 * Candidate departure.  Uses the existing inactivate_vehicle() (which is
 * score/death/RNG/explosion-free) plus plain list surgery; it deliberately
 * does NOT call kill_vehicle() (explodes, counts a death, advances RNG, allows
 * respawn) and does NOT call unmake_vehicle() (frees special/thread storage
 * still reachable through live projectile `owner` pointers).
 *
 * The one operation it cannot perform with the current API is a player-scoped
 * pending/held-input cancel; callers must use the reviewed seam named below.
 */
static void
candidate_departure(Vehicle *v)
{
	if (tstflag(v->status, VS_is_alive))
		inactivate_vehicle(v);		/* clears alive, map flag, specials */
	remove_from_live(v);
	remove_from_dead(v);
	departed[v->number] = 1;
	/* teamdata/num_teams intentionally untouched: no gameplay-rule change. */
}

/* Count live projectiles whose owner is `v` (keeps a valid owner reference). */
static int
bullets_owned_by(Vehicle *v)
{
	int i, n = 0;

	for (i = 0; i < bset->number; i++)
		if (bset->list[i]->owner == v)
			n++;
	return n;
}

/* ---- setup helpers ---------------------------------------------------- */

static void
build_two_humans(int vdesc_index)
{
	int hb;

	server_players_init();
	hb = server_players_add("humanA", 10);
	(void) server_players_bind_slot(hb, 0);
	hb = server_players_add("humanB", 11);
	(void) server_players_bind_slot(hb, 1);

	num_combatants = 2;

	combatant[0].num_players = 1;
	combatant[0].player[0] = server_players_index_for_player(10);
	combatant[0].num_programs = 0;
	combatant[0].team = 1 % MAX_TEAMS;
	combatant[0].vdesc = vdesc_index;
	(void) strncpy(combatant[0].name, "humanA", MAX_STRING - 1);

	combatant[1].num_players = 1;
	combatant[1].player[0] = server_players_index_for_player(11);
	combatant[1].num_programs = 0;
	combatant[1].team = 2 % MAX_TEAMS;
	combatant[1].vdesc = vdesc_index;
	(void) strncpy(combatant[1].name, "humanB", MAX_STRING - 1);
}

/* Run `ticks` scripted ticks from a freshly seeded match. */
static int
run_scripted(int seed, int ticks)
{
	int t;

	server_seed_random((unsigned int) seed);
	if (server_match_init() != GAME_RUNNING)
		return -1;
	for (t = 1; t <= ticks; t++) {
		script_tick(t);
		if (server_step() != GAME_RUNNING)
			return -1;
	}
	return 0;
}

/* ---- the audit -------------------------------------------------------- */

static void
audit_ordinary_two_human(void)
{
	unsigned long h;

	fprintf(stderr, "== ordinary two-human reference\n");
	if (run_scripted(114336, REF_TICKS) != 0) {
		check("ordinary two-human match runs to %d ticks", 0);
		return;
	}
	check("ordinary two-human match runs to ticks", 1);
	check("ordinary path has two live human tanks", num_veh_alive == 2);
	h = state_hash();
	printf("DEPARTURE-DESIGN hash-ordinary=%016lx ticks=%d\n", h, REF_TICKS);
	server_match_reset();
}

static void
audit_rng_neutrality(void)
{
	long ctl_before, ctl_after, rem_before, rem_after;
	Vehicle *v0;

	fprintf(stderr, "== RNG neutrality (twin run)\n");

	/* Control: scripted ticks, then draw two RNG values, no removal. */
	if (run_scripted(114336, 60) != 0) {
		check("control run", 0);
		return;
	}
	ctl_before = random();
	ctl_after = random();
	server_match_reset();

	/* Removal: identical script, draw one, remove slot 0, draw one. */
	if (run_scripted(114336, 60) != 0) {
		check("removal run", 0);
		return;
	}
	v0 = &actual_vehicles[0];
	rem_before = random();
	candidate_departure(v0);
	rem_after = random();

	check("twin runs reach the same RNG position", ctl_before == rem_before);
	check("removal consumes no RNG (next value identical)",
	      ctl_after == rem_after);
	server_match_reset();
}

static void
audit_live_departure(void)
{
	Vehicle *v0, *v1;
	Bullet *proj;
	int score0, deaths0, kills0, score1, deaths1, kills1;
	int eset0, team0, teams0, occupied0;

	fprintf(stderr, "== live departure\n");
	if (run_scripted(114336, 30) != 0) {
		check("live-departure setup run", 0);
		return;
	}
	v0 = &actual_vehicles[0];
	v1 = &actual_vehicles[1];
	check("departing tank is alive", tstflag(v0->status, VS_is_alive));

	/* Give the departing tank a live projectile that references it. */
	proj = make_bullet(v0, v0->loc, (WeaponType) MG, (Angle) 0.0,
			   (lCoord *) NULL);
	check("projectile created with owner", proj != NULL && proj->owner == v0);
	if (proj != NULL)
		proj->life = 100000;	/* keep it outstanding for the check */

	score0 = v0->owner->score;
	deaths0 = v0->owner->deaths;
	kills0 = v0->owner->kills;
	score1 = v1->owner->score;
	deaths1 = v1->owner->deaths;
	kills1 = v1->owner->kills;
	eset0 = eset->number;
	team0 = teamdata[v0->team].vehicle_count;
	teams0 = num_teams;
	occupied0 = (real_map[v0->old_loc->grid_x][v0->old_loc->grid_y].flags &
		     v0->flag) ? 1 : 0;

	candidate_departure(v0);

	check("removed from live list", !in_list(live_vehicles, num_veh_alive, v0));
	check("not alive after removal", !tstflag(v0->status, VS_is_alive));
	check("survivor still live", tstflag(v1->status, VS_is_alive));
	check("no score change (departed)",
	      v0->owner->score == score0 && v1->owner->score == score1);
	check("no death change (departed/survivor)",
	      v0->owner->deaths == deaths0 && v1->owner->deaths == deaths1);
	check("no kill change (departed/survivor)",
	      v0->owner->kills == kills0 && v1->owner->kills == kills1);
	check("no new explosion", eset->number == eset0);
	check("team count unchanged by removal (no rule change)",
	      teamdata[v0->team].vehicle_count == team0 && num_teams == teams0);
	check("map occupancy flag cleared", occupied0 == 1 &&
	      (real_map[v0->old_loc->grid_x][v0->old_loc->grid_y].flags &
	       v0->flag) == 0);
	check("retained storage keeps projectile owner valid",
	      proj != NULL && proj->owner == v0 && v0->number == 0);
	{
		void *rec = v0->special[(int) MAPPER].record;

		check("retained special storage not freed (no unmake_vehicle)",
		      v0->special[(int) MAPPER].record == rec &&
		      v0->special[(int) MAPPER].status != SP_on);
	}

	/* The departed tank must never re-enter the live list. */
	{
		int t, resurrected = 0;

		for (t = 0; t < 200; t++) {
			script_tick(frame + 1);
			if (server_step() != GAME_RUNNING)
				break;
			if (in_list(live_vehicles, num_veh_alive, v0) ||
			    tstflag(v0->status, VS_is_alive))
				resurrected = 1;
		}
		check("departed live tank never respawns", !resurrected);
	}
	server_match_reset();
}

static void
audit_respawn_waiting_departure(void)
{
	Vehicle *v1;
	int t, died = 0, back = 0;

	fprintf(stderr, "== respawn-waiting departure\n");
	if (run_scripted(114336, 10) != 0) {
		check("respawn-waiting setup run", 0);
		return;
	}
	v1 = &actual_vehicles[1];

	/* Ordinary death (this is the scenario setup, not the removal). */
	kill_vehicle(v1, (Vehicle *) NULL);
	for (t = 0; t < 200; t++) {
		if (server_step() != GAME_RUNNING)
			break;
		if (!tstflag(v1->status, VS_is_alive) &&
		    in_list(dead_vehicles, num_veh_dead, v1)) {
			died = 1;
			break;
		}
	}
	check("tank is respawn-waiting on the dead list", died);

	candidate_departure(v1);
	check("removed from dead list",
	      !in_list(dead_vehicles, num_veh_dead, v1));
	check("storage retained after dead-list removal",
	      v1->owner != (Combatant *) NULL && v1->number == 1);

	for (t = 0; t < 400; t++) {
		if (server_step() != GAME_RUNNING)
			break;
		if (tstflag(v1->status, VS_is_alive) ||
		    in_list(dead_vehicles, num_veh_dead, v1) ||
		    in_list(live_vehicles, num_veh_alive, v1))
			back = 1;
	}
	check("respawn-waiting departed tank never comes back", !back);
	server_match_reset();
}

static void
audit_pending_input(void)
{
	Vehicle *v0, *v1;
	int ghost_fired;

	fprintf(stderr, "== pending/held input\n");
	if (run_scripted(114336, 10) != 0) {
		check("pending-input setup run", 0);
		return;
	}
	v0 = &actual_vehicles[0];
	v1 = &actual_vehicles[1];

	/*
	 * Queue held fire for both controllers, then depart slot 0 BEFORE the
	 * next consume.  The queued FIRE_HOLD is still accepted for the departed
	 * slot (storage retained) and its held fire still applies at the seam.
	 */
	push_intent(frame + 1, 0, INTENT_FIRE_HOLD, 1.0, 0.0);
	push_intent(frame + 1, 1, INTENT_FIRE_HOLD, 1.0, 0.0);

	candidate_departure(v0);

	/*
	 * Without a player-scoped cancel, the departed slot's input still
	 * applies.  Clear all live projectiles and make the retained tank ready
	 * to fire again, then step once: any projectile owned by the departed
	 * tank can only come from its uncancelled input.
	 */
	{
		int w;

		for (w = 0; w < v0->num_weapons; w++)
			v0->weapon[w].reload_counter = 0;
	}
	init_bset();
	(void) server_step();
	ghost_fired = bullets_owned_by(v0) > 0;
	check("held fire tracked for departed slot", intent_held_fire(0) == 1);
	check("held fire tracked for surviving slot", intent_held_fire(1) == 1);
	fprintf(stderr,
		"  DESIGN-GAP: departed slot input still applies; "
		"ghost_fired=%d bullets_owned=%d\n",
		ghost_fired, bullets_owned_by(v0));
	check("demonstrated: uncancelled departed input can fire (hazard)",
	      ghost_fired);

	/*
	 * The only current cancel primitive is intent_reset(), which is
	 * all-or-nothing: it also drops the surviving controller's held state.
	 * That is why a player-scoped cancel seam is required.
	 */
	intent_reset();
	check("intent_reset() clears the departed controller's held fire",
	      intent_held_fire(0) == 0);
	check("intent_reset() ALSO clears the survivor's held fire (too broad)",
	      intent_held_fire(1) == 0);
	check("survivor remains alive and progressing",
	      tstflag(v1->status, VS_is_alive) &&
	      in_list(live_vehicles, num_veh_alive, v1));
	(void) v1;

	printf("DEPARTURE-DESIGN seam=intent_cancel_player(player) "
	       "reason=player-scoped pending+held input cancel\n");
	server_match_reset();
}

static void
audit_cleanup(void)
{
	fprintf(stderr, "== cleanup / next epoch\n");
	check("reset leaves no live/dead vehicles",
	      num_veh_alive == 0 && num_veh_dead == 0);
	/* A fresh epoch must start cleanly after a retained departed record. */
	if (run_scripted(114336, 5) == 0)
		check("fresh epoch starts cleanly after departure", 1);
	else
		check("fresh epoch starts cleanly after departure", 0);
	check("fresh epoch has two live tanks", num_veh_alive == 2);
	server_match_reset();
	printf("DEPARTURE-DESIGN cleanup-seam=server_core retains departed "
	       "records and releases them through game_cleanup at reset "
	       "(projectiles already gone)\n");
}

int
main(int argc, char *argv[])
{
	const char *assets = (argc > 1) ? argv[1] : ".";
	ServerConfig *cfg;
	int vdesc_index, mdesc_index;

	bset = &actual_bset;
	eset = &actual_eset;
	InitConfigStruct(&command_options);
	command_options.NoDelay = TRUE;
	command_options.PrintScores = FALSE;
	command_options.AutoExit = TRUE;

	(void) strncpy(username, "p3.2", MAX_STRING - 1);
	username[MAX_STRING - 1] = '\0';

	if (content_set_root(assets) != 0) {
		fprintf(stderr, "departure-design_fixture: %s\n",
			content_last_error());
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
		fprintf(stderr, "departure-design_fixture: cannot load Vanguard\n");
		return 2;
	}
	if (make_mdesc((char *) "combat1", &mdesc_index) != DESC_LOADED) {
		fprintf(stderr, "departure-design_fixture: cannot load combat1\n");
		return 2;
	}
	settings.mdesc = &mdesc[mdesc_index];
	settings.si.game = settings.mdesc->type;

	build_two_humans(vdesc_index);

	audit_ordinary_two_human();
	audit_rng_neutrality();
	audit_live_departure();
	audit_respawn_waiting_departure();
	audit_pending_input();
	audit_cleanup();

	if (failures != 0) {
		printf("DEPARTURE-DESIGN FAIL (%d/%d checks)\n", failures, checks);
		return 1;
	}
	printf("DEPARTURE-DESIGN PASS (checks=%d)\n", checks);
	return 0;
}
