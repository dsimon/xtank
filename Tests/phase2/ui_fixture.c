/*
 * Tests/phase2/ui_fixture.c -- P2.13 HUD/map/radar/menu acceptance.
 *
 * Standalone, SDL-free fixture for the client presentation modules.  It feeds
 * deterministic, already-authorized protocol-v1 DTOs into hud.c/map.c/radar.c/
 * menu.c and asserts the resulting presentation scene:
 *
 *   1. HUD shows exactly the authorized SelfState values (armor by side, fuel,
 *      heat, score, ammo, weapon enabled/reload) and never a player name;
 *   2. the mapper draws only authorized known MapCell knowledge (including a
 *      stale remembered wall), with the own marker and no unexplored cell;
 *   3. the radar draws anonymous blips whose size follows the legacy
 *      acquisition age, drops expired contacts and never names a contact;
 *   4. the lobby reduces WELCOME/CONTENT_CATALOG/CONFIG_STATUS to catalog
 *      names, reproduces the frozen reference menu texts/layout and selection;
 *   5. the results/banner come from the server RESULTS (three named scores and
 *      a winner), and RESET clears the retained results only;
 *   6. every run is byte-identical.
 *
 * Authority: docs/PHASE2-PLAN.md P2.13; docs/VIEW-v1.md; docs/PROTOCOL-v1.md.
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wire.h"
#include "hud.h"
#include "map.h"
#include "radar.h"
#include "menu.h"

static int checks, failures;
static const char *g_fixtures = ".";

static void check(int ok, const char *what)
{
	++checks;
	if (!ok) {
		++failures;
		printf("FAIL: %s\n", what);
	}
}

static void checkf(int ok, const char *fmt, ...)
{
	char buf[512];
	va_list ap;

	++checks;
	if (!ok) {
		++failures;
		va_start(ap, fmt);
		vsnprintf(buf, sizeof(buf), fmt, ap);
		va_end(ap);
		printf("FAIL: %s\n", buf);
	}
}

/* ---- golden fixture loading ------------------------------------------ */

