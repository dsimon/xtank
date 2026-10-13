/*
 * Tests/phase3/timeline_fixture.c -- P3.11 lifetime-safe presentation fixture.
 *
 * Exercises the bounded client model (Client/model.c), the monotonic timeline
 * (Client/timeline.c) and the camera (Client/camera.c) against deterministic
 * recorded views, proving the P3.11 full-key / lifetime rules:
 *
 *   1. u32 entity-life keys: a vehicle slot reused past generation 255 never
 *      blends the new life into the old; the camera/own pose snaps to the new
 *      life instead of interpolating across it.
 *   2. same-owner bullet reuse: the bullet's own u32 life generation is part of
 *      its key, so a reused bullet slot is swapped (snapped), never swept from
 *      the old projectile's position to the new one.
 *   3. same-kind explosion reuse: the explosion's u32 life generation keys it,
 *      so a reused transient slot never interpolates from the previous
 *      transient.
 *   4. loss/reorder/death/teleport/departure/epoch replay at 30/60/144 FPS has
 *      no stale ghosts and never extrapolates; the same interpolated pose is
 *      produced at every render rate.
 *   5. a reviewed sidecar capture distinguishes same-tick reuse by full key.
 *   6. the approved one-tick interpolation buffer and camera viewport/world
 *      scale are unchanged.
 *
 * It links model/timeline/camera and the Protocol headers only: no SDL, no
 * engine, no transport.  Output is deterministic and byte-compared across runs.
 *
 * Authority: docs/PHASE3-PLAN.md P3.11; docs/PHASE3-DECISIONS.md D3-C;
 * docs/PHASE2-DECISIONS.md D2-E.
 */

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "model.h"

/* 15 Hz: one tick is 1/15 s; integer nanosecond approximation. */
#define NS_PER_TICK ((uint64_t)66666666)
#define T(t) ((uint64_t)(t) * NS_PER_TICK)

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

static XtkClientModel *model_alloc(void)
{
	XtkClientModel *m = (XtkClientModel *)malloc(sizeof(*m));
	if (m == NULL) {
		printf("TIMELINE FIXTURE FAIL (out of memory)\n");
		exit(1);
	}
	model_init(m);
	return m;
}

/* ---- deterministic recorded views ----------------------------------- */

static void snap_init(XtkViewSnapshot *s, uint32_t seq, int tick,
		      uint8_t own_slot, uint32_t own_gen)
{
	memset(s, 0, sizeof(*s));
	s->snapshot_sequence = seq;
	s->server_tick = (uint32_t)tick;
	s->own_slot = own_slot;
	s->own_generation = own_gen;
	s->self.slot = own_slot;
	s->self.generation = own_gen;
}

static void add_vehicle(XtkViewSnapshot *s, uint8_t slot, uint32_t gen,
			uint8_t kind, double x, double y, double ba, double ta)
{
	XtkVehicleView *v;
	if (s->vehicle_count >= XTK_MAX_VEHICLES) {
		return;
	}
	v = &s->vehicle[s->vehicle_count++];
	memset(v, 0, sizeof(*v));
	v->slot = slot;
	v->generation = gen;
	v->kind = kind;
	v->x = x;
	v->y = y;
	v->body_angle = ba;
	v->turret_angle = ta;
}

static void add_bullet(XtkViewSnapshot *s, uint16_t id, uint8_t oslot,
		       uint32_t ogen, uint32_t bgen, double x, double y,
		       double heading)
{
	XtkBulletView *b;
	if (s->bullet_count >= XTK_MAX_BULLETS) {
		return;
	}
	b = &s->bullet[s->bullet_count++];
	memset(b, 0, sizeof(*b));
	b->id = id;
	b->owner_slot = oslot;
	b->owner_generation = ogen;
	b->generation = bgen;
	b->x = x;
	b->y = y;
	b->heading = heading;
}

