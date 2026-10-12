/*
 * Client/ui.h -- presentation scene vocabulary shared by the client HUD, map,
 * radar and menu panels (P2.13).
 *
 * The client presentation modules never draw from simulation globals: each one
 * turns an already-authorized protocol-v1 DTO (XtkViewSnapshot / XtkResults /
 * XtkCatalog / an EVENT) into a small, deterministic list of text, rectangle
 * and line primitives.  render_sdl.c is the only unit that rasterizes a scene
 * onto an SDL surface, so the builders here stay SDL-free, X-free and free of
 * any engine symbol and can be unit-tested in isolation.
 *
 * The logical panel geometry and palette are the frozen reference values from
 * Src/Include/screen.h (S1024x864), Src/Include/map.h and the Src/Display/x11.c
 * colorname[] table; the menu item texts and placement follow the legacy
 * Src/interface.c lobby (docs/PHASE2-PLAN.md P2.13).  Text metrics are a
 * portable built-in font; exact X font glyphs are backend-owned (P2.10 note).
 *
 * Authority: docs/PHASE2-PLAN.md P2.13; docs/VIEW-v1.md sections 4-6;
 * Src/interface.c, Src/newconsole.c, Src/mapper.c, Src/radar.c (read-only
 * references).
 */

#ifndef XTK_CLIENT_UI_H
#define XTK_CLIENT_UI_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "wire.h"		/* portable DTOs (never engine structs) */
#include "render.h"		/* XTK_RGB */

/* ---- Reference logical geometry (Src/Include/screen.h, S1024x864) ----- */

#define XTK_UI_SCREEN_W   1015
#define XTK_UI_SCREEN_H   864

#define XTK_UI_ANIM_X     0
#define XTK_UI_ANIM_Y     0
#define XTK_UI_ANIM_W     768
#define XTK_UI_ANIM_H     768

#define XTK_UI_MAP_X      768
#define XTK_UI_MAP_Y      0
#define XTK_UI_MAP_W      250
#define XTK_UI_MAP_H      250

#define XTK_UI_CONS_X     768
#define XTK_UI_CONS_Y     (XTK_UI_MAP_Y + XTK_UI_MAP_H + 6)	/* 256 */
#define XTK_UI_CONS_W     250
#define XTK_UI_CONS_H     306

#define XTK_UI_MSG_X      768
#define XTK_UI_MSG_Y      (XTK_UI_CONS_Y + XTK_UI_CONS_H + 6)	/* 568 */
#define XTK_UI_MSG_W      250
#define XTK_UI_MSG_H      92

#define XTK_UI_GAME_X     768
#define XTK_UI_GAME_Y     (XTK_UI_MSG_Y + XTK_UI_MSG_H + 6)	/* 666 */
#define XTK_UI_GAME_W     250
#define XTK_UI_GAME_H     192

/* Reference mapper geometry (Src/Include/map.h). */
#define XTK_UI_MAP_BOX_SIZE  9
#define XTK_UI_MAP_BORDER    (-13)
#define XTK_UI_GRID_W        30
#define XTK_UI_GRID_H        30

/* Left/top border inside a panel (graphics.h LEFT_BORDER/TOP_BORDER). */
#define XTK_UI_BORDER        5

/* ---- Reference lobby placement (Src/interface.h, S1024x864) ----------- */

#define XTK_UI_LEV0_X     15
#define XTK_UI_LEV0_Y     150
#define XTK_UI_MAZES_X    420
#define XTK_UI_VEHICLES_X 290
#define XTK_UI_TIER1_Y    350	/* MAZES_Y/VEHICLES_Y + TIER1_HEIGHT */

/* ---- Palette (Src/Display/x11.c colorname[], graphics.h indices) ------ */

#define XTK_UI_BLACK   XTK_RGB(0, 0, 0)
#define XTK_UI_WHITE   XTK_RGB(255, 255, 255)
#define XTK_UI_RED     XTK_RGB(255, 0, 0)
#define XTK_UI_ORANGE  XTK_RGB(255, 161, 68)
#define XTK_UI_YELLOW  XTK_RGB(255, 255, 0)
#define XTK_UI_GREEN   XTK_RGB(0, 255, 0)
#define XTK_UI_BLUE    XTK_RGB(135, 157, 255)
#define XTK_UI_VIOLET  XTK_RGB(218, 112, 214)
#define XTK_UI_GREY    XTK_RGB(181, 181, 181)
#define XTK_UI_CYAN    XTK_RGB(0, 255, 255)

/* ---- Built-in portable text metrics ---------------------------------- */

typedef enum {
	XTK_FONT_SMALL = 0,	/* legacy S_FONT: 5x7 cell, 6 px advance */
	XTK_FONT_MED   = 1,	/* legacy M_FONT                        */
	XTK_FONT_LARGE = 2,	/* legacy L_FONT                        */
	XTK_FONT_XL    = 3	/* legacy XL_FONT                       */
} XtkUiFont;

#define XTK_UI_FONT_COUNT 4

