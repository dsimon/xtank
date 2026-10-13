/*
 * Client/menu.c -- lobby, results and banner presentation (P2.13).
 *
 * Authority: docs/PHASE2-PLAN.md P2.13; docs/PROTOCOL-v1.md sections 4.5-4.13;
 * Src/interface.c, Src/game.c (read-only references for the frozen texts).
 */

#include "menu.h"

#include <stdio.h>
#include <string.h>

/*
 * Frozen reference lobby main-menu item texts.  Copied verbatim from
 * Src/interface.c main_entries[]; the layout below places them at the legacy
 * LEV0_X/LEV0_Y with the L_FONT row height.  The release preset exposes the
 * subset the server allowlists, but the frozen text and order are preserved.
 */
static const char *const REF_MAIN_ENTRIES[] = {
	"Play", "Settings", "Combatants", "View", "Load",
	"Design", "Add players", "Text entry", "Remove", "Help", "Quit"
};
#define REF_MAIN_COUNT \
	((int)(sizeof(REF_MAIN_ENTRIES) / sizeof(REF_MAIN_ENTRIES[0])))

const char *const *menu_reference_main_entries(int *count)
{
	if (count != NULL) {
		*count = REF_MAIN_COUNT;
	}
	return REF_MAIN_ENTRIES;
}

void menu_lobby_init(XtkLobbyState *ls)
{
	if (ls == NULL) {
		return;
	}
	memset(ls, 0, sizeof(*ls));
}

void menu_results_init(XtkResultsState *rs)
{
	if (rs == NULL) {
		return;
	}
	memset(rs, 0, sizeof(*rs));
}

static void copy_name(char *dst, const char *src)
{
	size_t n;

	if (src == NULL) {
		dst[0] = '\0';
		return;
	}
	n = strlen(src);
	if (n > (size_t)XTK_MAX_NAME_BYTES) {
		n = (size_t)XTK_MAX_NAME_BYTES;
	}
	memcpy(dst, src, n);
	dst[n] = '\0';
}

static int catalog_add(char names[][XTK_MAX_NAME_BYTES + 1], uint16_t *ids,
		       int count, const char *name, uint16_t id)
{
	int i;

	for (i = 0; i < count; i++) {
		if (ids[i] == id) {
			return count;
		}
	}
	if (count >= (int)XTK_MAX_CATALOG_ENTRIES) {
		return count;
	}
	copy_name(names[count], name);
	ids[count] = id;
	return count + 1;
}

static const char *catalog_name(const char names[][XTK_MAX_NAME_BYTES + 1],
				const uint16_t *ids, int count, uint16_t id)
{
	int i;

	for (i = 0; i < count; i++) {
		if (ids[i] == id) {
			return names[i];
		}
	}
	return NULL;
}

