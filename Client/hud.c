/*
 * Client/hud.c -- authorized own-player HUD/console panel (P2.13).
 *
 * Builds a presentation scene from the controlled player's SelfState only.  No
 * player name is read or drawn; the panel shows armor by side, fuel, heat,
 * score, per-weapon ammo/reload/enabled and the lifecycle flag, plus a
 * presentation-only connection status and transient banner.
 *
 * Authority: docs/PHASE2-PLAN.md P2.13; docs/VIEW-v1.md sections 4.1, 5.4;
 * Src/newconsole.c (read-only reference).
 */

#include "hud.h"

#include <stdio.h>

/* Fixed panel row baseline inside the console window. */
#define HUD_X   (XTK_UI_CONS_X + XTK_UI_BORDER)
#define HUD_TOP (XTK_UI_CONS_Y + XTK_UI_BORDER)

const char *hud_weapon_status(const XtkSelfState *self, int weapon)
{
	int enabled;

	if (self == NULL || weapon < 0 || weapon >= (int)self->weapon_count ||
	    weapon >= (int)XTK_MAX_WEAPONS) {
		return "---";
	}
	enabled = (self->weapon_enabled_bitmap >> weapon) & 1;
	if (!enabled) {
		return "offline";
	}
	return self->ammo[weapon] == 0 ? "EMPTY" : "ONLINE";
}

static void hud_row(XtkUiScene *s, int row, const char *text)
{
	ui_add_text(s, HUD_X, HUD_TOP + row * ui_font_height(XTK_FONT_SMALL),
		    XTK_FONT_SMALL, XTK_UI_WHITE, text);
}

int hud_build(const XtkSelfState *self, int have_self,
	      const XtkHudInfo *info, XtkUiScene *scene)
{
	char buf[XTK_UI_STR_MAX];
	int i, row = 0;

	if (scene == NULL) {
		return -1;
	}
	ui_scene_init(scene);

	if (!have_self || self == NULL) {
		hud_row(scene, row++, "Armor");
		hud_row(scene, row++, "  (no view received)");
		if (info != NULL && !info->connected) {
			hud_row(scene, row++, "CONNECTION LOST");
		}
		return 0;
	}

	hud_row(scene, row++, " Armor");
	snprintf(buf, sizeof(buf), "  Front: %-5u Back: %u",
		 (unsigned)self->armor[0], (unsigned)self->armor[1]);
	hud_row(scene, row++, buf);
	snprintf(buf, sizeof(buf), "  Left:  %-5u Right: %u",
		 (unsigned)self->armor[2], (unsigned)self->armor[3]);
	hud_row(scene, row++, buf);
	snprintf(buf, sizeof(buf), "  Fuel: %-7u Heat: %u",
		 (unsigned)self->fuel, (unsigned)self->heat);
	hud_row(scene, row++, buf);
	snprintf(buf, sizeof(buf), "  Score: %u", (unsigned)self->score);
	hud_row(scene, row++, buf);

	hud_row(scene, row++, "# Weapon   Ammo  Reload  Status");
	for (i = 0; i < (int)self->weapon_count && i < (int)XTK_MAX_WEAPONS; i++) {
		const char *st = hud_weapon_status(self, i);
		snprintf(buf, sizeof(buf), "%d          %-5u %-7u %s", i + 1,
			 (unsigned)self->ammo[i], (unsigned)self->reload[i],
			 st);
		hud_row(scene, row++, buf);
	}

	if (!(self->lifecycle_flags & XTK_SELF_ALIVE)) {
		hud_row(scene, row++, "Life: DEAD");
	} else {
		hud_row(scene, row++, "Life: ALIVE");
	}

	if (info != NULL && !info->connected) {
		hud_row(scene, row++, "CONNECTION LOST");
	}

	/* Own-death marker over the animation viewport (presentation only). */
	if (!(self->lifecycle_flags & XTK_SELF_ALIVE)) {
		ui_add_line(scene, XTK_UI_ANIM_X, XTK_UI_ANIM_Y,
			    XTK_UI_ANIM_X + XTK_UI_ANIM_W,
			    XTK_UI_ANIM_Y + XTK_UI_ANIM_H, XTK_UI_RED);
		ui_add_line(scene, XTK_UI_ANIM_X + XTK_UI_ANIM_W, XTK_UI_ANIM_Y,
			    XTK_UI_ANIM_X, XTK_UI_ANIM_Y + XTK_UI_ANIM_H,
			    XTK_UI_RED);
	}

	/* Transient banner (winner/status) supplied by the session; drawn at the
	 * top of the animation viewport.  It carries only what the server sent. */
	if (info != NULL && info->have_banner && info->banner != NULL &&
	    info->banner[0] != '\0') {
		ui_add_text_align(scene, XTK_UI_ANIM_X + XTK_UI_ANIM_W / 2,
				  XTK_UI_ANIM_Y + 24, XTK_FONT_LARGE,
				  XTK_UI_WHITE, XTK_UI_ALIGN_CENTER,
				  info->banner);
	}
	return 0;
}