static int hexval(int c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

static int load_hex(const char *dir, const char *name, uint8_t *out,
		    size_t cap, size_t *out_len)
{
	char path[1024];
	FILE *f;
	int c, hi = -1, v;
	size_t n = 0;

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	f = fopen(path, "rb");
	if (f == NULL) {
		return 0;
	}
	while ((c = fgetc(f)) != EOF) {
		if (c == '#') {
			while ((c = fgetc(f)) != EOF && c != '\n') {
			}
			continue;
		}
		v = hexval(c);
		if (v < 0) {
			continue;
		}
		if (hi < 0) {
			hi = v;
		} else {
			if (n >= cap) {
				fclose(f);
				return 0;
			}
			out[n++] = (uint8_t)((hi << 4) | v);
			hi = -1;
		}
	}
	fclose(f);
	*out_len = n;
	return 1;
}

/* Load a committed golden fixture from the supplied directory or a fallback. */
static int load_named(const char *name, uint8_t *out, size_t cap,
		      size_t *out_len)
{
	static const char *dirs[] = {
		NULL, ".", "fixtures/protocol", "../Tests/phase2/fixtures/protocol"
	};
	int i;

	dirs[0] = g_fixtures;
	for (i = 0; i < (int)(sizeof(dirs) / sizeof(dirs[0])); i++) {
		if (dirs[i] != NULL &&
		    load_hex(dirs[i], name, out, cap, out_len)) {
			return 1;
		}
	}
	return 0;
}

/* ---- HUD ------------------------------------------------------------- */

static void test_hud(void)
{
	XtkSelfState self;
	XtkHudInfo info;
	XtkUiScene s;

	memset(&self, 0, sizeof(self));
	self.slot = 0;
	self.generation = 2;
	self.lifecycle_flags = XTK_SELF_ALIVE;
	self.armor[0] = 111;
	self.armor[1] = 222;
	self.armor[2] = 333;
	self.armor[3] = 444;
	self.fuel = 555;
	self.heat = 42;
	self.score = 1234;
	self.ammo[0] = 10;
	self.ammo[1] = 20;
	self.ammo[2] = 30;
	self.weapon_enabled_bitmap = 0x07;
	self.weapon_count = 3;
	self.reload[0] = 5;
	self.reload[1] = 6;
	self.reload[2] = 7;

	memset(&info, 0, sizeof(info));
	info.connected = 1;

	check(hud_build(&self, 1, &info, &s) == 0, "hud_build ok");
	check(ui_scene_has_text(&s, "Armor"), "HUD has Armor label");
	check(ui_scene_has_text(&s, "Fuel"), "HUD has Fuel label");
	check(ui_scene_has_text(&s, "Heat"), "HUD has Heat label");
	check(ui_scene_has_text(&s, "Score"), "HUD has Score label");
	check(ui_scene_has_text(&s, "111"), "HUD armor front = 111");
	check(ui_scene_has_text(&s, "222"), "HUD armor back = 222");
	check(ui_scene_has_text(&s, "333"), "HUD armor left = 333");
	check(ui_scene_has_text(&s, "444"), "HUD armor right = 444");
	check(ui_scene_has_text(&s, "555"), "HUD fuel = 555");
	check(ui_scene_has_text(&s, "42"), "HUD heat = 42");
	check(ui_scene_has_text(&s, "1234"), "HUD score = 1234");
	check(ui_scene_has_text(&s, "10") && ui_scene_has_text(&s, "20") &&
	      ui_scene_has_text(&s, "30"), "HUD ammo 10/20/30");
	check(ui_scene_has_text(&s, "ONLINE"), "HUD weapon ONLINE status");
	check(ui_scene_has_text(&s, "Life: ALIVE"), "HUD life alive");
	check(!ui_scene_has_text(&s, "pilot"), "HUD has no player name");

	info.connected = 0;
	check(hud_build(&self, 1, &info, &s) == 0, "hud_build (disconnected)");
	check(ui_scene_has_text(&s, "CONNECTION LOST"),
	      "HUD shows connection status");

	/* Own death: not alive flag, no name, marker lines. */
	self.lifecycle_flags = 0;	/* all flags clear => dead */
	info.connected = 1;
	check(hud_build(&self, 1, &info, &s) == 0, "hud_build (dead)");
	check(ui_scene_has_text(&s, "Life: DEAD"), "HUD life dead");
	check(s.line_count >= 2, "HUD death marker lines");
	check(!ui_scene_has_text(&s, "pilot"), "HUD (dead) has no name");

	/* Weapon status vocabulary. */
	check(strcmp(hud_weapon_status(&self, 0), "ONLINE") == 0,
	      "weapon 0 ONLINE");
	self.ammo[1] = 0;
	check(strcmp(hud_weapon_status(&self, 1), "EMPTY") == 0,
	      "weapon 1 EMPTY");
	self.weapon_enabled_bitmap = 0x05;
	check(strcmp(hud_weapon_status(&self, 1), "offline") == 0,
	      "weapon 1 offline");
}

/* ---- map ------------------------------------------------------------- */

static void test_map(void)
{
	XtkViewSnapshot v;
	XtkUiScene s;
	int px, py, n;

	memset(&v, 0, sizeof(v));
	v.server_tick = 100;
	v.own_slot = 0;
	v.vehicle_count = 1;
	v.vehicle[0].slot = 0;
	v.vehicle[0].x = 192.0;	/* grid 1 */
	v.vehicle[0].y = 384.0;	/* grid 2 */
	v.map_cell_count = 4;
	/* Known cell with north+west wall. */
	v.cell[0].grid_x = 1;
	v.cell[0].grid_y = 2;
	v.cell[0].flags = XTK_MAPCELL_KNOWN | XTK_MAPCELL_NORTH_WALL |
			  XTK_MAPCELL_WEST_WALL;
	v.cell[0].type = 0;
	/* Known destination wall (stale remembered). */
	v.cell[1].grid_x = 3;
	v.cell[1].grid_y = 4;
	v.cell[1].flags = XTK_MAPCELL_KNOWN | XTK_MAPCELL_NORTH_WALL |
			  XTK_MAPCELL_NORTH_DEST;
	/* Known team cell with a landmark type. */
	v.cell[2].grid_x = 5;
	v.cell[2].grid_y = 6;
	v.cell[2].flags = XTK_MAPCELL_KNOWN | XTK_MAPCELL_TEAM_KNOWN;
	v.cell[2].type = 3;
	/* Unexplored cell: no KNOWN bit => must be omitted. */
	v.cell[3].grid_x = 7;
	v.cell[3].grid_y = 8;
	v.cell[3].flags = XTK_MAPCELL_NORTH_WALL;	/* not "known" */
	v.cell[3].type = 0;

	n = map_build(&v, &s);
	checkf(n == 3, "map drew 3 known cells (got %d)", n);
	check(s.line_count >= 4, "map drew wall lines");
	check(s.rect_count >= 2, "map drew own marker + landmark");

	map_cell_panel_pos(1, 2, &px, &py);
	checkf(px == XTK_UI_MAP_X + (1 * 9 - 13) &&
	       py == XTK_UI_MAP_Y + (2 * 9 - 13),
	       "map cell (1,2) panel pos (%d,%d)", px, py);

	/* Own marker follows the authorized displayed position. */
	check(map_own_marker(&v, &px, &py) == 1, "map own marker present");
	{
		int ex = XTK_UI_MAP_X + (1 * 9 - 13);
		int ey = XTK_UI_MAP_Y + (2 * 9 - 13);
		checkf(px == ex && py == ey, "own marker at own grid (%d,%d)",
		       px, py);
	}

	/* A dead own player is absent from VehicleView => no marker. */
	v.vehicle_count = 0;
	check(map_own_marker(&v, &px, &py) == 0,
	      "no marker when own vehicle hidden");

	/* No known cells => only the background and any marker. */
	v.map_cell_count = 0;
	v.vehicle_count = 1;
	n = map_build(&v, &s);
	checkf(n == 0, "empty mapper draws no knowledge (got %d)", n);
}

/* ---- radar ----------------------------------------------------------- */

static void test_radar(void)
{
	XtkViewSnapshot v;
	XtkUiScene s;
	XtkRadarBlip b;
	int n, px, py;

	check(radar_blip_size(0) == 6, "blip age 0 -> size 6");
	check(radar_blip_size(2) == 6, "blip age 2 -> size 6");
	check(radar_blip_size(3) == 5, "blip age 3 -> size 5");
	check(radar_blip_size(6) == 4, "blip age 6 -> size 4");
	check(radar_blip_size(9) == 3, "blip age 9 -> size 3");
	check(radar_blip_size(12) == 2, "blip age 12 -> size 2");
	check(radar_blip_size(15) == 1, "blip age 15 -> size 1");
	check(radar_blip_size(23) == 0, "blip age 23 expired");
	check(radar_blip_size(255) == 0, "blip age 255 expired");

	memset(&v, 0, sizeof(v));
	v.server_tick = 7;
	v.own_slot = 0;
	v.vehicle_count = 1;
	v.vehicle[0].slot = 0;
	v.vehicle[0].x = 192.0;
	v.vehicle[0].y = 192.0;
	v.radar_blip_count = 3;
	v.radar[0].x = 5;
	v.radar[0].y = 5;
	v.radar[0].age = 0;
	v.radar[0].classification = 0;	/* anonymous */
	v.radar[1].x = 50;
	v.radar[1].y = 50;
	v.radar[1].age = 12;
	v.radar[1].classification = 0;
	v.radar[2].x = 80;
	v.radar[2].y = 80;
	v.radar[2].age = 23;		/* expired */
	v.radar[2].classification = 0;

	n = radar_build(&v, &s);
	checkf(n == 2, "radar drew 2 live blips (got %d)", n);
	check(s.line_count >= 1, "radar drew the sweep");
	check(s.rect_count >= 3, "radar drew background + 2 blips");

	b.x = 5;
	b.y = 5;
	b.age = 0;
	radar_blip_panel_pos(&b, &px, &py);
	checkf(px == XTK_UI_MAP_X + 5 - 3, "blip panel x (%d)", px);

	/* Without an own vehicle there is no sweep. */
	v.vehicle_count = 0;
	radar_build(&v, &s);
	check(s.line_count == 0, "no sweep without own vehicle");
}

/* ---- menu lobby / results -------------------------------------------- */

static void send_catalog(const char *fixdir, XtkLobbyState *ls,
			 XtkResultsState *rs, int *ok)
{
	uint8_t buf[XTK_MAX_MESSAGE_BYTES];
	size_t len;
	XtkMessage msg;

	*ok = 0;
	(void)fixdir;
	if (!load_named("catalog.hex", buf, sizeof(buf), &len)) {
		return;
	}
	if (xtk_decode(buf, len, &msg) != XTK_ERR_NONE) {
		return;
	}
	check(menu_apply_message(ls, rs, &msg) == 1, "apply CONTENT_CATALOG");
	check(ls->num_mazes == 2, "catalog has 2 mazes");
	check(ls->num_vehicles == 1, "catalog has 1 vehicle");
	check(strcmp(ls->maze[0], "combat1") == 0, "maze 0 = combat1");
	check(strcmp(ls->maze[1], "Arena") == 0, "maze 1 = Arena");
	check(strcmp(ls->vehicle[0], "Vanguard") == 0, "vehicle 0 = Vanguard");
	*ok = 1;
}

static void test_menu(void)
{
	XtkLobbyState ls;
	XtkResultsState rs;
	XtkUiScene s;
	XtkMessage msg;
	uint8_t buf[XTK_MAX_MESSAGE_BYTES];
	size_t len;
	int i, ok, refcount;
	const char *const *ref;

	menu_lobby_init(&ls);
	menu_results_init(&rs);

	/* WELCOME makes the peer a connected admin/player. */
	memset(&msg, 0, sizeof(msg));
	msg.env.message_type = XTK_MSG_WELCOME;
	msg.body.welcome.role = 1;
	check(menu_apply_message(&ls, &rs, &msg) == 1, "apply WELCOME");
	check(ls.connected && ls.is_admin, "lobby connected + admin");

	/* Catalog from the committed golden bytes. */
	send_catalog(g_fixtures, &ls, &rs, &ok);
	check(ok, "catalog golden bytes applied");

	/* Selection validated against the received catalog. */
	check(menu_lobby_select_maze(&ls, 0x0102) == 0, "select Arena");
	check(strcmp(menu_lobby_maze_name(&ls, 0x0102), "Arena") == 0,
	      "lookup Arena name");
	check(menu_lobby_select_maze(&ls, 0xBEEF) == -1,
	      "reject unknown maze id");
	check(menu_lobby_select_vehicle(&ls, 0x0201) == 0, "select Vanguard");

	/* CONFIG_STATUS echoes the accepted selection. */
	memset(&msg, 0, sizeof(msg));
	msg.env.message_type = XTK_MSG_CONFIG_STATUS;
	msg.body.config_status.maze_id = 0x0102;
	msg.body.config_status.vehicle_id = 0x0201;
	msg.body.config_status.status = 0;
	check(menu_apply_message(&ls, &rs, &msg) == 1, "apply CONFIG_STATUS");
	check(ls.have_config && ls.config_status == 0, "config accepted");

	/* Frozen reference main-menu texts and layout. */
	ref = menu_reference_main_entries(&refcount);
	check(refcount == 11, "reference main menu has 11 entries");
	check(strcmp(ref[0], "Play") == 0, "reference entry 0 = Play");
	check(strcmp(ref[10], "Quit") == 0, "reference entry 10 = Quit");
	check(menu_build_lobby(&ls, &s) == 0, "menu_build_lobby ok");
	check(ui_scene_has_text(&s, "XTANK"), "lobby title XTANK");
	for (i = 0; i < refcount; i++) {
		checkf(ui_scene_has_text(&s, ref[i]),
		       "lobby shows reference entry '%s'", ref[i]);
	}
	check(ui_scene_has_text(&s, "Mazes"), "lobby has Mazes menu");
	check(ui_scene_has_text(&s, "Vehicles"), "lobby has Vehicles menu");
	check(ui_scene_has_text(&s, "combat1"), "lobby shows combat1");
	check(ui_scene_has_text(&s, "Arena"), "lobby shows Arena");
	check(ui_scene_has_text(&s, "Vanguard"), "lobby shows Vanguard");
	check(ui_scene_has_text(&s, "Admin"), "lobby shows Admin status");

	/* The first reference entry is at the frozen LEV0 position. */
	{
		const XtkUiText *t = NULL;
		for (i = 0; i < ui_scene_text_count(&s); i++) {
			const XtkUiText *c = ui_scene_text(&s, i);
			if (c != NULL && strcmp(c->text, "Play") == 0) {
				t = c;
				break;
			}
		}
		check(t != NULL, "lobby contains Play");
		if (t != NULL) {
			checkf(t->x == XTK_UI_LEV0_X && t->y == XTK_UI_LEV0_Y,
			       "Play at LEV0 (%d,%d)", t->x, t->y);
		}
	}

	/* Results from the golden RESULTS bytes (2 named scores). */
	if (load_named("results.hex", buf, sizeof(buf), &len)) {
		check(xtk_decode(buf, len, &msg) == XTK_ERR_NONE,
		      "decode results.hex");
		check(menu_apply_message(&ls, &rs, &msg) == 1, "apply RESULTS");
		check(rs.have_results, "results retained");
		check(rs.results.vehicle_count == 2, "results 2 entries");
		check(menu_build_results(&rs, &s) == 0, "menu_build_results");
		check(ui_scene_has_text(&s, "pilot"), "results show pilot");
		check(ui_scene_has_text(&s, "drone"), "results show drone");
		check(ui_scene_has_text(&s, "1000"), "results show 1000");
		check(ui_scene_has_text(&s, "100"), "results show 100");
		check(ui_scene_has_text(&s, "Current score:"),
		      "results current-score header");
		check(ui_scene_has_text(&s, "Battle won by pilot"),
		      "banner names the winner");
	} else {
		check(0, "results.hex not found");
	}

	/* Three-or-more player scores with names (release maximum 4). */
	menu_results_init(&rs);
	rs.have_results = 1;
	rs.results.end_tick = 500;
	rs.results.result_kind = 0;
	rs.results.winner_slot = 2;
	rs.results.vehicle_count = 3;
	strcpy(rs.results.entry[0].name, "alpha");
	rs.results.entry[0].slot = 0;
	rs.results.entry[0].score = 300;
	strcpy(rs.results.entry[1].name, "bravo");
	rs.results.entry[1].slot = 1;
	rs.results.entry[1].score = 450;
	strcpy(rs.results.entry[2].name, "charlie");
	rs.results.entry[2].slot = 2;
	rs.results.entry[2].score = 1000;
	check(menu_build_results(&rs, &s) == 0, "3-player results build");
	check(ui_scene_has_text(&s, "alpha: 300 points"), "alpha score");
	check(ui_scene_has_text(&s, "bravo: 450 points"), "bravo score");
	check(ui_scene_has_text(&s, "charlie: 1000 points"), "charlie score");
	check(ui_scene_has_text(&s, "Battle won by charlie"),
	      "3-player banner winner");

	/* Abort banner and RESET clearing. */
	rs.results.result_kind = 1;
	menu_banner_text(&rs.results, (char *)buf, sizeof(buf));
	check(strcmp((char *)buf, "Match aborted") == 0, "abort banner");
	memset(&msg, 0, sizeof(msg));
	msg.env.message_type = XTK_MSG_RESET;
	check(menu_apply_message(&ls, &rs, &msg) == 1, "apply RESET");
	check(!rs.have_results, "RESET clears results");
	check(ls.have_catalog, "RESET keeps the catalog");

	/* REJECT surfaces an actionable error without dropping the lobby. */
	memset(&msg, 0, sizeof(msg));
	msg.env.message_type = XTK_MSG_REJECT;
	msg.body.reject.error_code = XTK_ERR_CONTENT_MISMATCH;
	strcpy(msg.body.reject.text, "content mismatch");
	check(menu_apply_message(&ls, &rs, &msg) == 1, "apply REJECT");
	check(ls.have_error &&
	      ls.last_error == XTK_ERR_CONTENT_MISMATCH,
	      "lobby records the rejection");

	/* EPOCH_BEGIN clears results for the new epoch. */
	rs.have_results = 1;
	memset(&msg, 0, sizeof(msg));
	msg.env.message_type = XTK_MSG_EPOCH_BEGIN;
	msg.body.epoch_begin.epoch = 2;
	check(menu_apply_message(&ls, &rs, &msg) == 1, "apply EPOCH_BEGIN");
	check(!rs.have_results, "EPOCH_BEGIN clears results");
}

int main(int argc, char **argv)
{
	if (argc > 1) {
		g_fixtures = argv[1];
	}

	printf("== P2.13 UI fixture\n");
	test_hud();
	test_map();
	test_radar();
	test_menu();

	printf("checks=%d failures=%d\n", checks, failures);
	if (failures == 0) {
		printf("P2.13 UI FIXTURE PASS\n");
		return 0;
	}
	printf("P2.13 UI FIXTURE FAIL\n");
	return 1;
}
