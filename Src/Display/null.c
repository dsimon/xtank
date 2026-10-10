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
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
** Display/null.c -- neutral, display-free presentation backend (P1.5).
**
** This is a genuine GraphicsBackend implementation: it draws nothing, opens
** no display, allocates no window-system resources, and synthesises neutral
** input (no events).  It contains no <X11/...> include and no Xlib type or
** call, so the server translation unit that includes it (Display/graphics.c
** under -DNULL_BACKEND) has no display dependency.
**
** The presentation/UI code is still compiled and called for parity (P1.5
** retains the legacy presentation callers); every simulation-visible side
** effect that the X backend produced (window expose flags, Video preference
** defaults, picture bookkeeping) is reproduced here without a display.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "sysdep.h"
#include "graphics.h"			/* neutral contract and limits */
#include "proto.h"

/********************
** Neutral Video state **
********************/
/*
 * The X backend keeps window-system handles here; the null backend keeps only
 * the neutral bookkeeping state that presentation callers read through the
 * accessors in graphics_backend.h.
 */
struct Kludge_Defaults {
	char player_name[MAX_STRING + 1];
	char tank_name[MAX_STRING + 1];
	int wants_keypad;
	int mouse_speed;
	int mouse_drive;
	int mouse_heat;
	char keymap[MAX_STRING + 1];
};

typedef struct {
	int id;
	int width, height;
	int flags;
}
NullWin;

struct video {
	char display_name[50];
	int planes;
	long input_mask;
	int parent_id;
	int num_windows;
	NullWin win[MAX_WINDOWS];
	int num_pixids;
	int pixid[MAX_PIXMAPS];
	int width[MAX_PIXMAPS], height[MAX_PIXMAPS];
	unsigned long fg, bg, color[MAX_COLORS];
	int cursor[MAX_CURSORS];
	int escher_width;
	int last_expose_frame;
	unsigned int beep_flag:1;
	unsigned int display_names_flag:1;
	struct Kludge_Defaults kludge;
	int dest_wall;
	int rdf_green;
};

/* Neutral input-mask bits (no window system; bookkeeping only). */
#define NULL_MOTION_MASK	0x40
#define NULL_BUTTONUP_MASK	0x80

Video *vid;
Video *oldvid = NULL;

Boolean un_stingy = FALSE;

char video_error_str[256];

int team_color[] =
{GREY, RED, ORANGE, YELLOW, GREEN, BLUE, VIOLET};
int team_color_bright[] =
{WHITE, RED, ORANGE, YELLOW, GREEN, BLUE, VIOLET};

/* Cached font metrics (fixed, display-independent). */
static int fwidth[MAX_FONTS] = {6, 6, 6, 6, 6};
static int fheight[MAX_FONTS] = {15, 14, 14, 14, 14};
static int fascent[MAX_FONTS] = {13, 13, 13, 13, 13};

/**************************
** Neutral state accessors **
**************************/
int
video_toggle_beep(void)
{
	return (vid->beep_flag = !vid->beep_flag);
}

int
video_escher_width(void)
{
	return (vid->escher_width);
}

char *
video_player_name(void)
{
	return (vid->kludge.player_name);
}

char *
video_tank_name(void)
{
	return (vid->kludge.tank_name);
}

int
video_mouse_speed(void)
{
	return (vid->kludge.mouse_speed);
}

int
video_mouse_drive(void)
{
	return (vid->kludge.mouse_drive);
}

int
video_mouse_heat(void)
{
	return (vid->kludge.mouse_heat);
}

char *
video_keymap(void)
{
	return (vid->kludge.keymap);
}

int
video_last_expose_frame(Video *v)
{
	return (v->last_expose_frame);
}

const char *
video_display_name(Video *v)
{
	return (v->display_name);
}

int *
video_dest_wall(void)
{
	return (&vid->dest_wall);
}

