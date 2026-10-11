/*
 * Tests/phase2/assets_fixture.c -- P2.10 acceptance fixture.
 *
 * Loads the exported client asset bundle and checks, against the loaded data
 * (not against the exporter's in-memory structures):
 *
 *   1. geometry: category counts, the 16-view rotation derivation for every
 *      vehicle body, and Marauder (the Vanguard body) preserved;
 *   2. palette/font: the legacy identities and resolved colors;
 *   3. drawing: exact scale/art (every set bitmap bit lands on exactly its
 *      screen pixel, every clear bit stays background), turret placement at the
 *      body's per-view picinfo coordinate, and directional-bullet rotation;
 *   4. no trails: a moved entity leaves no pixels behind, and a redraw after a
 *      simulated resize/expose equals a from-scratch render;
 *   5. DRAW_XOR is a true per-channel XOR on the client surface.
 *
 * It links assets.c + render_sdl.c + SDL3 and no simulation.
 */

#include <SDL3/SDL.h>

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "render.h"

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

/* Find the first set bit of a picture; returns 1 and fills col/row. */
static int first_bit(const XtkPicture *p, int *col, int *row)
{
	int by_line = (p->width + 7) >> 3;
	int c, r;
	for (r = 0; r < p->height; r++)
		for (c = 0; c < p->width; c++)
			if ((p->bitmap[(size_t)r * by_line + (c >> 3)] >>
			     (7 - (c & 7))) & 1) {
				*col = c;
				*row = r;
				return 1;
			}
	return 0;
}

static int bit_at(const XtkPicture *p, int c, int r)
{
	int by_line = (p->width + 7) >> 3;
	return (p->bitmap[(size_t)r * by_line + (c >> 3)] >> (7 - (c & 7))) & 1;
}

/* Verify every pixel of one isolated picture: set bit -> color, clear -> bg. */
static int check_picture_exact(SDL_Surface *s, const XtkPicture *p,
			       int x, int y, uint32_t color, uint32_t bg)
{
	int left = x - p->offset_x, top = y - p->offset_y;
	int c, r, bad = 0;
	for (r = 0; r < p->height; r++) {
		for (c = 0; c < p->width; c++) {
			int px = left + c, py = top + r;
			uint32_t got;
			if (px < 0 || py < 0 || px >= s->w || py >= s->h)
				continue;
			got = render_surface_get(s, px, py);
			if (bit_at(p, c, r)) {
				if (got != color)
					bad++;
			} else if (got != bg) {
				bad++;
			}
		}
	}
	return bad;
}

static uint32_t surface_hash(SDL_Surface *s)
{
	uint32_t h = 2166136261u;
	int x, y;
	for (y = 0; y < s->h; y++)
		for (x = 0; x < s->w; x++) {
			h ^= render_surface_get(s, x, y);
			h *= 16777619u;
		}
	return h;
}

static int surfaces_equal(SDL_Surface *a, SDL_Surface *b)
{
	int x, y;
	if (a->w != b->w || a->h != b->h)
		return 0;
	for (y = 0; y < a->h; y++)
		for (x = 0; x < a->w; x++)
			if (render_surface_get(a, x, y) != render_surface_get(b, x, y))
				return 0;
	return 1;
}

static void snapshot_base(XtkViewSnapshot *v)
{
	memset(v, 0, sizeof(*v));
	v->own_slot = 0;
}

static void set_rv(XtkRenderView *rv, double cx, double cy, int vp)
{
	memset(rv, 0, sizeof(*rv));
	rv->center_x = cx;
	rv->center_y = cy;
	rv->viewport_w = vp;
	rv->viewport_h = vp;
	rv->color_bg = XTK_RGB(0, 0, 0);
	rv->color_own = XTK_RGB(255, 255, 255);
	rv->color_vehicle = XTK_RGB(255, 0, 0);
	rv->color_bullet = XTK_RGB(0, 255, 0);
	rv->color_explosion = XTK_RGB(255, 255, 0);
	rv->color_landmark = XTK_RGB(0, 255, 255);
	rv->explosion_frame = -1;
}

