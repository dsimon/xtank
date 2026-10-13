/*
 * Client/input.c -- client input controller (P2.12).
 *
 * Pure C: no SDL, no engine, no I/O and no allocation.  See Client/input.h for
 * the contract.  The controller turns neutral control edges into protocol-v1
 * commands, emits at 15 Hz with two-tick redundancy, and releases every held
 * control on quit or window defocus.
 *
 * Authority: docs/PHASE2-PLAN.md P2.12; docs/PROTOCOL-v1.md sections 6, 8, 9.
 */

#include "input.h"

#include <string.h>

/* ---- small helpers --------------------------------------------------- */

static int control_is_weapon(XtkControl ctl)
{
	return ctl >= XTK_CTL_WEAPON_1 && ctl <= XTK_CTL_WEAPON_6;
}

static int control_is_edge(XtkControl ctl)
{
	return ctl == XTK_CTL_FIRE || control_is_weapon(ctl);
}

static int control_is_held(XtkControl ctl)
{
	return ctl == XTK_CTL_DRIVE_FWD || ctl == XTK_CTL_DRIVE_BACK ||
	       ctl == XTK_CTL_FIRE_HOLD;
}

/*
 * True for commands whose loss must be recovered by the reliable companion:
 * one-shot fire/weapon edges and the one-shot releases of held controls.  A
 * lost held refresh is harmless (the server holds the last value until the
 * next refresh); a lost edge or release is not.
 */
static int command_needs_reliable(const XtkCommand *c)
{
	switch (c->op) {
	case XTK_OP_FIRE:
	case XTK_OP_WEAPON:
		return 1;
	case XTK_OP_DRIVE_REL:
	case XTK_OP_FIRE_HOLD:
		return c->a == 0.0;	/* the release, not the held refresh */
	default:
		return 0;
	}
}

static uint32_t alloc_command_id(XtkInputController *ic)
{
	uint32_t id = ic->next_command_id++;
	/* command_id is client-owned and deduplicated; 0 is a valid id but we
	 * skip it so an unwritten field can never masquerade as a command. */
	if (id == 0) {
		id = ic->next_command_id++;
	}
	return id;
}

static void ring_clear(XtkInputController *ic)
{
	int i;
	for (i = 0; i < (int)XTK_INPUT_RING; ++i) {
		ic->ring_count[i] = 0;
	}
	ic->ring_head = 0;
	ic->ring_filled = 0;
}

/* Release every held/edge control (quit, focus loss, leaving Running). */
static void release_all(XtkInputController *ic)
{
	memset(ic->held, 0, sizeof(ic->held));
	memset(ic->edge, 0, sizeof(ic->edge));
	ic->aim_valid = 0;
}

/* ---- lifecycle ------------------------------------------------------- */

void input_init(XtkInputController *ic)
{
	memset(ic, 0, sizeof(*ic));
	ic->state = XTK_INPUT_DISCONNECTED;
	ic->focused = 1;
	ic->next_command_id = 1;
	ic->next_packet_sequence = 1;
	ring_clear(ic);
}

void input_reset(XtkInputController *ic)
{
	uint32_t cid = ic->next_command_id;
	uint32_t psq = ic->next_packet_sequence;
	XtkInputState st = ic->state;

	memset(ic, 0, sizeof(*ic));
	ic->state = st;
	ic->focused = 1;
	ic->next_command_id = cid ? cid : 1;
	ic->next_packet_sequence = psq ? psq : 1;
	ring_clear(ic);
}

void input_set_state(XtkInputController *ic, XtkInputState state)
{
	if (state == ic->state) {
		return;
	}
	if (state != XTK_INPUT_RUNNING) {
		/* No gameplay state may survive outside Running. */
		release_all(ic);
		ic->have_sent_drive = 0;
		ic->sent_drive = 0.0;
		ic->have_sent_fire_hold = 0;
		ic->sent_fire_hold = 0;
		ring_clear(ic);
	} else {
		/* A fresh epoch must not retransmit a prior match's commands. */
		ic->have_sent_drive = 0;
		ic->sent_drive = 0.0;
		ic->have_sent_fire_hold = 0;
		ic->sent_fire_hold = 0;
		ring_clear(ic);
	}
	ic->state = state;
}

XtkInputState input_state(const XtkInputController *ic)
{
	return ic->state;
}

void input_set_focus(XtkInputController *ic, int focused)
{
	focused = focused ? 1 : 0;
	if (focused == ic->focused) {
		return;
	}
	ic->focused = focused;
	if (!focused) {
		/*
		 * A stuck control must never outlive focus.  The next emit, while
		 * still Running, transmits the neutral drive/fire release.
		 */
		release_all(ic);
	}
}

int input_focused(const XtkInputController *ic)
{
	return ic->focused;
}

/* ---- control edges --------------------------------------------------- */

