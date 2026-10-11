/*
 * Tests/phase2/input_fixture.c -- P2.12 client input-controller acceptance.
 *
 * Exercises Client/input.c (pure C, no SDL) against deterministic tick
 * sequences:
 *
 *   1. neutral controls map to authorized wire ops (edge vs held, drive/fire);
 *   2. no emission on tick 0 or outside Running;
 *   3. one-shot fire/weapon edges fire once and are carried only by the
 *      bounded two-tick redundancy window;
 *   4. held drive/fire-hold refresh each tick and release exactly once when
 *      lifted;
 *   5. pause is honored only outside Running;
 *   6. window defocus and quit release every held control;
 *   7. 15 Hz emission, packet sequencing and idle heartbeat;
 *   8. the inverse screen->world camera/DPI transform at corners, a nonzero
 *      origin and a scaled window.
 *
 * It links input.c + camera.c + the Protocol codec and nothing else: no SDL,
 * no simulation and no bots.
 *
 * Authority: docs/PHASE2-PLAN.md P2.12; docs/PROTOCOL-v1.md sections 6, 8, 9.
 */

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "input.h"
#include "camera.h"

static int checks, failures;

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

/* Count commands with `op` in the batch. */
static int count_op(const XtkMessage *m, uint8_t op)
{
	int i, n = 0;
	for (i = 0; i < (int)m->body.input_batch.command_count; ++i) {
		if (m->body.input_batch.command[i].op == op) {
			n++;
		}
	}
	return n;
}

/* Count commands with `op` carrying a specific command_id. */
static int count_id_op(const XtkMessage *m, uint8_t op, uint32_t id)
{
	int i, n = 0;
	for (i = 0; i < (int)m->body.input_batch.command_count; ++i) {
		const XtkCommand *c = &m->body.input_batch.command[i];
		if (c->op == op && c->command_id == id) {
			n++;
		}
	}
	return n;
}

/* Encode and decode a batch, asserting byte-level validity. */
static int roundtrip(const XtkMessage *m, XtkMessage *back, size_t *len_out)
{
	uint8_t buf[XTK_MAX_MESSAGE_BYTES];
	size_t len = 0;
	int rc;

	rc = xtk_encode(buf, sizeof(buf), m, &len);
	checkf(rc == XTK_ERR_NONE, "encode -> err %d", rc);
	if (rc != XTK_ERR_NONE) {
		return -1;
	}
	rc = xtk_decode(buf, len, back);
	checkf(rc == XTK_ERR_NONE, "decode -> err %d", rc);
	if (len_out != NULL) {
		*len_out = len;
	}
	return rc;
}

static void test_separation_and_ops(void)
{
	XtkInputController ic;
	XtkMessage msg;
	int i;

	input_init(&ic);
	input_set_state(&ic, XTK_INPUT_RUNNING);

	/* Every gameplay control must map to a release-authorized wire op. */
	input_control_event(&ic, XTK_CTL_DRIVE_FWD, 1);
	input_control_event(&ic, XTK_CTL_FIRE, 1);
	input_control_event(&ic, XTK_CTL_WEAPON_3, 1);
	input_control_event(&ic, XTK_CTL_FIRE_HOLD, 1);
	input_aim_world(&ic, 1234.5, -678.25);
	input_emit(&ic, 1, 7, 9, &msg);

	for (i = 0; i < (int)msg.body.input_batch.command_count; ++i) {
		const XtkCommand *c = &msg.body.input_batch.command[i];
		checkf(XTK_OP_RELEASE_AUTHORIZED(c->op), "op 0x%02x authorized",
		       c->op);
		checkf(c->reserved == 0, "reserved zero");
		checkf(c->command_id != 0, "nonzero command id");
	}

	/* Wire ops are the protocol identifiers, never SDL/engine values. */
	check(XTK_OP_FIRE == 0x04 && XTK_OP_DRIVE_REL == 0x01 &&
	      XTK_OP_WEAPON == 0x05 && XTK_OP_AIM_WORLD == 0x06,
	      "frozen wire op values");
	check(count_op(&msg, XTK_OP_DRIVE_REL) == 1, "drive emitted");
	check(count_op(&msg, XTK_OP_FIRE) == 1, "fire edge emitted");
	check(count_op(&msg, XTK_OP_WEAPON) == 1, "weapon edge emitted");
	check(count_op(&msg, XTK_OP_FIRE_HOLD) == 1, "fire-hold emitted");
	check(count_op(&msg, XTK_OP_AIM_WORLD) == 1, "aim emitted");

	/* The fire-hold command carries a=1 (held). */
	for (i = 0; i < (int)msg.body.input_batch.command_count; ++i) {
		const XtkCommand *c = &msg.body.input_batch.command[i];
		if (c->op == XTK_OP_FIRE_HOLD) {
			check(c->a == 1.0 && c->flags == XTK_FLAG_HELD,
			      "fire-hold held flag/value");
		}
		if (c->op == XTK_OP_WEAPON) {
			check(c->a == 2.0 && c->b == 1.0 &&
			      c->flags == XTK_FLAG_EDGE,
			      "weapon 3 -> a=2,b=1 edge");
		}
		if (c->op == XTK_OP_FIRE) {
			check(c->flags == XTK_FLAG_EDGE, "fire edge flag");
		}
	}
}

