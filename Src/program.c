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

#include <assert.h>
#include "xtank.h"
#include "xtanklib.h"
#include "thread.h"
#include "icounter.h"
#include "vehicle.h"
#include "globals.h"
#include "graphics.h"
#include "proto.h"

extern int frame;
extern Vehicle *cv;
extern Map real_map;
extern Settings settings;

/* ------------------------------------------------------------------ */
/* P1.11 -- knowledge versus draw                                       */
/* ------------------------------------------------------------------ */

/* Read-only dispatch instrumentation (see special.h). */
SpecialStats special_stats;

/*
** Presentation gate.  Enabled by default so the X front-end is unchanged;
** the display-free server clears it.  XTANK_NO_PRESENTATION is a test-only
** seam used by Tests/phase1/specials.sh.
*/
static int specials_presentation = 1;

void
server_set_presentation(int enabled)
{
	specials_presentation = enabled ? 1 : 0;
}

int
special_presentation_active(void)
{
	return specials_presentation;
}

/*
** Release/MVP special preset (PHASE0-BRIEF section 3).  Ordinary Radar,
** Mapper, Console and HUD remain; New Radar, Tac Link, Camo and Rdf are
** disabled.  The oracle control scenarios never call this, so they keep all
** four for the regression.
*/
void
special_apply_mvp_preset(char *force_states)
{
	int i;

	for (i = 0; i < MAX_SPECIALS; i++)
		force_states[i] = INT_FORCE_DONT;

	force_states[CONSOLE] = INT_FORCE_DONT;
	force_states[MAPPER] = INT_FORCE_DONT;
	force_states[RADAR] = INT_FORCE_DONT;
#ifndef NO_HUD
	force_states[HUD] = INT_FORCE_DONT;
#else
	force_states[DEEPRADAR] = INT_FORCE_DONT;
#endif
	force_states[NEW_RADAR] = INT_FORCE_OFF;
	force_states[TACLINK] = INT_FORCE_OFF;
#ifndef NO_CAMO
	force_states[CAMO] = INT_FORCE_OFF;
	force_states[RDF] = INT_FORCE_OFF;
#endif
}

/*
** Zero-presentation probe (P1.11 test only).  When XTANK_SPECIALS_PROBE is
** set, force one draw action per live special through the dispatch point and
** then one model update.  With presentation disabled every draw is elided;
** the model scan still applies.  It changes no state by itself beyond the
** (deliberate) model update, and is never active in a normal run.
*/
static void
specials_probe(Vehicle *v)
{
	static const SpecialType probe[] = { MAPPER, RADAR, NEW_RADAR, HUD };
	unsigned int i;

	for (i = 0; i < sizeof(probe) / sizeof(probe[0]); i++) {
		if (v->special[(int) probe[i]].status != SP_nonexistent) {
			do_special(v, probe[i], SP_draw);
			do_special(v, probe[i], SP_erase);
		}
	}
	do_special(v, MAPPER, SP_update);
}

/* Emit the counters at process exit; only when XTANK_SPECIALS_REPORT is set. */
void
specials_report(void)
{
	char *path = getenv("XTANK_SPECIALS_REPORT");
	FILE *fp;

	if (path == NULL)
		return;
	if (strcmp(path, "-") == 0)
		fp = stderr;
	else
		fp = fopen(path, "w");
	if (fp == NULL)
		fp = stderr;

	fprintf(fp,
		"SPECIALS-STATS model=%lu draw=%lu suppressed=%lu "
		"mapper=%lu radar_acquired=%lu radar_expired=%lu "
		"presentation=%d\n",
		special_stats.model_invocations, special_stats.draw_invocations,
		special_stats.draw_suppressed, special_stats.mapper_updates,
		special_stats.radar_acquired, special_stats.radar_expired,
		special_presentation_active());
	if (fp != stderr)
		fclose(fp);
}

static void
specials_report_atexit(void)
{
	specials_report();
}

__attribute__((constructor))
static void
specials_report_init(void)
{
	if (getenv("XTANK_NO_PRESENTATION") != NULL)
		server_set_presentation(0);
	atexit(specials_report_atexit);
}

/* Time (in microseconds) a program is allotted per frame (on average) */
#define PROGRAM_ALLOT 5000

