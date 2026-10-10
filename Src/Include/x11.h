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

/*
** Comment: X11 backend-private declarations.
**
** This header is the only place in the tree (besides the X11 translation
** units themselves) that pulls in <X11/...>.  It must only be included by
** backend-private code; simulation and bot translation units go through the
** neutral contract in graphics_backend.h.
*/

#ifndef INCLUDED_X11_H
#define INCLUDED_X11_H

#include <assert.h>
#include <X11/Xlib.h>

/* Neutral limits/types this private header depends on come from graphics.h. */
#ifndef _GRAPHICS_H_
#error "x11.h is backend-private; include graphics.h first"
#endif

#ifdef lint
/* stupid X11/Xlib.h leaves these undefined, which makes lint complain: */
  struct _XrmHashBucketRec {
	  int x;
  };
  struct XKeytrans {
	  int x;
  };
#endif

/* XMultiSync.c */
int XMultiSync(Display *dpys[], int num_dpys, int discard);

/**********************
** Batching machinery **
**********************/
#ifdef BATCH_POINTS

#define BATCHPDEPTH 50

extern XPoint pointBatch[MAX_COLORS][BATCHPDEPTH];
extern int pointsBatched[];
extern int pointBatchFunc;
extern int pointBatchWin;

#endif

#ifdef USE_BATCHED_POINTS
#define flush_point_batch \
   { \
      int col; \
      for (col = 0; col < MAX_COLORS; ++col) { \
         if (pointsBatched[col]) { \
            XDrawPoints(vid->dpy, \
                 vid->win[pointBatchWin].id, \
                 vid->graph_gc[pointBatchFunc][col], \
                 &pointBatch[col][0], \
                 pointsBatched[col], \
                 CoordModeOrigin); \
            pointsBatched[col] = 0; \
         } \
      } \
   }
#else
#define flush_point_batch
#endif

#ifdef BATCH_LINES

#define BATCHDEPTH 80

#ifdef BATCH_COLOR_LINES
extern XSegment lineBatch[MAX_COLORS][BATCHDEPTH];
extern int linesBatched[MAX_COLORS];

#else /* BATCH_COLOR_LINES */
extern XSegment lineBatch[];
extern int linesBatched;
extern int lineBatchColor;

#endif /* BATCH_COLOR_LINES */
extern int lineBatchFunc;
extern int lineBatchWin;

#endif

#ifdef USE_BATCHED_LINES
#ifdef BATCH_COLOR_LINES
#define flush_line_batch \
   { \
      int col; \
      for (col = 0; col < MAX_COLORS; ++col) { \
         if (linesBatched[col]) { \
            XDrawSegments(vid->dpy, \
                 vid->win[lineBatchWin].id, \
                 vid->graph_gc[lineBatchFunc][col], \
                 lineBatch[col], \
                 linesBatched[col]); \
      linesBatched[col] = 0; \
         } \
      } \
   }
#else /* BATCH_COLOR_LINES */
#define flush_line_batch \
   if (linesBatched) { \
      XDrawSegments(vid->dpy, \
                 vid->win[lineBatchWin].id, \
                 vid->graph_gc[lineBatchFunc][lineBatchColor], \
                 lineBatch, \
                 linesBatched); \
      linesBatched = 0; \
  }
#endif /* BATCH_COLOR_LINES */
#else
#define flush_line_batch
#endif

  typedef struct {
	  Window id;
	  int width, height;
	  int flags;
  }
Win;

  struct Kludge_Defaults {
	  char player_name[MAX_STRING + 1];
	  char tank_name[MAX_STRING + 1];
	  int wants_keypad;
	  int mouse_speed;
	  int mouse_drive;
	  int mouse_heat;
	  char keymap[MAX_STRING +1];
  };

  struct video {
	  char display_name[50];
	  Display *dpy;
	  int planes;
	  long input_mask;
	  Window parent_id;
	  int num_windows;
	  Win win[MAX_WINDOWS];
	  GC graph_gc[MAX_DRAW_FUNCS][MAX_COLORS];
	  GC text_gc[MAX_FONTS][MAX_DRAW_FUNCS][MAX_COLORS];
	  XFontStruct *fs[MAX_FONTS];
	  int num_pixids;
	  Pixmap pixid[MAX_PIXMAPS];
	  int width[MAX_PIXMAPS], height[MAX_PIXMAPS];
	  unsigned long fg, bg, color[MAX_COLORS];
	  Cursor cursor[MAX_CURSORS];
	  int escher_width;
	  int last_expose_frame;
	  unsigned int beep_flag:1;
	  unsigned int display_names_flag:1;
	  struct Kludge_Defaults kludge;
	  int dest_wall;
	  int rdf_green;
  };

extern Video *vid;

#endif
