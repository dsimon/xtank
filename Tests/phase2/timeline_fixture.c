/*
 * Tests/phase2/timeline_fixture.c -- P2.11 acceptance fixture.
 *
 * Exercises the client model (Client/model.c), monotonic timeline
 * (Client/timeline.c), camera (Client/camera.c) and the presentation glue
 * (Client/world_render.c) against deterministic recorded views:
 *
 *   1. bounded ring and one-tick interpolation between matching snapshots;
 *   2. frame-rate-independent replay at 30/60/144 FPS (same fed ticks, same
 *      interpolated pose at the same wall time);
 *   3. loss/reordering and no extrapolation;
 *   4. underrun freezes the last authorized pose;
 *   5. hidden entities leave no ghost (model and rendered pixels);
 *   6. teleport and death/respawn snap, never blend across lifetimes;
 *   7. epoch reset clears history and snaps;
 *   8. the camera follows the displayed own pose with a fixed logical viewport
 *      and 1:1 world scale.
 *
 * It links model/timeline/camera/world_render + assets/render_sdl + SDL3 and no
 * simulation.  A bundle path may be given to enable the pixel ghost check.
 */

#define SDL_MAIN_HANDLED 1

#include <SDL3/SDL.h>

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "render.h"
#include "model.h"

/* 15 Hz: one tick is 1/15 s; use an integer nanosecond approximation. */
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
		      uint8_t own_slot, uint8_t own_gen)
{
	memset(s, 0, sizeof(*s));
	s->snapshot_sequence = seq;
	s->server_tick = (uint32_t)tick;
	s->own_slot = own_slot;
	s->own_generation = own_gen;
	s->self.slot = own_slot;
	s->self.generation = own_gen;
}

static void add_vehicle(XtkViewSnapshot *s, uint8_t slot, uint8_t gen,
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
	v->body_picture = 0;
	v->turret_picture = 0;
}

static void add_bullet(XtkViewSnapshot *s, uint16_t id, uint8_t oslot,
		       uint8_t ogen, uint8_t kind, double x, double y,
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
	b->kind = kind;
	b->x = x;
	b->y = y;
	b->heading = heading;
}

/*
 * Main recorded stream: own slot 0, remote slot 1, a remote slot 2 that is
 * present on ticks 0..4 then hidden, a bullet, a teleport of the own vehicle at
 * tick 20 and a generation-advancing respawn at tick 25.
 */
static void build_stream_snap(XtkViewSnapshot *s, int tick)
{
	uint8_t own_gen = (tick >= 25) ? 2 : 1;
	double own_x;
	double own_y = 200.0 + tick * 2.0;

	if (tick >= 25) {
		own_x = 50.0 + (tick - 25) * 10.0;
	} else if (tick >= 20) {
		own_x = 5000.0 + (tick - 20) * 10.0;
	} else {
		own_x = 100.0 + tick * 10.0;
	}

	snap_init(s, (uint32_t)(tick + 1), tick, 0, own_gen);
	add_vehicle(s, 0, own_gen, 6, own_x, own_y, tick * 0.1, tick * 0.15);
	add_vehicle(s, 1, 1, 6, 900.0 - tick * 5.0, 700.0, 0.0, 0.0);
	if (tick <= 4) {
		add_vehicle(s, 2, 5, 6, 300.0, 300.0, 0.0, 0.0);
	}
	add_bullet(s, 0, 0, own_gen, 3, 100.0 + tick * 20.0, 100.0,
		   tick * 0.2);
}

/*
 * Death/respawn stream: own slot 0 lives on ticks 0..4 (generation 1), is dead
 * (absent) on ticks 5..7, and respawns with generation 2 on tick 8+.
 */
