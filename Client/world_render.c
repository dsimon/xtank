/*
 * Client/world_render.c -- presentation glue between the client model/camera
 * and the snapshot-driven renderer (P2.11).
 *
 * The renderer (Client/render_sdl.c, P2.10) draws a supplied XtkViewSnapshot
 * through an XtkRenderView.  P2.11 supplies the interpolated snapshot and the
 * camera; this unit adapts the fixed Client/camera.h camera plus the default
 * presentation colors into that XtkRenderView and draws one full frame.  It is
 * SDL-free: it only passes the caller's surface pointer through.
 *
 * Authority: docs/PHASE2-PLAN.md P2.11; docs/PHASE2-DECISIONS.md D2-E;
 * Client/render.h.
 */

#include "render.h"

#include <string.h>

int world_render_present(const XtkAssets *a, const XtkViewSnapshot *v,
			 const XtkCamera *cam, struct SDL_Surface *surf,
			 char *err, size_t errcap)
{
	XtkRenderView rv;

	if (a == NULL || v == NULL || cam == NULL || surf == NULL) {
		if (err != NULL && errcap > 0) {
			strncpy(err, "world_render_present: null argument",
				errcap - 1);
			err[errcap - 1] = '\0';
		}
		return -1;
	}

	memset(&rv, 0, sizeof(rv));
	rv.center_x = cam->center_x;
	rv.center_y = cam->center_y;
	rv.viewport_w = cam->viewport_w;
	rv.viewport_h = cam->viewport_h;

	/* Distinct default colors so the fixture can tell entities apart. */
	rv.color_bg = XTK_RGB(0, 0, 0);
	rv.color_own = XTK_RGB(0, 255, 0);
	rv.color_vehicle = XTK_RGB(255, 0, 0);
	rv.color_bullet = XTK_RGB(255, 255, 0);
	rv.color_explosion = XTK_RGB(255, 255, 255);
	rv.color_landmark = XTK_RGB(0, 255, 255);
	rv.explosion_frame = -1;

	return render_frame_surface(a, v, &rv, surf, err, errcap);
}