void input_control_event(XtkInputController *ic, XtkControl ctl, int down)
{
	down = down ? 1 : 0;
	if (ctl <= XTK_CTL_NONE || ctl >= XTK_CTL_COUNT) {
		return;
	}

	switch (ctl) {
	case XTK_CTL_PAUSE:
		/* Pause is only meaningful outside a Running match. */
		if (ic->state != XTK_INPUT_RUNNING && down) {
			ic->paused = !ic->paused;
		}
		return;

	case XTK_CTL_QUIT:
		if (down) {
			ic->quit = 1;
			release_all(ic);
		}
		return;

	default:
		break;
	}

	/* Gameplay controls exist only while Running. */
	if (ic->state != XTK_INPUT_RUNNING) {
		return;
	}

	if (control_is_held(ctl)) {
		ic->held[ctl] = (uint8_t)down;
	} else if (control_is_edge(ctl)) {
		if (down) {
			ic->edge[ctl] = 1;	/* latch: never lose a press */
		}
	}
}

void input_set_life_generation(XtkInputController *ic, uint32_t generation)
{
	if (ic == NULL || generation == 0) {
		/* 0 = not yet known; never clears or stamps the last known life. */
		return;
	}
	if (generation == ic->life_generation) {
		return;
	}
	/*
	 * A new life: an edge latched or produced in the previous life must not
	 * fire under the new one.  Drop the latched one-shot edges and the
	 * redundancy history; the held state is re-derived each tick, so a
	 * still-held control is simply restamped with the new generation.
	 */
	if (ic->life_generation != 0) {
		memset(ic->edge, 0, sizeof(ic->edge));
		ring_clear(ic);
	}
	ic->life_generation = generation;
}

uint32_t input_life_generation(const XtkInputController *ic)
{
	return (ic != NULL) ? ic->life_generation : 0;
}

void input_aim_world(XtkInputController *ic, double world_x, double world_y)
{
	if (ic->state != XTK_INPUT_RUNNING) {
		return;
	}
	if (!xtk_finite_f64(world_x) || !xtk_finite_f64(world_y)) {
		return;
	}
	ic->aim_valid = 1;
	ic->aim_x = world_x;
	ic->aim_y = world_y;
}

void input_screen_to_world(const XtkCamera *cam, float logical_x, float logical_y,
			   float display_scale, double *world_x, double *world_y)
{
	double ox, oy, sx, sy;

	if (display_scale <= 0.0f) {
		display_scale = 1.0f;
	}
	if (cam == NULL) {
		if (world_x != NULL) *world_x = 0.0;
		if (world_y != NULL) *world_y = 0.0;
		return;
	}

	/* Logical window points -> drawable pixels (P2.9 DPI mapping). */
	sx = (double)logical_x * (double)display_scale;
	sy = (double)logical_y * (double)display_scale;

	/* Inverse of render_sdl.c world_to_screen: screen = world - ox. */
	ox = cam->center_x - (double)cam->viewport_w / 2.0;
	oy = cam->center_y - (double)cam->viewport_h / 2.0;

	if (world_x != NULL) *world_x = sx + ox;
	if (world_y != NULL) *world_y = sy + oy;
}

int input_paused(const XtkInputController *ic)
{
	return ic->paused;
}

int input_quit_requested(const XtkInputController *ic)
{
	return ic->quit;
}

/* ---- emission -------------------------------------------------------- */

static void add_command(XtkInputController *ic, XtkCommand *arr, int *n,
			int32_t tick, uint8_t op, uint8_t flags, double a,
			double b)
{
	XtkCommand *c;

	if (*n >= (int)XTK_INPUT_MAX_BATCH) {
		return;
	}
	c = &arr[(*n)++];
	memset(c, 0, sizeof(*c));
	c->command_id = alloc_command_id(ic);
	c->tick_hint = (uint32_t)tick;
	c->life_generation = ic->life_generation;
	c->op = op;
	c->flags = flags;
	c->a = a;
	c->b = b;
	c->reserved = 0;
}

/*
 * Build this tick's new commands into `cur`.  Held controls are refreshed
 * every tick and released exactly once when lifted; edges are produced once
 * and their latch cleared.  A HEARTBEAT keeps an idle session alive.
 */