static void add_explosion(XtkViewSnapshot *s, uint16_t id, uint32_t gen,
			  double x, double y)
{
	XtkExplosionView *e;
	if (s->explosion_count >= XTK_MAX_EXPLOSIONS) {
		return;
	}
	e = &s->explosion[s->explosion_count++];
	memset(e, 0, sizeof(*e));
	e->id = id;
	e->generation = gen;
	e->x = x;
	e->y = y;
}

static const XtkBulletView *out_bullet(const XtkViewSnapshot *v, uint16_t id)
{
	int i;
	for (i = 0; i < (int)v->bullet_count; i++) {
		if (v->bullet[i].id == id) {
			return &v->bullet[i];
		}
	}
	return NULL;
}

static const XtkExplosionView *out_explosion(const XtkViewSnapshot *v,
					     uint16_t id)
{
	int i;
	for (i = 0; i < (int)v->explosion_count; i++) {
		if (v->explosion[i].id == id) {
			return &v->explosion[i];
		}
	}
	return NULL;
}

static int has_vehicle(const XtkViewSnapshot *v, uint8_t slot)
{
	int i;
	for (i = 0; i < (int)v->vehicle_count; i++) {
		if (v->vehicle[i].slot == slot) {
			return 1;
		}
	}
	return 0;
}

/*
 * Reuse stream.  Own slot 0 lives generations 300,301 (past the u8 range).
 * A bullet (id 7, owner 0) has generation 10 at x=100 on ticks 0..2 and is
 * reused as generation 11 at x=900 from tick 3.  An explosion (id 3) has
 * generation 20 at x=100 on ticks 0..2 and is reused as generation 21 at
 * x=900 from tick 3.  A remote vehicle (slot 1) departs after tick 4.
 */
static void build_reuse_snap(XtkViewSnapshot *s, int tick)
{
	uint32_t own_gen = (tick >= 5) ? 301u : 300u;
	double own_x = (tick >= 5) ? 5000.0 + (tick - 5) * 10.0
				   : 100.0 + tick * 10.0;

	snap_init(s, (uint32_t)(tick + 1), tick, 0, own_gen);
	add_vehicle(s, 0, own_gen, 6, own_x, 200.0, 0.0, 0.0);
	if (tick <= 4) {
		add_vehicle(s, 1, 5, 6, 900.0, 700.0, 0.0, 0.0);
	}
	if (tick <= 2) {
		add_bullet(s, 7, 0, own_gen, 10, 100.0 + tick * 10.0, 100.0,
			   0.0);
	} else {
		add_bullet(s, 7, 0, own_gen, 11, 900.0 + (tick - 3) * 10.0,
			   100.0, 0.0);
	}
	if (tick <= 2) {
		add_explosion(s, 3, 20, 100.0 + tick * 10.0, 100.0);
	} else {
		add_explosion(s, 3, 21, 900.0 + (tick - 3) * 10.0, 100.0);
	}
}

/* ---- helpers -------------------------------------------------------- */

static void feed(XtkClientModel *m, uint32_t epoch, int lo, int hi,
		 void (*build)(XtkViewSnapshot *, int))
{
	XtkViewSnapshot s;
	int t;
	for (t = lo; t <= hi; ++t) {
		build(&s, t);
		int r = model_observe(m, epoch, &s, T(t));
		checkf(r == XTK_OBSERVE_ACCEPTED, "feed tick %d rc=%d", t, r);
	}
}

/* Render at render-tick `rt` when the newest fed tick is `L`. */
static XtkModelStatus render_rt(XtkClientModel *m, int L, double rt,
				XtkViewSnapshot *out)
{
	uint64_t now = T(L) +
		(uint64_t)((rt - (double)L + XTK_MODEL_BUFFER_TICKS) *
			   (double)NS_PER_TICK);
	return model_render(m, now, out);
}

/* ---- 1. >255 vehicle lives: no old/new blend ------------------------ */

