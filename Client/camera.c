/*
 * Client/camera.c -- client presentation camera (P2.11).  See camera.h.
 */

#include "camera.h"

#include <stddef.h>

void camera_init(XtkCamera *c)
{
	if (c == NULL) {
		return;
	}
	c->center_x = 0.0;
	c->center_y = 0.0;
	c->viewport_w = XTK_CAMERA_VIEWPORT;
	c->viewport_h = XTK_CAMERA_VIEWPORT;
	c->half_extent = (double)XTK_CAMERA_HALF;
}

void camera_center_on(XtkCamera *c, double x, double y)
{
	if (c == NULL) {
		return;
	}
	c->center_x = x;
	c->center_y = y;
	/* viewport_w/h and half_extent are deliberately left unchanged. */
}

int camera_in_view(const XtkCamera *c, double x, double y)
{
	if (c == NULL) {
		return 0;
	}
	return (x >= c->center_x - c->half_extent) &&
	       (x <  c->center_x + c->half_extent) &&
	       (y >= c->center_y - c->half_extent) &&
	       (y <  c->center_y + c->half_extent);
}