static int build_tick(XtkInputController *ic, int32_t tick, XtkCommand *cur)
{
	int n = 0;
	double drive = 0.0;
	int fire_hold = ic->held[XTK_CTL_FIRE_HOLD] ? 1 : 0;
	int i;

	if (ic->held[XTK_CTL_DRIVE_FWD]) {
		drive += XTK_INPUT_DRIVE_SPEED;
	}
	if (ic->held[XTK_CTL_DRIVE_BACK]) {
		drive -= XTK_INPUT_DRIVE_SPEED;
	}

	/* Held drive: refresh while active, release once when it stops. */
	if (drive != 0.0 || (ic->have_sent_drive && ic->sent_drive != 0.0)) {
		add_command(ic, cur, &n, tick, XTK_OP_DRIVE_REL, XTK_FLAG_HELD,
			    drive, 0.0);
	}
	ic->sent_drive = drive;
	ic->have_sent_drive = 1;

	/* Held fire: refresh while held, release once. */
	if (fire_hold || (ic->have_sent_fire_hold && ic->sent_fire_hold)) {
		add_command(ic, cur, &n, tick, XTK_OP_FIRE_HOLD, XTK_FLAG_HELD,
			    (double)fire_hold, 0.0);
	}
	ic->sent_fire_hold = fire_hold;
	ic->have_sent_fire_hold = 1;

	/* World aim is one-shot but refreshed each tick while aiming. */
	if (ic->aim_valid) {
		add_command(ic, cur, &n, tick, XTK_OP_AIM_WORLD, 0, ic->aim_x,
			    ic->aim_y);
	}

	/* Edge fire: one command per press (latch cleared after emit). */
	if (ic->edge[XTK_CTL_FIRE]) {
		add_command(ic, cur, &n, tick, XTK_OP_FIRE, XTK_FLAG_EDGE,
			    0.0, 0.0);
		ic->edge[XTK_CTL_FIRE] = 0;
	}

	/* Edge weapon selects: weapon index 0..5, action "on". */
	for (i = XTK_CTL_WEAPON_1; i <= XTK_CTL_WEAPON_6; ++i) {
		if (ic->edge[i]) {
			add_command(ic, cur, &n, tick, XTK_OP_WEAPON,
				    XTK_FLAG_EDGE,
				    (double)(i - XTK_CTL_WEAPON_1), 1.0);
			ic->edge[i] = 0;
		}
	}

	/* Idle traffic: at least a heartbeat every tick. */
	if (n == 0) {
		add_command(ic, cur, &n, tick, XTK_OP_HEARTBEAT, 0, 0.0, 0.0);
	}

	return n;
}

int input_emit_reliable(XtkInputController *ic, int32_t server_tick,
			uint32_t epoch, uint32_t session_id,
			XtkMessage *batch_out, XtkMessage *reliable_out,
			int *have_reliable)
{
	XtkCommand cur[XTK_INPUT_MAX_BATCH];
	int cur_n = 0;
	int total = 0, rel_total = 0;
	int slot, i;

	if (have_reliable != NULL) {
		*have_reliable = 0;
	}
	memset(batch_out, 0, sizeof(*batch_out));
	batch_out->env.message_type = XTK_MSG_INPUT_BATCH;
	batch_out->env.epoch = epoch;
	batch_out->env.session_id = session_id;
	batch_out->env.tick = server_tick;
	batch_out->body.input_batch.packet_sequence = ic->next_packet_sequence;
	batch_out->body.input_batch.last_observed_server_tick =
		(uint32_t)server_tick;

	if (reliable_out != NULL) {
		memset(reliable_out, 0, sizeof(*reliable_out));
		reliable_out->env.message_type = XTK_MSG_RELIABLE_CMD;
		reliable_out->env.epoch = epoch;
		reliable_out->env.session_id = session_id;
		reliable_out->env.tick = server_tick;
		reliable_out->body.reliable_cmd.last_observed_server_tick =
			(uint32_t)server_tick;
	}

	/* Never emit on tick 0, and never emit gameplay outside Running. */
	if (server_tick <= 0 || ic->state != XTK_INPUT_RUNNING) {
		return 0;
	}

	cur_n = build_tick(ic, server_tick, cur);

	/* Store this tick's new commands as the newest ring slot. */
	slot = (ic->ring_head + 1) % (int)XTK_INPUT_RING;
	if (cur_n > 0) {
		memcpy(ic->ring[slot], cur, (size_t)cur_n * sizeof(XtkCommand));
		ic->ring_count[slot] = (uint8_t)cur_n;
		ic->ring_head = slot;
		if (ic->ring_filled < (int)XTK_INPUT_RING) {
			ic->ring_filled++;
		}
	}

	/*
	 * Oldest valid tick first, newest last: admitted order is preserved.  The
	 * same ring walk fills the reliable companion with the one-shot edges and
	 * held releases (identical command ids), so a lost unreliable datagram is
	 * recovered on channel 0 and the server's shared dedup applies once.
	 */
	for (i = ic->ring_filled - 1; i >= 0; --i) {
		int s = (ic->ring_head - i + (int)XTK_INPUT_RING) %
			(int)XTK_INPUT_RING;
		int k, c = ic->ring_count[s];
		for (k = 0; k < c; ++k) {
			const XtkCommand *cmd = &ic->ring[s][k];

			if (total < (int)XTK_INPUT_MAX_BATCH) {
				batch_out->body.input_batch.command[total++] =
					*cmd;
			}
			if (reliable_out != NULL &&
			    rel_total < (int)XTK_INPUT_MAX_BATCH &&
			    command_needs_reliable(cmd)) {
				reliable_out->body.reliable_cmd
					.command[rel_total++] = *cmd;
			}
		}
	}

	batch_out->body.input_batch.command_count = (uint16_t)total;
	if (reliable_out != NULL) {
		reliable_out->body.reliable_cmd.command_count =
			(uint16_t)rel_total;
		if (have_reliable != NULL && rel_total > 0) {
			*have_reliable = 1;
		}
	}
	if (total > 0) {
		ic->next_packet_sequence++;
	}
	return total;
}

int input_emit(XtkInputController *ic, int32_t server_tick, uint32_t epoch,
	       uint32_t session_id, XtkMessage *out)
{
	return input_emit_reliable(ic, server_tick, epoch, session_id, out,
				   NULL, NULL);
}