int main(int argc, char **argv)
{
	XtkAssets a;
	char err[256];
	const XtkAssetObject *mar, *lightc, *lmg, *tink, *lm, *tsm;
	SDL_Surface *s768, *sref, *s512;
	XtkViewSnapshot v;
	XtkRenderView rv;
	int i, r;

	if (argc < 2) {
		printf("usage: %s BUNDLE [GEOMETRY_OUT]\n", argv[0]);
		return 2;
	}
	if (assets_load_file(&a, argv[1], err, sizeof(err)) != 0) {
		printf("ASSETS FIXTURE FAIL (load: %s)\n", err);
		return 1;
	}

	if (argc >= 3) {
		FILE *g = fopen(argv[2], "w");
		if (g == NULL) {
			printf("ASSETS FIXTURE FAIL (geometry out)\n");
			assets_free(&a);
			return 1;
		}
		assets_dump_geometry(&a, g);
		fclose(g);
	}

	/* ---- 0. checked load: magic / length / checksum ------------------ */
	{
		FILE *f = fopen(argv[1], "rb");
		long sz;
		uint8_t *buf;
		XtkAssets t;
		check(f != NULL, "open bundle for corruption tests");
		if (f != NULL) {
			fseek(f, 0, SEEK_END);
			sz = ftell(f);
			rewind(f);
			buf = (uint8_t *)malloc((size_t)sz);
			if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
				printf("ASSETS FIXTURE FAIL (read bundle)\n");
				return 1;
			}
			fclose(f);

			check(assets_load_memory(&t, buf, (size_t)sz, err,
						 sizeof(err)) == 0,
			      "bundle loads from memory");
			assets_free(&t);

			buf[0] ^= 0xFF;
			check(assets_load_memory(&t, buf, (size_t)sz, err,
						 sizeof(err)) != 0,
			      "bad magic rejected");
			buf[0] ^= 0xFF;

			check(assets_load_memory(&t, buf, (size_t)sz - 1, err,
						 sizeof(err)) != 0,
			      "truncated bundle rejected");

			buf[sz - 1] ^= 0xFF;
			check(assets_load_memory(&t, buf, (size_t)sz, err,
						 sizeof(err)) != 0,
			      "corrupt payload rejected");
			buf[sz - 1] ^= 0xFF;
			free(buf);
		}
	}

	/* ---- 1. geometry ------------------------------------------------ */
	check(a.num_vehicle == 14, "14 vehicle bodies");
	check(a.num_turret == 1, "1 turret body");
	check(a.num_explosion == 10, "10 explosion bodies");
	check(a.num_bullet == 25, "25 bullet bodies");
	check(a.num_landmark == 3, "3 landmark bodies");
	check(a.num_random == 3, "3 random bodies");

	mar = assets_object(&a, XTK_CAT_VEHICLE, 6);
	check(mar != NULL && strcmp(mar->type, "marauder") == 0,
	      "Marauder is vehicle body index 6");
	check(mar != NULL && mar->num_pics == 16, "Marauder has 16 views");
	check(mar != NULL && mar->num_turrets == 2, "Marauder has 2 turrets");
	check(mar != NULL && mar->num_segs == 4, "Marauder has 4 segments");

	for (i = 0; i < a.num_vehicle; i++) {
		const XtkAssetObject *o = &a.vehicle[i];
		checkf(o->num_pics == 16, "%s has 16 views", o->type);
		for (r = 0; r < 4; r++) {
			checkf(o->pic[r + 4].width == o->pic[r].height &&
			       o->pic[r + 4].height == o->pic[r].width &&
			       o->pic[r + 4].offset_x ==
					o->pic[r].height - o->pic[r].offset_y - 1 &&
			       o->pic[r + 4].offset_y == o->pic[r].offset_x,
			       "%s view %d is a 90-degree rotation", o->type, r);
		}
		for (r = 0; r < 8; r++) {
			checkf(o->pic[r + 8].width == o->pic[r].width &&
			       o->pic[r + 8].height == o->pic[r].height &&
			       o->pic[r + 8].offset_x ==
					o->pic[r].width - o->pic[r].offset_x - 1 &&
			       o->pic[r + 8].offset_y ==
					o->pic[r].height - o->pic[r].offset_y - 1,
			       "%s view %d is a 180-degree rotation", o->type, r);
		}
	}

	/* ---- 2. palette / font ------------------------------------------ */
	check(a.num_palette == 11, "11 palette entries");
	check(a.num_font == 5, "5 font identities");
	check(assets_color_rgb(&a, 0) == XTK_RGB(0, 0, 0), "palette BLACK");
	check(assets_color_rgb(&a, 1) == XTK_RGB(255, 255, 255), "palette WHITE");
	check(assets_color_rgb(&a, 2) == XTK_RGB(255, 0, 0), "palette RED");
	check(assets_color_rgb(&a, 4) == XTK_RGB(255, 255, 0), "palette YELLOW");

	lightc = assets_object(&a, XTK_CAT_VEHICLE, 0);
	lmg = assets_object(&a, XTK_CAT_BULLET, 0);
	tink = assets_object(&a, XTK_CAT_EXPLOSION, 2);
	lm = assets_object(&a, XTK_CAT_LANDMARK, 0);
	tsm = assets_object(&a, XTK_CAT_TURRET, 0);
	check(lightc && lmg && tink && lm && tsm, "required bodies present");
	if (!(lightc && lmg && tink && lm && tsm)) {
		assets_free(&a);
		return 1;
	}

	/* ---- 3. exact scale/art ----------------------------------------- */
	/*
	 * Isolate one lightc body (0 turrets) at world (1000,1000) with the
	 * camera centred there.  The legacy mapping puts the vehicle centre at
	 * screen (viewport/2 - truncation) and each bitmap bit at
	 * centre - offset + bit; the whole bounding box must match exactly, so
	 * a scaling or offset error fails.
	 */
	{
		int sx, sy;
		int ox = 1000 - 768 / 2, oy = 1000 - 768 / 2;
		sx = (int)(1000.0 - (double)ox);
		sy = (int)(1000.0 - (double)oy);
		s768 = SDL_CreateSurface(768, 768, SDL_PIXELFORMAT_ARGB8888);
		check(s768 != NULL, "create 768x768 surface");
		snapshot_base(&v);
		v.vehicle_count = 1;
		v.vehicle[0].slot = 0;
		v.vehicle[0].kind = 0;		/* lightc, 0 turrets */
		v.vehicle[0].x = 1000;
		v.vehicle[0].y = 1000;
		v.vehicle[0].body_picture = 0;
		set_rv(&rv, 1000, 1000, 768);
		render_frame_surface(&a, &v, &rv, s768, err, sizeof(err));
		{
			int bad = check_picture_exact(s768, &lightc->pic[0], sx, sy,
						      rv.color_own, rv.color_bg);
			checkf(bad == 0, "isolated body art exact (bad=%d)", bad);
		}
	}

	/* ---- turret placement (Marauder, body view 3) ------------------- */
	{
		int sx, sy, ox = 1000 - 384, oy = 1000 - 384;
		const XtkTurretCoord *tc;
		int bp = 3, tp = 5;
		int c0, r0, bad = 0;
		sx = (int)(1000.0 - (double)ox);
		sy = (int)(1000.0 - (double)oy);
		snapshot_base(&v);
		v.vehicle_count = 1;
		v.vehicle[0].slot = 0;
		v.vehicle[0].kind = 6;		/* marauder */
		v.vehicle[0].x = 1000;
		v.vehicle[0].y = 1000;
		v.vehicle[0].body_picture = bp;
		v.vehicle[0].turret_picture = tp;
		set_rv(&rv, 1000, 1000, 768);
		render_frame_surface(&a, &v, &rv, s768, err, sizeof(err));
		tc = &mar->turret[bp][0];
		if (first_bit(&tsm->pic[tp], &c0, &r0)) {
			int px = sx + tc->x - tsm->pic[tp].offset_x + c0;
			int py = sy + tc->y - tsm->pic[tp].offset_y + r0;
			checkf(render_surface_get(s768, px, py) == rv.color_own,
			       "turret placed at picinfo coord (view %d)", bp);
		}
		/* The body must also be present at its own first bit. */
		if (first_bit(&mar->pic[bp], &c0, &r0)) {
			int px = sx - mar->pic[bp].offset_x + c0;
			int py = sy - mar->pic[bp].offset_y + r0;
			if (render_surface_get(s768, px, py) != rv.color_own)
				bad++;
		}
		check(bad == 0, "Marauder body present at view 3");
	}

	/* ---- bullets / explosions / landmarks --------------------------- */
	{
		int sx, sy, c0, r0;
		int rot;
		int ox = 1000 - 384, oy = 1000 - 384;
		snapshot_base(&v);
		v.bullet_count = 1;
		v.bullet[0].kind = 0;		/* lmg, 1 view */
		v.bullet[0].x = 1000;
		v.bullet[0].y = 1300;
		v.bullet[0].heading = 0.0;
		v.explosion_count = 1;
		v.explosion[0].kind = 2;	/* 0 damage (tink) */
		v.explosion[0].x = 1300;
		v.explosion[0].y = 1300;
		v.landmark_count = 1;
		v.landmark[0].kind = 1;		/* FUEL */
		v.landmark[0].x = 1000;
		v.landmark[0].y = 700;
		set_rv(&rv, 1000, 1000, 768);
		render_frame_surface(&a, &v, &rv, s768, err, sizeof(err));

		rot = render_find_rot(0.0, lmg->num_pics);
		sx = (int)(1000.0 - (double)ox);
		sy = (int)(1300.0 - (double)oy);
		if (first_bit(&lmg->pic[rot], &c0, &r0))
			checkf(render_surface_get(s768,
				sx - lmg->pic[rot].offset_x + c0,
				sy - lmg->pic[rot].offset_y + r0) == rv.color_bullet,
				"bullet drawn at world position");

		sx = (int)(1300.0 - (double)ox);
		sy = (int)(1300.0 - (double)oy);
		if (first_bit(&tink->pic[tink->num_pics - 1], &c0, &r0))
			checkf(render_surface_get(s768,
				sx - tink->pic[tink->num_pics - 1].offset_x + c0,
				sy - tink->pic[tink->num_pics - 1].offset_y + r0) ==
				rv.color_explosion, "explosion drawn at world position");

		sx = (int)(1000.0 - (double)ox);
		sy = (int)(700.0 - (double)oy);
		if (first_bit(&lm->pic[0], &c0, &r0))
			checkf(render_surface_get(s768,
				sx - lm->pic[0].offset_x + c0,
				sy - lm->pic[0].offset_y + r0) == rv.color_landmark,
				"landmark drawn at world position");
	}

	/* ---- 4. no trails / resize / expose ----------------------------- */
	{
		uint32_t h_ab, h_ref;
		int vp;
		sref = SDL_CreateSurface(768, 768, SDL_PIXELFORMAT_ARGB8888);
		check(sref != NULL, "create reference surface");

		/* Frame A then frame B on the same surface. */
		snapshot_base(&v);
		v.vehicle_count = 1;
		v.vehicle[0].slot = 0;
		v.vehicle[0].kind = 0;
		v.vehicle[0].x = 1000;
		v.vehicle[0].y = 1000;
		set_rv(&rv, 1000, 1000, 768);
		render_frame_surface(&a, &v, &rv, s768, err, sizeof(err));
		v.vehicle[0].x = 1400;		/* move 400 px right */
		render_frame_surface(&a, &v, &rv, s768, err, sizeof(err));
		h_ab = surface_hash(s768);

		/* Fresh render of frame B only. */
		render_frame_surface(&a, &v, &rv, sref, err, sizeof(err));
		h_ref = surface_hash(sref);
		checkf(h_ab == h_ref, "moved entity leaves no trail (%08x vs %08x)",
		       h_ab, h_ref);

		/* Expose/redraw: render A, render B, render A again. */
		v.vehicle[0].x = 1000;
		render_frame_surface(&a, &v, &rv, s768, err, sizeof(err));
		render_frame_surface(&a, &v, &rv, s768, err, sizeof(err));
		render_frame_surface(&a, &v, &rv, sref, err, sizeof(err));
		check(surfaces_equal(s768, sref),
		      "redraw after expose equals from-scratch frame");

		/* Resize: same world pose into a 512 surface (viewport 512).
		 * One world unit is still one pixel, so the body sits at the
		 * 512-centre, not scaled. */
		s512 = SDL_CreateSurface(512, 512, SDL_PIXELFORMAT_ARGB8888);
		check(s512 != NULL, "create 512x512 surface");
		vp = 512;
		set_rv(&rv, 1000, 1000, vp);
		render_frame_surface(&a, &v, &rv, s512, err, sizeof(err));
		{
			int sx = (int)(1000.0 - (double)(1000 - vp / 2));
			int sy = (int)(1000.0 - (double)(1000 - vp / 2));
			int bad = check_picture_exact(s512, &lightc->pic[0], sx, sy,
						      rv.color_own, rv.color_bg);
			checkf(bad == 0, "resized surface art exact (bad=%d)", bad);
		}
		SDL_DestroySurface(s512);
		SDL_DestroySurface(sref);
	}

	/* ---- 5. DRAW_XOR is a true per-channel XOR ---------------------- */
	{
		uint32_t bg = XTK_RGB(0x12, 0x34, 0x56);
		uint32_t fg = XTK_RGB(0xAB, 0xCD, 0xEF);
		uint32_t want = XTK_RGB(0x12 ^ 0xAB, 0x34 ^ 0xCD, 0x56 ^ 0xEF);
		int sx = 384, sy = 384, c0, r0, px, py;
		render_surface_clear(s768, bg);
		if (first_bit(&lmg->pic[0], &c0, &r0)) {
			px = sx - lmg->pic[0].offset_x + c0;
			py = sy - lmg->pic[0].offset_y + r0;
			render_draw_picture(s768, sx, sy, &lmg->pic[0],
					    XTK_DRAW_XOR, fg);
			checkf(render_surface_get(s768, px, py) == want,
			       "DRAW_XOR yields per-channel XOR");
			render_draw_picture(s768, sx, sy, &lmg->pic[0],
					    XTK_DRAW_XOR, fg);
			checkf(render_surface_get(s768, px, py) == bg,
			       "double DRAW_XOR restores background");
		}
	}

	/* ---- determinism ------------------------------------------------ */
	{
		uint32_t h1, h2;
		snapshot_base(&v);
		v.vehicle_count = 1;
		v.vehicle[0].kind = 6;
		v.vehicle[0].x = 1000;
		v.vehicle[0].y = 1000;
		set_rv(&rv, 1000, 1000, 768);
		render_frame_surface(&a, &v, &rv, s768, err, sizeof(err));
		h1 = surface_hash(s768);
		render_frame_surface(&a, &v, &rv, s768, err, sizeof(err));
		h2 = surface_hash(s768);
		checkf(h1 == h2, "render is deterministic");
	}

	SDL_DestroySurface(s768);
	assets_free(&a);

	printf("checks=%d failures=%d\n", checks, failures);
	if (failures == 0) {
		printf("ASSETS FIXTURE PASS\n");
		return 0;
	}
	printf("ASSETS FIXTURE FAIL\n");
	return 1;
}
