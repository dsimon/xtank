/*-
 * Copyright (c) 1988 Terry Donahue
 * Copyright (c) 1993 Josh Osborne
 * Copyright (c) 1993 Kurt Lidl
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
** Comment: Neutral graphics backend contract.
**
** This header is the boundary between the (display-independent) simulation
** and bot code and a concrete presentation backend.  It deliberately contains
** no X11 (or other window-system) types or headers: physics and bot
** translation units may include it transitively without gaining a display
** dependency.  A backend (currently Src/Display/x11.c) defines the opaque
** Video structure and implements the operations declared here.
*/

#ifndef _GRAPHICS_BACKEND_H_
#define _GRAPHICS_BACKEND_H_

#include <assert.h>
#include "tanktypes.h"
#include "object.h"

/************************
** Neutral geometry types **
************************/
/*
 * These mirror the layout of the platform point/segment structures used by
 * the current X11 backend but are expressed in neutral terms so callers do
 * not need the window-system header.
 */
typedef struct {
	short x, y;
}
GraphicsPoint;

typedef struct {
	short x1, y1, x2, y2;
}
GraphicsSegment;

/*******************
** Drawing constants **
*******************/
#define DRAW_XOR	0
#define DRAW_COPY	1
#define MAX_DRAW_FUNCS	2

/***************
** Window flags **
***************/
#define WIN_exposed	(1<<0)
#define WIN_mapped      (1<<1)

/*
 * Pointer-input semantics (P1.4).
 *
 * The legacy callers in setup.c/interface.c enable button-release and mouse
 * -motion tracking on the animation window, while menu.c toggles them only on
 * backends that do not keep pointer input always on.  Both call sites were
 * historically spelled with `#ifdef X11`/`#ifndef X11`.  The condition is
 * named explicitly here so that removing -DX11 from server compilation
 * (P1.5) cannot silently change X input handling; the X presentation backend
 * defines it, and a later backend sets it to match its own input model.
 */
#ifdef X11
#define GRAPHICS_POINTER_INPUT_ALWAYS_ON	1
#endif

/*
 * Opaque presentation state.  The concrete definition lives in the active
 * backend; only backend-private code may dereference it.
 */
typedef struct video Video;

/**********************
** Backend operations **
**********************/
/*
 * The operations a presentation backend must supply.  The neutral entry
 * points below (draw_*, beep/flush/sync, window-state accessors, font
 * metrics) are defined in Src/Display/graphics.c and forward through the
 * installed vtable; a concrete backend (Src/Display/x11.c) names its
 * implementations x11_* and installs this table.
 *
 * Preferences (player/tank name, mouse, keymap, escher width, ...) are routed
 * through the accessors declared at the bottom of this header rather than the
 * table, because they are per-terminal state reads.
 */
typedef struct GraphicsBackend {
	const char *name;

	void (*draw_point) (int w, int x, int y, int func, int color);
	void (*draw_line) (int w, int x1, int y1, int x2, int y2, int func, int color);
	void (*draw_vert) (int w, int x, int y, int size, int func, int color);
	void (*draw_hor) (int w, int x, int y, int size, int func, int color);
	void (*draw_rect) (int w, int x, int y, int width, int height, int func, int color);
	void (*draw_filled_rect) (int w, int x, int y, int width, int height, int func, int color);
	void (*draw_square) (int w, int x, int y, int size, int func, int color);
	void (*draw_filled_square) (int w, int x, int y, int size, int func, int color);
	void (*draw_picture) (int w, int x, int y, Picture *pic, int func, int color);
	void (*draw_arc) (int w, int x, int y, int width, int height,
					  int angle1, int angle2, int func, int color);
	void (*draw_text) (int w, int x, int y, char *str, int font, int func, int color);
	void (*draw_text_left) (int w, int x, int y, char *str, int font, int func, int color);
	void (*draw_text_rc) (int w, int x, int y, char *str, int font, int color);
	void (*beep_window) (void);
	void (*flush_output) (void);
	void (*sync_output) (Boolean discard);

	int (*win_flags) (int w);
	int (*expose_win) (int w, int status);
	int (*mapped) (int w);
	int (*color_display) (void);
	int (*font_height) (int font);
	int (*font_string_width) (char *str, int font);
} GraphicsBackend;

extern GraphicsBackend *graphics_backend;

/***************************
** Neutral drawing wrappers **
***************************/
void draw_point(int w, int x, int y, int func, int color);
void draw_line(int w, int x1, int y1, int x2, int y2, int func, int color);
void draw_vert(int w, int x, int y, int size, int func, int color);
void draw_hor(int w, int x, int y, int size, int func, int color);
void draw_rect(int w, int x, int y, int width, int height, int func, int color);
void draw_filled_rect(int w, int x, int y, int width, int height, int func, int color);
void draw_square(int w, int x, int y, int size, int func, int color);
void draw_filled_square(int w, int x, int y, int size, int func, int color);
void draw_picture(int w, int x, int y, Picture *pic, int func, int color);
void draw_arc(int w, int x, int y, int width, int height, int angle1, int angle2, int func, int color);

/* Window state accessors (replace the former x11.h macros). */
int win_exposed(int w);
int win_mapped(int w);
int expose_win(int w, int status);
int color_display(void);
int video_win_flags(int w);

/*
 * Batching hooks.  USE_BATCHED_POINTS/USE_BATCHED_LINES are not defined in
 * the supported build (the legacy guard sits in the backend-private x11.h and
 * these macros are empty there), so callers see no-ops exactly as in the
 * pinned build.  The draw entry points above already route through the
 * backend, so enabling real batching is a backend-local change that does not
 * touch callers.
 */
#define flush_point_batch
#define flush_line_batch

/* Per-terminal preference/state accessors used by the presentation UI. */
int video_toggle_beep(void);
int video_escher_width(void);
char *video_player_name(void);
char *video_tank_name(void);
int video_mouse_speed(void);
int video_mouse_drive(void);
int video_mouse_heat(void);
char *video_keymap(void);
int video_last_expose_frame(Video *v);
const char *video_display_name(Video *v);

/* Backend wrapper for escape-hatch callers that draw on an explicit Video. */
void video_draw_line(Video *v, int w, int x1, int y1, int x2, int y2, int func, int color);

/*
 * Pseudo colors.  These are backend-owned but exposed as assignable lvalues
 * because the color engine (and the X resource parser) still store them here.
 */
int *video_dest_wall(void);
int *video_rdf_safe(void);
#define DEST_WALL (*video_dest_wall())
#define RDF_SAFE  (*video_rdf_safe())

/* Debugging flag, formerly declared in x11.h. */
extern Boolean un_stingy;

#endif /* _GRAPHICS_BACKEND_H_ */
