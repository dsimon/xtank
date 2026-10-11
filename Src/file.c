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

#include "xtank.h"

#ifdef UNIX
#include <sys/param.h>
#ifdef NEED_DIRENT_H
#include <dirent.h>
#endif
#ifdef NEED_SYS_DIR_H
#include <sys/dir.h>
#endif

#if defined(linux)
#define MAXNAMLEN NAME_MAX
#endif
#if !defined(MAXNAMLEN) && defined(__DARWIN_MAXPATHLEN)
#define MAXNAMLEN __DARWIN_MAXPATHLEN
#endif
#endif /* UNIX */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#include <sys/stat.h>	/* for chmod() */

#include "tanklimits.h"
#include <assert.h>
#include "graphics.h"
#include "gr.h"
#include "vdesc.h"
#include "setup.h"
#include "bullet.h"
#include "vehicle.h"
#include "terminal.h"
#include "globals.h"
#include "vstructs.h"
#include "server.h"
#include "proto.h"

char pathname[MAXPATHLEN];
char headersdir[MAXPATHLEN];	/* full name of directory to find headers in */
char vehiclesdir[MAXPATHLEN], mazesdir[MAXPATHLEN], programsdir[MAXPATHLEN];
char username[MAX_STRING], displayname[256];

#ifdef NEED_AUX_FONT
char fontdir[MAXNAMLEN];

#endif

extern char *games_entries[];
extern Settings settings;

extern int num_prog_descs;
extern Prog_desc *prog_desc[];

extern char **vehicles_entries, **mazes_entries, **setups_entries;

Vdesc *vdesc = NULL;
int num_vdescs = 0;

Mdesc *mdesc = NULL;
int num_mdescs = 0;

Sdesc *sdesc = NULL;
int num_sdescs = 0;

static int external_type[] =
{
	NORMAL,
	FUEL,
	AMMO,
	ARMOR,
	GOAL,
	OUTPOST,
	SCROLL_N,
	SCROLL_NE,
	SCROLL_E,
	SCROLL_SE,
	SCROLL_S,
	SCROLL_SW,
	SCROLL_W,
	SCROLL_NW,
	SLIP,
	SLOW,
	START_POS,
	NORTH_SYM,
	WEST_SYM,
	NORTH_DEST_SYM,
	WEST_DEST_SYM,
	PEACE,
	TELEPORT,
	-1
};

#define TO_INTERNAL_TYPE       0
#define TO_EXTERNAL_TYPE       1

void
convert_maze(Mdesc *d, int convtype)
{
	Byte flags, *dptr;
	int ctr;

	/* For each box there is a byte that contains 8 flags.  If EMPTY_BOXES is
       set, the remaining 7 bits give the number of empty boxes (excluding this
       one) to make before reading the next byte.  Otherwise, If TYPE_EXISTS is
       set, the next byte is the box type.  Otherwise, the type is 0. If
       TEAM_EXISTS is set, the next byte is the box team.  Otherwise, the team
       is 0. */
	dptr = d->data;
	while (*dptr) {
		flags = *(dptr++);

		/* Check for empty box flag */
		if (!(flags & EMPTY_BOXES)) {
			if (flags & TYPE_EXISTS) {
				if (convtype == TO_INTERNAL_TYPE)
					*dptr = external_type[(LandmarkType) *dptr];
				else {
					ctr = 0;
					while (external_type[ctr] != -1) {
						if (external_type[ctr] == (int) *dptr) {
							*dptr = ctr;
							break;
						}
						ctr++;
					}
					dptr++;
				}

				if (flags & TEAM_EXISTS) {
					dptr++;
				}
			}
		}
	}
}

/*
** Makes all vehicles listed in [vehiclesdir]/list and all mazes listed in
** [mazesdir]/list.
*/
void
load_desc_lists(void)
{
	FILE *file;
	char filename[MAXPATHLEN];
	char name[MAXNAMLEN];
	int num;

	/* Make all the vehicle descriptions in the vehicle list */

#ifdef UNIX
	(void) strcpy(filename, pathname);
	(void) strcat(filename, "/");
	(void) strcat(filename, vehiclesdir);
	(void) strcat(filename, "/");
	(void) strcat(filename, "list");
#endif /* UNIX */

#ifdef AMIGA
	(void) strcpy(filename, "XVDIR:list");
#endif /* AMIGA */

#ifndef NULL_BACKEND
	/* The display-free server has no progress window (P1.14). */
	draw_text_rc(ANIM_WIN, 0, 2, "Reading vehicle list...", M_FONT, WHITE);
	sync_output(TRUE);
#endif
	if ((file = fopen(filename, "r")) != NULL) {
		while (fscanf(file, "%s", name) != EOF)
			make_vdesc(name, &num);

		(void) fclose(file);
	} else {
		fprintf(stderr, "Could not open file '%s'.\n", filename);
		exit(1);
	}


	/* Make all the maze descriptions in the maze list */

#ifdef UNIX
	(void) strcpy(filename, pathname);
	(void) strcat(filename, "/");
	(void) strcat(filename, mazesdir);
	(void) strcat(filename, "/");
	(void) strcat(filename, "list");
#endif /* UNIX */

#ifdef AMIGA
	(void) strcpy(filename, "XMDIR:list");
#endif /* AMIGA */

#ifndef NULL_BACKEND
	/* The display-free server has no progress window (P1.14). */
	draw_text_rc(ANIM_WIN, 0, 3, "Reading maze list...", M_FONT, WHITE);
	sync_output(TRUE);
#endif
	if ((file = fopen(filename, "r")) != NULL) {
		while (fscanf(file, "%s", name) != EOF)
			make_mdesc(name, &num);

		(void) fclose(file);
	} else {
		fprintf(stderr, "Could not open file '%s'.\n", filename);
		exit(1);
	}
}