int *
video_rdf_safe(void)
{
	return (&vid->rdf_green);
}

/* Escape-hatch caller: draws nothing. */
void
video_draw_line(Video *v, int w, int x1, int y1, int x2, int y2, int func, int color)
{
	(void) v;
	(void) w;
	(void) x1;
	(void) y1;
	(void) x2;
	(void) y2;
	(void) func;
	(void) color;
}

/*********************
** Window functions **
*********************/

void
open_graphics(void)
{
}

void
close_graphics(void)
{
}

void
reset_video(void)
{
	vid->last_expose_frame = 0;
}

void
set_video(Video *video)
{
	vid = video;
}

char *
get_default(char *itemname, char *itemclass, char *defaultstr)
{
	(void) itemname;
	(void) itemclass;
	return (defaultstr);
}

int
get_num_default(char *itemname, char *itemclass, int defaultnum)
{
	char *ptr;

	ptr = get_default(itemname, itemclass, (char *) NULL);
	if (!ptr)
		return (defaultnum);
	return (atoi(ptr));
}

/*
 * Gets default resources, sets the neutral color table and caches font
 * metrics.  No display or colormap is touched.
 */
static int
make_gcs(void)
{
	char dummy = '\0';
	int i;

	vid->planes = 24;
	vid->fg = 0;
	vid->bg = 0;

	strncpy(vid->kludge.player_name, get_default("playerName", "PlayerName",
												 &dummy), MAX_STRING);
	strncpy(vid->kludge.tank_name, get_default("tankName", "TankName",
											   &dummy), MAX_STRING);
	vid->kludge.mouse_speed = get_num_default("mouseSpeed", "MouseSpeed", 0);
	vid->kludge.mouse_drive = get_num_default("mouseDrive", "MouseDrive", 0);
	vid->kludge.mouse_heat = get_num_default("mouseHeat", "MouseHeat", 0);
	strncpy(vid->kludge.keymap, get_default("keyMap", "KeyMap", &dummy),
			MAX_STRING);
#ifdef KEYPAD_DETECT
	vid->kludge.wants_keypad = get_num_default("keypad", "Keypad", 1);
#endif

	for (i = 0; i < MAX_COLORS; i++)
		vid->color[i] = (i == BLACK) ? vid->bg : vid->fg;

	return (0);
}

static int
make_cursors(void)
{
	return (0);
}

static int
make_cursor(int c, int width, int height, int xhot, int yhot,
			const char bits[], const char mask[])
{
	(void) c;
	(void) width;
	(void) height;
	(void) xhot;
	(void) yhot;
	(void) bits;
	(void) mask;
	return (0);
}

int
make_parent(void)
{
	return (0);
}

/*
 * Makes a Video structure, initializes it without a display.
 */
