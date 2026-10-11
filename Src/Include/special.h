/*-
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

#ifndef _SPECIAL_H_
#define _SPECIAL_H_

#include "tanktypes.h"
#include "xtanklib.h"
#include "xtank.h"

typedef struct {
	char string[MAX_STRING];
	int value;
	int color;
} Entry;

#ifdef OLDCONSOLE
typedef struct {
	Entry _entry[MAX_ENTRIES];
	int num_changes;
	int change[MAX_ENTRIES];	/* indices of entries that changed */
} Console;

#else
typedef enum {
	CINT,
	CFLOAT,
	CNONE,
	CSTRING_CONST,
	CSTRING_ARRAY,
	CVBAR,
	CHBAR,
	CHFBAR
} cstype;

typedef struct cstat {
	cstype type;
	void (*action) ();		/* NYI */

	/* Non-string */
	int red, white, green;
	/* If white == 0, white will allways be rendered */
	/* Either red < white < green, or green < white < red */

	int old_color;

	/* Values (for all types) */
	FLOAT *fval;
	int *ival;
	FLOAT oldf;
	int oldi;

	/* For everything */
	int changed;			/* Set this to force re-draw */
	char *str_const;

	/* For the string types only */
	char **str_array;
	int *color_array;			/* If NULL use RWG */

	/* Positions (filled out by con_init), for everyone */
	int x, y;

	/* Size of bars (spec'ed in chars, gets changed to pixels in con_init) */
	int w, h;
	int min, max;
} cstat;

typedef struct {
	cstat item[MAX_ENTRIES];
	int num;
	int changed;
} crecord, Console;

#endif

typedef struct {
	Boolean need_redisplay;
	Boolean initial_update;
	Boolean map_invalid;
	Coord marker;				/* shows vehicle location */
	Coord old_marker;
	Map map;
	/* new symbols to display: */
	int num_symbols;
	Landmark_info symbol[SQR(2 * NUM_BOXES + 1)];
	/* list of some known landmarks: */
	int num_landmarks;
	Landmark_info landmark[MAX_LANDMARKS];
} Mapper;

typedef struct {
	Coord draw_loc;
	Coord drawn_loc;
	Coord draw_grid;
	Coord drawn_grid;
	Boolean draw_radar;
	Boolean drawn_radar;
	Boolean draw_friend;
	Boolean drawn_friend;
	Boolean draw_tactical;
	Boolean drawn_tactical;
} newBlip;

typedef struct {
	int frame_updated;
	Boolean need_redisplay;
	newBlip blip[MAX_VEHICLES];
	newBlip *map[GRID_WIDTH][GRID_HEIGHT];
} newRadar;

typedef struct {
	Byte x;
	Byte y;
	Byte life;
	Byte view;
	Byte old_view;
	Byte flags;
} Blip;

typedef struct {
	int num_blips;
	Blip blip[MAX_BLIPS];
	int pos;			/* current rotation of sweep */
	int start_x, start_y;		/* starting coordinate of sweep line */
	int end_x, end_y;		/* ending coordinate of sweep line */
	int old_start_x, old_start_y;
	int old_end_x, old_end_y;
} Radar;

typedef struct {
	SpecialStatus status;		/* status of the special */
	SpecialStatus (*proc) ();	/* function to call for special */
	void *record;			/* pointer to special structure */
	int damage_flag;
} Special;

#ifndef NO_HUD

  typedef struct {
	  int old_xspeed, old_yspeed;
	  int frame_updated;
	  int saved_status[MAX_WEAPONS];
	  Boolean need_redisplay_weap;
	  Boolean need_redisplay_arm;

	  Boolean armor_draw[MAX_SIDES];
	  Boolean armor_drawn[MAX_SIDES];

	  Angle drawn_arm_angle;
	  Angle draw_arm_angle;

		struct {
			int x1, y1, x2, y2;
			int color;
			int weapon;
		}
	  draw[NUM_MOUNTS], drawn[NUM_MOUNTS];

		struct {
			Angle old_angle;
		}
	  turret[MAX_TURRETS];

  }
Hud;

#endif /* !NO_HUD */

#ifndef NO_CAMO

#define STEALTH_DELAY 96