static void build_death_snap(XtkViewSnapshot *s, int tick)
{
	snap_init(s, (uint32_t)(tick + 1), tick, 0, 1);
	if (tick <= 4) {
		add_vehicle(s, 0, 1, 6, 100.0 + tick * 10.0, 200.0, 0.0, 0.0);
	} else if (tick >= 8) {
		s->own_generation = 2;
		s->self.generation = 2;
		add_vehicle(s, 0, 2, 6, 800.0 + (tick - 8) * 10.0, 300.0, 0.0,
			    0.0);
	}
	/* ticks 5..7: dead; self carries the last life (generation 1). */
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

/* Render at render-tick `rt` when the newest fed tick is `L`. */
static XtkModelStatus render_rt(XtkClientModel *m, int L, double rt,
				XtkViewSnapshot *out)
{
	/* est = rt + buffer(1) = L + (now-anchor)/tick. */
	uint64_t now = T(L) +
		(uint64_t)((rt - (double)L + XTK_MODEL_BUFFER_TICKS) *
			   (double)NS_PER_TICK);
	return model_render(m, now, out);
}

/* ---- 1. bounded ring, interpolation, no extrapolation --------------- */

static void test_basic(void)
{
	XtkClientModel *m = model_alloc();
	XtkViewSnapshot out;
	static const double k[4] = { 0.0, 0.25, 0.5, 0.75 };
	static const double expect[4] = { 170.0, 172.5, 175.0, 177.5 };
	double prev = -1e18;
	int i;
	XtkModelStatus st;

	feed(m, 1, 0, 8, build_stream_snap);
	check(model_snapshot_count(m) == 8, "ring is bounded to 8 entries");
	check(model_epoch(m) == 1, "epoch recorded");
	check(model_camera(m)->viewport_w == 768 &&
	      model_camera(m)->viewport_h == 768 &&
	      model_camera(m)->half_extent == 384.0,
	      "camera viewport/world scale fixed");

	for (i = 0; i < 4; ++i) {
		st = render_rt(m, 8, 7.0 + k[i], &out);
		checkf(st == XTK_MODEL_OK, "interpolating @ k=%.2f st=%d", k[i],
		       (int)st);
		checkf(fabs(model_camera(m)->center_x - expect[i]) < 1e-4,
		       "own x @ k=%.2f = %.6f (expected %.6f)", k[i],
		       model_camera(m)->center_x, expect[i]);
		check(model_camera(m)->center_x >= prev - 1e-9,
		      "own x monotonic");
		prev = model_camera(m)->center_x;
	}

	/* At the newest tick: frozen, never beyond. */
	st = render_rt(m, 8, 8.0, &out);
	check(st == XTK_MODEL_UNDERRUN, "underrun at the newest tick");
	checkf(fabs(model_camera(m)->center_x - 180.0) < 1e-4,
	       "clamped at newest (%.3f)", model_camera(m)->center_x);

	/* Far beyond the newest tick: still clamped (no extrapolation). */
	st = model_render(m, T(8) + UINT64_C(10000000000), &out);
	check(st == XTK_MODEL_UNDERRUN, "underrun far in the future");
	checkf(fabs(model_camera(m)->center_x - 180.0) < 1e-4,
	       "no extrapolation beyond the newest view");

	free(m);
}

/* ---- 2. frame-rate independence ------------------------------------- */

static double sample_at(int fps, uint64_t cp_ns, int *fed)
{
	XtkClientModel *m = model_alloc();
	uint64_t frame_ns = (uint64_t)(1.0e9 / (double)fps);
	uint64_t now = 0;
	XtkViewSnapshot s, out;
	int next = 0;

	for (;;) {
		uint64_t cap = (now < cp_ns) ? now : cp_ns;
		while (next <= 60 && T(next) <= cap) {
			build_stream_snap(&s, next);
			model_observe(m, 1, &s, T(next));
			++next;
		}
		if (now >= cp_ns) {
			break;
		}
		model_render(m, now, &out);
		now += frame_ns;
	}
	while (next <= 60 && T(next) <= cp_ns) {
		build_stream_snap(&s, next);
		model_observe(m, 1, &s, T(next));
		++next;
	}
	model_render(m, cp_ns, &out);
	*fed = next;
	{
		double x = model_camera(m)->center_x;
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
		       "same ticks fed at 30/60/144 FPS (%d/%d/%d) @ %.1fs",
		       fed[0], fed[1], fed[2], (double)cps[a] / 1e9);
		checkf(fabs(x[0] - x[1]) < 1e-6 && fabs(x[1] - x[2]) < 1e-6,
		       "same interpolated pose across FPS (%.9f/%.9f/%.9f)",
		       x[0], x[1], x[2]);
	}
}

/* ---- 3. loss / reordering ------------------------------------------- */

static void test_loss_reorder(void)
{
	XtkClientModel *m = model_alloc();
	XtkViewSnapshot s, out;
	XtkModelStatus st;

	build_stream_snap(&s, 0);
	check(model_observe(m, 1, &s, T(0)) == XTK_OBSERVE_ACCEPTED, "tick 0");
	build_stream_snap(&s, 1);
	check(model_observe(m, 1, &s, T(1)) == XTK_OBSERVE_ACCEPTED, "tick 1");
	build_stream_snap(&s, 3);
	check(model_observe(m, 1, &s, T(3)) == XTK_OBSERVE_ACCEPTED,
	      "tick 3 (tick 2 lost)");
	build_stream_snap(&s, 2);
	check(model_observe(m, 1, &s, T(2)) == XTK_OBSERVE_DUPLICATE,
	      "late tick 2 ignored (no rewind)");
	check(model_snapshot_count(m) == 3, "ring holds 0,1,3");

	st = render_rt(m, 3, 2.0, &out);
	check(st == XTK_MODEL_OK, "interpolate across the gap");
	checkf(fabs(model_camera(m)->center_x - 120.0) < 1e-4,
	       "gap interpolation = 120 (%.3f)", model_camera(m)->center_x);

	build_stream_snap(&s, 4);
	check(model_observe(m, 1, &s, T(4)) == XTK_OBSERVE_ACCEPTED, "tick 4");
	st = render_rt(m, 4, 3.5, &out);
	check(st == XTK_MODEL_OK, "interpolate after recovery");
	checkf(fabs(model_camera(m)->center_x - 135.0) < 1e-4,
	       "post-recovery x = 135 (%.3f)", model_camera(m)->center_x);

	free(m);
}

