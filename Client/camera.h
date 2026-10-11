/*
 * Client/camera.h -- client presentation camera (P2.11).
 *
 * The camera is the reference animation-viewport rule from docs/VIEW-v1.md §3,
 * expressed as presentation policy: it follows the controlled player's
 * **displayed** (interpolated) pose, and it has a fixed logical viewport and
 * world scale (one world unit maps to one logical pixel at the legacy
 * ANIM_WIN_WIDTH = NUM_BOXES * BOX_WIDTH = 768 window).  The camera takes no
 * client input and requests no region beyond the authorized own-pose window, so
 * it can never widen the authorized world data.  Logical viewport and world
 * scale are invariant under frame rate, window size and interpolation.
 *
 * Authority: docs/PHASE2-PLAN.md P2.11; docs/VIEW-v1.md §3.
 */

#ifndef XTK_CLIENT_CAMERA_H
#define XTK_CLIENT_CAMERA_H

/* Legacy animation window: NUM_BOXES (4) * BOX_WIDTH (192) = 768 world units,
 * half-extent 384 on each axis (docs/VIEW-v1.md §3). */
#define XTK_CAMERA_VIEWPORT 768
#define XTK_CAMERA_HALF     (XTK_CAMERA_VIEWPORT / 2)

typedef struct {
	double center_x, center_y;	/* world coordinate at window centre   */
	int    viewport_w, viewport_h;	/* logical pixels (1 world unit = 1 px) */
	double half_extent;		/* world units from centre to edge     */
} XtkCamera;

/* A default camera centred at the origin with the frozen viewport/scale. */
void camera_init(XtkCamera *c);

/* Move the camera centre; viewport and world scale are never changed. */
void camera_center_on(XtkCamera *c, double x, double y);

/* True iff the point is in the half-open legacy animation window. */
int camera_in_view(const XtkCamera *c, double x, double y);

#endif /* XTK_CLIENT_CAMERA_H */