int menu_apply_message(XtkLobbyState *ls, XtkResultsState *rs,
		       const XtkMessage *msg)
{
	int i;

	if (ls == NULL || msg == NULL) {
		return 0;
	}

	switch (msg->env.message_type) {
	case XTK_MSG_WELCOME:
		ls->connected = 1;
		/* The WELCOME role is observer/player only; it never grants
		 * administrator (P3.10).  Authority arrives in LOBBY_STATE. */
		ls->is_admin = 0;
		return 1;
	case XTK_MSG_LOBBY_STATE:
		ls->have_lobby_state = 1;
		ls->self_index = msg->body.lobby_state.self_index;
		ls->admin_index = msg->body.lobby_state.admin_index;
		ls->session_state = msg->body.lobby_state.session_state;
		ls->drone_count = msg->body.lobby_state.drone_count;
		ls->selected_drone_count =
			msg->body.lobby_state.drone_count;
		ls->is_admin =
			(msg->body.lobby_state.admin_index != 0xFF &&
			 msg->body.lobby_state.admin_index ==
			 msg->body.lobby_state.self_index) ? 1 : 0;
		/* D3-B: a config revision change wipes every peer's readiness. */
		if (msg->body.lobby_state.config_revision !=
		    ls->config_revision) {
			ls->ready = 0;
		}
		ls->config_revision = msg->body.lobby_state.config_revision;
		if (msg->body.lobby_state.maze_id != 0) {
			ls->selected_maze_id = msg->body.lobby_state.maze_id;
			ls->selected_vehicle_id =
				msg->body.lobby_state.vehicle_id;
		}
		{
			int i;
			int n = msg->body.lobby_state.human_count;
			if (n > (int)XTK_LOBBY_ROSTER_MAX) {
				n = (int)XTK_LOBBY_ROSTER_MAX;
			}
			ls->roster_count = n;
			for (i = 0; i < n; i++) {
				ls->roster[i] = msg->body.lobby_state.entry[i];
			}
			/* Readiness is server-authored per roster entry. */
			for (i = 0; i < n; i++) {
				if (i == (int)msg->body.lobby_state.self_index) {
					ls->ready =
						msg->body.lobby_state.entry[i]
							.ready ? 1 : 0;
					break;
				}
			}
		}
		return 1;
	case XTK_MSG_CONTENT_CATALOG:
		ls->have_catalog = 1;
		ls->num_mazes = 0;
		ls->num_vehicles = 0;
		for (i = 0; i < (int)msg->body.catalog.entry_count; i++) {
			const XtkCatalogEntry *e = &msg->body.catalog.entry[i];
			if (e->kind == XTK_CONTENT_MAZE) {
				ls->num_mazes = catalog_add(
					ls->maze, ls->maze_id, ls->num_mazes,
					e->name, e->id);
			} else if (e->kind == XTK_CONTENT_VEHICLE) {
				ls->num_vehicles = catalog_add(
					ls->vehicle, ls->vehicle_id,
					ls->num_vehicles, e->name, e->id);
			}
		}
		return 1;
	case XTK_MSG_CONFIG_STATUS:
		ls->have_config = 1;
		ls->selected_maze_id = msg->body.config_status.maze_id;
		ls->selected_vehicle_id = msg->body.config_status.vehicle_id;
		ls->selected_drone_count = msg->body.config_status.drone_count;
		ls->config_status = msg->body.config_status.status;
		ls->config_error = msg->body.config_status.error_code;
		if (msg->body.config_status.config_revision !=
		    ls->config_revision) {
			ls->ready = 0;
		}
		ls->config_revision = msg->body.config_status.config_revision;
		if (ls->config_status == 0) {
			ls->have_error = 0;
		}
		return 1;
	case XTK_MSG_CONFIG_SELECT:
		ls->selected_maze_id = msg->body.config_select.maze_id;
		ls->selected_vehicle_id = msg->body.config_select.vehicle_id;
		ls->selected_drone_count = msg->body.config_select.drone_count;
		return 1;
	case XTK_MSG_RESULTS:
		ls->session_state = (uint8_t)XTK_SESSION_RESULTS;
		ls->ready = 0;
		if (rs != NULL) {
			rs->have_results = 1;
			rs->results = msg->body.results;
		}
		return 1;
	case XTK_MSG_RESET:
		ls->session_state = (uint8_t)XTK_SESSION_LOBBY;
		ls->ready = 0;
		if (rs != NULL) {
			menu_results_init(rs);
		}
		return 1;
	case XTK_MSG_START:
	case XTK_MSG_EPOCH_BEGIN:
		ls->session_state = (uint8_t)XTK_SESSION_RUNNING;
		ls->ready = 0;
		if (rs != NULL) {
			menu_results_init(rs);
		}
		return 1;
	case XTK_MSG_REJECT:
	case XTK_MSG_PROTOCOL_ERROR:
		ls->have_error = 1;
		if (msg->env.message_type == XTK_MSG_REJECT) {
			ls->last_error = msg->body.reject.error_code;
			copy_name(ls->last_error_text, msg->body.reject.text);
		} else {
			ls->last_error = msg->body.protocol_error.error_code;
			ls->last_error_text[0] = '\0';
		}
		return 1;
	case XTK_MSG_QUIT:
		ls->connected = 0;
		return 1;
	default:
		return 0;
	}
}