/*
** Loads the vdesc from the appropriate file into the next empty slot.
** Puts the number of the vehicle into num, if loaded.
** Returns one of DESC_LOADED, DESC_NOT_FOUND, DESC_BAD_FORMAT, DESC_NO_ROOM.
*/
int
make_vdesc(char *name, int *num)
{
	Vdesc *d;
	int retval, i;
        Vdesc tmpveh;

	/* if (num_vdescs >= MAX_VDESCS) return DESC_NO_ROOM; */

	/* if the vehicle could not be found, we will be leaving room for an
       extra Vdesc.  This should probably be cleaned up later. */
        /* It is fixed, I cloned the KNG fix from make_mdesc up here. -ane */

	/* If name present, load into that slot, else load into next empty slot */
	for (i = 0; i < num_vdescs; i++) {
		if (!strcmp(name, vdesc[i].name)) {
			break;
		}
	}

      /* Load the vehicle before resizing the array; if it's not found, return
       * (don't resize the array).  Fix cloned from KNG fix in make_mdesc.  -ane
       */
        retval = load_vdesc(&tmpveh, name);
        if (retval != DESC_LOADED) {
           return(retval);
        }


	if (i == num_vdescs) {
		if (vdesc == NULL) {
			vdesc = (Vdesc *) malloc(sizeof(Vdesc));
			vehicles_entries = (char **) malloc(sizeof(char *));
		} else {
			vdesc = (Vdesc *) realloc((char *) vdesc,
							   (unsigned) (num_vdescs + 1) * sizeof(Vdesc));

			vehicles_entries = (char **) realloc((char *) vehicles_entries,
											   (unsigned) (num_vdescs + 1) *
												 sizeof(char *));
		}

		assert(vdesc != NULL);
		assert(vehicles_entries != NULL);
	}
	d = &vdesc[i];
	memcpy(d, &tmpveh, sizeof(Vdesc));

	/* Increment the number if a vdesc was loaded in the last slot */
	if (retval == DESC_LOADED && i == num_vdescs) {
		num_vdescs++;
	}
	*num = i;
	reset_dynamic_entries();
	return retval;
}

#define check_range(val,mx) \
  if((int)(val) < 0 || (int)(val) >= (int)(mx)) return DESC_BAD_FORMAT;

int
ReadVehicleFormat0(FILE *file, Vdesc *d)
{
	int i;
	int num_tur;
	int iRetCode = DESC_LOADED;
	WeaponType weapon;
	MountLocation mount;

	/* Initialize max side to 0 */
	d->armor.max_side = 0;

	/* Load the values into the vdesc structure, checking their validity */
	(void) fscanf(file, "%s", d->name);
	(void) fscanf(file, "%s", d->designer);

	(void) fscanf(file, "%d", &d->body);
	check_range(d->body, num_vehicle_objs);
	num_tur = vehicle_obj[d->body]->num_turrets;

	if (num_tur == 4) {
		iRetCode = DESC_BAD_FORMAT;
	} else {
		(void) fscanf(file, "%d", &d->engine);
		check_range(d->engine, MAX_ENGINES);

		(void) fscanf(file, "%d", &d->num_weapons);
		check_range(d->num_weapons, MAX_WEAPONS + 1);

		for (i = 0; i < d->num_weapons; i++) {
			(void) fscanf(file, "%d %d", &weapon, &mount);
			check_range((int) weapon, MAX_WEAPON_STATS);

			if (mount >= MOUNT_TURRET4) {
				++mount;
			}
			switch (mount) {
			  case MOUNT_TURRET1:
				  if (num_tur < 1) {
					  iRetCode = DESC_BAD_FORMAT;
				  }
				  break;
			  case MOUNT_TURRET2:
				  if (num_tur < 2) {
					  iRetCode = DESC_BAD_FORMAT;
				  }
				  break;
			  case MOUNT_TURRET3:
				  if (num_tur < 3) {
					  iRetCode = DESC_BAD_FORMAT;
				  }
				  break;
			  case MOUNT_FRONT:
			  case MOUNT_BACK:
			  case MOUNT_LEFT:
			  case MOUNT_RIGHT:
				  break;
			  default:
				  iRetCode = DESC_BAD_FORMAT;
				  break;
			}

			if (iRetCode) {
				break;
			}
			d->weapon[i] = weapon;
			d->mount[i] = mount;
		}
	}

	if (iRetCode == 0) {
		(void) fscanf(file, "%d", &d->armor.type);
		check_range(d->armor.type, MAX_ARMORS);

		for (i = 0; i < MAX_SIDES; i++) {
			(void) fscanf(file, "%d", &d->armor.side[i]);
			if (d->armor.max_side < d->armor.side[i]) {
				d->armor.max_side = d->armor.side[i];
			}
		}

		(void) fscanf(file, "%d", &d->specials);
		(void) fscanf(file, "%d", &d->heat_sinks);

		(void) fscanf(file, "%d", &d->suspension);
		check_range(d->suspension, MAX_SUSPENSIONS);

		(void) fscanf(file, "%d", &d->treads);
		check_range(d->treads, MAX_TREADS);

		(void) fscanf(file, "%d", &d->bumpers);
		check_range(d->bumpers, MAX_BUMPERS);
	}
	return (iRetCode);
}

