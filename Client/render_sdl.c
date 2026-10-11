/*
 * Client/render_sdl.c -- snapshot-driven full-frame drawing on SDL surfaces
 * (P2.10).
 *
 * The only client translation unit besides platform_sdl.c that includes SDL3.
 * It rasterizes an authorized XtkViewSnapshot through the P2.10 asset bundle
 * onto an ARGB SDL_Surface, reproducing the original 2D appearance:
 *
 *   - one world unit maps to one logical pixel, exactly the legacy 768x768
 *     animation window (NUM_BOXES * BOX_WIDTH);
 *   - a picture is drawn with its hot-spot at the entity's screen point
 *     (top-left = point - offset), matching Src/Display/x11.c x11_draw_picture;
 *   - the body uses the exported body_picture rotation, the turret is placed at
 *     the body's per-view picinfo turret_coord and uses turret_picture;
 *   - directional bullets use the legacy find_rot() rotation index.
 *
 * The whole frame is cleared before drawing, so a redraw after a resize or an
 * expose leaves no stale pixels.  DRAW_XOR is implemented as a true per-channel
 * XOR; the client never installs the legacy GraphicsBackend vtable, so no
 * legacy caller's XOR semantics are redefined.
 *
 * Authority: docs/PHASE2-PLAN.md P2.10; Src/display.c and Src/Include/bullet.h
 * (read-only references).
 */

#define SDL_MAIN_HANDLED 1

#include <SDL3/SDL.h>

#include <math.h>
#include <string.h>

#include "render.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static void argb_split(uint32_t color, Uint8 *r, Uint8 *g, Uint8 *b, Uint8 *a)
{
	if (r != NULL) *r = (Uint8)((color >> 16) & 0xFF);
	if (g != NULL) *g = (Uint8)((color >> 8) & 0xFF);
	if (b != NULL) *b = (Uint8)(color & 0xFF);
	if (a != NULL) *a = (Uint8)((color >> 24) & 0xFF);
}

void render_surface_clear(struct SDL_Surface *surf, uint32_t color)
{
	Uint8 r, g, b, a;
	SDL_Rect rect;

	if (surf == NULL)
		return;
	argb_split(color, &r, &g, &b, &a);
	rect.x = 0;
	rect.y = 0;
	rect.w = surf->w;
	rect.h = surf->h;
	SDL_FillSurfaceRect(surf, &rect, SDL_MapSurfaceRGBA(surf, r, g, b, a));
}

uint32_t render_surface_get(const struct SDL_Surface *surf, int x, int y)
{
	Uint8 r = 0, g = 0, b = 0, a = 0;
	if (surf == NULL)
		return 0;
	if (!SDL_ReadSurfacePixel((SDL_Surface *)surf, x, y, &r, &g, &b, &a))
		return 0;
	return XTK_RGB(r, g, b);
}

static int bitmap_bit(const uint8_t *bitmap, int by_line, int col, int row)
{
	const uint8_t *p = bitmap + (size_t)row * (size_t)by_line + (col >> 3);
	return (*p >> (7 - (col & 7))) & 1;
}

int render_draw_picture(struct SDL_Surface *surf, int x, int y,
			const XtkPicture *pic, int func, uint32_t color)
{
	int by_line, col, row;
	int left, top;
	Uint8 cr, cg, cb, ca;

	if (surf == NULL || pic == NULL || pic->bitmap == NULL ||
	    pic->bitmap_len == 0 || pic->width <= 0 || pic->height <= 0)
		return 0;

	argb_split(color, &cr, &cg, &cb, &ca);
	if (ca == 0)
		ca = 0xFF;

	by_line = (pic->width + 7) >> 3;
	left = x - pic->offset_x;
	top = y - pic->offset_y;

	for (row = 0; row < pic->height; row++) {
		int py = top + row;
		if (py < 0 || py >= surf->h)
			continue;
		for (col = 0; col < pic->width; col++) {
			int px = left + col;
			if (px < 0 || px >= surf->w)
				continue;
			if (!bitmap_bit(pic->bitmap, by_line, col, row))
				continue;
			if (func == XTK_DRAW_XOR) {
				Uint8 r = 0, g = 0, b = 0, a = 0;
				if (!SDL_ReadSurfacePixel(surf, px, py,
							  &r, &g, &b, &a))
					continue;
				SDL_WriteSurfacePixel(surf, px, py,
					(Uint8)(r ^ cr), (Uint8)(g ^ cg),
					(Uint8)(b ^ cb), 0xFF);
			} else {
				SDL_WriteSurfacePixel(surf, px, py, cr, cg, cb, ca);
			}
		}
	}
	return 0;
}

int render_find_rot(double angle, int num_views)
{
	int r;
	if (num_views <= 1)
		return 0;
	r = (int)((angle + M_PI / num_views + M_PI * 2.0) *
		  ((double)num_views / (M_PI * 2.0)));
	r %= num_views;
	if (r < 0)
		r += num_views;
	return r;
}

