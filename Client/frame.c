/*
 * Client/frame.c -- playable client frame loop (P2.14).
 *
 * See Client/frame.h.  Pure C apart from the SDL_Surface pointer it is handed
 * (render_sdl.c owns the only SDL include): it wires the session, model, input
 * controller and presentation panels together without blocking, without
 * scheduling a simulation tick and without running any simulation.
 *
 * Authority: docs/PHASE2-PLAN.md P2.14; docs/PROTOCOL-v1.md; docs/VIEW-v1.md;
 * docs/PHASE2-DECISIONS.md D2-E/D2-F.
 */

#include "frame.h"

#include <stdlib.h>
#include <string.h>

#include "content-manifest.h"
#include "hud.h"
#include "map.h"
#include "radar.h"
#include "menu.h"

struct XtkFrame {
	XtkSessionClient *session;
	const XtkAssets  *assets;
	struct SDL_Surface *surface;

	XtkClientModel *model;	/* heap: hundreds of KiB */

	XtkInputController input;
	XtkLobbyState      lobby;
	XtkResultsState    results;

	XtkViewSnapshot view;	/* last interpolated authorized frame  */
	XtkUiScene      scene;	/* last combined presentation scene    */
	XtkHudInfo      hud_info;

	int      auto_lobby;
	int      config_sent;
	int      ready_sent;

	int      have_epoch;
	uint32_t epoch;
	int32_t  start_tick;
	int32_t  newest_tick;
	int32_t  last_emit_tick;
	uint64_t last_emit_ns;
	double   tick_ns;
	uint64_t now_ns;

	int      running;
	int      quit;

	XtkFrameStats stats;
};

/* ---- scene merge ----------------------------------------------------- */

static void scene_merge(XtkUiScene *dst, const XtkUiScene *src)
{
	int i;

	for (i = 0; i < src->rect_count; i++) {
		const XtkUiRect *r = &src->rect[i];
		ui_add_rect(dst, r->x, r->y, r->w, r->h, r->color, r->filled);
	}
	for (i = 0; i < src->line_count; i++) {
		const XtkUiLine *l = &src->line[i];
		ui_add_line(dst, l->x1, l->y1, l->x2, l->y2, l->color);
	}
	for (i = 0; i < src->text_count; i++) {
		const XtkUiText *t = &src->text[i];
		ui_add_text_align(dst, t->x, t->y, (XtkUiFont)t->font, t->color,
				  t->align, t->text);
	}
}

/* ---- lifecycle ------------------------------------------------------- */

XtkFrame *frame_create(XtkSessionClient *session, const XtkAssets *assets,
		       struct SDL_Surface *surface)
{
	XtkFrame *f;

	if (session == NULL) {
		return NULL;
	}
	f = (XtkFrame *)calloc(1, sizeof(*f));
	if (f == NULL) {
		return NULL;
	}
	f->model = (XtkClientModel *)malloc(sizeof(*f->model));
	if (f->model == NULL) {
		free(f);
		return NULL;
	}
	model_init(f->model);
	f->session = session;
	f->assets = assets;
	f->surface = surface;
	f->auto_lobby = 1;
	f->tick_ns = 1e9 / (double)XTK_SERVER_TICK_HZ;
	f->last_emit_tick = 0;
	input_init(&f->input);
	menu_lobby_init(&f->lobby);
	menu_results_init(&f->results);
	f->stats.session_state = session_client_state(session);
	return f;
}

void frame_destroy(XtkFrame *f)
{
	if (f == NULL) {
		return;
	}
	free(f->model);
	free(f);
}

void frame_set_surface(XtkFrame *f, struct SDL_Surface *surface)
{
	if (f != NULL) {
		f->surface = surface;
	}
}

/* ---- inbound messages ------------------------------------------------ */

static XtkInputState map_input_state(XtkSessionState st)
{
	switch (st) {
	case XTK_SC_RUNNING:	return XTK_INPUT_RUNNING;
	case XTK_SC_RESULTS:	return XTK_INPUT_RESULTS;
	case XTK_SC_DISCONNECTED:
	case XTK_SC_FAILED:	return XTK_INPUT_DISCONNECTED;
	default:		return XTK_INPUT_LOBBY;
	}
}

