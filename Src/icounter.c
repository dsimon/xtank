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
#include "icounter.h"
#include "common.h"
#include "tanktypes.h"
#include "clfkr.h"
#include "bullet.h"
#include "graphics.h"
#include "terminal.h"
#include "vehicle.h"
#include "program.h"
#include "server.h"
#include "proto.h"

#ifdef UNIX
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>		/* for exit() */
#include <unistd.h>		/* for pause() */
#include <sys/types.h>
#include <sys/time.h>

#define INC_TIME 10000

int elapsed_time;

/*
** P1.13 bot yield watchdog.  In tick-cooperative server mode this virtual
** timer is armed around a single program run.  If the program does not yield
** within the configured amount of CPU time, the scheduler is hung: the
** watchdog reports the bot and aborts.  It never preempts or silently yields
** the program, so scheduling is unchanged for a well-behaved bot.
*/
static volatile sig_atomic_t watchdog_watching = 0;
static long watchdog_limit_us = 1000000;	/* 1 s of CPU, overridable */
static int watchdog_inited = 0;

extern Vehicle *cv;
extern int frame;

static void
watchdog_report(void)
{
	char buf[192];
	int n;
	const char *name = "?";
	int veh = -1;

	if (cv != NULL) {
		veh = (int) cv->number;
		if (cv->current_prog != NULL &&
		    cv->current_prog->desc != NULL &&
		    cv->current_prog->desc->name != NULL)
			name = cv->current_prog->desc->name;
	}
	n = snprintf(buf, sizeof(buf),
		"XTANK WATCHDOG: bot '%s' (vehicle %d) did not yield in %ld ms "
		"of cpu time at frame %d\n",
		name, veh, watchdog_limit_us / 1000, frame);
	if (n > (int) sizeof(buf) - 1)
		n = (int) sizeof(buf) - 1;
	if (n > 0)
		(void) write(STDERR_FILENO, buf, (size_t) n);
	_exit(70);
}

/*
** Virtual timer alarm handler.  Updates how much time has passed
** by incrementing elapsed_time by INC_TIME.
*/

static void
increment_time(int sig /* __unused */)
{
	elapsed_time += INC_TIME;
	if (watchdog_watching && watchdog_limit_us > 0 &&
	    elapsed_time >= watchdog_limit_us)
		watchdog_report();
#ifdef mmax
	sigset(SIGVTALRM, increment_time);
#endif
#ifdef linux
	signal(SIGVTALRM, increment_time);
#endif
}

#if defined(MOTOROLA) && defined(m68k)
#undef SIGVTALRM
#define SIGVTALRM SIGALRM
#endif

void
setup_counter(void)
{
#if defined(MOTOROLA) && defined(m88k) || defined(SVR4)
	if ((int) sigset(SIGVTALRM, increment_time) == -1)
#else
	if ((int) signal(SIGVTALRM, increment_time) == -1)
#endif
		rorre("Could not set up interval timer signal handler.");
}

#if defined(MOTOROLA) && defined(m68k)

static unsigned int start_val = INC_TIME / 1000;
static unsigned int stop_val = 0;

#else /* !defined(MOTOROLA) !! !definded(m68k) (ie everything else) */

static struct itimerval start_val =
{
	{0, INC_TIME},
	{0, INC_TIME}};
static struct itimerval stop_val =
{
	{0, 0},
	{0, 0}};

#endif /* !defined(MOTOROLA) !! !definded(m68k) (ie everything else) */

/*
** Start up interval timer to call increment_time every INC_TIME ms.
*/
void
start_counter(void)
{
#if defined(MOTOROLA) && defined(m68k)
	(void) alarm(start_val);
#else
	(void) setitimer(ITIMER_VIRTUAL, &start_val, (struct itimerval *) NULL);
#endif
}

/*
** Stop the interval timer.
*/
void
stop_counter(void)
{
#if defined(MOTOROLA) && defined(m68k)
	(void) alarm(stop_val);
#else
	(void) setitimer(ITIMER_VIRTUAL, &stop_val, (struct itimerval *) NULL);
#endif
}