static void world_to_screen(const XtkRenderView *rv, double wx, double wy,
			    int *sx, int *sy)
{
	int ox = (int)(rv->center_x - rv->viewport_w / 2.0);
	int oy = (int)(rv->center_y - rv->viewport_h / 2.0);
	*sx = (int)(wx - (double)ox);
	*sy = (int)(wy - (double)oy);
}

static void draw_landmarks(const XtkAssets *a, const XtkViewSnapshot *v,
			   const XtkRenderView *rv, struct SDL_Surface *surf)
{
	const XtkAssetObject *lm;
	int i;

	if (a->num_landmark < 1)
		return;
	lm = &a->landmark[0];
	for (i = 0; i < (int)v->landmark_count; i++) {
		const XtkLandmarkView *lv = &v->landmark[i];
		int sx, sy;
		int idx = (int)lv->kind - 1;
		if (idx < 0 || idx >= lm->num_pics)
			continue;
		world_to_screen(rv, lv->x, lv->y, &sx, &sy);
		render_draw_picture(surf, sx, sy, &lm->pic[idx], XTK_DRAW_COPY,
				    rv->color_landmark);
	}
}

static void draw_vehicles(const XtkAssets *a, const XtkViewSnapshot *v,
			  const XtkRenderView *rv, struct SDL_Surface *surf)
{
	int i;

	for (i = 0; i < (int)v->vehicle_count; i++) {
		const XtkVehicleView *vv = &v->vehicle[i];
		const XtkAssetObject *body;
		uint32_t color;
		int bp, sx, sy;

		if (vv->kind >= (uint8_t)a->num_vehicle)
			continue;
		body = &a->vehicle[vv->kind];
		if (body->num_pics <= 0)
			continue;
		bp = (int)vv->body_picture % body->num_pics;
		color = (vv->slot == v->own_slot) ? rv->color_own
						  : rv->color_vehicle;

		world_to_screen(rv, vv->x, vv->y, &sx, &sy);
		render_draw_picture(surf, sx, sy, &body->pic[bp], XTK_DRAW_COPY,
				    color);

		if (body->num_turrets > 0 && a->num_turret > 0) {
			const XtkAssetObject *tobj = &a->turret[0];
			const XtkTurretCoord *tc = &body->turret[bp][0];
			int tp;
			if (tobj->num_pics <= 0)
				continue;
			tp = (int)vv->turret_picture % tobj->num_pics;
			render_draw_picture(surf, sx + tc->x, sy + tc->y,
					    &tobj->pic[tp], XTK_DRAW_COPY,
					    color);
		}
	}
}

static void draw_bullets(const XtkAssets *a, const XtkViewSnapshot *v,
			 const XtkRenderView *rv, struct SDL_Surface *surf)
{
	int i;

	for (i = 0; i < (int)v->bullet_count; i++) {
		const XtkBulletView *bv = &v->bullet[i];
		const XtkAssetObject *bobj;
		int rot, sx, sy;

		if (bv->kind >= (uint8_t)a->num_bullet)
			continue;
		bobj = &a->bullet[bv->kind];
		if (bobj->num_pics <= 0)
			continue;
		rot = render_find_rot(bv->heading, bobj->num_pics);
		world_to_screen(rv, bv->x, bv->y, &sx, &sy);
		render_draw_picture(surf, sx, sy, &bobj->pic[rot],
				    XTK_DRAW_COPY, rv->color_bullet);
	}
}

static void draw_explosions(const XtkAssets *a, const XtkViewSnapshot *v,
			    const XtkRenderView *rv, struct SDL_Surface *surf)
{
	int i;

	for (i = 0; i < (int)v->explosion_count; i++) {
		const XtkExplosionView *ev = &v->explosion[i];
		const XtkAssetObject *eobj;
		int frame, sx, sy;

		if (ev->kind >= (uint8_t)a->num_explosion)
			continue;
		eobj = &a->explosion[ev->kind];
		if (eobj->num_pics <= 0)
			continue;
		frame = rv->explosion_frame;
		if (frame < 0 || frame >= eobj->num_pics)
			frame = eobj->num_pics - 1;
		world_to_screen(rv, ev->x, ev->y, &sx, &sy);
		render_draw_picture(surf, sx, sy, &eobj->pic[frame],
				    XTK_DRAW_COPY, rv->color_explosion);
	}
}

int render_frame_surface(const XtkAssets *a, const XtkViewSnapshot *v,
			 const XtkRenderView *rv, struct SDL_Surface *surf,
			 char *err, size_t errcap)
{
	if (a == NULL || v == NULL || rv == NULL || surf == NULL) {
		if (err != NULL && errcap > 0) {
			strncpy(err, "render_frame_surface: null argument",
				errcap - 1);
			err[errcap - 1] = '\0';
		}
		return -1;
	}

	render_surface_clear(surf, rv->color_bg);
	draw_landmarks(a, v, rv, surf);
	draw_vehicles(a, v, rv, surf);
	draw_bullets(a, v, rv, surf);
	draw_explosions(a, v, rv, surf);
	return 0;
}
