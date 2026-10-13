/*
 * Tests/phase3/input_fixture.c -- P3.11 resilient live-input fixture.
 *
 * Exercises Client/input.c (pure C, no SDL, no engine) against deterministic
 * tick sequences, proving the P3.11 generation-stamped / loss-resilient rules:
 *
 *   1. every emitted command carries the controlled entity-life generation;
 *   2. one-shot fire/weapon edges and held releases are duplicated into a
 *      reliable companion (RELIABLE_CMD) that shares their command ids with the
 *      redundant INPUT_BATCH, so the server dedup applies each at most once;
 *   3. a held release survives the loss of the unreliable batch;
 *   4. a stale edge latched or already queued in a previous life cannot fire
 *      after a life change (latched edges and the redundancy history are
 *      discarded and the held state is restamped);
 *   5. input and heartbeat refresh at 15 Hz with no model/snapshot dependency;
 *   6. disconnect/reset clears held edges and the redundancy history while
 *      command ids stay monotonic (no reuse);
 *   7. the camera/DPI world-aim transform is unchanged.
 *
 * It links input.c + camera.c + the Protocol codec and nothing else.
 *
 * Authority: docs/PHASE3-PLAN.md P3.11; docs/PROTOCOL-v1.1.md sections 6, 8, 9.
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

/* ---- message helpers ------------------------------------------------- */

static int is_batch(const XtkMessage *m)
{
	return m->env.message_type == XTK_MSG_INPUT_BATCH;
}

static int batch_count(const XtkMessage *m)
{
	return (int)m->body.input_batch.command_count;
}

static const XtkCommand *batch_cmd(const XtkMessage *m, int i)
{
	return &m->body.input_batch.command[i];
}

static int rel_count(const XtkMessage *m)
{
	return (int)m->body.reliable_cmd.command_count;
}

static const XtkCommand *rel_cmd(const XtkMessage *m, int i)
{
	return &m->body.reliable_cmd.command[i];
}

static int batch_count_op(const XtkMessage *m, uint8_t op)
{
	int i, n = 0;
	for (i = 0; i < batch_count(m); i++) {
		if (batch_cmd(m, i)->op == op) {
			n++;
		}
	}
	return n;
}

static int batch_has_id(const XtkMessage *m, uint32_t id)
{
	int i;
	for (i = 0; i < batch_count(m); i++) {
		if (batch_cmd(m, i)->command_id == id) {
			return 1;
		}
	}
	return 0;
}

static int rel_has_id(const XtkMessage *m, uint32_t id)
{
	int i;
	for (i = 0; i < rel_count(m); i++) {
		if (rel_cmd(m, i)->command_id == id) {
			return 1;
		}
	}
	return 0;
}

static uint32_t fire_id_of(const XtkMessage *m)
{
	int i;
	for (i = 0; i < batch_count(m); i++) {
		if (batch_cmd(m, i)->op == XTK_OP_FIRE) {
			return batch_cmd(m, i)->command_id;
		}
	}
	return 0;
}

/* A tiny server-like shared dedup over both channels. */
typedef struct { uint32_t id[128]; int n; } Dedup;

static int dedup_apply(Dedup *d, uint32_t id)
{
	int i;
	for (i = 0; i < d->n; i++) {
		if (d->id[i] == id) {
			return 0;	/* already applied */
		}
	}
	if (d->n < (int)(sizeof(d->id) / sizeof(d->id[0]))) {
		d->id[d->n++] = id;
	}
	return 1;
}

/* ---- 1. every command carries the current life generation ------------ */

static void test_life_stamp(void)
{
	XtkInputController ic;
	XtkMessage batch, rel;
	int i, have_rel = 0;

	input_init(&ic);
	input_set_state(&ic, XTK_INPUT_RUNNING);
	input_set_life_generation(&ic, 42u);
	check(input_life_generation(&ic) == 42u, "life generation recorded");

	input_control_event(&ic, XTK_CTL_DRIVE_FWD, 1);
	input_control_event(&ic, XTK_CTL_FIRE, 1);
	input_control_event(&ic, XTK_CTL_WEAPON_2, 1);
	input_aim_world(&ic, 5.0, 6.0);
	input_emit_reliable(&ic, 1, 7, 9, &batch, &rel, &have_rel);

	for (i = 0; i < batch_count(&batch); i++) {
		checkf(batch_cmd(&batch, i)->life_generation == 42u,
		       "batch command carries the life stamp");
	}
	for (i = 0; i < rel_count(&rel); i++) {
		checkf(rel_cmd(&rel, i)->life_generation == 42u,
		       "reliable command carries the life stamp");
	}
	check(have_rel == 1, "reliable companion present for the edges");
}

/* ---- 2. reliable companion shares ids and applies once --------------- */