/* maximum time (in microseconds) a program is allowed to run on any given
   frame */
#define PROGRAM_TIME 10000

/* stack space programs get */
#ifndef THREAD_POSIX
#define THREAD_TOTALSIZE (sizeof(XtankThread) + STACK_SIZE)
#else
#define THREAD_TOTALSIZE (sizeof(XtankThread))
#endif

/* Clock values to time robot program execution */
Clock current_time, end_time;

Prog_desc *prog_desc[MAX_PDESCS];
int num_prog_descs;

/* Pointer to the program scheduler's thread */
XtankThread *scheduler_thread;

/* ------------------------------------------------------------------ */
/* P1.13 -- tick-cooperative server scheduling                         */
/* ------------------------------------------------------------------ */
/*
** The legacy front-end decides how long a program may run from measured CPU
** time (the virtual interval timer and check_time()).  The display-free server
** is tick-cooperative: each eligible program runs exactly one iteration and
** yields at done(), so scheduling never depends on CPU time.  threadglue.c
** selects the mode at startup from server_loop_active(); nothing else in this
** file changes for the front-end path.
*/
static int prog_cooperative = 0;

void
program_set_cooperative(int on)
{
	prog_cooperative = on ? 1 : 0;
}

int
program_cooperative(void)
{
	return prog_cooperative;
}

/*
** Changes the current vehicle pointer to the specified vehicle.
** All xtanklib functions act on the current vehicle.
*/
void
set_current_vehicle(Vehicle *v)
{
	cv = v;
}

/*
** Initializes the array of program descriptions at startup time.
*/
void
init_prog_descs(void)
{
	int iCtr;
	extern Prog_desc
#ifdef SERVER_ONLY
		drone_prog;
	/*
	 * The shipped server links and registers only the cooperative drone
	 * program (P1.15).  The four-drone/Madman oracle harness builds without
	 * -DSERVER_ONLY and keeps the full program set.
	 */
#else
		drone_prog,
		eliza_prog,
		Flipper_prog,
		spot_prog,
		Diophantine_prog,
		Dio_prog,
	/* New programs with 1.3b and extensively tested: */
		tagman_prog,
		Bootlegger_prog,
		gnat_prog,
		rdfbot_prog,
		mmtf_prog;
#endif

	num_prog_descs = 0;
	prog_desc[num_prog_descs++] = &drone_prog;
#ifndef SERVER_ONLY
	prog_desc[num_prog_descs++] = &eliza_prog;
	prog_desc[num_prog_descs++] = &Flipper_prog;
	prog_desc[num_prog_descs++] = &Diophantine_prog;
	prog_desc[num_prog_descs++] = &spot_prog;
	prog_desc[num_prog_descs++] = &Dio_prog;
	prog_desc[num_prog_descs++] = &tagman_prog;
	prog_desc[num_prog_descs++] = &Bootlegger_prog;
	prog_desc[num_prog_descs++] = &gnat_prog;
	prog_desc[num_prog_descs++] = &rdfbot_prog;
	prog_desc[num_prog_descs++] = &mmtf_prog;
#endif

	for (iCtr = 0; iCtr < num_prog_descs; iCtr++) {
		prog_desc[iCtr]->filename = (char *) 0;
	}
}

void
init_specials(Vehicle *v)
{
	int i, j;
	Mapper *m = (Mapper *) v->special[(int) MAPPER].record;
	extern char force_states[];

/*
    for (i = 0; i < MAX_SPECIALS; i++)
    {
        if (force_states[i] == INT_FORCE_OFF)
        {
            v->special[i].status = SP_nonexistent;
        }
		else
        if (force_states[i] == INT_FORCE_ON)
        {
            v->special[i].status = SP_off;
        }
    }
*/
	/* Copy maze into mapper if full_map is on */
	if (v->special[(int) MAPPER].status != SP_nonexistent &&
		settings.si.full_map) {
		Landmark_info *s;

		/* copy map */
		bcopy((char *) real_map, (char *) m->map, sizeof(real_map));

		/* Initialize landmarks */
		m->num_landmarks = 0;
		for (i = 0; i < GRID_WIDTH; i++)
			for (j = 0; j < GRID_HEIGHT; j++) {
				if (m->map[i][j].type != NORMAL &&
					m->num_landmarks < MAX_LANDMARKS) {
					s = &m->landmark[m->num_landmarks++];
					s->type = m->map[i][j].type;
					s->team = m->map[i][j].team;
					s->x = i;
					s->y = j;
				}
			}
	} else {
		/* clear out any old map memories */
		if (m)
			bzero((char *) m, sizeof(*m));
	}

	/* Activate the specials */
	for (i = 0; i < MAX_SPECIALS; i++) {
		if (v->special[i].status != SP_nonexistent) {
			v->special[i].status = SP_off;	/* otherwise activation fails */
			do_special(v, (SpecialType) i, SP_activate);
			v->special[i].damage_flag = SPDF_clear;
		}
	}

	/* P1.11 zero-presentation probe (test only; see specials_probe). */
	if (getenv("XTANK_SPECIALS_PROBE") != NULL)
		specials_probe(v);
}


