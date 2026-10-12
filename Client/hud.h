/*
 * Client/hud.h -- authorized own-player HUD/console panel (P2.13).
 *
 * hud_build() turns the controlled player's SelfState record (class SELF in
 * docs/VIEW-v1.md section 4.1) into a presentation scene.  It displays exactly
 * the authorized values -- armor by side, fuel, heat, score, per-weapon ammo,
 * reload and enabled state, and the lifecycle flag -- and never a player name:
 * the SelfState DTO carries no name and this module reads none (the anonymity
 * rule, docs/VIEW-v1.md sections 4.7/5.2).  Public names appear only on the
 * results/banner surface built by Client/menu.c.
 *
 * Authority: docs/PHASE2-PLAN.md P2.13; docs/VIEW-v1.md sections 4.1, 5.4, 6;
 * Src/newconsole.c (read-only reference for the panel labels and status
 * vocabulary).
 */

#ifndef XTK_CLIENT_HUD_H
#define XTK_CLIENT_HUD_H

#include "ui.h"

/* Lifecycle flag bits exposed for SELF (docs/VIEW-v1.md section 4.1); mirror
 * of the engine VS_is_alive bit (Src/Include/xtank.h). */
#define XTK_SELF_ALIVE   (1u << 1)

/*
 * Presentation-only state the HUD cannot obtain from SelfState: whether the
 * transport is connected and an optional transient banner.  Neither carries
 * hidden authoritative state.
 */
typedef struct {
	int         connected;		/* transport connected            */
	int         have_banner;	/* draw `banner`                  */
	const char *banner;		/* NUL-terminated, may be NULL    */
} XtkHudInfo;

/*
 * Build the HUD scene for one authorized SelfState.  `self` may be NULL only
 * when `have_self` is false (nothing received).  Returns 0, or -1 on a full
 * scene.
 */
int hud_build(const XtkSelfState *self, int have_self,
	      const XtkHudInfo *info, XtkUiScene *scene);

/* Weapon status vocabulary, mirrored from Src/newconsole.c console_w_status[]. */
const char *hud_weapon_status(const XtkSelfState *self, int weapon);

#endif /* XTK_CLIENT_HUD_H */