static void test_reliable_companion(void)
{
	XtkInputController ic;
	XtkMessage batch, rel, back;
	uint8_t buf[XTK_MAX_MESSAGE_BYTES];
	size_t len = 0;
	Dedup d;
	int i, fire_applied = 0, weapon_applied = 0, have_rel = 0;

	input_init(&ic);
	input_set_state(&ic, XTK_INPUT_RUNNING);
	input_set_life_generation(&ic, 3u);
	input_control_event(&ic, XTK_CTL_FIRE, 1);
	input_control_event(&ic, XTK_CTL_WEAPON_1, 1);
	input_emit_reliable(&ic, 1, 1, 1, &batch, &rel, &have_rel);

	check(is_batch(&batch), "primary message is INPUT_BATCH");
	check(rel.env.message_type == XTK_MSG_RELIABLE_CMD,
	      "companion is RELIABLE_CMD");
	check(have_rel == 1 && rel_count(&rel) == 2,
	      "companion carries the two one-shot edges");
	check(batch_count_op(&batch, XTK_OP_FIRE) == 1 &&
	      batch_count_op(&batch, XTK_OP_WEAPON) == 1,
	      "batch carries both edges");

	/* Same command ids on both channels. */
	for (i = 0; i < batch_count(&batch); i++) {
		if (batch_cmd(&batch, i)->op == XTK_OP_FIRE ||
		    batch_cmd(&batch, i)->op == XTK_OP_WEAPON) {
			checkf(rel_has_id(&rel, batch_cmd(&batch, i)->command_id),
			       "companion shares the edge command id");
		}
	}

	/* A shared dedup (as the server's) applies each edge exactly once. */
	memset(&d, 0, sizeof(d));
	for (i = 0; i < batch_count(&batch); i++) {
		const XtkCommand *c = batch_cmd(&batch, i);
		if (dedup_apply(&d, c->command_id)) {
			if (c->op == XTK_OP_FIRE) fire_applied++;
			if (c->op == XTK_OP_WEAPON) weapon_applied++;
		}
	}
	for (i = 0; i < rel_count(&rel); i++) {
		const XtkCommand *c = rel_cmd(&rel, i);
		if (dedup_apply(&d, c->command_id)) {
			if (c->op == XTK_OP_FIRE) fire_applied++;
			if (c->op == XTK_OP_WEAPON) weapon_applied++;
		}
	}
	check(fire_applied == 1, "fire edge applies exactly once");
	check(weapon_applied == 1, "weapon edge applies exactly once");

	/* The companion is a valid channel-0 wire message. */
	check(xtk_encode(buf, sizeof(buf), &rel, &len) == XTK_ERR_NONE,
	      "companion encodes");
	check(xtk_decode(buf, len, &back) == XTK_ERR_NONE, "companion decodes");
	check(back.env.channel == XTK_CH_CONTROL, "companion on channel 0");
	check(back.body.reliable_cmd.command_count == rel_count(&rel),
	      "companion count preserved");
	check(back.body.reliable_cmd.command[0].life_generation == 3u,
	      "companion life stamp preserved");

	/* The edge is re-carried reliably through the redundancy window. */
	input_emit_reliable(&ic, 2, 1, 1, &batch, &rel, &have_rel);
	check(have_rel == 1 && rel_has_id(&rel, fire_id_of(&batch)),
	      "edge remains reliably redundant in the window");

	/* Past the redundancy window an idle tick carries no companion. */
	input_emit_reliable(&ic, 3, 1, 1, &batch, &rel, &have_rel);
	input_emit_reliable(&ic, 4, 1, 1, &batch, &rel, &have_rel);
	check(have_rel == 0, "no reliable companion on an idle tick");
	check(batch_count_op(&batch, XTK_OP_HEARTBEAT) >= 1,
	      "idle tick is a heartbeat");
}

/* ---- 3. held release survives loss of the unreliable batch ----------- */