/*
** Loads the vehicle description from the file "[vehiclesdir]/[name].v".
** Returns one of DESC_LOADED, DESC_NOT_FOUND, DESC_BAD_FORMAT.
*/
int
load_vdesc(Vdesc *d, char *name)
{
	extern int num_vehicle_objs;
	extern Object *vehicle_obj[];
	FILE *file;
	char filename[MAXPATHLEN];
	int i;
	int iFormatType;
	int iRetCode = DESC_LOADED;

	/* initialize the vdesc struct */
	memset(d, 0, sizeof(Vdesc));

	/* Open the vehicle description file */

#ifdef UNIX
	(void) strcpy(filename, pathname);
	(void) strcat(filename, "/");
	(void) strcat(filename, vehiclesdir);
	(void) strcat(filename, "/");
#endif /* UNIX */

#ifdef AMIGA
	(void) strcpy(filename, "XVDIR:");
#endif /* AMIGA */

	(void) strcat(filename, name);
	(void) strcat(filename, ".V");

	if ((file = fopen(filename, "r")) == NULL) {
		filename[strlen(filename) - 1] = 'v';
		if ((file = fopen(filename, "r")) == NULL) {
			iRetCode = DESC_NOT_FOUND;
		} else {
			iFormatType = 0;
		}
	} else {
		iFormatType = 1;
	}

	if (iRetCode == 0) {
		switch (iFormatType) {
		  case 0:
			  iRetCode = ReadVehicleFormat0(file, d);
			  break;

		  case 1:
			  iRetCode = ReadVehicleFormat1(file, d);
			  break;
		}

		if (iRetCode == 0) {
			/* Compute parameters, and see if there are any problems */
			if (compute_vdesc(d)) {
				iRetCode = DESC_BAD_FORMAT;
			}
		}
	}
	if (file) {
		(void) fclose(file);
	}
	return (iRetCode);
}


/*
** Saves the specified vehicle description.
** Returns one of DESC_NOT_FOUND, DESC_SAVED.
*/
int
save_vdesc(Vdesc *d)
{
	return (SaveVehicleFormat1(d));
}

/*
** Loads the mdesc from the appropriate file into the next empty slot.
** Puts the number of the maze into num if loaded.
** Returns one of DESC_LOADED, DESC_NOT_FOUND, DESC_BAD_FORMAT, DESC_NO_ROOM.
*/
int
make_mdesc(char *name, int *num)
{
	Mdesc *d;
	int retval, i;
        Mdesc tmpmaze;

	/* If name present, load into that slot, else load into next empty slot */
	for (i = 0; i < num_mdescs; i++) {
		if (!strcmp(name, mdesc[i].name)) {
			break;
		}
	}

       /* Load the maze before resizing the array; if it's not found, return
        * (don't resize the array).  This way settings.mdesc is left pointing
        * into valid data (see ask_desc() in interface.c).  -KNG 930916
        */
        retval = load_mdesc(&tmpmaze, name);
        if (retval != DESC_LOADED) {
            return(retval);
        }


	if (i == num_mdescs) {
		/* if the maze could not be found, we will be leaving room for an extra
       Mdesc.  This should probably be cleaned up later. */
                /* It just was, see KNG code above -ane */
		if (mdesc == NULL) {
			mdesc = (Mdesc *) malloc(2 * sizeof(Mdesc));
			mazes_entries = (char **) malloc(2 * sizeof(char *));
		} else {
			mdesc = (Mdesc *) realloc((char *) mdesc,
									  (unsigned) ((num_mdescs + 2) *
												  sizeof(Mdesc)));

			mazes_entries = (char **) realloc((char *) mazes_entries,
											  (unsigned) ((num_mdescs + 2) *
														  sizeof(char *)));
		}

		assert(mdesc != NULL);
		assert(mazes_entries != NULL);
	}
	d = &mdesc[i];
        memcpy(d, &tmpmaze, sizeof(Mdesc));	

	/* Increment the number if a mdesc was loaded in the last slot */
	if (retval == DESC_LOADED && i == num_mdescs) {
		num_mdescs++;
	}
	*num = i;
	reset_dynamic_entries();
	return retval;
}