Video *
make_video(char *name)
{
	Video *video;
	int i;

	video = (Video *) malloc(sizeof(Video));
	if (video == NULL)
		return (Video *) NULL;

	(void) strcpy(video->display_name, name);
	video->planes = 1;
	video->parent_id = 0;
	video->num_windows = 0;
	video->num_pixids = 0;
	video->last_expose_frame = 0;
	set_video(video);

	if (make_gcs())
		return (Video *) NULL;
	make_cursors();
	make_parent();

	/*
	 * A display-free backend has no resource database, so terminal player
	 * names cannot come from X resources the way the X front-end obtains
	 * them.  The test harness supplies per-terminal names (colon separated,
	 * consumed in terminal order) via XTANK_NULL_PLAYERS so the bot-free v2
	 * oracle can bind named human slots; when unset the name stays empty,
	 * preserving the legacy v1 behavior.
	 */
	{
		static char *null_players = NULL;
		static int null_player_next = 0;

		if (null_players == NULL) {
			null_players = getenv("XTANK_NULL_PLAYERS");
			if (null_players == NULL)
				null_players = "";
		}
		if (*null_players != '\0') {
			const char *s = null_players;
			int k;

			for (k = 0; k < null_player_next && *s; k++) {
				s = strchr(s, ':');
				if (s != NULL)
					s++;
			}
			if (s != NULL && *s != '\0') {
				const char *e = strchr(s, ':');
				size_t n = (e != NULL) ? (size_t) (e - s) : strlen(s);

				if (n > MAX_STRING)
					n = MAX_STRING;
				memcpy(video->kludge.player_name, s, n);
				video->kludge.player_name[n] = '\0';
			}
			null_player_next++;
		}
	}

	video->escher_width = get_num_default("escherWidth", "Width", 10);

	DEST_WALL = get_num_default("color.dest_wall_index", "Color.Team",
								(vid->planes > 1) ? GREY : DASHED);
	if (DEST_WALL < BLACK || DEST_WALL >= MAX_COLORS)
		DEST_WALL = GREY;

	RDF_SAFE = get_num_default("color.green_rdf", "Color.Team",
							   (vid->planes > 1) ? GREEN : DASHED);
	if (RDF_SAFE < BLACK || DEST_WALL >= MAX_COLORS)
		RDF_SAFE = GREEN;

	i = get_num_default("beep", "Beep", 1);
	video->beep_flag = (i == 0) ? 0 : 1;

	i = get_num_default("drawName", "Drawname", 1);
	video->display_names_flag = (i == 0) ? 0 : 1;

	video->last_expose_frame = 0;
	return (video);
}

void
close_video(Video *video)
{
	free((char *) video);
}

int
make_window(int w, int x, int y, int width, int height, int border)
{
	(void) x;
	(void) y;
	(void) border;
	vid->input_mask = 0;
	vid->win[w].id = w;
	vid->win[w].width = width;
	vid->win[w].height = height;
	/* No Expose events arrive without a server; treat the window as
	   exposed so map_windows() terminates. */
	vid->win[w].flags = WIN_exposed;
	vid->num_windows = (vid->num_windows > w + 1) ? vid->num_windows : w + 1;
	return (0);
}

void
map_window(int w)
{
	vid->win[w].flags |= WIN_mapped;
}

void
unmap_window(int w)
{
	vid->win[w].flags &= ~WIN_mapped;
}

void
clear_window(int w)
{
	(void) w;
}

int
should_disp_name(void)
{
	return (vid->display_names_flag);
}

void
follow_mouse(int w, Boolean status)
{
	(void) w;
	if (status == TRUE)
		vid->input_mask |= NULL_MOTION_MASK;
	else
		vid->input_mask &= ~NULL_MOTION_MASK;
}

void
button_up(int w, Boolean status)
{
	(void) w;
	if (status == TRUE)
		vid->input_mask |= NULL_BUTTONUP_MASK;
	else
		vid->input_mask &= ~NULL_BUTTONUP_MASK;
}

/*
 * No display and no event source: report no events, but keep the animation
 * window flagged exposed so map_windows() terminates.
 */
void
get_events(int *num_events, Event *event)
{
	(void) event;
	*num_events = 0;
	if (vid != NULL && vid->num_windows > 0)
		vid->win[0].flags |= WIN_exposed;	/* ANIM_WIN == 0 */
}

#ifdef MULTI_SYNC
void
multi_sync(Video *video[], int num_videos, Boolean discard)
{
	(void) video;
	(void) num_videos;
	(void) discard;
}
#endif

/****************************
** Cursors, fonts, pixmaps **
****************************/

void
set_cursor(int c)
{
	(void) c;
}

/*
 * Clears a rectangle of text rows and columns in a specified window.
 * Drawing is a no-op in the null backend.
 */
void
clear_text_rc(int w, int x, int y, int width, int height, int font)
{
	(void) w;
	(void) x;
	(void) y;
	(void) width;
	(void) height;
	(void) font;
}