static void test_no_emit_tick0_or_nonrunning(void)
{
	XtkInputController ic;
	XtkMessage msg;
	int n;

	input_init(&ic);
	n = input_emit(&ic, 0, 1, 1, &msg);
	check(n == 0, "no emit on tick 0");
	n = input_emit(&ic, -5, 1, 1, &msg);
	check(n == 0, "no emit on negative tick");

	input_set_state(&ic, XTK_INPUT_LOBBY);
	input_control_event(&ic, XTK_CTL_FIRE, 1);
	input_control_event(&ic, XTK_CTL_DRIVE_FWD, 1);
	n = input_emit(&ic, 5, 1, 1, &msg);
	check(n == 0, "no emit in Lobby");
	check(msg.body.input_batch.command_count == 0, "empty Lobby batch");

	/* Pressing before the start must not leak into the first Running tick. */
	input_set_state(&ic, XTK_INPUT_RUNNING);
	n = input_emit(&ic, 6, 1, 1, &msg);
	check(count_op(&msg, XTK_OP_FIRE) == 0, "lobby fire did not leak");
	check(count_op(&msg, XTK_OP_DRIVE_REL) == 0, "lobby drive did not leak");
	check(n == 1 && count_op(&msg, XTK_OP_HEARTBEAT) == 1,
	      "first Running tick is heartbeat");
}

static void test_edge_redundancy(void)
{
	XtkInputController ic;
	XtkMessage msg, back;
	uint32_t fire_id = 0;
	int i, seen_first = 0, seen_window = 0, seen_after = 0;

	input_init(&ic);
	input_set_state(&ic, XTK_INPUT_RUNNING);

	/* Press fire on tick 1; release immediately (still one edge). */
	input_control_event(&ic, XTK_CTL_FIRE, 1);
	input_control_event(&ic, XTK_CTL_FIRE, 0);
	input_emit(&ic, 1, 1, 1, &msg);
	check(count_op(&msg, XTK_OP_FIRE) == 1, "exactly one fire edge");
	for (i = 0; i < (int)msg.body.input_batch.command_count; ++i) {
		const XtkCommand *c = &msg.body.input_batch.command[i];
		if (c->op == XTK_OP_FIRE) {
			fire_id = c->command_id;
			seen_first = 1;
			check(c->tick_hint == 1, "fire tick_hint");
		}
	}
	check(seen_first && fire_id != 0, "fire id captured");

	/* Redundancy window: the same id reappears, never a second edge. */
	for (i = 0; i < (int)XTK_INPUT_REDUNDANCY_TICKS; ++i) {
		int t = 2 + i;
		input_emit(&ic, t, 1, 1, &msg);
		checkf(count_op(&msg, XTK_OP_FIRE) == 1,
		       "tick %d carries the single edge", t);
		if (count_id_op(&msg, XTK_OP_FIRE, fire_id) == 1) {
			seen_window++;
		}
		/* A second edge would carry a new id; there must be exactly one. */
		checkf(count_op(&msg, XTK_OP_FIRE) == 1,
		       "no second edge on tick %d", t);
	}
	check(seen_window == (int)XTK_INPUT_REDUNDANCY_TICKS,
	      "edge carried by every redundancy tick");

	/* After the window the command is gone. */
	input_emit(&ic, 2 + (int)XTK_INPUT_REDUNDANCY_TICKS + 1, 1, 1, &msg);
	if (count_id_op(&msg, XTK_OP_FIRE, fire_id) > 0) {
		seen_after = 1;
	}
	check(seen_after == 0, "edge expires after the window");

	/* The whole batch is codec-valid on the input channel. */
	input_control_event(&ic, XTK_CTL_FIRE, 1);
	input_emit(&ic, 20, 3, 4, &msg);
	if (roundtrip(&msg, &back, NULL) == XTK_ERR_NONE) {
		check(back.env.channel == XTK_CH_INPUT, "batch on channel 2");
		check(back.env.message_type == XTK_MSG_INPUT_BATCH, "type");
		check(back.env.epoch == 3 && back.env.session_id == 4,
		      "epoch/session preserved");
		check(back.env.tick == 20, "tick preserved");
		check(back.body.input_batch.command_count ==
		      msg.body.input_batch.command_count, "count preserved");
		check(back.body.input_batch.packet_sequence ==
		      msg.body.input_batch.packet_sequence, "packet sequence");
	}
}

