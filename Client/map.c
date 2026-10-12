/*
 * Client/map.c -- mapper panel from remembered knowledge (P2.13).
 *
 * Authority: docs/PHASE2-PLAN.md P2.13; docs/VIEW-v1.md sections 4.5, 4.6;
 * Src/mapper.c (read-only reference).
 */

#include "map.h"

#include <math.h>

/* Panel-relative map coordinate of a grid index. */
static int grid_to_panel(int grid)
{
	return grid * XTK_UI_MAP_BOX_SIZE + XTK_UI_MAP_BORDER;
}

void map_cell_panel_pos(int grid_x, int grid_y, int *px, int *py)
{
	if (px != NULL) {
		*px = XTK_UI_MAP_X + grid_to_panel(grid_x);
	}
	if (py != NULL) {
		*py = XTK_UI_MAP_Y + grid_to_panel(grid_y);
	}
}

int map_world_to_grid(double world)
{
	if (world < 0.0) {
		return 0;
	}
	return (int)(world / (double)XTK_MAP_BOX_WORLD);
}

int map_own_marker(const XtkViewSnapshot *v, int *px, int *py)
{
	int i;

	if (v == NULL) {
		return 0;
	}
	for (i = 0; i < (int)v->vehicle_count; i++) {
		const XtkVehicleView *vv = &v->vehicle[i];
		if (vv->slot == v->own_slot) {
			int gx = map_world_to_grid(vv->x);
			int gy = map_world_to_grid(vv->y);
			if (gx >= XTK_UI_GRID_W) {
				gx = XTK_UI_GRID_W - 1;
			}
			if (gy >= XTK_UI_GRID_H) {
				gy = XTK_UI_GRID_H - 1;
			}
			map_cell_panel_pos(gx, gy, px, py);
			return 1;
		}
	}
	return 0;
}

/* Clip a horizontal wall segment at panel row y to the map panel width. */
static void wall_h(XtkUiScene *s, int x, int y, uint32_t color)
{
	int x2 = x + XTK_UI_MAP_BOX_SIZE;

	if (x < XTK_UI_MAP_X) {
		x = XTK_UI_MAP_X;
	}
	if (x2 > XTK_UI_MAP_X + XTK_UI_MAP_W) {
		x2 = XTK_UI_MAP_X + XTK_UI_MAP_W;
	}
	if (x2 > x) {
		ui_add_line(s, x, y, x2, y, color);
	}
}

static void wall_v(XtkUiScene *s, int x, int y, uint32_t color)
{
	int y2 = y + XTK_UI_MAP_BOX_SIZE;

	if (y < XTK_UI_MAP_Y) {
		y = XTK_UI_MAP_Y;
	}
	if (y2 > XTK_UI_MAP_Y + XTK_UI_MAP_H) {
		y2 = XTK_UI_MAP_Y + XTK_UI_MAP_H;
	}
	if (y2 > y) {
		ui_add_line(s, x, y, x, y2, color);
	}
}

int map_build(const XtkViewSnapshot *v, XtkUiScene *scene)
{
	int i, drawn = 0;

	if (scene == NULL) {
		return -1;
	}
	ui_scene_init(scene);

	/* Map panel background. */
	ui_add_rect(scene, XTK_UI_MAP_X, XTK_UI_MAP_Y, XTK_UI_MAP_W,
		    XTK_UI_MAP_H, XTK_UI_BLACK, 1);

	if (v == NULL) {
		return 0;
	}

	for (i = 0; i < (int)v->map_cell_count; i++) {
		const XtkMapCell *c = &v->cell[i];
		int mx, my, gx, gy;
		uint32_t wall, dest;

		if (!(c->flags & XTK_MAPCELL_KNOWN)) {
			continue;
		}
		gx = c->grid_x;
		gy = c->grid_y;
		if (gx >= XTK_UI_GRID_W || gy >= XTK_UI_GRID_H) {
			continue;
		}
		mx = XTK_UI_MAP_X + grid_to_panel(gx);
		my = XTK_UI_MAP_Y + grid_to_panel(gy);
		drawn++;

		wall = (c->flags & XTK_MAPCELL_TEAM_KNOWN) ? XTK_UI_ORANGE
							   : XTK_UI_WHITE;
		dest = XTK_UI_YELLOW;
		if (c->flags & XTK_MAPCELL_NORTH_WALL) {
			wall_h(scene, mx, my,
			       (c->flags & XTK_MAPCELL_NORTH_DEST) ? dest : wall);
		}
		if (c->flags & XTK_MAPCELL_WEST_WALL) {
			wall_v(scene, mx, my,
			       (c->flags & XTK_MAPCELL_WEST_DEST) ? dest : wall);
		}
		if (c->type != 0) {
			ui_add_rect(scene, mx + 2, my + 2, XTK_UI_MAP_BOX_SIZE - 3,
				    XTK_UI_MAP_BOX_SIZE - 3,
				    (c->flags & XTK_MAPCELL_TEAM_KNOWN)
					    ? XTK_UI_VIOLET
					    : XTK_UI_CYAN,
				    1);
		}
	}

	/* Own marker from the authorized displayed position. */
	{
		int px, py;
		if (map_own_marker(v, &px, &py)) {
			ui_add_rect(scene, px, py, 4, 4, XTK_UI_WHITE, 1);
			ui_add_line(scene, px, py,
				    px + XTK_UI_MAP_BOX_SIZE - 1,
				    py + XTK_UI_MAP_BOX_SIZE - 1, XTK_UI_WHITE);
			ui_add_line(scene, px + XTK_UI_MAP_BOX_SIZE - 1, py, px,
				    py + XTK_UI_MAP_BOX_SIZE - 1, XTK_UI_WHITE);
		}
	}
	return drawn;
}