/*
 * This is an unused stub, but I plan to use it in the
 * future. It might be wired up now, I'm not sure.
 */

/*
 * This IS used, it's called by inactivate_vehicle() in vehicle.c, if
 * do_special( ..., ..., SP_deactivate) isn't called, then NEW_RADAR (and
 * RADAR?) leaves blips in the Mapper Window :-(
 *
 * MDB June 1993
 */

/*
 * I put zap_specials in to correspond to init_specials so that there was
 * a final SP_deactivate to pair up wth the initial SP_activate call.
 *
 *
 * -ane Dec '93 (in 4c)
 *
 */

void
zap_specials(Vehicle *v)
{
	int i;

	for (i = 0; i < MAX_SPECIALS; i++) {
		if (v->special[i].status == SP_on)
		  do_special(v, (SpecialType) i, SP_deactivate);
	}
}


/*
** Initializes all programs for the specified vehicle.
*/
void
init_programs(Vehicle *v)
{
	Program *prog;
	int i;

	/*
	** XXX - I think is desirable for all systems.  When pthreads
	** are in use, the start function for the thread may be
	** executed before the function to create the thread has
	** resumed running.  This ensures that lowlib calls will get
	** the correct vehicle.
	*/
#if 1 || defined (__bsdi__)
	/* Set the current vehicle pointer to this vehicle for xtanklib */
	set_current_vehicle(v);
#endif

	for (i = 0; i < v->num_programs; i++) {
		/* Set the current program in the vehicle */
		prog = &v->program[i];

		if (prog->thread_buf == NULL) {
			/* Allocate the memory for the thread and stack */
			if ((prog->thread_buf = calloc(1, THREAD_TOTALSIZE)) == NULL)
				rorre("init_programs(): malloc failed");
		}
		assert(prog->thread == NULL);
		if (prog->stack_buf == NULL) {
			/* Allocate the memory for the stack */
			if ((prog->stack_buf = calloc(1, STACK_SIZE)) == NULL)
				rorre("init_programs(): malloc failed");
		}
		prog->thread = (char *)
			thread_init(prog->thread_buf, prog->stack_buf,
			STACK_SIZE, (void *(*)()) prog->desc->func);

#ifdef THREADING_DEFINED
		if (prog->thread == NULL)
			rorre("thread_init() failed in init_programs()");
#endif

		/* programs shouldn't run the first frame, since mapper */
		/* has not received its initial update by then */
		prog->total_time = (frame + 1) * PROGRAM_ALLOT;
		prog->status = PROG_on;

		prog->next_message = 0;	/* no new messages yet */
		prog->cleanup = NULL;	/* no cleanup function yet */
		v->current_prog = (Program *) NULL;
	}
}

/*
** Runs all the programs that should be run during a frame of execution.
*/
void
run_all_programs(void)
{
	Vehicle *v;
	Program *prog;
	int i, j;

	/* For every vehicle, run all of its programs */
	for (i = 0; i < num_veh_alive; i++) {
		v = live_vehicles[i];

		/* Set the current vehicle pointer to this vehicle for xtanklib */
		set_current_vehicle(v);

		/* Run all the programs for this vehicle */
		for (j = 0; j < v->num_programs; j++) {
			/* Set the current program in the vehicle */
			prog = v->current_prog = &v->program[j];

			/* If the prog hasn't used its alloted time, run it */
			if (prog->total_time <= frame * PROGRAM_ALLOT)
				run_program(prog);

			/* Nullify the current program pointer once the program is finished */
			v->current_prog = (Program *) NULL;
		}
	}
}

