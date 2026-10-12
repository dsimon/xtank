/*
 * Client/radar.c -- anonymous radar panel (P2.13).
 *
 * Authority: docs/PHASE2-PLAN.md P2.13; docs/VIEW-v1.md section 4.7;
 * Src/radar.c (read-only reference).
 */

#include "radar.h"

#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

int radar_blip_size(unsigned age)
{
	unsigned view;

	if (age >= XTK_RADAR_BORN) {
		return 0;
	}
	/* view increments at ages 3,6,9,12,15 (Src/radar.c life thresholds). */
	view = age / 3u;
	if (view > 5u) {
		view = 5u;
	}
	return 6 - (int)view;
}

void radar_blip_panel_pos(const XtkRadarBlip *b, int *px, int *py)
{
	int size;

	if (b == NULL) {
		if (px != NULL) *px = 0;
		if (py != NULL) *py = 0;
		return;
	}
	size = radar_blip_size(b->age);
	/* Blip x/y are already map-window coordinates (exporter grid2map). */
	if (px != NULL) {
		*px = XTK_UI_MAP_X + (int)b->x - size / 2;
	}
	if (py != NULL) {
		*py = XTK_UI_MAP_Y + (int)b->y - size / 2;
	}
}

int radar_build(const XtkViewSnapshot *v, XtkUiScene *scene)
{
	int i, drawn = 0;
	int own_px = 0, own_py = 0, have_own;

	if (scene == NULL) {
		return -1;
	}
	ui_scene_init(scene);

	ui_add_rect(scene, XTK_UI_MAP_X, XTK_UI_MAP_Y, XTK_UI_MAP_W,
		    XTK_UI_MAP_H, XTK_UI_BLACK, 1);

	if (v == NULL) {
		return 0;
	}

	have_own = 0;
	for (i = 0; i < (int)v->vehicle_count; i++) {
		if (v->vehicle[i].slot == v->own_slot) {
			int gx = (int)(v->vehicle[i].x / 192.0);
			int gy = (int)(v->vehicle[i].y / 192.0);
			own_px = XTK_UI_MAP_X + gx * XTK_UI_MAP_BOX_SIZE +
				 XTK_UI_MAP_BORDER + XTK_UI_MAP_BOX_SIZE / 2;
			own_py = XTK_UI_MAP_Y + gy * XTK_UI_MAP_BOX_SIZE +
				 XTK_UI_MAP_BORDER + XTK_UI_MAP_BOX_SIZE / 2;
			have_own = 1;
			break;
		}
	}

	/* Presentation sweep: 24 steps driven only by the authorized server_tick,
	 * never by hidden radar state. */
	if (have_own) {
		double ang = (double)(v->server_tick % XTK_RADAR_SWEEP_STEPS) *
			     (2.0 * M_PI / (double)XTK_RADAR_SWEEP_STEPS);
		int ex = own_px + (int)(XTK_RADAR_SWEEP_RADIUS * cos(ang));
		int ey = own_py + (int)(XTK_RADAR_SWEEP_RADIUS * sin(ang));
		ui_add_line(scene, own_px, own_py, ex, ey, XTK_UI_CYAN);
	}

	for (i = 0; i < (int)v->radar_blip_count; i++) {
		const XtkRadarBlip *b = &v->radar[i];
		int size = radar_blip_size(b->age);
		int px, py;

		if (size <= 0) {
			continue;
		}
		radar_blip_panel_pos(b, &px, &py);
		ui_add_rect(scene, px, py, size, size, XTK_UI_CYAN, 1);
		drawn++;
	}
	return drawn;
}