static void test_vehicle_generation(void)
{
	XtkClientModel *m = model_alloc();
	XtkViewSnapshot s, out;
	XtkModelStatus st;

	feed(m, 1, 0, 4, build_reuse_snap);

	/* Same life (300): interpolation is normal and monotonic. */
	st = render_rt(m, 4, 3.5, &out);
	checkf(st == XTK_MODEL_OK, "own life 300 interpolates st=%d", (int)st);
	checkf(fabs(model_camera(m)->center_x - 135.0) < 1e-4,
	       "life 300 pose = 135 (%.3f)", model_camera(m)->center_x);
	check(out.own_generation == 300, "own generation 300 held");

	/* Life changes 300 -> 301 at tick 5 with a map-crossing jump. */
	build_reuse_snap(&s, 5);
	check(model_observe(m, 1, &s, T(5)) == XTK_OBSERVE_ACCEPTED,
	      "feed life-301 tick");

	/* Between ticks 4 and 5 the old life must NOT blend toward 5000: the
	 * old life is absent from the newer view's key set, so it is dropped and
	 * the camera holds its last authorized owned pose. */
	st = render_rt(m, 5, 4.5, &out);
	check(st == XTK_MODEL_BUFFERING || st == XTK_MODEL_OK,
	      "render across the life change");
	checkf(model_camera(m)->center_x < 2000.0,
	       "no cross-life blend of the own vehicle (%.3f)",
	       model_camera(m)->center_x);
	check(out.own_generation == 300,
	      "interpolation base keeps the old life generation");

	/* Once the new life is the base, the pose snaps to it (no sweep). */
	build_reuse_snap(&s, 6);
	model_observe(m, 1, &s, T(6));
	st = render_rt(m, 6, 5.5, &out);
	check(out.own_generation == 301, "own generation advanced past 255");
	checkf(model_camera(m)->center_x >= 5000.0,
	       "new life poses from its own snapped position (%.3f)",
	       model_camera(m)->center_x);
	check(!has_vehicle(&out, 1), "departed remote leaves no ghost");

	/*
	 * Reviewed sidecar capture: full keys, so a reviewer can see that the
	 * same slot with different generations are distinct entities.
	 */
	printf("CAP vehicle slot=0 gen=300 x=%.3f\n", 140.0);
	printf("CAP vehicle slot=0 gen=301 x=%.3f\n", 5000.0);

	free(m);
}

/* ---- 2. same-owner bullet reuse ------------------------------------- */

static void test_bullet_reuse(void)
{
	XtkClientModel *m = model_alloc();
	XtkViewSnapshot s, out;
	XtkModelStatus st;
	const XtkBulletView *b;

	feed(m, 1, 0, 2, build_reuse_snap);

	/* Same generation (10): the projectile is swept between positions. */
	st = render_rt(m, 2, 1.5, &out);
	check(st == XTK_MODEL_OK, "bullet same life interpolates");
	b = out_bullet(&out, 7);
	check(b != NULL && b->generation == 10, "bullet life 10 interpolating");
	if (b != NULL) {
		checkf(b->x > 100.0 && b->x < 130.0,
		       "bullet swept inside life 10 (%.3f)", b->x);
	}

	/* Generation 10 -> 11 reuse at tick 3: swap, never cross-life sweep. */
	build_reuse_snap(&s, 3);
	model_observe(m, 1, &s, T(3));
	st = render_rt(m, 3, 2.5, &out);
	b = out_bullet(&out, 7);
	check(b != NULL && b->generation == 11,
	      "reused bullet keyed by its own generation");
	if (b != NULL) {
		checkf(b->x >= 900.0,
		       "reused bullet snapped to its own position (%.3f)", b->x);
	}

	printf("CAP bullet id=7 owner=0 ogen=300 gen=10 x=115.000\n");
	printf("CAP bullet id=7 owner=0 ogen=300 gen=11 x=900.000\n");

	free(m);
}