/*
** Runs the specified program and returns the amount of time it took to run.
*/
static int
run_program(Program *prog)
{
	if (prog_cooperative) {
		/*
		 * One iteration per eligible tick.  The program yields back to
		 * this scheduler at done(); a program that fails to yield is
		 * diagnosed by the watchdog rather than silently preempted.
		 * Charge exactly the per-frame allotment so the legacy
		 * eligibility gate -- and hence the first-tick/respawn
		 * deferral -- is preserved bit for bit.
		 */
		program_watchdog_start();
		thread_switch((XtankThread *) prog->thread);
		program_watchdog_stop();

		prog->total_time += PROGRAM_ALLOT;
		return PROGRAM_ALLOT;
	}

	/* Start up interval counter to time programs */
	clear_clock();
	write_clock(&end_time, PROGRAM_TIME);
	start_counter();

	/* Call the function */
#ifdef __bsdi__
	fprintf(stderr,"Switching to %s\n", cv->owner->name);
#endif
	thread_switch((XtankThread *) prog->thread);

	/* Stop the interval timer */
	stop_counter();

	/* Add the elapsed program time to the total time for the prog */
	prog->total_time += read_clock(&current_time);

	/* Return the elapsed program time */
	return read_clock(&current_time);
}

/*
** If the elapsed time is more than the amount of time a program
** is supposed to have, check_time switches control back to the scheduler.
** If there is no current program, or the current program has not run
** for long enough, check_time does nothing.
*/
void
check_time(void)
{
	/*
	 * Server scheduling is tick-cooperative, so a program may not be
	 * preempted on a CPU-time budget; it always runs to its next done().
	 */
	if (prog_cooperative)
		return;

	/* If there is a program running */
	if (cv->current_prog != (Program *) NULL) {
		/* If the program has been running too long, stop it */
		get_clock(&current_time);
		if (compare_clocks(&current_time, &end_time) == -1) {
#ifdef __bsdi__
			fprintf(stderr,"check_time: Switching to %s\n",
				cv->owner->name);
#endif
			thread_switch(scheduler_thread);
		}
	}
}

/*
** Stops the current program, upping elapsed_prog_time to PROGRAM_ALLOT.
*/
void
stop_program(void)
{
	Clock temp;

	if (prog_cooperative) {
		/*
		 * done(): yield to the scheduler without any CPU-time
		 * accounting.  The per-run charge is applied by run_program().
		 */
		if (cv->current_prog != (Program *) NULL)
			thread_switch(scheduler_thread);
		else
			printf("stop_program(): no program??\n");
		return;
	}

	if (cv->current_prog != (Program *) NULL) {
		/* Up the elapsed time to the given amount */
		write_clock(&temp, PROGRAM_ALLOT);
		get_clock(&current_time);
		if (compare_clocks(&current_time, &temp) == 1) {
			write_clock(&current_time, PROGRAM_ALLOT);
		}
#ifdef __bsdi__
		fprintf(stderr,"stop_program: Switching to %s\n",
			cv->owner->name);
#endif
		thread_switch(scheduler_thread);	/* Stop the program */
	} else {
		printf("stop_program(): no program??\n");
	}
}

/*
** Makes the specified programs for the specified vehicle.
*/
void
make_programs(Vehicle *v, int num_progs, int *prog_num)
{
	int num, i;

	/* Insert the proper program descriptions into the program array */
	for (i = 0; i < num_progs; i++) {
		/* Make sure we aren't making too many programs */
		if (v->num_programs == MAX_PROGRAMS)
			break;

		/* Make sure the program number is reasonable */
		num = prog_num[i];
		if (num < 0 || num >= num_prog_descs)
			continue;

		/* Copy the program description pointer into the program */
		v->program[v->num_programs++].desc = prog_desc[num];
	}
}

int
find_pdesc(char *prog_name, int *index_return)
{
	int i;

	for (i = 0; i < num_prog_descs; i++) {
		if (!prog_desc[i] || strcmp(prog_name, prog_desc[i]->name))
			continue;

		*index_return = i;
		return DESC_LOADED;
	}

	return DESC_NOT_FOUND;
}