int menu_lobby_select_maze(XtkLobbyState *ls, uint16_t maze_id)
{
	if (ls == NULL || !ls->have_catalog) {
		return -1;
	}
	if (catalog_name(ls->maze, ls->maze_id, ls->num_mazes, maze_id) ==
	    NULL) {
		return -1;
	}
	ls->selected_maze_id = maze_id;
	return 0;
}

int menu_lobby_select_vehicle(XtkLobbyState *ls, uint16_t vehicle_id)
{
	if (ls == NULL || !ls->have_catalog) {
		return -1;
	}
	if (catalog_name(ls->vehicle, ls->vehicle_id, ls->num_vehicles,
			 vehicle_id) == NULL) {
		return -1;
	}
	ls->selected_vehicle_id = vehicle_id;
	return 0;
}

int menu_lobby_select_drones(XtkLobbyState *ls, uint8_t drone_count)
{
	if (ls == NULL) {
		return -1;
	}
	if (drone_count > (uint8_t)XTK_MAX_DRONES) {
		return -1;
	}
	ls->selected_drone_count = drone_count;
	return 0;
}

int menu_lobby_can_config(const XtkLobbyState *ls)
{
	return ls != NULL && ls->connected && ls->is_admin &&
	       ls->session_state == (uint8_t)XTK_SESSION_LOBBY;
}

int menu_lobby_can_start(const XtkLobbyState *ls)
{
	return ls != NULL && ls->connected && ls->is_admin &&
	       ls->session_state == (uint8_t)XTK_SESSION_LOBBY;
}

int menu_lobby_can_reset(const XtkLobbyState *ls)
{
	return ls != NULL && ls->connected && ls->is_admin &&
	       ls->session_state != (uint8_t)XTK_SESSION_LOBBY;
}

const char *menu_lobby_maze_name(const XtkLobbyState *ls, uint16_t maze_id)
{
	if (ls == NULL) {
		return NULL;
	}
	return catalog_name(ls->maze, ls->maze_id, ls->num_mazes, maze_id);
}

const char *menu_lobby_vehicle_name(const XtkLobbyState *ls,
				    uint16_t vehicle_id)
{
	if (ls == NULL) {
		return NULL;
	}
	return catalog_name(ls->vehicle, ls->vehicle_id, ls->num_vehicles,
			    vehicle_id);
}

static void menu_title(XtkUiScene *s, int x, int y, const char *title)
{
	ui_add_text(s, x, y, XTK_FONT_MED, XTK_UI_WHITE, title);
}