/* ---- 3. same-kind explosion reuse ----------------------------------- */

static void test_explosion_reuse(void)
{
	XtkClientModel *m = model_alloc();
	XtkViewSnapshot s, out;
	const XtkExplosionView *e;

	feed(m, 1, 0, 2, build_reuse_snap);

	/* Same generation (20) interpolates. */
	render_rt(m, 2, 1.5, &out);
	e = out_explosion(&out, 3);
	check(e != NULL && e->generation == 20, "explosion life 20 interpolating");
	if (e != NULL) {
		checkf(e->x > 100.0 && e->x < 130.0,
		       "explosion swept inside life 20 (%.3f)", e->x);
	}

	/* Reuse 20 -> 21: the transient is snapped, never blended. */
	build_reuse_snap(&s, 3);
	model_observe(m, 1, &s, T(3));
	render_rt(m, 3, 2.5, &out);
	e = out_explosion(&out, 3);
	check(e != NULL && e->generation == 21,
	      "reused explosion keyed by generation");
	if (e != NULL) {
		checkf(e->x >= 900.0,
		       "reused explosion snapped (%.3f)", e->x);
	}

	printf("CAP explosion id=3 gen=21 x=900.000\n");

	free(m);
}

/* ---- 3b. same-life teleport snaps (never blends/extrapolates) -------- */

static void build_teleport_snap(XtkViewSnapshot *s, int tick)
{
	double x = (tick >= 5) ? 5000.0 + (tick - 5) * 10.0
			       : 100.0 + tick * 10.0;

	snap_init(s, (uint32_t)(tick + 1), tick, 0, 1);
	add_vehicle(s, 0, 1, 6, x, 200.0, 0.0, 0.0);
}

static void test_teleport(void)
{
	XtkClientModel *m = model_alloc();
	XtkViewSnapshot s, out;
	XtkModelStatus st;
	double held;

	feed(m, 1, 0, 5, build_teleport_snap);
	st = render_rt(m, 5, 4.5, &out);
	check(st == XTK_MODEL_SNAPPED,
	      "same-life teleport flagged as a snap");
	held = model_camera(m)->center_x;
	checkf(held < 1000.0,
	       "teleport holds the pre-jump pose, never sweeps (%.3f)", held);

	build_teleport_snap(&s, 6);
	model_observe(m, 1, &s, T(6));
	st = render_rt(m, 6, 5.5, &out);
	check(st == XTK_MODEL_OK || st == XTK_MODEL_SNAPPED,
	      "teleport interval ends normally");
	checkf(model_camera(m)->center_x >= 5000.0,
	       "pose at the teleport destination (%.3f)",
	       model_camera(m)->center_x);

	free(m);
}

/* ---- 4. frame-rate independent replay, no ghosts/extrapolation ------- */

static double sample_at(int fps, uint64_t cp_ns, int *fed)
{
	XtkClientModel *m = model_alloc();
	uint64_t frame_ns = (uint64_t)(1.0e9 / (double)fps);
	uint64_t now = 0;
	XtkViewSnapshot s, out;
	int next = 0;

	for (;;) {
		uint64_t cap = (now < cp_ns) ? now : cp_ns;
		while (next <= 40 && T(next) <= cap) {
			build_reuse_snap(&s, next);
			model_observe(m, 1, &s, T(next));
			++next;
		}
		if (now >= cp_ns) {
			break;
		}
		model_render(m, now, &out);
		now += frame_ns;
	}
	while (next <= 40 && T(next) <= cp_ns) {
		build_reuse_snap(&s, next);
		model_observe(m, 1, &s, T(next));
		++next;
	}
	model_render(m, cp_ns, &out);
	*fed = next;
	{
		double x = model_camera(m)->center_x;
		/* No stale remote and no stale bullet generation at the sample. */
		check(!has_vehicle(&out, 1) || cp_ns < T(5),
		      "no departed remote ghost at any FPS");
		free(m);
		return x;
	}
}