int
make_picture(Picture *pic, char *bitmap)
{
	(void) bitmap;
	if (vid->num_pixids >= MAX_PIXMAPS)
		return (1);
	pic->pixmap = vid->num_pixids++;
	vid->width[pic->pixmap] = pic->width;
	vid->height[pic->pixmap] = pic->height;
	vid->pixid[pic->pixmap] = pic->pixmap;
	return (0);
}

void
free_picture(Picture *pic)
{
	(void) pic;
}

/* Lookup table to reverse the bits in a byte */
unsigned char reverse_byte[256] =
{
	0x00, 0x80, 0x40, 0xc0, 0x20, 0xa0, 0x60, 0xe0, 0x10, 0x90, 0x50, 0xd0, 0x30, 0xb0, 0x70,
	0xf0, 0x08, 0x88, 0x48, 0xc8, 0x28, 0xa8, 0x68, 0xe8, 0x18, 0x98, 0x58, 0xd8, 0x38, 0xb8,
	0x78, 0xf8, 0x04, 0x84, 0x44, 0xc4, 0x24, 0xa4, 0x64, 0xe4, 0x14, 0x94, 0x54, 0xd4, 0x34,
	0xb4, 0x74, 0xf4, 0x0c, 0x8c, 0x4c, 0xcc, 0x2c, 0xac, 0x6c, 0xec, 0x1c, 0x9c, 0x5c, 0xdc,
	0x3c, 0xbc, 0x7c, 0xfc, 0x02, 0x82, 0x42, 0xc2, 0x22, 0xa2, 0x62, 0xe2, 0x12, 0x92, 0x52,
	0xd2, 0x32, 0xb2, 0x72, 0xf2, 0x0a, 0x8a, 0x4a, 0xca, 0x2a, 0xaa, 0x6a, 0xea, 0x1a, 0x9a,
	0x5a, 0xda, 0x3a, 0xba, 0x7a, 0xfa, 0x06, 0x86, 0x46, 0xc6, 0x26, 0xa6, 0x66, 0xe6, 0x16,
	0x96, 0x56, 0xd6, 0x36, 0xb6, 0x76, 0xf6, 0x0e, 0x8e, 0x4e, 0xce, 0x2e, 0xae, 0x6e, 0xee,
	0x1e, 0x9e, 0x5e, 0xde, 0x3e, 0xbe, 0x7e, 0xfe, 0x01, 0x81, 0x41, 0xc1, 0x21, 0xa1, 0x61,
	0xe1, 0x11, 0x91, 0x51, 0xd1, 0x31, 0xb1, 0x71, 0xf1, 0x09, 0x89, 0x49, 0xc9, 0x29, 0xa9,
	0x69, 0xe9, 0x19, 0x99, 0x59, 0xd9, 0x39, 0xb9, 0x79, 0xf9, 0x05, 0x85, 0x45, 0xc5, 0x25,
	0xa5, 0x65, 0xe5, 0x15, 0x95, 0x55, 0xd5, 0x35, 0xb5, 0x75, 0xf5, 0x0d, 0x8d, 0x4d, 0xcd,
	0x2d, 0xad, 0x6d, 0xed, 0x1d, 0x9d, 0x5d, 0xdd, 0x3d, 0xbd, 0x7d, 0xfd, 0x03, 0x83, 0x43,
	0xc3, 0x23, 0xa3, 0x63, 0xe3, 0x13, 0x93, 0x53, 0xd3, 0x33, 0xb3, 0x73, 0xf3, 0x0b, 0x8b,
	0x4b, 0xcb, 0x2b, 0xab, 0x6b, 0xeb, 0x1b, 0x9b, 0x5b, 0xdb, 0x3b, 0xbb, 0x7b, 0xfb, 0x07,
	0x87, 0x47, 0xc7, 0x27, 0xa7, 0x67, 0xe7, 0x17, 0x97, 0x57, 0xd7, 0x37, 0xb7, 0x77, 0xf7,
	0x0f, 0x8f, 0x4f, 0xcf, 0x2f, 0xaf, 0x6f, 0xef, 0x1f, 0x9f, 0x5f, 0xdf, 0x3f, 0xbf, 0x7f,
	0xff
};