/*
** Loads the maze description from the file "[mazesdir]/[name].m".
** Returns one of DESC_LOADED, DESC_NOT_FOUND, DESC_BAD_FORMAT.
*/
int
load_mdesc(Mdesc *d, char *name)
{
	FILE *file;
	char filename[MAXPATHLEN];
	int temp;

#ifdef UNIX
	(void) strcpy(filename, pathname);
	(void) strcat(filename, "/");
	(void) strcat(filename, mazesdir);
	(void) strcat(filename, "/");
#endif /* UNIX */

#ifdef AMIGA
	(void) strcpy(filename, "XVDIR:");
#endif /* AMIGA */

	(void) strcat(filename, name);
	(void) strcat(filename, ".m");
	if ((file = fopen(filename, "r")) == NULL)
		return DESC_NOT_FOUND;

	if ((temp = getc(file)) == EOF)
		return DESC_BAD_FORMAT;
	d->type = (Game) temp;
	if (alloc_str(file, &d->name) == DESC_BAD_FORMAT)
		return DESC_BAD_FORMAT;
	if (alloc_str(file, &d->designer) == DESC_BAD_FORMAT)
		return DESC_BAD_FORMAT;
	if (alloc_str(file, &d->desc) == DESC_BAD_FORMAT)
		return DESC_BAD_FORMAT;
	if (alloc_str(file, (char **) &d->data) == DESC_BAD_FORMAT)
		return DESC_BAD_FORMAT;

	convert_maze(d, TO_INTERNAL_TYPE);

	(void) fclose(file);

	return DESC_LOADED;
}

/* At most 3 bytes per box in the grid for a maze description */
#define MAX_DATA_BYTES    GRID_WIDTH * GRID_HEIGHT * 3 + 1

/*
** Reads in a string from the file.  Mallocs the appropriate amount
** of memory for str, and copies the string into str.
** Returns DESC_BAD_FORMAT if string is too long or EOF is reached.
** Otherwise, returns DESC_LOADED.
*/
int
alloc_str(FILE *file, char **strp)
{
	int ret;
	char temp[MAX_DATA_BYTES];
	unsigned int len;

	/* Read chars from file into temp up to and including the first '\0' */
	len = 0;
	do {
		if ((ret = getc(file)) == EOF || len >= MAX_DATA_BYTES)
			return DESC_BAD_FORMAT;
		temp[len++] = (char) ret;
	} while ((char) ret != '\0');

	*strp = (char *)malloc(len);
	assert(*strp != NULL);
	(void) strcpy(*strp, temp);

	return DESC_LOADED;
}

/*
** Saves the specified maze description.
** Returns one of DESC_NOT_FOUND, DESC_SAVED.
*/
int
save_mdesc(Mdesc *d)
{
	FILE *file;
	char filename[MAXPATHLEN];

#ifdef UNIX
	(void) strcpy(filename, pathname);
	(void) strcat(filename, "/");
	(void) strcat(filename, mazesdir);
	(void) strcat(filename, "/");
#endif /* UNIX */

#ifdef AMIGA
	(void) strcpy(filename, "XVDIR:");
#endif /* AMIGA */

	(void) strcat(filename, d->name);
	(void) strcat(filename, ".m");
	if ((file = fopen(filename, "w")) == NULL)
		return DESC_NOT_FOUND;

	convert_maze(d, TO_EXTERNAL_TYPE);

	(void) fputc((char) d->type, file);
	fputs(d->name, file);
	(void) fputc('\0', file);
	fputs(d->designer, file);
	(void) fputc('\0', file);
	fputs(d->desc, file);
	(void) fputc('\0', file);
	fputs((char *) d->data, file);
	(void) fputc('\0', file);

	(void) fclose(file);

#ifdef UNIX
	/* Change protections so others can read in the maze */
	(void) chmod(filename, 0644);
#endif

	return DESC_SAVED;
}

int
load_sdesc(Sdesc *d, char *name)
{
	FILE *file;
	char filename[MAXPATHLEN];

#ifdef UNIX
	(void) strcpy(filename, pathname);
	(void) strcat(filename, "/");
	(void) strcat(filename, mazesdir);
	(void) strcat(filename, "/");
#endif /* UNIX */

#ifdef AMIGA
	(void) strcpy(filename, "XVDIR:");
#endif /* AMIGA */

	(void) strcat(filename, name);
	(void) strcat(filename, ".s");
	if ((file = fopen(filename, "r")) == NULL)
		return DESC_NOT_FOUND;

/*
	if ((temp = getc(file)) == EOF)
		return DESC_BAD_FORMAT;
	d->type = (Game) temp;
	if (alloc_str(file, &d->name) == DESC_BAD_FORMAT)
		return DESC_BAD_FORMAT;
	if (alloc_str(file, &d->designer) == DESC_BAD_FORMAT)
		return DESC_BAD_FORMAT;
	if (alloc_str(file, &d->desc) == DESC_BAD_FORMAT)
		return DESC_BAD_FORMAT;
	if (alloc_str(file, (char **) &d->data) == DESC_BAD_FORMAT)
		return DESC_BAD_FORMAT;

	convert_maze(d, TO_INTERNAL_TYPE);
*/

	(void) fclose(file);

	return DESC_LOADED;
}

/*
** Loads the sdesc from the appropriate file into the next empty slot.
** Puts the number of the setup into num if loaded.
** Returns one of DESC_LOADED, DESC_NOT_FOUND, DESC_BAD_FORMAT, DESC_NO_ROOM.
*/
/*ARGSUSED*/
int
make_sdesc(char *name, int *num)
{

	return (DESC_NOT_FOUND);
}