static int frame_session_message(void *user, const XtkMessage *msg)
{
	XtkFrame *f = (XtkFrame *)user;

	(void)menu_apply_message(&f->lobby, &f->results, msg);

	switch (msg->env.message_type) {
	case XTK_MSG_EPOCH_BEGIN:
		f->have_epoch = 1;
		f->epoch = msg->body.epoch_begin.epoch;
		f->start_tick = (int32_t)msg->body.epoch_begin.first_tick;
		f->newest_tick = 0;
		f->last_emit_tick = f->start_tick - 1;
		if (f->last_emit_tick < 0) {
			f->last_emit_tick = 0;
		}
		f->last_emit_ns = f->now_ns;
		f->running = 1;
		/* Fresh epoch: no prior match state may survive (D2-E). */
		model_reset(f->model);
		f->stats.emitted_batches = 0;
		f->stats.first_emit_tick = -1;
		f->stats.snapshots = 0;
		f->stats.cmd_acks = 0;
		break;
	case XTK_MSG_START:
		f->epoch = msg->env.epoch;
		break;
	case XTK_MSG_VIEW_SNAPSHOT:
		if (f->running) {
			const XtkViewSnapshot *snap = &msg->body.view_snapshot;
			(void)model_observe(f->model, msg->env.epoch, snap,
					    f->now_ns);
			f->stats.snapshots++;
			if ((int32_t)snap->server_tick > f->newest_tick) {
				f->newest_tick = (int32_t)snap->server_tick;
			}
		}
		break;
	case XTK_MSG_CMD_ACK:
		f->stats.cmd_acks++;
		break;
	case XTK_MSG_RESULTS:
		f->running = 0;
		break;
	case XTK_MSG_REJECT:
		f->stats.rejected = 1;
		f->stats.last_error = msg->body.reject.error_code;
		break;
	case XTK_MSG_PROTOCOL_ERROR:
		f->stats.last_error = msg->body.protocol_error.error_code;
		break;
	default:
		break;
	}
	return 0;
}

/* ---- lobby auto-advance ---------------------------------------------- */

static int pick_maze(const XtkLobbyState *ls, uint16_t *out)
{
	int i;

	for (i = 0; i < ls->num_mazes; i++) {
		if (ls->maze_id[i] == XTK_CONTENT_ID_COMBAT1) {
			*out = ls->maze_id[i];
			return 1;
		}
	}
	if (ls->num_mazes > 0) {
		*out = ls->maze_id[0];
		return 1;
	}
	return 0;
}

static int pick_vehicle(const XtkLobbyState *ls, uint16_t *out)
{
	int i;

	for (i = 0; i < ls->num_vehicles; i++) {
		if (ls->vehicle_id[i] == XTK_CONTENT_ID_VANGUARD) {
			*out = ls->vehicle_id[i];
			return 1;
		}
	}
	if (ls->num_vehicles > 0) {
		*out = ls->vehicle_id[0];
		return 1;
	}
	return 0;
}

static void frame_lobby_step(XtkFrame *f)
{
	uint16_t maze, veh;

	if (!f->auto_lobby || session_client_state(f->session) != XTK_SC_LOBBY) {
		return;
	}
	if (f->lobby.have_catalog && !f->config_sent) {
		if (!pick_maze(&f->lobby, &maze) ||
		    !pick_vehicle(&f->lobby, &veh)) {
			return;
		}
		(void)menu_lobby_select_maze(&f->lobby, maze);
		(void)menu_lobby_select_vehicle(&f->lobby, veh);
		(void)session_client_send_config(f->session, maze, veh);
		f->config_sent = 1;
		return;
	}
	if (f->config_sent && f->lobby.have_config &&
	    f->lobby.config_status == 0 && !f->ready_sent) {
		(void)session_client_send_ready(f->session);
		f->ready_sent = 1;
	}
}

/* ---- input emission (15 Hz; never tick 0) ---------------------------- */

static void frame_emit(XtkFrame *f)
{
	XtkMessage batch;
	int32_t etick;
	int n;

	if (!f->running) {
		return;
	}
	etick = f->newest_tick;
	if (!(etick >= 1 && etick > f->last_emit_tick)) {
		/* Heartbeat fallback if snapshots stall (keeps the 2 s solo
		 * abort from firing on an idle-but-connected client). */
		if (f->newest_tick > 0 && f->now_ns >= f->last_emit_ns +
		    (uint64_t)(f->tick_ns * 3.0)) {
			etick = f->last_emit_tick + 1;
			if (etick < 1) {
				etick = 1;
			}
		} else {
			return;
		}
	}
	n = input_emit(&f->input, etick, f->epoch,
		       session_client_id(f->session), &batch);
	if (n > 0) {
		(void)session_client_send_input(f->session, &batch);
		if (f->stats.emitted_batches == 0) {
			f->stats.first_emit_tick = etick;
		}
		f->stats.emitted_batches++;
	}
	f->last_emit_tick = etick;
	f->last_emit_ns = f->now_ns;
}

/* ---- render ---------------------------------------------------------- */

static void frame_render(XtkFrame *f)
{
	XtkUiScene part;
	const XtkCamera *cam;
	char err[128];

	if (f->surface == NULL) {
		return;
	}

	f->stats.model_status = model_render(f->model, f->now_ns, &f->view);
	f->stats.have_self = model_snapshot_count(f->model) > 0;
	f->stats.server_tick = (int32_t)f->view.server_tick;

	cam = model_camera(f->model);
	if (f->assets != NULL) {
		err[0] = '\0';
		(void)world_render_present(f->assets, &f->view, cam,
					   f->surface, err, sizeof(err));
	} else {
		render_surface_clear(f->surface, XTK_RGB(0, 0, 0));
	}

	ui_scene_init(&f->scene);
	f->stats.map_cells = 0;
	f->stats.radar_blips = 0;

	if (f->stats.have_self) {
		f->hud_info.connected = f->stats.connected;
		f->hud_info.have_banner = 0;
		f->hud_info.banner = NULL;
		(void)hud_build(&f->view.self, 1, &f->hud_info, &part);
		scene_merge(&f->scene, &part);
		f->stats.map_cells = map_build(&f->view, &part);
		scene_merge(&f->scene, &part);
		f->stats.radar_blips = radar_build(&f->view, &part);
		scene_merge(&f->scene, &part);
	}

	if (f->results.have_results) {
		(void)menu_build_results(&f->results, &part);
		scene_merge(&f->scene, &part);
	} else if (!f->stats.have_self) {
		(void)menu_build_lobby(&f->lobby, &part);
		scene_merge(&f->scene, &part);
	}

	(void)render_frame_ui(&f->scene, f->surface);
}

