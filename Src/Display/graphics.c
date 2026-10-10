/*-
 * Copyright (c) 1988 Terry Donahue
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

#include "sysdep.h"
#include "graphics.h"

/*
** Neutral backend dispatch layer (P1.4).
**
** The drawing/text/beep/flush/sync and window-state entry points declared by
** graphics_backend.h are defined here, not in the backend, and forwarded
** through the installed GraphicsBackend vtable.  Presentation callers
** therefore depend only on the neutral contract: no caller or this file
** references an Xlib call or type.  The concrete backend (currently x11.c,
** compiled below) names its implementations x11_* and installs the vtable.
*/

void
draw_point(int w, int x, int y, int func, int color)
{
	graphics_backend->draw_point(w, x, y, func, color);
}

void
draw_line(int w, int x1, int y1, int x2, int y2, int func, int color)
{
	graphics_backend->draw_line(w, x1, y1, x2, y2, func, color);
}

void
draw_vert(int w, int x, int y, int size, int func, int color)
{
	graphics_backend->draw_vert(w, x, y, size, func, color);
}

void
draw_hor(int w, int x, int y, int size, int func, int color)
{
	graphics_backend->draw_hor(w, x, y, size, func, color);
}

void
draw_rect(int w, int x, int y, int width, int height, int func, int color)
{
	graphics_backend->draw_rect(w, x, y, width, height, func, color);
}

void
draw_filled_rect(int w, int x, int y, int width, int height, int func, int color)
{
	graphics_backend->draw_filled_rect(w, x, y, width, height, func, color);
}

void
draw_square(int w, int x, int y, int size, int func, int color)
{
	graphics_backend->draw_square(w, x, y, size, func, color);
}

void
draw_filled_square(int w, int x, int y, int size, int func, int color)
{
	graphics_backend->draw_filled_square(w, x, y, size, func, color);
}

void
draw_picture(int w, int x, int y, Picture *pic, int func, int color)
{
	graphics_backend->draw_picture(w, x, y, pic, func, color);
}

void
draw_arc(int w, int x, int y, int width, int height,
		 int angle1, int angle2, int func, int color)
{
	graphics_backend->draw_arc(w, x, y, width, height, angle1, angle2, func, color);
}

void
draw_text(int w, int x, int y, char *str, int font, int func, int color)
{
	graphics_backend->draw_text(w, x, y, str, font, func, color);
}

void
draw_text_left(int w, int x, int y, char *str, int font, int func, int color)
{
	graphics_backend->draw_text_left(w, x, y, str, font, func, color);
}

void
draw_text_rc(int w, int x, int y, char *str, int font, int color)
{
	graphics_backend->draw_text_rc(w, x, y, str, font, color);
}

void
beep_window(void)
{
	graphics_backend->beep_window();
}

void
flush_output(void)
{
	graphics_backend->flush_output();
}

void
sync_output(Boolean discard)
{
	graphics_backend->sync_output(discard);
}

/**************************
** Window state accessors **
**************************/
int
win_exposed(int w)
{
	return (graphics_backend->win_flags(w) & WIN_exposed);
}

int
win_mapped(int w)
{
	return (graphics_backend->mapped(w));
}

int
video_win_flags(int w)
{
	return (graphics_backend->win_flags(w));
}

int
expose_win(int w, int status)
{
	return (graphics_backend->expose_win(w, status));
}

int
color_display(void)
{
	return (graphics_backend->color_display());
}

int
font_height(int font)
{
	return (graphics_backend->font_height(font));
}

int
font_string_width(char *str, int font)
{
	return (graphics_backend->font_string_width(str, font));
}

#ifdef X10
#include "x10.c"
#endif

#ifdef X11
#include "x11.c"
#endif

#ifdef AMIGA
#include "amigagfx.c"
#endif