#ifdef UNIX
/*
** Reads environment variables for the pathname, username,
** vehicles directory, mazes directory, and programs directory.
*/
void
get_environment(void)
{
	extern char *getenv();
	char *p;

	/* Read in variables, providing appropriate defaults if not found */
	strcpy(username, (p = getenv("USER")) ? p :
		   ((p = getenv("LOGNAME")) ? p : "user"));

	/*
	 * P1.14: an explicit asset root (XTANK_ASSET_ROOT) takes precedence over
	 * the legacy XTANK_DIR lookup.  It is resolved exactly once, here; the
	 * content loaders themselves never consult the environment.  When
	 * neither variable is set the historical default is retained so the X
	 * front-end is unchanged.
	 */
	if ((p = getenv("XTANK_ASSET_ROOT")) != NULL) {
		if (content_set_root(p) != 0) {
			fprintf(stderr, "xtank: %s\n", content_last_error());
			exit(2);
		}
	} else {
		strcpy(pathname, (p = getenv("XTANK_DIR")) ? p
			   : "/usr/local/share/XTank");
		strcpy(vehiclesdir, (p = getenv("XTANK_VEHICLES")) ? p : "Vehicles");
		strcpy(mazesdir, (p = getenv("XTANK_MAZES")) ? p : "Mazes");
		strcpy(programsdir, (p = getenv("XTANK_PROGRAMS")) ? p : "Programs");
		if ((p = getenv("XTANK_HEADERS")) != NULL) {
			strcpy(headersdir, p);
		} else {
			strcpy(headersdir, pathname);
			strcat(headersdir, "/Src/Include");
		}
	}
	strcpy(displayname, (p = getenv("DISPLAY")) ? p : "unix:0.0");

#ifdef NEED_AUX_FONT
	strcpy(fontdir, (p = getenv("FONT_DIR")) ? p : "Fonts");
#endif

	/* Test-only content/config self-check; never set in normal runs. */
	if (getenv("XTANK_CONTENT_CHECK") != NULL)
		content_check_run();
}

#endif /* UNIX */

/* ------------------------------------------------------------------ */
/* P1.14 -- explicit asset root and display-free content loading       */
/* ------------------------------------------------------------------ */

static char content_error[MAXPATHLEN + 256];
static char content_root_dir[MAXPATHLEN];

const char *
content_last_error(void)
{
	return content_error;
}

const char *
content_root(void)
{
	return content_root_dir;
}

static int
content_fail(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	(void) vsnprintf(content_error, sizeof(content_error), fmt, ap);
	va_end(ap);
	return -1;
}

/*
** Installs an explicit asset root and derives the Vehicles/Mazes/Programs
** and Src/Include directories beneath it.  Reads no environment variable,
** opens no display and reports a precise reason on failure.
*/
int
content_set_root(const char *root)
{
	struct stat st;

	if (root == NULL || *root == '\0') {
		(void) content_fail("empty asset root");
		return -1;
	}
	if (stat(root, &st) != 0 || !S_ISDIR(st.st_mode)) {
		(void) content_fail("asset root '%s' is not a directory", root);
		return -1;
	}
	if (strlen(root) >= sizeof(pathname)) {
		(void) content_fail("asset root '%s' is too long", root);
		return -1;
	}

	strcpy(pathname, root);
	strcpy(vehiclesdir, "Vehicles");
	strcpy(mazesdir, "Mazes");
	strcpy(programsdir, "Programs");
	(void) snprintf(headersdir, sizeof(headersdir), "%s/Src/Include", root);
	(void) strncpy(content_root_dir, root, sizeof(content_root_dir) - 1);
	content_root_dir[sizeof(content_root_dir) - 1] = '\0';
	content_error[0] = '\0';
	return 0;
}

/*
** Loads a single named maze by explicit path under the installed root.  It
** does not consult the directory list and never substitutes a random maze;
** a missing or malformed file returns a DESC_* code with a reason.
*/
int
content_load_maze(Mdesc *d, const char *name)
{
	int rc;

	if (d == NULL || name == NULL || *name == '\0') {
		(void) content_fail("maze name required");
		return DESC_BAD_FORMAT;
	}
	rc = load_mdesc(d, (char *) name);
	if (rc != DESC_LOADED)
		(void) content_fail("maze '%s' not found under %s/%s (rc=%d)",
							name, pathname, mazesdir, rc);
	return rc;
}

/*
** Loads a single named vehicle by explicit path under the installed root.
*/
int
content_load_vehicle(Vdesc *d, const char *name)
{
	int rc;

	if (d == NULL || name == NULL || *name == '\0') {
		(void) content_fail("vehicle name required");
		return DESC_BAD_FORMAT;
	}
	rc = load_vdesc(d, (char *) name);
	if (rc != DESC_LOADED)
		(void) content_fail("vehicle '%s' not found under %s/%s (rc=%d)",
							name, pathname, vehiclesdir, rc);
	return rc;
}