static void test_held_release_reliable(void)
{
	XtkInputController ic;
	XtkMessage batch, rel;
	int i, have_rel = 0, drive_rel = 0, fh_rel = 0;
	uint32_t drive_id = 0, fh_id = 0;

	input_init(&ic);
	input_set_state(&ic, XTK_INPUT_RUNNING);
	input_set_life_generation(&ic, 5u);

	input_control_event(&ic, XTK_CTL_DRIVE_FWD, 1);
	input_control_event(&ic, XTK_CTL_FIRE_HOLD, 1);
	input_emit_reliable(&ic, 1, 1, 1, &batch, &rel, &have_rel);

	/* Release both on tick 3. */
	input_control_event(&ic, XTK_CTL_DRIVE_FWD, 0);
	input_control_event(&ic, XTK_CTL_FIRE_HOLD, 0);
	input_emit_reliable(&ic, 3, 1, 1, &batch, &rel, &have_rel);

	check(have_rel == 1, "release tick has a reliable companion");
	for (i = 0; i < batch_count(&batch); i++) {
		const XtkCommand *c = batch_cmd(&batch, i);
		if (c->op == XTK_OP_DRIVE_REL && c->a == 0.0) drive_id = c->command_id;
		if (c->op == XTK_OP_FIRE_HOLD && c->a == 0.0) fh_id = c->command_id;
	}
	check(drive_id != 0 && fh_id != 0, "batch carries both releases");
	for (i = 0; i < rel_count(&rel); i++) {
		const XtkCommand *c = rel_cmd(&rel, i);
		if (c->op == XTK_OP_DRIVE_REL && c->a == 0.0) {
			drive_rel = 1;
			check(c->command_id == drive_id,
			      "drive release companion shares its id");
		}
		if (c->op == XTK_OP_FIRE_HOLD && c->a == 0.0) {
			fh_rel = 1;
			check(c->command_id == fh_id,
			      "fire release companion shares its id");
		}
	}
	check(drive_rel && fh_rel, "reliable companion carries both releases");

	/* Simulate the channel-2 batch being lost: only the companion arrives. */
	{
		Dedup d;
		int released = 0;
		memset(&d, 0, sizeof(d));
		for (i = 0; i < rel_count(&rel); i++) {
			if (dedup_apply(&d, rel_cmd(&rel, i)->command_id)) {
				if (rel_cmd(&rel, i)->a == 0.0) released++;
			}
		}
		check(released >= 2,
		      "held release survives loss of the unreliable batch");
	}
}

/* ---- 4. stale edge cannot fire after a life change ------------------- */

static void test_stale_edge_on_life_change(void)
{
	XtkInputController ic;
	XtkMessage batch, rel;
	int i, have_rel = 0;
	uint32_t old_fire_id = 0;

	input_init(&ic);
	input_set_state(&ic, XTK_INPUT_RUNNING);
	input_set_life_generation(&ic, 1u);

	/* A fire latched under life 1 and emitted (queued in the ring). */
	input_control_event(&ic, XTK_CTL_FIRE, 1);
	input_emit_reliable(&ic, 1, 1, 1, &batch, &rel, &have_rel);
	old_fire_id = fire_id_of(&batch);
	check(old_fire_id != 0, "life-1 fire edge emitted");

	/* Life changes to 2 before the redundancy window closes. */
	input_set_life_generation(&ic, 2u);
	check(input_life_generation(&ic) == 2u, "life advanced to 2");

	/* A newly latched fire and the held drive must carry the new life. */
	input_control_event(&ic, XTK_CTL_FIRE, 1);
	input_control_event(&ic, XTK_CTL_DRIVE_FWD, 1);
	input_emit_reliable(&ic, 2, 1, 1, &batch, &rel, &have_rel);

	check(!batch_has_id(&batch, old_fire_id),
	      "queued old-life edge discarded on life change");
	check(!rel_has_id(&rel, old_fire_id),
	      "queued old-life edge absent from the companion");
	for (i = 0; i < batch_count(&batch); i++) {
		checkf(batch_cmd(&batch, i)->life_generation == 2u,
		       "post-change command carries life 2");
	}
	check(batch_count_op(&batch, XTK_OP_FIRE) == 1,
	      "the new-life fire is still emitted");

	/* A latched edge is dropped if the life changes before it is emitted. */
	input_set_life_generation(&ic, 3u);
	input_control_event(&ic, XTK_CTL_FIRE, 1);
	input_set_life_generation(&ic, 4u);
	input_emit_reliable(&ic, 3, 1, 1, &batch, &rel, &have_rel);
	for (i = 0; i < batch_count(&batch); i++) {
		checkf(batch_cmd(&batch, i)->op != XTK_OP_FIRE ||
		       batch_cmd(&batch, i)->life_generation == 4u,
		       "stale latched fire never emitted");
	}
	check(batch_count_op(&batch, XTK_OP_FIRE) == 0,
	      "fire latched in life 3 is not fired under life 4");
}

/* ---- 5. 15 Hz input/heartbeat, no model dependency ------------------- */

static void test_15hz_independent(void)
{
	XtkInputController ic;
	XtkMessage batch, rel;
	uint32_t prev_seq = 0;
	int t, ok = 1;

	input_init(&ic);
	input_set_state(&ic, XTK_INPUT_RUNNING);
	/* No life generation is set: no snapshot/model is involved at all. */

	for (t = 1; t <= 15; t++) {
		int have_rel = 1;
		int n = input_emit_reliable(&ic, t, 1, 1, &batch, &rel,
					    &have_rel);
		if (n < 1) {
			ok = 0;
		}
		if (t > 1 &&
		    batch.body.input_batch.packet_sequence <= prev_seq) {
			ok = 0;
		}
		prev_seq = batch.body.input_batch.packet_sequence;
		if (batch_count_op(&batch, XTK_OP_HEARTBEAT) < 1) {
			ok = 0;
		}
		if (have_rel != 0) {
			ok = 0;	/* idle -> no false reliable traffic */
		}
	}
	check(ok, "15 idle ticks emit heartbeat without any snapshot/model");
}