int menu_build_lobby(const XtkLobbyState *ls, XtkUiScene *s)
{
	char buf[XTK_UI_STR_MAX];
	int i, y, line;

	if (s == NULL) {
		return -1;
	}
	ui_scene_init(s);

	/* Reference "XTANK" main menu at LEV0, with the frozen entry texts. */
	line = ui_font_height(XTK_FONT_LARGE);
	menu_title(s, XTK_UI_LEV0_X, XTK_UI_LEV0_Y - line - 2, "XTANK");
	for (i = 0; i < REF_MAIN_COUNT; i++) {
		uint32_t color = (i == 0) ? XTK_UI_GREEN : XTK_UI_WHITE;
		ui_add_text(s, XTK_UI_LEV0_X, XTK_UI_LEV0_Y + i * line,
			    XTK_FONT_LARGE, color, REF_MAIN_ENTRIES[i]);
	}

	/* Mazes and vehicles from the server catalog (names, not indices). */
	if (ls != NULL && ls->have_catalog) {
		menu_title(s, XTK_UI_MAZES_X, XTK_UI_TIER1_Y - line, "Mazes");
		for (i = 0; i < ls->num_mazes; i++) {
			uint32_t color = (ls->maze_id[i] ==
					  ls->selected_maze_id)
						 ? XTK_UI_YELLOW
						 : XTK_UI_WHITE;
			ui_add_text(s, XTK_UI_MAZES_X,
				    XTK_UI_TIER1_Y + i * line, XTK_FONT_MED,
				    color, ls->maze[i]);
		}
		menu_title(s, XTK_UI_VEHICLES_X, XTK_UI_TIER1_Y - line,
			   "Vehicles");
		for (i = 0; i < ls->num_vehicles; i++) {
			uint32_t color = (ls->vehicle_id[i] ==
					  ls->selected_vehicle_id)
						 ? XTK_UI_YELLOW
						 : XTK_UI_WHITE;
			ui_add_text(s, XTK_UI_VEHICLES_X,
				    XTK_UI_TIER1_Y + i * line, XTK_FONT_MED,
				    color, ls->vehicle[i]);
		}
	}

	/* Reference game-parameter panel (Src/interface.c). */
	y = XTK_UI_GAME_Y + XTK_UI_BORDER;
	ui_add_text(s, XTK_UI_GAME_X + XTK_UI_BORDER, y, XTK_FONT_SMALL,
		    XTK_UI_WHITE, "Game:  Combat");
	y += ui_font_height(XTK_FONT_SMALL);
	{
		const char *mn = (ls != NULL)
			? menu_lobby_maze_name(ls, ls->selected_maze_id)
			: NULL;
		const char *vn = (ls != NULL)
			? menu_lobby_vehicle_name(ls, ls->selected_vehicle_id)
			: NULL;
		snprintf(buf, sizeof(buf), "Maze:  %s",
			 mn != NULL ? mn : "(none)");
		ui_add_text(s, XTK_UI_GAME_X + XTK_UI_BORDER, y, XTK_FONT_SMALL,
			    XTK_UI_WHITE, buf);
		y += ui_font_height(XTK_FONT_SMALL);
		snprintf(buf, sizeof(buf), "Vehicle:  %s",
			 vn != NULL ? vn : "(none)");
		ui_add_text(s, XTK_UI_GAME_X + XTK_UI_BORDER, y, XTK_FONT_SMALL,
			    XTK_UI_WHITE, buf);
		y += ui_font_height(XTK_FONT_SMALL);
		snprintf(buf, sizeof(buf), "Drones:  %u",
			 (unsigned)(ls != NULL ? ls->selected_drone_count : 0));
		ui_add_text(s, XTK_UI_GAME_X + XTK_UI_BORDER, y, XTK_FONT_SMALL,
			    XTK_UI_WHITE, buf);
		y += ui_font_height(XTK_FONT_SMALL);
		snprintf(buf, sizeof(buf), "Config rev:  %u",
			 (unsigned)(ls != NULL ? ls->config_revision : 0));
		ui_add_text(s, XTK_UI_GAME_X + XTK_UI_BORDER, y, XTK_FONT_SMALL,
			    XTK_UI_WHITE, buf);
		y += ui_font_height(XTK_FONT_SMALL);
	}

	/* Server-authored roster: each player's name, role and readiness. */
	if (ls != NULL && ls->have_lobby_state) {
		y = XTK_UI_MSG_Y + XTK_UI_BORDER;
		ui_add_text(s, XTK_UI_MSG_X + XTK_UI_BORDER, y, XTK_FONT_SMALL,
			    XTK_UI_WHITE, "Players:");
		y += ui_font_height(XTK_FONT_SMALL);
		for (i = 0; i < ls->roster_count; i++) {
			const XtkRosterEntry *e = &ls->roster[i];
			uint32_t color =
				(e->flags & XTK_FLAG_DEPARTED)
					? XTK_UI_GREY
					: (i == ls->self_index ? XTK_UI_GREEN
							       : XTK_UI_WHITE);
			snprintf(buf, sizeof(buf), "%s%s %s",
				 (i == ls->admin_index) ? "*" : " ",
				 e->name[0] != '\0' ? e->name : "player",
				 (e->flags & XTK_FLAG_DEPARTED)
					? "left"
					: (e->ready ? "ready" : "waiting"));
			ui_add_text(s, XTK_UI_MSG_X + XTK_UI_BORDER, y,
				    XTK_FONT_SMALL, color, buf);
			y += ui_font_height(XTK_FONT_SMALL);
		}
	}

	/* Lobby status: connection, admin/ready and the last refusal. */
	if (ls == NULL || !ls->connected) {
		ui_add_text(s, XTK_UI_GAME_X + XTK_UI_BORDER, y, XTK_FONT_SMALL,
			    XTK_UI_RED, "Disconnected");
	} else {
		snprintf(buf, sizeof(buf), "%s  %s",
			 ls->is_admin ? "Admin" : "Player",
			 ls->ready ? "Ready" : "Not ready");
		ui_add_text(s, XTK_UI_GAME_X + XTK_UI_BORDER, y, XTK_FONT_SMALL,
			    XTK_UI_WHITE, buf);
		y += ui_font_height(XTK_FONT_SMALL);
		if (ls->have_config && ls->config_status != 0) {
			snprintf(buf, sizeof(buf), "Config refused: 0x%04x",
				 (unsigned)ls->config_error);
			ui_add_text(s, XTK_UI_GAME_X + XTK_UI_BORDER, y,
				    XTK_FONT_SMALL, XTK_UI_RED, buf);
			y += ui_font_height(XTK_FONT_SMALL);
		}
		if (ls->have_error) {
			char ebuf[XTK_MAX_ERROR_TEXT_BYTES + 32];
			snprintf(ebuf, sizeof(ebuf), "Error: 0x%04x %s",
				 (unsigned)ls->last_error,
				 ls->last_error_text);
			ui_add_text(s, XTK_UI_GAME_X + XTK_UI_BORDER, y,
				    XTK_FONT_SMALL, XTK_UI_RED, ebuf);
		}
	}
	return 0;
}