/*
** Parses a settings file with explicit error reporting.
*/
int
content_load_settings(const char *path)
{
	if (path == NULL || *path == '\0') {
		(void) content_fail("settings path required");
		return -1;
	}
	if (load_settings((char *) path) == 0) {
		(void) content_fail("settings file '%s' could not be loaded", path);
		return -1;
	}
	content_error[0] = '\0';
	return 0;
}

static void
content_print_settings(const char *tag, Settings *s)
{
	printf("%s game_speed=%d\n", tag, s->game_speed);
	printf("%s maze_density=%d\n", tag, s->maze_density);
	printf("%s point_bullets=%d\n", tag, (int) s->point_bullets);
	printf("%s commentator=%d\n", tag, (int) s->commentator);
	printf("%s robots_dont_win=%d\n", tag, (int) s->robots_dont_win);
	printf("%s max_armor_scale=%d\n", tag, (int) s->max_armor_scale);
	printf("%s difficulty=%d\n", tag, s->difficulty);
	printf("%s si_game=%d\n", tag, (int) s->si.game);
	printf("%s si_ricochet=%d\n", tag, (int) s->si.ricochet);
	printf("%s si_rel_shoot=%d\n", tag, (int) s->si.rel_shoot);
	printf("%s si_no_wear=%d\n", tag, (int) s->si.no_wear);
	printf("%s si_restart=%d\n", tag, (int) s->si.restart);
	printf("%s si_full_map=%d\n", tag, (int) s->si.full_map);
	printf("%s si_pay_to_play=%d\n", tag, (int) s->si.pay_to_play);
	printf("%s si_no_nametags=%d\n", tag, (int) s->si.no_nametags);
	printf("%s si_team_score=%d\n", tag, (int) s->si.team_score);
	printf("%s si_player_teleport=%d\n", tag, (int) s->si.player_teleport);
	printf("%s si_disc_teleport=%d\n", tag, (int) s->si.disc_teleport);
	printf("%s si_teleport_from_team=%d\n", tag, (int) s->si.teleport_from_team);
	printf("%s si_teleport_from_neutral=%d\n", tag, (int) s->si.teleport_from_neutral);
	printf("%s si_teleport_to_team=%d\n", tag, (int) s->si.teleport_to_team);
	printf("%s si_teleport_to_neutral=%d\n", tag, (int) s->si.teleport_to_neutral);
	printf("%s si_teleport_any_to_any=%d\n", tag, (int) s->si.teleport_any_to_any);
	printf("%s si_war_goals_only=%d\n", tag, (int) s->si.war_goals_only);
	printf("%s si_relative_disc=%d\n", tag, (int) s->si.relative_disc);
	printf("%s si_ultimate_own_goal=%d\n", tag, (int) s->si.ultimate_own_goal);
	printf("%s si_winning_score=%d\n", tag, s->si.winning_score);
	printf("%s si_takeover_time=%d\n", tag, s->si.takeover_time);
	printf("%s si_outpost_strength=%d\n", tag, s->si.outpost_strength);
	printf("%s si_shocker_walls=%d\n", tag, s->si.shocker_walls);
	printf("%s si_scroll_speed=%.6f\n", tag, s->si.scroll_speed);
	printf("%s si_slip_friction=%.6f\n", tag, s->si.slip_friction);
	printf("%s si_normal_friction=%.6f\n", tag, s->si.normal_friction);
	printf("%s si_disc_friction=%.6f\n", tag, s->si.disc_friction);
	printf("%s si_disc_speed=%.6f\n", tag, s->si.disc_speed);
	printf("%s si_disc_damage=%.6f\n", tag, s->si.disc_damage);
	printf("%s si_disc_heat=%.6f\n", tag, s->si.disc_heat);
	printf("%s si_box_slowdown=%.6f\n", tag, s->si.box_slowdown);
	printf("%s si_owner_slowdown=%.6f\n", tag, s->si.owner_slowdown);
	printf("%s si_num_discs=%d\n", tag, s->si.num_discs);
}

