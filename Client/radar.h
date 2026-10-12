/*
 * Client/radar.h -- anonymous radar panel (P2.13).
 *
 * radar_build() draws the controlled player's ordinary radar contacts from the
 * XtkViewSnapshot RadarBlip records (class RADAR, docs/VIEW-v1.md section 4.7).
 * Every contact is anonymous: the DTO carries only map-window position, age and
 * the (always anonymous, in the release preset) classification, so a blip can
 * never reveal which vehicle it is.  Blip size shrinks with age exactly as the
 * legacy sweep does, and the server already removes expired contacts.
 *
 * Authority: docs/PHASE2-PLAN.md P2.13; docs/VIEW-v1.md sections 4.7, 5.5;
 * Src/radar.c (read-only reference for the blip life/size and 24-step sweep).
 */

#ifndef XTK_CLIENT_RADAR_H
#define XTK_CLIENT_RADAR_H

#include "ui.h"

/* Blip life at acquisition (mirror of Src/Include/server_view.h). */
#define XTK_RADAR_BORN 23

/* The reference sweep has 24 positions. */
#define XTK_RADAR_SWEEP_STEPS 24
#define XTK_RADAR_SWEEP_RADIUS 63

/*
 * Build the radar scene onto the map-window panel.  Returns the number of
 * anonymous blips drawn, or -1 on a full scene.
 */
int radar_build(const XtkViewSnapshot *v, XtkUiScene *scene);

/* Filled-blip side length (1..6) for an acquisition age in ticks, or 0 when
 * the contact has expired (age >= XTK_RADAR_BORN). */
int radar_blip_size(unsigned age);

/* Panel-relative top-left of a blip's centred square. */
void radar_blip_panel_pos(const XtkRadarBlip *b, int *px, int *py);

#endif /* XTK_CLIENT_RADAR_H */