/*
 * Puts a copy of pic rotated 90 degrees into rot_pic.
 */
Byte *
rotate_pic_90(Picture *pic, Picture *rot_pic, Byte *bitmap)
{
	Byte *rot_bitmap;
	int by_line, rot_by_line, num_bytes, rot_num_bytes;
	register Byte *s_ptr, *d_ptr, *s_ptr_begin, *d_ptr_begin;
	register Byte d_mask;
	register int s_bit_begin, d_bit;
	int itemp1, itemp2;

	rot_pic->width = pic->height;
	rot_pic->height = pic->width;
	rot_pic->offset_x = pic->height - pic->offset_y - 1;
	rot_pic->offset_y = pic->offset_x;

	by_line = (pic->width + 7) >> 3;
	num_bytes = by_line * pic->height;
	rot_by_line = (rot_pic->width + 7) >> 3;
	rot_num_bytes = rot_by_line * rot_pic->height;
	rot_bitmap = (Byte *) calloc((unsigned) (rot_num_bytes + 4), sizeof(Byte));

	s_ptr_begin = bitmap + num_bytes - 1;
	s_bit_begin = (pic->width - 1) & 0x7;
	d_ptr_begin = rot_bitmap + rot_num_bytes - rot_by_line;
	d_bit = 0;
	s_ptr = s_ptr_begin;
	do {
		d_ptr = d_ptr_begin;
		d_mask = 1 << d_bit;
		s_ptr_begin -= by_line;

		itemp1 = -99;
		itemp2 = s_bit_begin;
		do {
			if (itemp1 != -99)
				d_ptr -= rot_by_line;
			for (itemp1 = itemp2; itemp1 >= 0; itemp1--) {
				if (*s_ptr & (1 << itemp1))
					*d_ptr |= d_mask;
				if (itemp1)
					d_ptr -= rot_by_line;
				else
					itemp2 = 7;
			}
		}
		while (--s_ptr > s_ptr_begin);

		if (++d_bit & 1 << 3) {
			d_bit = 0;
			d_ptr_begin++;
		}
	} while (s_ptr >= bitmap);

	return rot_bitmap;
}

/*
 * Puts a copy of pic rotated 180 degrees into rot_pic.
 */
Byte *
rotate_pic_180(Picture *pic, Picture *rot_pic, Byte *bitmap)
{
	Byte *rot_bitmap;
	int num_bytes;
	register Byte *d_ptr;
	register Byte rev, dest;
	register int rshift, lshift;

	rot_pic->width = pic->width;
	rot_pic->height = pic->height;
	rot_pic->offset_x = pic->width - pic->offset_x - 1;
	rot_pic->offset_y = pic->height - pic->offset_y - 1;

	num_bytes = ((pic->width + 7) >> 3) * pic->height;
	rot_bitmap = (Byte *) malloc((unsigned) (num_bytes * sizeof(Byte)));

	lshift = pic->width & 0x7;
	rshift = 8 - lshift;
	d_ptr = rot_bitmap;

	if (lshift == 0) {
		while (--num_bytes >= 0)
			*d_ptr++ = reverse_byte[bitmap[num_bytes]];
	} else {
		dest = reverse_byte[bitmap[--num_bytes]] >> rshift;
		while (--num_bytes >= 0) {
			rev = reverse_byte[bitmap[num_bytes]];
			dest |= rev << lshift;
			*d_ptr++ = dest;
			dest = rev >> rshift;
		}
		*d_ptr = dest;
	}

	return rot_bitmap;
}