/*
** Test-only content/config self-check (P1.14).  Invoked from
** get_environment() when XTANK_CONTENT_CHECK is set; exits the process.
*/
void
content_check_run(void)
{
	Mdesc m;
	Vdesc v;
	Settings def, rel;
	ServerConfig cfg;
	char force[MAX_SPECIALS];
	int rc, i, fail = 0;

	rc = content_load_maze(&m, "combat1");
	printf("CONTENT-MAZE combat1 rc=%d name=%s type=%d\n", rc,
		   (rc == DESC_LOADED && m.name) ? m.name : "-",
		   (rc == DESC_LOADED) ? (int) m.type : -1);
	if (rc != DESC_LOADED) {
		printf("CONTENT-ERROR %s\n", content_last_error());
		fail = 1;
	}

	rc = content_load_maze(&m, "Arena");
	printf("CONTENT-MAZE Arena rc=%d name=%s type=%d\n", rc,
		   (rc == DESC_LOADED && m.name) ? m.name : "-",
		   (rc == DESC_LOADED) ? (int) m.type : -1);
	if (rc != DESC_LOADED) {
		printf("CONTENT-ERROR %s\n", content_last_error());
		fail = 1;
	}

	rc = content_load_vehicle(&v, "Vanguard");
	printf("CONTENT-VEHICLE Vanguard rc=%d name=%s body=%d weapons=%d specials=0x%x\n",
		   rc, (rc == DESC_LOADED) ? v.name : "-",
		   (rc == DESC_LOADED) ? v.body : -1,
		   (rc == DESC_LOADED) ? v.num_weapons : -1,
		   (rc == DESC_LOADED) ? (unsigned) v.specials : 0);
	if (rc != DESC_LOADED) {
		printf("CONTENT-ERROR %s\n", content_last_error());
		fail = 1;
	}

	/* Release config derived from init_settings() defaults. */
	init_settings();
	def = settings;
	server_release_settings(&settings);
	rel = settings;
	content_print_settings("SETDEF", &def);
	content_print_settings("SETREL", &rel);

	server_config_init(&cfg);
	server_config_release(&cfg);
	printf("CONTENT-CONFIG max_vehicles=%d tick_hz=%d release_max=%d\n",
		   cfg.max_vehicles, cfg.tick_hz, SERVER_RELEASE_MAX_VEHICLES);

	/* Approved special preset. */
	for (i = 0; i < MAX_SPECIALS; i++)
		force[i] = INT_FORCE_DONT;
	special_apply_mvp_preset(force);
	for (i = 0; i < MAX_SPECIALS; i++)
		printf("PRESET %d=%d\n", i, (int) force[i]);

	printf("%s\n", fail ? "CONTENT-FAIL" : "CONTENT-OK");
	(void) fflush(stdout);
	exit(fail ? 1 : 0);
}

#ifdef UNIX
#include <sys/types.h>
#include <sys/stat.h>
#endif

#ifdef AMIGA
#include <dos.h>
#endif

/*
** Reads a file, returning its contents in a null-terminated string.
** Returns an empty string if the file is not found.
*/
char *
read_file(char *filename)
{
	FILE *file;
	char *string, temp[MAXPATHLEN];
	long size;

#ifdef AMIGA
	strcpy(temp, "XVDIR:");
#endif

#ifdef UNIX
	if (filename[0] != '/') {
		strcpy(temp, pathname);
		strcat(temp, "/");
	} else
		temp[0] = '\0';
#endif

	strcat(temp, filename);

	file = fopen(temp, "r");
	if (file == (FILE *) NULL) {
		char *empty;

		empty = (char *)malloc(1);		/* JMO 1/19/90 is free()'ed later */
		*empty = '\0';
		return (empty);
	}
	/* Get file size */

#ifdef AMIGA
	{
		struct FILEINFO finfo;

		if (dfind(&finfo, temp, 0))
			return ("");
		size = finfo.fib_Size;
	}
#endif /* AMIGA */

#ifdef UNIX
	{
		struct stat fileinfo;

		if (fstat(fileno(file), &fileinfo)) {
			char *empty;

			empty = malloc(1);	/* It get's fred'ed JMO - 1/19/90 */
			*empty = '\0';
			return (empty);
		}
		size = fileinfo.st_size;
	}
#endif /* UNIX */

	/* Leave space for the NULL */
	string = (char *)malloc((unsigned) size + 1);

	/* Copy the file into memory and close the file */
	(void) fread(string, sizeof(char), (int) size, file);

	(void) fclose(file);

	/* Put NULL at end of string */
	string[size] = '\0';

	return (string);
}