static void test_held_refresh_and_release(void)
{
	XtkInputController ic;
	XtkMessage msg;
	uint32_t drive_id = 0, fh_id = 0;
	int i;

	input_init(&ic);
	input_set_state(&ic, XTK_INPUT_RUNNING);

	/* Press and hold drive + fire-hold. */
	input_control_event(&ic, XTK_CTL_DRIVE_FWD, 1);
	input_control_event(&ic, XTK_CTL_FIRE_HOLD, 1);
	input_emit(&ic, 1, 1, 1, &msg);
	for (i = 0; i < (int)msg.body.input_batch.command_count; ++i) {
		const XtkCommand *c = &msg.body.input_batch.command[i];
		if (c->op == XTK_OP_DRIVE_REL) {
			drive_id = c->command_id;
			check(c->a == XTK_INPUT_DRIVE_SPEED && c->flags == XTK_FLAG_HELD,
			      "forward drive value");
		}
		if (c->op == XTK_OP_FIRE_HOLD) {
			fh_id = c->command_id;
		}
	}
	check(drive_id != 0 && fh_id != 0, "held ids present");

	/* Tick 2: refreshed with a new id (freshness), still held.  The redundant
	 * tick-1 copy keeps the old id, so a fresh id is what proves refresh. */
	input_emit(&ic, 2, 1, 1, &msg);
	{
		int fresh = 0;
		for (i = 0; i < (int)msg.body.input_batch.command_count; ++i) {
			const XtkCommand *c = &msg.body.input_batch.command[i];
			if (c->op == XTK_OP_DRIVE_REL &&
			    c->command_id != drive_id) {
				fresh = 1;
			}
		}
		check(fresh, "drive refreshed with a new id");
	}

	/* Release; the release (a=0) appears once, then expires after the
	 * redundancy window (release at tick 3 is carried through tick 5). */
	input_control_event(&ic, XTK_CTL_DRIVE_FWD, 0);
	input_control_event(&ic, XTK_CTL_FIRE_HOLD, 0);
	input_emit(&ic, 3, 1, 1, &msg);
	{
		int drive0 = 0, fh0 = 0;
		for (i = 0; i < (int)msg.body.input_batch.command_count; ++i) {
			const XtkCommand *c = &msg.body.input_batch.command[i];
			if (c->op == XTK_OP_DRIVE_REL && c->a == 0.0) drive0 = 1;
			if (c->op == XTK_OP_FIRE_HOLD && c->a == 0.0) fh0 = 1;
		}
		check(drive0 && fh0, "release commands emitted");
	}
	input_emit(&ic, 4, 1, 1, &msg);
	input_emit(&ic, 5, 1, 1, &msg);
	input_emit(&ic, 6, 1, 1, &msg);
	check(count_op(&msg, XTK_OP_DRIVE_REL) == 0,
	      "drive release not repeated (beyond redundancy)");
	check(count_op(&msg, XTK_OP_FIRE_HOLD) == 0,
	      "fire-hold release not repeated (beyond redundancy)");
}

static void test_pause_only_outside_running(void)
{
	XtkInputController ic;
	XtkMessage msg;

	input_init(&ic);
	input_set_state(&ic, XTK_INPUT_RUNNING);
	input_control_event(&ic, XTK_CTL_PAUSE, 1);
	check(input_paused(&ic) == 0, "pause ignored while Running");
	input_emit(&ic, 1, 1, 1, &msg);
	check(count_op(&msg, XTK_OP_HEARTBEAT) == 1, "running unaffected");

	/* Outside Running, pause toggles and still emits no gameplay. */
	input_set_state(&ic, XTK_INPUT_LOBBY);
	input_control_event(&ic, XTK_CTL_PAUSE, 1);
	check(input_paused(&ic) == 1, "pause toggles in Lobby");
	input_control_event(&ic, XTK_CTL_PAUSE, 1);
	check(input_paused(&ic) == 0, "pause toggles back in Lobby");
	input_control_event(&ic, XTK_CTL_PAUSE, 1);
	input_control_event(&ic, XTK_CTL_FIRE, 1);
	input_control_event(&ic, XTK_CTL_DRIVE_FWD, 1);
	check(input_emit(&ic, 2, 1, 1, &msg) == 0,
	      "paused lobby emits nothing");
}

