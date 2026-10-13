/*
 * Client/frame.h -- playable client frame loop (P2.14).
 *
 * Wires the P2.8 session protocol (Client/session_client.c), the P2.11
 * snapshot model / camera / one-tick interpolation, the P2.12 input controller
 * and the P2.10/P2.13 renderer + HUD/map/radar/menu into a single nonblocking
 * frame loop:
 *
 *   session messages -> menu/lobby state and model_observe(ViewSnapshot)
 *   input edges       -> XtkInputController
 *   every 15 Hz tick  -> input_emit() -> session INPUT_BATCH on channel 2
 *   every frame       -> model_render() -> world + HUD/map/radar on a surface
 *
 * The loop never blocks, never schedules a simulation tick and runs no
 * simulation: the authoritative server owns movement, collisions, damage, RNG,
 * bots and scoring.  Presentation speed cannot advance the world; the client
 * only renders the authorized snapshots at the model's estimated server time
 * minus the approved one-tick buffer (D2-E) and emits input at the server's
 * 15 Hz cadence.
 *
 * Authority: docs/PHASE2-PLAN.md P2.14; docs/PROTOCOL-v1.md; docs/VIEW-v1.md;
 * docs/PHASE2-DECISIONS.md D2-E/D2-F.
 */

#ifndef XTK_CLIENT_FRAME_H
#define XTK_CLIENT_FRAME_H

#include <stdint.h>

#include "wire.h"
#include "ui.h"
#include "input.h"
#include "session_client.h"
#include "model.h"
#include "render.h"
#include "menu.h"

struct SDL_Surface;

typedef struct XtkFrame XtkFrame;

/* Read-only per-frame statistics for status display and acceptance tests. */
typedef struct {
	XtkSessionState session_state;
	XtkModelStatus model_status;
	int      have_self;		/* a snapshot has been rendered       */
	int      running;		/* between EPOCH_BEGIN and RESULTS    */
	int      connected;
	uint32_t epoch;
	int32_t  server_tick;		/* newest observed tick / rendered    */
	int      snapshots;		/* VIEW_SNAPSHOT messages accepted     */
	int      cmd_acks;		/* CMD_ACK messages received           */
	int      emitted_batches;	/* input batches sent this epoch       */
	int32_t  first_emit_tick;	/* first tick an input batch was sent  */
	int32_t  last_emit_tick;
	int      map_cells;		/* known MapCell records drawn         */
	int      radar_blips;		/* anonymous blips drawn               */
	int      rejected;		/* REJECT received                     */
	uint16_t last_error;
} XtkFrameStats;

/*
 * Create a frame loop bound to `session`.  `assets` may be NULL (the world is
 * then only cleared); `surface` may be NULL and set later.  The model is
 * allocated on the heap.  Returns NULL on allocation failure.
 */
XtkFrame *frame_create(XtkSessionClient *session, const XtkAssets *assets,
		       struct SDL_Surface *surface);
void frame_destroy(XtkFrame *f);

/* Replace the target surface (as on resize/DPI change). */
void frame_set_surface(XtkFrame *f, struct SDL_Surface *surface);

/* ---- input from the platform layer ----------------------------------- */

/* Feed one neutral control edge (held/edge/pause/quit) to the controller. */
void frame_control(XtkFrame *f, XtkControl ctl, int down);

/* Feed a logical window-point aim; transformed to world through the camera. */
void frame_aim_screen(XtkFrame *f, float logical_x, float logical_y,
		      float display_scale);
/* Feed a world aim directly (already transformed). */
void frame_aim_world(XtkFrame *f, double world_x, double world_y);

/* Window focus; focus loss releases every held control unconditionally. */
void frame_focus(XtkFrame *f, int focused);

/* ---- lobby / admin --------------------------------------------------- */

/* Auto-select the first available maze/Vanguard and send READY (default on).
 * Suppressed on a multi-human server so a solo auto-start cannot race the
 * second player (P3.10). */
void frame_set_auto_lobby(XtkFrame *f, int on);

/*
 * Manually select a catalog configuration (validated against the catalog and
 * only sent by the server-authored administrator).  `drones` is 0..2 (D3-B);
 * frame_lobby_select() keeps the current drone count.  Returns 0 when sent,
 * -1 when refused locally (not admin / no catalog / bad selection).
 */
int  frame_lobby_select(XtkFrame *f, uint16_t maze_id, uint16_t vehicle_id);
int  frame_lobby_select_config(XtkFrame *f, uint16_t maze_id,
			       uint16_t vehicle_id, uint8_t drones);

/* Explicit ready toggle (any admitted player) against the current revision. */
int  frame_ready(XtkFrame *f, int ready);

/* Administrator start request (only the server-authored admin). */
int  frame_request_start(XtkFrame *f);

/* Admin reset (epoch+1) and quit. */
int  frame_request_reset(XtkFrame *f);
int  frame_request_quit(XtkFrame *f);
int  frame_quit_requested(const XtkFrame *f);

/*
 * Drop the current connection and return to Connect so a failed/disconnected
 * client can rejoin a fresh lobby in-process (no restart).
 */
void frame_reconnect(XtkFrame *f);

/* Presentation-side control gates (mirror menu_lobby_can_*; server is boss). */
int  frame_can_config(const XtkFrame *f);
int  frame_can_start(const XtkFrame *f);
int  frame_can_reset(const XtkFrame *f);

/*
 * One nonblocking iteration.  Pumps the session and handles messages, maps the
 * session state to the controller, auto-advances the lobby, emits one input
 * batch per new server tick while Running (never tick 0), and renders one
 * frame into the surface.  `now_ns` is a monotonic clock.  Returns 0, or -1 on
 * a fatal transport error.
 */
int  frame_tick(XtkFrame *f, uint64_t now_ns);

/* ---- read-only views for tests --------------------------------------- */

const XtkViewSnapshot   *frame_view(const XtkFrame *f);
const XtkUiScene        *frame_scene(const XtkFrame *f);
const XtkClientModel    *frame_model(const XtkFrame *f);
const XtkInputController *frame_input(const XtkFrame *f);
const XtkLobbyState     *frame_lobby(const XtkFrame *f);
const XtkResultsState   *frame_results(const XtkFrame *f);
int frame_running(const XtkFrame *f);
int frame_snapshot_count(const XtkFrame *f);
void frame_stats(const XtkFrame *f, XtkFrameStats *out);

#endif /* XTK_CLIENT_FRAME_H */