int
save_settings(char *pcFileName)
{
	int ctri, ctrj;
	int iProg;
	char *dispname;
	char *ProgWritten;
	FILE *outfile, *fopen();
	Video *vidptr;
	Vehicle *vptr;
	Prog_desc *pdesc;
	int i;
	extern char *teams_entries[];
	extern Terminal *terminal[];
	extern int num_terminals;
	extern char force_states[MAX_SPECIALS];

	if (!*pcFileName) {
		return 1;
	}
	outfile = fopen(pcFileName, "w");
	assert(outfile);

	fprintf(outfile, "xtank: 114336\n");
	/* Settings */
	/* Maze */
	fprintf(outfile, "Maze: %s\n", (settings.mdesc) ? settings.mdesc->name
			: "Random");

	/* Game */
	fprintf(outfile, "Game: %s\n", games_entries[settings.si.game]);

	/* FLAGS */
	fprintf(outfile, "Point Bullets: %d\n", settings.point_bullets);
	fprintf(outfile, "Ricochet: %d\n", settings.si.ricochet);
	fprintf(outfile, "Rel Fire: %d\n", settings.si.rel_shoot);
	fprintf(outfile, "No Wear: %d\n", settings.si.no_wear);
	fprintf(outfile, "Restart: %d\n", settings.si.restart);
	fprintf(outfile, "Commentator: %d\n", settings.commentator);
	fprintf(outfile, "Full Map: %d\n", settings.si.full_map);
	fprintf(outfile, "Pay To Play: %d\n", settings.si.pay_to_play);
	fprintf(outfile, "Robots Don't Win: %d\n", settings.robots_dont_win);
	fprintf(outfile, "Max Armor Scale: %d\n", settings.max_armor_scale);
	fprintf(outfile, "Nametags: %d\n", settings.si.no_nametags);
	fprintf(outfile, "Team Score: %d\n", settings.si.team_score);
	fprintf(outfile, "Player Teleport: %d\n", settings.si.player_teleport);
	fprintf(outfile, "Disc Teleport: %d\n", settings.si.disc_teleport);
	fprintf(outfile, "Teleport From Team: %d\n", settings.si.teleport_from_team);
	fprintf(outfile, "Teleport From Neutral: %d\n", settings.si.teleport_from_neutral);
	fprintf(outfile, "Teleport To Team: %d\n", settings.si.teleport_to_team);
	fprintf(outfile, "Teleport To Neutral: %d\n", settings.si.teleport_to_neutral);
	fprintf(outfile, "Teleport Any To Any: %d\n", settings.si.teleport_any_to_any);
	fprintf(outfile, "War Goals Only: %d\n", settings.si.war_goals_only);
	fprintf(outfile, "Relative Disc: %d\n", settings.si.relative_disc);
	fprintf(outfile, "Ultimate Own Goal: %d\n", settings.si.ultimate_own_goal);

	/* other settings */
	fprintf(outfile, "Winning Score: %d\n", settings.si.winning_score);
	fprintf(outfile, "Outpost Strength: %d\n", settings.si.outpost_strength);
	fprintf(outfile, "Scroll Speed: %f\n", settings.si.scroll_speed);
	fprintf(outfile, "Box slowdown: %f\n", settings.si.box_slowdown);
	fprintf(outfile, "Disc Friction: %f\n", settings.si.disc_friction);
	fprintf(outfile, "Disc Speed: %f\n", settings.si.disc_speed);
	fprintf(outfile, "Disc Damage: %f\n", settings.si.disc_damage);
	fprintf(outfile, "Disc Heat: %f\n", settings.si.disc_heat);
	fprintf(outfile, "Owner Slowdown: %f\n", settings.si.owner_slowdown);
	fprintf(outfile, "Slip Friction: %f\n", settings.si.slip_friction);
	fprintf(outfile, "Normal Friction: %f\n", settings.si.normal_friction);
	fprintf(outfile, "Shocker Walls: %d\n", settings.si.shocker_walls);
	fprintf(outfile, "Difficulty: %d\n", settings.difficulty);

	for (i = 0; i < MAX_SPECIALS; i++) {
		if (force_states[i] == INT_FORCE_OFF) {
			fprintf(outfile, "Force Off: %s\n", special_stat[i].type);
		}
	}
	for (i = 0; i < MAX_SPECIALS; i++) {
		if (force_states[i] == INT_FORCE_ON) {
			fprintf(outfile, "Force On: %s\n", special_stat[i].type);
		}
	}

	customized_combatants();
	init_combatants();
	setup_game(False);

	ProgWritten = (char *)calloc(num_prog_descs, sizeof(char));

	assert(ProgWritten);

	fprintf(outfile, "Tanks: %d\n", num_veh_alive);

	/* Information per vehicle */
	for (ctri = 0; ctri < num_veh_alive; ctri++) {
		vptr = live_vehicles[ctri];

		/* join the players grid's programs with the prog_desc dynamic progs */
		/* and write the result */
		if (vptr->num_programs) {
			for (ctrj = 0; ctrj < vptr->num_programs; ctrj++) {
				for (iProg = 0; iProg < num_prog_descs; iProg++) {
					pdesc = prog_desc[iProg];

					/* if it is the same struct */
					if (vptr->program[ctrj].desc == pdesc) {
						/* if the program is dynamically loaded */
						if (pdesc->filename != (char *) 0) {
							/* if I have not already written it */
							if (ProgWritten[iProg] == False) {
								fprintf(outfile, "Program:\t%s\t%s\n",
										pdesc->name, pdesc->filename);

								ProgWritten[iProg] = True;
							}
						}
					}
				}
			}
		}
		dispname = "NONE";
		for (ctrj = 0; ctrj < num_terminals; ctrj++) {
			if (terminal[ctrj]->vehicle == vptr) {
				vidptr = (Video *) terminal[ctrj]->video;
				dispname = video_display_name(vidptr);
			}
		}

		/* Vehicle # */
		fprintf(outfile, "V#%d:\t", vptr->number);

		/* Combatant */
		fprintf(outfile, "%s,\t", vptr->owner->name);

		/* Display */
		fprintf(outfile, "%s,\t", dispname);

		/* Vehicle */
		fprintf(outfile, "%s,\t", vptr->name);

		/* Team */
		fprintf(outfile, "%s,\t", teams_entries[vptr->team]);

		/* Programs */
		assert(vptr->num_programs == vptr->owner->num_programs);

		/* Num Programs */
		fprintf(outfile, "%d,\t", vptr->owner->num_programs);

		if (vptr->num_programs) {
			for (ctrj = 0; ctrj < vptr->num_programs; ctrj++) {
				fprintf(outfile, "%s", vptr->program[ctrj].desc->name);
				if (ctrj != vptr->num_programs - 1)
					fprintf(outfile, ",\t");
			}
		} else {
			fprintf(outfile, "NONE");
		}

		fprintf(outfile, "\n");
	}							/* end of for - for each vehicle */

	fclose(outfile);
	return 0;
}