/* ---- 4. underrun freezes --------------------------------------------- */

static void test_underrun(void)
{
	XtkClientModel *m = model_alloc();
	XtkViewSnapshot out;
	XtkModelStatus st;
	double frozen;

	feed(m, 1, 0, 5, build_stream_snap);
	st = render_rt(m, 5, 5.0, &out);
	check(st == XTK_MODEL_UNDERRUN, "underrun when render reaches newest");
	frozen = model_camera(m)->center_x;
	checkf(fabs(frozen - 150.0) < 1e-4, "frozen at newest (%.3f)", frozen);

	st = model_render(m, T(5) + UINT64_C(2000000000), &out);
	check(st == XTK_MODEL_UNDERRUN, "still underrun after 2 s");
	checkf(fabs(model_camera(m)->center_x - frozen) < 1e-4,
	       "pose frozen on underrun");

	free(m);
}

/* ---- 5. hidden entity leaves no ghost -------------------------------- */

static void test_hidden(XtkAssets *a)
{
	XtkClientModel *m = model_alloc();
	XtkViewSnapshot out, s;
	SDL_Surface *surf = NULL;

	feed(m, 1, 0, 4, build_stream_snap);
	render_rt(m, 4, 3.0, &out);
	check(has_vehicle(&out, 2), "hidden-later entity visible before hiding");

	build_stream_snap(&s, 5);
	model_observe(m, 1, &s, T(5));
	render_rt(m, 5, 4.0, &out);
	check(!has_vehicle(&out, 2), "hidden entity removed (no ghost)");

	if (a != NULL) {
		int red_before = 0, red_after = 0, green = 0, x, y;
		char err[256];
		surf = SDL_CreateSurface(768, 768, SDL_PIXELFORMAT_ARGB8888);
		check(surf != NULL, "create ghost-check surface");

		/* Render the frame while the remote vehicle is visible. */
		{
			XtkClientModel *m1 = model_alloc();
			XtkViewSnapshot o1;
			feed(m1, 1, 0, 4, build_stream_snap);
			render_rt(m1, 4, 3.0, &o1);
			check(world_render_present(a, &o1, model_camera(m1), surf,
						   err, sizeof(err)) == 0,
			      "render visible frame");
			for (y = 0; y < surf->h; y++)
				for (x = 0; x < surf->w; x++) {
					uint32_t p = render_surface_get(surf, x, y);
					if (p == XTK_RGB(255, 0, 0)) red_before++;
					if (p == XTK_RGB(0, 255, 0)) green++;
				}
			free(m1);
		}
		checkf(red_before > 0, "remote vehicle drawn (red=%d)",
		       red_before);
		checkf(green > 0, "own vehicle drawn (green=%d)", green);

		/* Render the frame after the remote vehicle is hidden. */
		check(world_render_present(a, &out, model_camera(m), surf, err,
					   sizeof(err)) == 0,
		      "render hidden frame");
		for (y = 0; y < surf->h; y++)
			for (x = 0; x < surf->w; x++) {
				if (render_surface_get(surf, x, y) ==
				    XTK_RGB(255, 0, 0)) red_after++;
			}
		checkf(red_after == 0, "no stale remote pixels (red=%d)",
		       red_after);
		SDL_DestroySurface(surf);
	}
	free(m);
}

/* ---- 6. teleport snaps ---------------------------------------------- */

static void test_teleport(void)
{
	XtkClientModel *m = model_alloc();
	XtkViewSnapshot out, s;
	XtkModelStatus st;

	feed(m, 1, 0, 20, build_stream_snap);
	st = render_rt(m, 20, 19.0, &out);
	check(st == XTK_MODEL_SNAPPED, "teleport flagged as snap");
	checkf(fabs(model_camera(m)->center_x - 290.0) < 1e-4,
	       "teleport holds pre-jump pose (%.3f)",
	       model_camera(m)->center_x);

	build_stream_snap(&s, 21);
	model_observe(m, 1, &s, T(21));
	st = render_rt(m, 21, 20.0, &out);
	check(st == XTK_MODEL_OK, "teleport interval ends normally");
	checkf(fabs(model_camera(m)->center_x - 5000.0) < 1e-4,
	       "pose at destination (%.3f)", model_camera(m)->center_x);

	free(m);
}