int menu_banner_text(const XtkResults *r, char *buf, size_t cap)
{
	const char *name = NULL;
	int i;

	if (buf == NULL || cap == 0) {
		return -1;
	}
	buf[0] = '\0';
	if (r == NULL) {
		return 0;
	}
	if (r->result_kind != 0) {
		snprintf(buf, cap, "Match aborted");
		return 0;
	}
	for (i = 0; i < (int)r->vehicle_count; i++) {
		if (r->entry[i].slot == r->winner_slot) {
			name = r->entry[i].name;
			break;
		}
	}
	if (name == NULL) {
		snprintf(buf, cap, "Match over");
	} else {
		snprintf(buf, cap, "Battle won by %s", name);
	}
	return 0;
}

int menu_build_results(const XtkResultsState *rs, XtkUiScene *s)
{
	char buf[XTK_UI_STR_MAX];
	int i, y, line;

	if (s == NULL) {
		return -1;
	}
	ui_scene_init(s);
	line = ui_font_height(XTK_FONT_MED);

	if (rs == NULL || !rs->have_results) {
		ui_add_text(s, XTK_UI_ANIM_X + XTK_UI_BORDER,
			    XTK_UI_ANIM_Y + XTK_UI_BORDER, XTK_FONT_MED,
			    XTK_UI_WHITE, "No results");
		return 0;
	}

	y = XTK_UI_ANIM_Y + XTK_UI_BORDER;
	snprintf(buf, sizeof(buf), "Game: Combat     Frame: %d",
		 (int)rs->results.end_tick);
	ui_add_text(s, XTK_UI_ANIM_X + XTK_UI_BORDER, y, XTK_FONT_MED,
		    XTK_UI_WHITE, buf);
	y += line;

	menu_banner_text(&rs->results, buf, sizeof(buf));
	ui_add_text(s, XTK_UI_ANIM_X + XTK_UI_BORDER, y, XTK_FONT_MED,
		    XTK_UI_YELLOW, buf);
	y += line;
	y += line;

	ui_add_text(s, XTK_UI_ANIM_X + XTK_UI_BORDER, y, XTK_FONT_MED,
		    XTK_UI_WHITE, "Current score:");
	y += line;

	for (i = 0; i < (int)rs->results.vehicle_count; i++) {
		const XtkResultEntry *e = &rs->results.entry[i];
		uint32_t color = (e->slot == rs->results.winner_slot)
					 ? XTK_UI_GREEN
					 : XTK_UI_WHITE;
		snprintf(buf, sizeof(buf), "%s: %u points", e->name,
			 (unsigned)e->score);
		ui_add_text(s, XTK_UI_ANIM_X + XTK_UI_BORDER, y, XTK_FONT_MED,
			    color, buf);
		y += line;
	}
	return 0;
}