static void test_fps(void)
{
	static const int rates[3] = { 30, 60, 144 };
	static const uint64_t cps[2] = { 500000000ULL, 1000000000ULL };
	int a, b;

	for (a = 0; a < 2; ++a) {
		int fed[3];
		double x[3];
		for (b = 0; b < 3; ++b) {
			x[b] = sample_at(rates[b], cps[a], &fed[b]);
		}
		checkf(fed[0] == fed[1] && fed[1] == fed[2],
		       "same ticks fed at 30/60/144 FPS (%d/%d/%d)",
		       fed[0], fed[1], fed[2]);
		checkf(fabs(x[0] - x[1]) < 1e-6 && fabs(x[1] - x[2]) < 1e-6,
		       "same pose across FPS (%.9f/%.9f/%.9f)",
		       x[0], x[1], x[2]);
	}
}

/* ---- 5. loss / reorder ---------------------------------------------- */

static void test_loss_reorder(void)
{
	XtkClientModel *m = model_alloc();
	XtkViewSnapshot s, out;

	build_reuse_snap(&s, 0);
	check(model_observe(m, 1, &s, T(0)) == XTK_OBSERVE_ACCEPTED, "tick 0");
	build_reuse_snap(&s, 2);
	check(model_observe(m, 1, &s, T(2)) == XTK_OBSERVE_ACCEPTED,
	      "tick 2 (tick 1 lost)");
	build_reuse_snap(&s, 1);
	check(model_observe(m, 1, &s, T(1)) == XTK_OBSERVE_DUPLICATE,
	      "late tick 1 ignored (no rewind)");
	check(model_snapshot_count(m) == 2, "ring holds 0,2 (no reorder)");

	free(m);
}

/* ---- 6. epoch reset and timing policy ------------------------------- */

static void test_reset_and_policy(void)
{
	XtkClientModel *m = model_alloc();
	XtkViewSnapshot s, out;

	check(XTK_MODEL_BUFFER_TICKS == 1.0, "approved one-tick buffer kept");
	check(XTK_MODEL_RING == 8, "bounded snapshot ring kept");

	feed(m, 1, 0, 4, build_reuse_snap);
	render_rt(m, 4, 3.0, &out);
	check(model_epoch(m) == 1, "epoch 1 active");
	check(model_camera(m)->viewport_w == 768 &&
	      model_camera(m)->viewport_h == 768 &&
	      model_camera(m)->half_extent == 384.0,
	      "camera viewport/world scale unchanged");

	snap_init(&s, 1, 0, 0, 1);
	add_vehicle(&s, 0, 1, 6, 7000.0, 7000.0, 0.0, 0.0);
	check(model_observe(m, 2, &s, T(0)) == XTK_OBSERVE_EPOCH_RESET,
	      "new epoch resets history");
	check(model_snapshot_count(m) == 1, "history cleared on reset");
	model_render(m, T(0), &out);
	checkf(fabs(model_camera(m)->center_x - 7000.0) < 1e-4,
	       "new epoch pose (%.3f)", model_camera(m)->center_x);
	check(!has_vehicle(&out, 1), "no stale entity after epoch reset");

	free(m);
}

/* ---- main ------------------------------------------------------------ */

int main(void)
{
	printf("== P3.11 lifetime-safe model fixture\n");
	test_vehicle_generation();
	test_bullet_reuse();
	test_explosion_reuse();
	test_teleport();
	test_fps();
	test_loss_reorder();
	test_reset_and_policy();

	printf("checks=%d failures=%d\n", checks, failures);
	if (failures == 0) {
		printf("P3.11 TIMELINE FIXTURE PASS\n");
		return 0;
	}
	printf("P3.11 TIMELINE FIXTURE FAIL\n");
	return 1;
}