/*******************************
** Backend operations vtable  **
*******************************/
static void
null_draw_point(int w, int x, int y, int func, int color)
{
	(void) w; (void) x; (void) y; (void) func; (void) color;
}

static void
null_draw_line(int w, int x1, int y1, int x2, int y2, int func, int color)
{
	(void) w; (void) x1; (void) y1; (void) x2; (void) y2;
	(void) func; (void) color;
}

static void
null_draw_vert(int w, int x, int y, int size, int func, int color)
{
	(void) w; (void) x; (void) y; (void) size; (void) func; (void) color;
}

static void
null_draw_hor(int w, int x, int y, int size, int func, int color)
{
	(void) w; (void) x; (void) y; (void) size; (void) func; (void) color;
}

static void
null_draw_rect(int w, int x, int y, int width, int height, int func, int color)
{
	(void) w; (void) x; (void) y; (void) width; (void) height;
	(void) func; (void) color;
}

static void
null_draw_filled_rect(int w, int x, int y, int width, int height, int func, int color)
{
	(void) w; (void) x; (void) y; (void) width; (void) height;
	(void) func; (void) color;
}

static void
null_draw_square(int w, int x, int y, int size, int func, int color)
{
	(void) w; (void) x; (void) y; (void) size; (void) func; (void) color;
}

static void
null_draw_filled_square(int w, int x, int y, int size, int func, int color)
{
	(void) w; (void) x; (void) y; (void) size; (void) func; (void) color;
}

static void
null_draw_picture(int w, int x, int y, Picture *pic, int func, int color)
{
	(void) w; (void) x; (void) y; (void) pic; (void) func; (void) color;
}

static void
null_draw_arc(int w, int x, int y, int width, int height,
			  int angle1, int angle2, int func, int color)
{
	(void) w; (void) x; (void) y; (void) width; (void) height;
	(void) angle1; (void) angle2; (void) func; (void) color;
}

static void
null_draw_text(int w, int x, int y, char *str, int font, int func, int color)
{
	(void) w; (void) x; (void) y; (void) str; (void) font;
	(void) func; (void) color;
}

static void
null_draw_text_left(int w, int x, int y, char *str, int font, int func, int color)
{
	(void) w; (void) x; (void) y; (void) str; (void) font;
	(void) func; (void) color;
}

static void
null_draw_text_rc(int w, int x, int y, char *str, int font, int color)
{
	(void) w; (void) x; (void) y; (void) str; (void) font; (void) color;
}

static void
null_beep_window(void)
{
}

static void
null_flush_output(void)
{
}

static void
null_sync_output(Boolean discard)
{
	(void) discard;
}

static int
null_win_flags(int w)
{
	return (vid->win[w].flags);
}

static int
null_expose_win(int w, int status)
{
	if (status)
		vid->win[w].flags |= WIN_exposed;
	else
		vid->win[w].flags &= ~WIN_exposed;
	return (vid->win[w].flags);
}

static int
null_mapped(int w)
{
	return (vid->win[w].flags & WIN_mapped);
}

static int
null_color_display(void)
{
	return (vid->planes > 1);
}

static int
null_font_height(int font)
{
	return (fheight[font]);
}

static int
null_font_string_width(char *str, int font)
{
	return (fwidth[font] * strlen(str));
}

static GraphicsBackend null_backend = {
	"null",
	null_draw_point,
	null_draw_line,
	null_draw_vert,
	null_draw_hor,
	null_draw_rect,
	null_draw_filled_rect,
	null_draw_square,
	null_draw_filled_square,
	null_draw_picture,
	null_draw_arc,
	null_draw_text,
	null_draw_text_left,
	null_draw_text_rc,
	null_beep_window,
	null_flush_output,
	null_sync_output,
	null_win_flags,
	null_expose_win,
	null_mapped,
	null_color_display,
	null_font_height,
	null_font_string_width
};

GraphicsBackend *graphics_backend = &null_backend;