/* ---- public tick ----------------------------------------------------- */

int frame_tick(XtkFrame *f, uint64_t now_ns)
{
	if (f == NULL) {
		return -1;
	}
	f->now_ns = now_ns;

	if (session_client_pump(f->session, now_ns,
				frame_session_message, f) < 0) {
		/* Fatal transport error: freeze on the last authorized frame. */
		model_set_disconnected(f->model, 1);
	}

	f->stats.session_state = session_client_state(f->session);
	f->stats.connected = session_client_connected(f->session);
	f->stats.running = f->running;
	f->stats.epoch = f->epoch;
	f->stats.rejected = f->stats.rejected ||
		(f->stats.session_state == XTK_SC_FAILED);
	f->stats.last_error = session_client_last_error(f->session);

	input_set_state(&f->input, map_input_state(f->stats.session_state));

	frame_lobby_step(f);
	frame_emit(f);
	frame_render(f);

	if (f->quit && f->stats.session_state == XTK_SC_DISCONNECTED) {
		/* Clean disconnect completed. */
	}
	return 0;
}

/* ---- platform input -------------------------------------------------- */

void frame_control(XtkFrame *f, XtkControl ctl, int down)
{
	if (f != NULL) {
		input_control_event(&f->input, ctl, down);
	}
}

void frame_aim_screen(XtkFrame *f, float logical_x, float logical_y,
		      float display_scale)
{
	double wx = 0.0, wy = 0.0;

	if (f == NULL) {
		return;
	}
	input_screen_to_world(model_camera(f->model), logical_x, logical_y,
			      display_scale, &wx, &wy);
	input_aim_world(&f->input, wx, wy);
}

void frame_aim_world(XtkFrame *f, double world_x, double world_y)
{
	if (f != NULL) {
		input_aim_world(&f->input, world_x, world_y);
	}
}

void frame_focus(XtkFrame *f, int focused)
{
	if (f != NULL) {
		input_set_focus(&f->input, focused);
	}
}

/* ---- lobby / admin --------------------------------------------------- */

void frame_set_auto_lobby(XtkFrame *f, int on)
{
	if (f != NULL) {
		f->auto_lobby = on ? 1 : 0;
	}
}

int frame_lobby_select(XtkFrame *f, uint16_t maze_id, uint16_t vehicle_id)
{
	if (f == NULL) {
		return -1;
	}
	if (menu_lobby_select_maze(&f->lobby, maze_id) != 0) {
		return -1;
	}
	if (menu_lobby_select_vehicle(&f->lobby, vehicle_id) != 0) {
		return -1;
	}
	if (session_client_send_config(f->session, maze_id, vehicle_id) != 0) {
		return -1;
	}
	f->config_sent = 1;
	return 0;
}

int frame_request_reset(XtkFrame *f)
{
	if (f == NULL) {
		return -1;
	}
	return session_client_send_reset(f->session);
}

int frame_request_quit(XtkFrame *f)
{
	if (f == NULL) {
		return -1;
	}
	f->quit = 1;
	input_control_event(&f->input, XTK_CTL_QUIT, 1);
	return session_client_send_quit(f->session);
}

int frame_quit_requested(const XtkFrame *f)
{
	return (f != NULL) ? f->quit : 0;
}

/* ---- accessors ------------------------------------------------------- */

const XtkViewSnapshot *frame_view(const XtkFrame *f)
{
	return (f != NULL) ? &f->view : NULL;
}

const XtkUiScene *frame_scene(const XtkFrame *f)
{
	return (f != NULL) ? &f->scene : NULL;
}

const XtkClientModel *frame_model(const XtkFrame *f)
{
	return (f != NULL) ? f->model : NULL;
}

const XtkInputController *frame_input(const XtkFrame *f)
{
	return (f != NULL) ? &f->input : NULL;
}

const XtkLobbyState *frame_lobby(const XtkFrame *f)
{
	return (f != NULL) ? &f->lobby : NULL;
}

const XtkResultsState *frame_results(const XtkFrame *f)
{
	return (f != NULL) ? &f->results : NULL;
}

int frame_running(const XtkFrame *f)
{
	return (f != NULL) ? f->running : 0;
}

int frame_snapshot_count(const XtkFrame *f)
{
	return (f != NULL) ? model_snapshot_count(f->model) : 0;
}

void frame_stats(const XtkFrame *f, XtkFrameStats *out)
{
	if (f != NULL && out != NULL) {
		*out = f->stats;
	}
}