/* ---- 7. death / respawn --------------------------------------------- */

static void test_death(void)
{
	XtkClientModel *m = model_alloc();
	XtkViewSnapshot out, s;
	double frozen;

	feed(m, 1, 0, 4, build_death_snap);
	render_rt(m, 4, 3.0, &out);
	checkf(fabs(model_camera(m)->center_x - 130.0) < 1e-4,
	       "own displayed before death (%.3f)", model_camera(m)->center_x);

	build_death_snap(&s, 5);
	model_observe(m, 1, &s, T(5));
	render_rt(m, 5, 4.0, &out);
	check(!has_vehicle(&out, 0), "dead own vehicle not drawn");
	frozen = model_camera(m)->center_x;
	checkf(fabs(frozen - 130.0) < 1e-4, "death camera frozen (%.3f)", frozen);

	build_death_snap(&s, 6);
	model_observe(m, 1, &s, T(6));
	build_death_snap(&s, 7);
	model_observe(m, 1, &s, T(7));
	render_rt(m, 7, 6.0, &out);
	checkf(fabs(model_camera(m)->center_x - frozen) < 1e-4,
	       "camera stays frozen while dead");

	build_death_snap(&s, 8);
	model_observe(m, 1, &s, T(8));
	render_rt(m, 8, 7.0, &out);
	checkf(fabs(model_camera(m)->center_x - frozen) < 1e-4,
	       "no blend into a new life/position");
	check(out.own_generation == 1, "held generation until the new life");

	build_death_snap(&s, 9);
	model_observe(m, 1, &s, T(9));
	render_rt(m, 9, 8.0, &out);
	check(out.own_generation == 2, "respawn generation advances");
	checkf(fabs(model_camera(m)->center_x - 800.0) < 1e-4,
	       "camera snaps to the respawned pose (%.3f)",
	       model_camera(m)->center_x);

	free(m);
}

/* ---- 8. epoch reset -------------------------------------------------- */

static void test_reset(void)
{
	XtkClientModel *m = model_alloc();
	XtkViewSnapshot out, s;

	feed(m, 1, 0, 4, build_stream_snap);
	render_rt(m, 4, 3.0, &out);
	check(model_epoch(m) == 1, "epoch 1 active");

	/* Epoch 2, tick 0, own at a distinct position. */
	snap_init(&s, 1, 0, 0, 1);
	add_vehicle(&s, 0, 1, 6, 7000.0, 7000.0, 0.0, 0.0);
	check(model_observe(m, 2, &s, T(0)) == XTK_OBSERVE_EPOCH_RESET,
	      "new epoch resets");
	check(model_epoch(m) == 2, "epoch advanced");
	check(model_snapshot_count(m) == 1, "history cleared on reset");

	/* A stale epoch and a duplicate are ignored. */
	build_stream_snap(&s, 3);
	check(model_observe(m, 1, &s, T(3)) == XTK_OBSERVE_STALE_EPOCH,
	      "stale epoch ignored");
	snap_init(&s, 1, 0, 0, 1);
	check(model_observe(m, 2, &s, T(0)) == XTK_OBSERVE_DUPLICATE,
	      "duplicate snapshot ignored");

	model_render(m, T(0), &out);
	checkf(fabs(model_camera(m)->center_x - 7000.0) < 1e-4,
	       "renders the new epoch pose (%.3f)",
	       model_camera(m)->center_x);
	check(!has_vehicle(&out, 1) && !has_vehicle(&out, 2),
	      "no stale entities after reset");

	free(m);
}

/* ---- main ------------------------------------------------------------ */

int main(int argc, char **argv)
{
	XtkAssets a;
	char err[256];
	int have_assets = 0;

	if (argc >= 2) {
		if (assets_load_file(&a, argv[1], err, sizeof(err)) != 0) {
			printf("TIMELINE FIXTURE FAIL (load: %s)\n", err);
			return 1;
		}
		have_assets = 1;
	} else {
		printf("note: no bundle given; pixel ghost check skipped\n");
	}

	test_basic();
	test_fps();
	test_loss_reorder();
	test_underrun();
	test_hidden(have_assets ? &a : NULL);
	test_teleport();
	test_death();
	test_reset();

	if (have_assets) {
		assets_free(&a);
	}

	printf("checks=%d failures=%d\n", checks, failures);
	if (failures == 0) {
		printf("TIMELINE FIXTURE PASS\n");
		return 0;
	}
	printf("TIMELINE FIXTURE FAIL\n");
	return 1;
}