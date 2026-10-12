/*
 * Client/map.h -- mapper panel from remembered knowledge (P2.13).
 *
 * map_build() draws the controlled player's mapper knowledge from the
 * XtkViewSnapshot MapCell records (class MAP, docs/VIEW-v1.md section 4.6):
 * explored cells, legitimate stale remembered walls and landmark/team symbols.
 * It never reads the raw world map, never invents geometry and never draws a cell the
 * exporter did not authorize, so a wall the player has not (or no longer)
 * explores simply does not appear.  The Marker is the own vehicle's authorized
 * displayed position.
 *
 * Authority: docs/PHASE2-PLAN.md P2.13; docs/VIEW-v1.md sections 4.5, 4.6, 5.1,
 * 5.3; Src/mapper.c and Src/Include/map.h (read-only references).
 */

#ifndef XTK_CLIENT_MAP_H
#define XTK_CLIENT_MAP_H

#include "ui.h"

/* MapCell.flags bits (mirror of Src/Include/server_view.h; frozen wire). */
#define XTK_MAPCELL_KNOWN      (1u << 0)
#define XTK_MAPCELL_NORTH_WALL (1u << 1)
#define XTK_MAPCELL_WEST_WALL  (1u << 2)
#define XTK_MAPCELL_NORTH_DEST (1u << 3)
#define XTK_MAPCELL_WEST_DEST  (1u << 4)
#define XTK_MAPCELL_TEAM_KNOWN (1u << 5)

/* World units per maze box (Src/Include/screen.h BOX_WIDTH with the frozen
 * ANIM_WIN_WIDTH=768 / NUM_BOXES=4 reference viewport). */
#define XTK_MAP_BOX_WORLD 192

/*
 * Build the mapper scene from the snapshot's MapCell records and the own
 * vehicle's displayed position.  Unexplored cells are omitted; only authorized
 * remembered knowledge is drawn.  Returns the number of known cells drawn, or
 * -1 on a full scene.
 */
int map_build(const XtkViewSnapshot *v, XtkUiScene *scene);

/* Panel-relative top-left of a grid cell's map box, for tests and callers. */
void map_cell_panel_pos(int grid_x, int grid_y, int *px, int *py);

/* Map a world coordinate's box index (0..XTK_UI_GRID_*). */
int map_world_to_grid(double world);

/*
 * Own marker panel coordinates from the authorized snapshot (the own vehicle's
 * displayed position).  Returns 1 and writes px/py when the own vehicle is in
 * the view, else 0.
 */
int map_own_marker(const XtkViewSnapshot *v, int *px, int *py);

#endif /* XTK_CLIENT_MAP_H */