/*
 * If PERSIST is 24, ie, the update rate for
 * both new and old radar, RDF lines won't
 * blink as they are updated, the will just be
 * replaced with the new data.
 *
 * 23 makes them blink off for a frame just
 * before the update comes in
 *
 * Set to 24 so 'bots don't miss it.
 */
#define PERSIST 24

#define MAP_OFF (MAP_BOX_SIZE / 2)

typedef struct {
	int camo_countdown;	/* how many frames till we are invisible */
} Camo;

  typedef struct {
		struct LINEPAIR {
			int start_x, start_y;
			int end_x, end_y;
			int color;
		}
	  draw, drawn;
	  Boolean to_draw;
	  Boolean is_drawn;
  }
Trace;

  typedef struct {
	  Boolean zapped;
	  Trace trace[MAX_VEHICLES][MAX_VEHICLES];
  }
Rdf;


  typedef struct {
	  int activate_frame;		/* frame we can be stealty */
  }
Stealth;

typedef struct {
	int x, y;
	int grid_x, grid_y;
	int color;
} TacCoord;

#define HARM_TRACKING_SLOTS 2
  typedef struct {
	  char *harm[HARM_TRACKING_SLOTS];	/* Should be Bullet */
	  int frame_updated;
	  TacCoord draw_harm[HARM_TRACKING_SLOTS], drawn_harm[HARM_TRACKING_SLOTS];
  }
Taclink;


#endif /* !NO_CAMO */

#define TAC_UPDATE_INTERVAL 12
#define RAD_UPDATE_INTERVAL 24

/*
** P1.11 -- knowledge versus draw.
**
** A special proc is driven by one of seven actions.  They divide into two
** classes with different requirements:
**
**   model transitions (SP_update, SP_activate, SP_deactivate) advance the
**   simulation's knowledge -- mapper exploration, radar contacts, camouflage
**   and stealth surface, HUD refresh state -- and MUST run even when no
**   terminal is presenting.  They never call a draw primitive.
**
**   draw actions (SP_redisplay, SP_draw, SP_erase) only paint an existing
**   terminal.  They are delivered only while presentation is active; with
**   presentation disabled the knowledge is kept and every draw is elided.
**
** SP_ACTION_IS_DRAW/SP_ACTION_IS_MODEL classify an action without knowing
** which special it belongs to.  do_special() is the single dispatch point
** and enforces the separation.
*/
#define SP_ACTION_IS_DRAW(a) \
	((a) == SP_redisplay || (a) == SP_draw || (a) == SP_erase)
#define SP_ACTION_IS_MODEL(a) \
	((a) == SP_update || (a) == SP_activate || (a) == SP_deactivate)

/*
** Read-only instrumentation for the zero-presentation test.  None of these
** values influence behaviour; they only record what the dispatch point did.
*/
typedef struct {
	unsigned long model_invocations;	/* model actions delivered */
	unsigned long draw_invocations;		/* draw actions delivered */
	unsigned long draw_suppressed;		/* draw actions elided (no presentation) */
	unsigned long mapper_updates;		/* mapper model scans applied */
	unsigned long radar_acquired;		/* radar blips created */
	unsigned long radar_expired;		/* radar blips removed on life expiry */
} SpecialStats;
extern SpecialStats special_stats;

/*
** Presentation gate (P1.11).  Defaults to enabled so the X front-end is
** unchanged; the display-free server clears it (server_set_presentation(0))
** and the P1.11 test can clear it with XTANK_NO_PRESENTATION.  A draw action
** is only delivered to a special proc while this is true.
*/
int special_presentation_active(void);
void server_set_presentation(int enabled);

/*
** Release/MVP special preset (PHASE0-BRIEF section 3): permit Console,
** Mapper, HUD and ordinary Radar, and disable New Radar, Tac Link, Camo and
** Rdf.  Oracle controls never call this, so they retain every special.
** Writes INT_FORCE_* values into the caller's force_states[] array.
*/
void special_apply_mvp_preset(char *force_states);

/* Emit the SpecialStats line; a no-op unless XTANK_SPECIALS_REPORT is set. */
void specials_report(void);

#endif /* ndef _SPECIAL_H_ */