static void test_defocus_and_quit_release(void)
{
	XtkInputController ic;
	XtkMessage msg;
	int i, drive0, fh0;

	input_init(&ic);
	input_set_state(&ic, XTK_INPUT_RUNNING);

	/* Defocus releases held controls. */
	input_control_event(&ic, XTK_CTL_DRIVE_FWD, 1);
	input_control_event(&ic, XTK_CTL_FIRE_HOLD, 1);
	input_emit(&ic, 1, 1, 1, &msg);
	input_set_focus(&ic, 0);
	check(input_focused(&ic) == 0, "focus lost reported");
	input_emit(&ic, 2, 1, 1, &msg);
	drive0 = fh0 = 0;
	for (i = 0; i < (int)msg.body.input_batch.command_count; ++i) {
		const XtkCommand *c = &msg.body.input_batch.command[i];
		if (c->op == XTK_OP_DRIVE_REL && c->a == 0.0) drive0 = 1;
		if (c->op == XTK_OP_FIRE_HOLD && c->a == 0.0) fh0 = 1;
	}
	check(drive0 && fh0, "defocus released drive and fire-hold");

	/* Quit releases and clears held state. */
	input_set_focus(&ic, 1);
	input_control_event(&ic, XTK_CTL_DRIVE_FWD, 1);
	input_emit(&ic, 3, 1, 1, &msg);
	input_control_event(&ic, XTK_CTL_QUIT, 1);
	check(input_quit_requested(&ic) == 1, "quit requested");
	input_emit(&ic, 4, 1, 1, &msg);
	drive0 = 0;
	for (i = 0; i < (int)msg.body.input_batch.command_count; ++i) {
		const XtkCommand *c = &msg.body.input_batch.command[i];
		if (c->op == XTK_OP_DRIVE_REL && c->a == 0.0) drive0 = 1;
	}
	check(drive0, "quit released drive");
}

static void test_rate_and_sequencing(void)
{
	XtkInputController ic;
	XtkMessage msg;
	uint32_t prev_seq = 0;
	int t, ok = 1;

	input_init(&ic);
	input_set_state(&ic, XTK_INPUT_RUNNING);

	/* One batch per 15 Hz tick, monotonic packet sequence, idle heartbeat. */
	for (t = 1; t <= 15; ++t) {
		int n = input_emit(&ic, t, 1, 1, &msg);
		if (n < 1) {
			ok = 0;
		}
		if (t > 1 && msg.body.input_batch.packet_sequence <= prev_seq) {
			ok = 0;
		}
		prev_seq = msg.body.input_batch.packet_sequence;
		if (count_op(&msg, XTK_OP_HEARTBEAT) < 1) {
			ok = 0;
		}
		if (msg.body.input_batch.command_count >
		    XTK_INPUT_MAX_BATCH) {
			ok = 0;
		}
	}
	check(ok, "15 batches with heartbeat and monotonic sequence");
}

static void test_screen_transform(void)
{
	XtkCamera cam;
	double wx, wy;
	float scale = 2.0f;

	camera_init(&cam);
	/* Nonzero origin camera. */
	camera_center_on(&cam, 1000.0, 2000.0);

	/* Top-left window corner at 2x DPI: drawable (0,0). */
	input_screen_to_world(&cam, 0.0f, 0.0f, scale, &wx, &wy);
	checkf(wx == 1000.0 - 384.0 && wy == 2000.0 - 384.0,
	       "corner TL = center - half (%.1f,%.1f)", wx, wy);

	/* Bottom-right window corner at the legacy viewport size. */
	input_screen_to_world(&cam, 384.0f, 384.0f, scale, &wx, &wy);
	checkf(wx == 1000.0 + 384.0 && wy == 2000.0 + 384.0,
	       "center+half = center + half (%.1f,%.1f)", wx, wy);

	/* Display scale scales the drawable offset. */
	input_screen_to_world(&cam, 100.0f, 0.0f, 1.0f, &wx, &wy);
	checkf(wx == 1000.0 - 384.0 + 100.0, "1x scale offset (%.1f)", wx);
	input_screen_to_world(&cam, 100.0f, 0.0f, 2.0f, &wx, &wy);
	checkf(wx == 1000.0 - 384.0 + 200.0, "2x scale offset (%.1f)", wx);

	/* Default camera at the origin. */
	camera_init(&cam);
	input_screen_to_world(&cam, 0.0f, 0.0f, 1.0f, &wx, &wy);
	checkf(wx == -384.0 && wy == -384.0, "default origin (%.1f,%.1f)", wx,
	       wy);
}

int main(void)
{
	printf("== P2.12 input fixture\n");
	test_separation_and_ops();
	test_no_emit_tick0_or_nonrunning();
	test_edge_redundancy();
	test_held_refresh_and_release();
	test_pause_only_outside_running();
	test_defocus_and_quit_release();
	test_rate_and_sequencing();
	test_screen_transform();

	printf("checks=%d failures=%d\n", checks, failures);
	if (failures == 0) {
		printf("P2.12 INPUT FIXTURE PASS\n");
		return 0;
	}
	printf("P2.12 INPUT FIXTURE FAIL\n");
	return 1;
}