/* ---- 6. disconnect/reset clears state, command ids stay monotonic ---- */

static void test_reset_scope(void)
{
	XtkInputController ic;
	XtkMessage batch, rel;
	uint32_t max_id = 0;
	int i, have_rel = 0;

	input_init(&ic);
	input_set_state(&ic, XTK_INPUT_RUNNING);
	input_set_life_generation(&ic, 7u);

	input_control_event(&ic, XTK_CTL_FIRE, 1);
	input_control_event(&ic, XTK_CTL_DRIVE_FWD, 1);
	input_emit_reliable(&ic, 1, 1, 1, &batch, &rel, &have_rel);
	for (i = 0; i < batch_count(&batch); i++) {
		if (batch_cmd(&batch, i)->command_id > max_id) {
			max_id = batch_cmd(&batch, i)->command_id;
		}
	}
	check(max_id != 0, "commands issued before reset");

	/* Disconnect clears held/edges/history. */
	input_set_state(&ic, XTK_INPUT_DISCONNECTED);
	check(input_emit_reliable(&ic, 2, 1, 1, &batch, &rel, &have_rel) == 0,
	      "no emission while disconnected");

	/* Reconnect: no stale edge/hold survives, ids continue. */
	input_set_state(&ic, XTK_INPUT_RUNNING);
	input_set_life_generation(&ic, 8u);
	input_emit_reliable(&ic, 3, 1, 1, &batch, &rel, &have_rel);
	check(batch_count_op(&batch, XTK_OP_FIRE) == 0,
	      "disconnect discarded the latched fire");
	check(batch_count_op(&batch, XTK_OP_DRIVE_REL) == 0,
	      "disconnect discarded the held drive");
	for (i = 0; i < batch_count(&batch); i++) {
		if (batch_cmd(&batch, i)->op == XTK_OP_HEARTBEAT) {
			check(batch_cmd(&batch, i)->command_id > max_id,
			      "command id scope stays monotonic across disconnect");
		}
	}

	/* input_reset clears redundancy/history without reusing an id. */
	max_id = 0;
	input_emit_reliable(&ic, 4, 1, 1, &batch, &rel, &have_rel);
	for (i = 0; i < batch_count(&batch); i++) {
		if (batch_cmd(&batch, i)->command_id > max_id) {
			max_id = batch_cmd(&batch, i)->command_id;
		}
	}
	input_reset(&ic);
	input_set_state(&ic, XTK_INPUT_RUNNING);
	input_emit_reliable(&ic, 5, 1, 1, &batch, &rel, &have_rel);
	check(batch_count(&batch) >= 1, "post-reset tick emits");
	for (i = 0; i < batch_count(&batch); i++) {
		checkf(batch_cmd(&batch, i)->command_id > max_id,
		       "command id not reused after reset");
	}
}

/* ---- 7. camera/DPI world-aim transform is unchanged ------------------ */

static void test_screen_transform(void)
{
	XtkCamera cam;
	double wx, wy;

	camera_init(&cam);
	camera_center_on(&cam, 1000.0, 2000.0);
	input_screen_to_world(&cam, 0.0f, 0.0f, 2.0f, &wx, &wy);
	checkf(wx == 1000.0 - 384.0 && wy == 2000.0 - 384.0,
	       "screen->world corner unchanged (%.1f,%.1f)", wx, wy);
	input_screen_to_world(&cam, 100.0f, 0.0f, 1.0f, &wx, &wy);
	checkf(wx == 1000.0 - 384.0 + 100.0, "1x DPI offset unchanged (%.1f)", wx);
	input_screen_to_world(&cam, 100.0f, 0.0f, 2.0f, &wx, &wy);
	checkf(wx == 1000.0 - 384.0 + 200.0, "2x DPI offset unchanged (%.1f)", wx);
}

int main(void)
{
	printf("== P3.11 resilient input fixture\n");
	test_life_stamp();
	test_reliable_companion();
	test_held_release_reliable();
	test_stale_edge_on_life_change();
	test_15hz_independent();
	test_reset_scope();
	test_screen_transform();

	printf("checks=%d failures=%d\n", checks, failures);
	if (failures == 0) {
		printf("P3.11 INPUT FIXTURE PASS\n");
		return 0;
	}
	printf("P3.11 INPUT FIXTURE FAIL\n");
	return 1;
}
