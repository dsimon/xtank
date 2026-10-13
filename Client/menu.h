/*
 * Client/menu.h -- lobby, results and banner presentation wired to protocol v1
 * (P2.13).
 *
 * menu_apply_message() is the presentation-side reduction of the frozen v1
 * control messages into a lobby view (content catalog, selected configuration,
 * ready/admin state, errors) and the immutable results view.  menu_build_lobby()
 * reproduces the frozen reference lobby menu texts and layout (Src/interface.c),
 * menu_build_results() reproduces the reference end-of-match score listing
 * including public player names (allowed only here, never in the HUD).
 *
 * No simulation state is read; the module consumes decoded XtkMessage DTOs and
 * stores no pointer into them.
 *
 * Authority: docs/PHASE2-PLAN.md P2.13; docs/PROTOCOL-v1.md sections 4.5-4.13;
 * docs/VIEW-v1.md section 4.8; Src/interface.c and Src/game.c (read-only
 * references for the frozen texts).
 */

#ifndef XTK_CLIENT_MENU_H
#define XTK_CLIENT_MENU_H

#include "ui.h"

/* Presentation-side lobby state (never authoritative). */
typedef struct {
	int      connected;		/* WELCOME received                 */
	int      is_admin;		/* LOBBY_STATE admin_index == self  */
	int      have_catalog;
	char     maze[XTK_MAX_CATALOG_ENTRIES][XTK_MAX_NAME_BYTES + 1];
	uint16_t maze_id[XTK_MAX_CATALOG_ENTRIES];
	int      num_mazes;
	char     vehicle[XTK_MAX_CATALOG_ENTRIES][XTK_MAX_NAME_BYTES + 1];
	uint16_t vehicle_id[XTK_MAX_CATALOG_ENTRIES];
	int      num_vehicles;
	uint16_t selected_maze_id;
	uint16_t selected_vehicle_id;
	uint8_t  selected_drone_count;
	int      have_config;
	uint16_t config_status;		/* 0 accepted                       */
	uint16_t config_error;

	/* Server-authored LOBBY_STATE (P3.10). */
	int      have_lobby_state;
	uint8_t  self_index;
	uint8_t  admin_index;
	uint8_t  drone_count;
	uint8_t  session_state;		/* XTK_SESSION_*                    */
	uint32_t config_revision;
	int      roster_count;
	XtkRosterEntry roster[XTK_LOBBY_ROSTER_MAX];

	int      ready;
	int      have_error;
	uint16_t last_error;
	char     last_error_text[XTK_MAX_ERROR_TEXT_BYTES + 1];
} XtkLobbyState;

/* Immutable results retained until RESET (docs/VIEW-v1.md section 4.8). */
typedef struct {
	int        have_results;
	XtkResults results;
} XtkResultsState;

void menu_lobby_init(XtkLobbyState *ls);
void menu_results_init(XtkResultsState *rs);

/*
 * Reduce one decoded control message into the lobby/results view.  Only the
 * lobby/results-relevant types are applied; all others are ignored.  Returns 1
 * when the message changed the view, 0 otherwise.
 */
int menu_apply_message(XtkLobbyState *ls, XtkResultsState *rs,
		       const XtkMessage *msg);

/* Selection helpers (validated only against the received catalog). */
int menu_lobby_select_maze(XtkLobbyState *ls, uint16_t maze_id);
int menu_lobby_select_vehicle(XtkLobbyState *ls, uint16_t vehicle_id);
int menu_lobby_select_drones(XtkLobbyState *ls, uint8_t drone_count);

/*
 * Control enablement (presentation gates only; the server is authoritative).
 * Config/start require the server-authored administrator and the Lobby state;
 * reset requires the administrator and a live/results epoch.
 */
int menu_lobby_can_config(const XtkLobbyState *ls);
int menu_lobby_can_start(const XtkLobbyState *ls);
int menu_lobby_can_reset(const XtkLobbyState *ls);

/* Catalog name lookup by stable content id; NULL when absent. */
const char *menu_lobby_maze_name(const XtkLobbyState *ls, uint16_t maze_id);
const char *menu_lobby_vehicle_name(const XtkLobbyState *ls, uint16_t vehicle_id);

/* Build the frozen reference lobby scene. Returns 0 or -1 on a full scene. */
int menu_build_lobby(const XtkLobbyState *ls, XtkUiScene *scene);

/*
 * Build the end-of-match results scene/banner from the immutable results.
 * Returns 0 or -1 on a full scene.
 */
int menu_build_results(const XtkResultsState *rs, XtkUiScene *scene);

/* Reference banner line ("Battle won by <name>" / "Match aborted"), or "" when
 * there are no results.  Never truncates silently. */
int menu_banner_text(const XtkResults *r, char *buf, size_t cap);

/* The exact frozen reference lobby main-menu item texts (Src/interface.c
 * main_entries[]), for review and tests. */
const char *const *menu_reference_main_entries(int *count);

#endif /* XTK_CLIENT_MENU_H */