/* Horizontal advance and line height of one font, in logical pixels.  The
 * built-in 5x7 glyph is scaled by 1/1/2/2 for SMALL/MED/LARGE/XL. */
static inline int ui_font_scale(XtkUiFont f)
{
	return (f == XTK_FONT_LARGE || f == XTK_FONT_XL) ? 2 : 1;
}

static inline int ui_font_advance(XtkUiFont f)
{
	return 6 * ui_font_scale(f);
}

static inline int ui_font_height(XtkUiFont f)
{
	return 8 * ui_font_scale(f);
}

/* Width in logical pixels of a NUL-terminated ASCII string. */
static inline int ui_text_width(const char *s, XtkUiFont f)
{
	return s == NULL ? 0 : (int)strlen(s) * ui_font_advance(f);
}

/* ---- Scene primitives ------------------------------------------------ */

#define XTK_UI_TEXT_MAX 512
#define XTK_UI_RECT_MAX 2048
#define XTK_UI_LINE_MAX 1024
#define XTK_UI_STR_MAX  64

#define XTK_UI_ALIGN_LEFT   0
#define XTK_UI_ALIGN_CENTER 1
#define XTK_UI_ALIGN_RIGHT  2

typedef struct {
	int      x, y;
	uint8_t  font;		/* XtkUiFont                          */
	uint8_t  align;		/* XTK_UI_ALIGN_*                     */
	uint32_t color;		/* 0xRRGGBB                           */
	char     text[XTK_UI_STR_MAX];
} XtkUiText;

typedef struct {
	int      x, y, w, h;
	uint32_t color;
	uint8_t  filled;
} XtkUiRect;

typedef struct {
	int      x1, y1, x2, y2;
	uint32_t color;
} XtkUiLine;

typedef struct XtkUiScene {
	XtkUiRect rect[XTK_UI_RECT_MAX];
	int       rect_count;
	XtkUiLine line[XTK_UI_LINE_MAX];
	int       line_count;
	XtkUiText text[XTK_UI_TEXT_MAX];
	int       text_count;
} XtkUiScene;

static inline void ui_scene_init(XtkUiScene *s)
{
	s->rect_count = 0;
	s->line_count = 0;
	s->text_count = 0;
}

/* Append primitives; return 0 on success, -1 when the scene is full.  A text
 * string longer than XTK_UI_STR_MAX-1 is truncated, never split. */
static inline int ui_add_text_align(XtkUiScene *s, int x, int y, XtkUiFont f,
				    uint32_t color, int align,
				    const char *text)
{
	XtkUiText *t;

	if (s == NULL || s->text_count >= XTK_UI_TEXT_MAX) {
		return -1;
	}
	t = &s->text[s->text_count++];
	t->x = x;
	t->y = y;
	t->font = (uint8_t)f;
	t->align = (uint8_t)align;
	t->color = color;
	if (text == NULL) {
		text = "";
	}
	{
		size_t n = strlen(text);
		if (n > XTK_UI_STR_MAX - 1) {
			n = XTK_UI_STR_MAX - 1;
		}
		memcpy(t->text, text, n);
		t->text[n] = '\0';
	}
	return 0;
}

static inline int ui_add_text(XtkUiScene *s, int x, int y, XtkUiFont f,
			      uint32_t color, const char *text)
{
	return ui_add_text_align(s, x, y, f, color, XTK_UI_ALIGN_LEFT, text);
}

static inline int ui_add_rect(XtkUiScene *s, int x, int y, int w, int h,
			      uint32_t color, int filled)
{
	XtkUiRect *r;

	if (s == NULL || s->rect_count >= XTK_UI_RECT_MAX) {
		return -1;
	}
	r = &s->rect[s->rect_count++];
	r->x = x;
	r->y = y;
	r->w = w;
	r->h = h;
	r->color = color;
	r->filled = (uint8_t)(filled ? 1 : 0);
	return 0;
}

static inline int ui_add_line(XtkUiScene *s, int x1, int y1, int x2, int y2,
			      uint32_t color)
{
	XtkUiLine *l;

	if (s == NULL || s->line_count >= XTK_UI_LINE_MAX) {
		return -1;
	}
	l = &s->line[s->line_count++];
	l->x1 = x1;
	l->y1 = y1;
	l->x2 = x2;
	l->y2 = y2;
	l->color = color;
	return 0;
}

/* Read-only scene queries used by the acceptance fixture. */
static inline const XtkUiText *ui_scene_text(const XtkUiScene *s, int index)
{
	if (s == NULL || index < 0 || index >= s->text_count) {
		return NULL;
	}
	return &s->text[index];
}

static inline int ui_scene_text_count(const XtkUiScene *s)
{
	return s == NULL ? 0 : s->text_count;
}

static inline int ui_scene_has_text(const XtkUiScene *s, const char *needle)
{
	int i;

	if (s == NULL || needle == NULL) {
		return 0;
	}
	for (i = 0; i < s->text_count; i++) {
		if (strstr(s->text[i].text, needle) != NULL) {
			return 1;
		}
	}
	return 0;
}

#endif /* XTK_CLIENT_UI_H */