/*
** P1.13 watchdog arm/disarm.  Same virtual timer as the legacy CPU-time
** scheduler, but here elapsed_time is consulted only to diagnose a bot that
** fails to yield -- never to decide who runs.
*/
void
program_watchdog_start(void)
{
	const char *e;

	if (!watchdog_inited) {
		e = getenv("XTANK_BOT_WATCHDOG_MS");
		if (e != NULL) {
			long ms = atol(e);

			if (ms > 0)
				watchdog_limit_us = ms * 1000;
		}
		watchdog_inited = 1;
	}
	elapsed_time = 0;
	watchdog_watching = 1;
#if defined(MOTOROLA) && defined(m68k)
	(void) alarm(start_val);
#else
	(void) setitimer(ITIMER_VIRTUAL, &start_val, (struct itimerval *) NULL);
#endif
}

void
program_watchdog_stop(void)
{
	watchdog_watching = 0;
#if defined(MOTOROLA) && defined(m68k)
	(void) alarm(stop_val);
#else
	(void) setitimer(ITIMER_VIRTUAL, &stop_val, (struct itimerval *) NULL);
#endif
}

/* Has enough real time passed since the last frame? */
static Boolean real_timer_expired = TRUE;

static void
sigalrm_handler(int sig /* __unused */)
{
	real_timer_expired = TRUE;
#ifdef linux
	/* Reinstall the sigalrm_handler each time */
	signal(SIGALRM, sigalrm_handler);
#endif
}

#if defined(MOTOROLA) && defined(m88k)
#define timerclear(tvp)	(tvp)->tv_sec = (tvp)->tv_usec = 0
#endif

#if defined(MOTOROLA) && defined(m68k)

void
start_real_counter(int time)
{
}

void
wait_for_real_counter(void)
{
}

#else


extern CLFkr command_options;	/* options for how xtank starts / exits */

void
start_real_counter(int time)
{
	struct itimerval real_timer;

	/* Server path: the monotonic loop owns pacing; do not install any
	   SIGALRM real-time frame timer (P1.9). */
	if (server_loop_active())
		return;

	if (!command_options.NoDelay) {
		/* Set up a real-time interval timer that expires every time useconds.
           Each time it expires, the variable real_timer_expired will be set to
	       true. */
		timerclear(&real_timer.it_interval);
		timerclear(&real_timer.it_value);
		real_timer.it_interval.tv_usec = real_timer.it_value.tv_usec = time;
		(void) setitimer(ITIMER_REAL, &real_timer, (struct itimerval *) NULL);

		/* Call the sigalrm_handler function every time the handler expires */
#if defined(MOTOROLA) && defined(m88k) || defined(SVR4)
		sigset(SIGALRM, sigalrm_handler);
#else
		signal(SIGALRM, sigalrm_handler);
#endif
	}
}

void
wait_for_real_counter(void)
{
	/* Server path: the monotonic loop already waited on the absolute
	   deadline; never pause() on a signal-driven frame timer here (P1.9). */
	if (server_loop_active())
		return;

	/* The variable real_timer_expires will be set to true when the ITIMER_REAL
       timer expires.  Until then, pause() is called, which gives up process
       control until an interval timer expires. This way we don't waste CPU
       time. */
	if (!command_options.NoDelay) {
		while (!real_timer_expired)
			pause();
	}
	real_timer_expired = FALSE;
}
#endif /* MOTOROLA && m68k */

#endif

#ifdef AMIGA
struct Custom *c = (struct Custom *) 0xDFF000;
extern void VB_count();
struct Interrupt intserver;
int count;

setup_counter()
{
	intserver.is_Node.ln_Type = NT_INTERRUPT;
	intserver.is_Node.ln_Pri = 120;
	intserver.is_Node.ln_Name = "VB-count";
	intserver.is_Data = (APTR) & count;
	intserver.is_Code = VB_count;
}

start_counter()
{
	AddIntServer(INTB_VERTB, &intserver);
}

stop_counter()
{
	RemIntServer(INTB_VERTB, &intserver);
}

start_real_counter(time)
int time;
{
}

wait_for_real_counter()
{
}

#endif
