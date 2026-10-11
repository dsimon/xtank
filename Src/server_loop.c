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
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 */

/*
** server_loop.c -- monotonic 15 Hz simulation loop (P1.9).
**
** The simulation advances in a fixed step (one animate()/server_step() per
** tick) that is independent of rendering and of how long the work took.  The
** loop paces itself against absolute CLOCK_MONOTONIC deadlines computed from
** the rational period 1/tick_hz: the deadline for tick N is
**
**     t0 + N * 1e9 / tick_hz   nanoseconds
**
** evaluated in integer arithmetic, so the phase never drifts even though the
** period (66666666.66... ns at 15 Hz) is not representable as an integer.
** Because every deadline is absolute, a slow tick does not push the schedule
** later: the next deadline is still the true one, and the loop catches up by
** running ticks back to back -- it never skips a tick.
**
** Fast replay selects the same simulation path and simply bypasses the sleep.
** It is used by the oracle/replay harness so a finite dump can be produced
** without waiting for wall time; it cannot change simulation state.
**
** The loop bounds its own work reporting: it records the wall time spent in
** each server_step(), including the maximum, the 99th percentile and the
** number of ticks that exceeded the period, and prints a one-line summary.
**
** This file is server-only: the frozen X reference and the X front-end keep
** their legacy pacer (icounter.c / check_game_speed), which this loop
** bypasses whenever it is active (tick_hz > 0).
*/

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "xtank.h"
#include "clfkr.h"
#include "server.h"
#include "proto.h"

extern CLFkr command_options;
extern int frame;

/*
 * P2.7 bounded network admission seam.  Declared weak so a build that does not
 * link Src/server_net.c (the frozen reference, the Phase 1 candidate and every
 * build without networking) resolves them to NULL and this loop is
 * byte-for-byte unchanged.  When present, the admission layer services the
 * transport without blocking, admits inputs for the next unconsumed tick
 * before the step, and flushes acknowledgments after it.
 */
extern int server_net_active(void) __attribute__((weak));
extern int server_net_pre_step(int assign_tick) __attribute__((weak));
extern void server_net_post_step(int tick) __attribute__((weak));

/* Configured simulation rate; 0 selects the legacy front-end pacer. */
static int loop_hz = 0;

/* Monotonic epoch: deadline(N) = t0_ns + N * 1e9 / loop_hz. */
static long long loop_t0_ns;
static int loop_started = 0;
static int loop_fast = 0;
static long loop_tick_limit = -1;
static long loop_match_limit = -1;
static long loop_ticks = 0;

/* Bounded per-tick work reporting (1 us resolution up to 100 ms). */
#define LOOP_WORK_BUCKET_NS	5000LL
#define LOOP_WORK_BUCKETS	20001
static unsigned long work_hist[LOOP_WORK_BUCKETS];
static long long work_max_ns;
static long long work_sum_ns;
static long work_samples;
static long work_overruns;
static long late_ticks;

static long long
now_ns(void)
{
	struct timespec ts;

	(void) clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long) ts.tv_sec * 1000000000LL + (long long) ts.tv_nsec;
}

void
server_loop_configure(int hz)
{
	loop_hz = (hz > 0) ? hz : 0;
}

int
server_loop_active(void)
{
	return loop_hz > 0;
}

int
server_loop_hz(void)
{
	return loop_hz;
}

long
server_loop_ticks(void)
{
	return loop_ticks;
}

/*
** Absolute deadline, in nanoseconds, for a given (1-based) tick number.  The
** division is the rational accumulation the contract requires: the deadline
** for tick N is exactly N/tick_hz seconds after the epoch, with no per-tick
** rounding residue.
*/
static long long
loop_deadline_ns(long tick)
{
	return loop_t0_ns + (long long) tick * 1000000000LL / (long long) loop_hz;
}

static void loop_reset(void);

/*
 * P2.8: let a session-driven loop (server_session_run) reuse the exact same
 * absolute-deadline pacer as the production loop.  Rebases the monotonic epoch
 * so a session epoch starts a fresh 1/tick_hz schedule; server_loop_pace() is
 * then called by the session before each step.  A build without the session
 * module never calls this.
 */
void
server_loop_epoch_begin(void)
{
	loop_reset();
}

static void
loop_reset(void)
{
	loop_t0_ns = now_ns();
	loop_started = 1;
	loop_ticks = 0;
	loop_fast = command_options.NoDelay ? 1 : 0;
	{
		const char *e = getenv("XTANK_SERVER_FAST");

		if (e != NULL)
			loop_fast = (atoi(e) != 0);
	}
	{
		const char *e = getenv("XTANK_SERVER_TICKS");

		loop_tick_limit = (e != NULL) ? atol(e) : -1;
	}
	{
		/*
		 * Per-match tick cap (server policy, not a gameplay rule): when the
		 * cap is reached the match is finalized as GAME_OVER through the
		 * normal results/reset path.  XTANK_SERVER_TICKS remains the
		 * test-only hard stop that exits after reporting.
		 */
		const char *e = getenv("XTANK_SERVER_MATCH_TICKS");

		loop_match_limit = (e != NULL) ? atol(e) : -1;
	}

	(void) memset(work_hist, 0, sizeof(work_hist));
	work_max_ns = work_sum_ns = 0;
	work_samples = work_overruns = late_ticks = 0;
}

/*
** Wait until the absolute deadline for `tick` has passed.  If the deadline is
** already in the past the loop is behind and returns immediately, so the loop
** catches up instead of skipping the tick.  Fast replay bypasses only this
** wait.
*/
void
server_loop_pace(int tick)
{
	long long deadline;
	struct timespec ts;

	if (loop_hz <= 0 || !loop_started || loop_fast)
		return;

	deadline = loop_deadline_ns(tick);
	if (now_ns() >= deadline)
		return;

	ts.tv_sec = (time_t) (deadline / 1000000000LL);
	ts.tv_nsec = (long) (deadline % 1000000000LL);
	while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR)
		;
}

/*
** Record the wall time a single tick's simulation work consumed.
*/
static void
loop_note_work(long long work)
{
	long bucket;

	if (work < 0)
		work = 0;
	work_samples++;
	work_sum_ns += work;
	if (work > work_max_ns)
		work_max_ns = work;
	bucket = (long) (work / LOOP_WORK_BUCKET_NS);
	if (bucket >= LOOP_WORK_BUCKETS)
		bucket = LOOP_WORK_BUCKETS - 1;
	work_hist[bucket]++;
	if (loop_hz > 0 && work > 1000000000LL / loop_hz)
		work_overruns++;
}

/*
** p99 of the recorded per-tick work, in nanoseconds (upper bucket edge).
*/
static long long
loop_work_p99_ns(void)
{
	unsigned long target, cum = 0;
	long i;

	if (work_samples == 0)
		return 0;
	target = (unsigned long) ((work_samples * 99 + 99) / 100);
	for (i = 0; i < LOOP_WORK_BUCKETS; i++) {
		cum += work_hist[i];
		if (cum >= target)
			return (long long) (i + 1) * LOOP_WORK_BUCKET_NS;
	}
	return work_max_ns;
}

static void
loop_report(void)
{
	long long p99 = loop_work_p99_ns();
	double avg_us = work_samples ? (double) work_sum_ns / work_samples / 1000.0
				    : 0.0;
	const char *path = getenv("XTANK_LOOP_REPORT");
	FILE *fp = stderr;

	if (path != NULL) {
		FILE *f = fopen(path, "w");

		if (f != NULL)
			fp = f;
	}
	fprintf(fp,
		"SERVER-LOOP hz=%d ticks=%ld fast=%d avg_us=%.1f p99_us=%.1f "
		"max_us=%.1f overruns=%ld late=%ld\n",
		loop_hz, loop_ticks, loop_fast, avg_us,
		(double) p99 / 1000.0, (double) work_max_ns / 1000.0,
		work_overruns, late_ticks);
	fflush(fp);
	if (fp != stderr)
		fclose(fp);
}

/*
** The production loop: pace to each tick's absolute deadline, run exactly one
** fixed simulation step, then account for the work.  Returns the final game
** status (GAME_OVER/GAME_QUIT/GAME_RESET) so play_game() can settle scores and
** reset exactly as before.
**
** XTANK_SERVER_TICKS (test-only) stops after that many ticks and reports, so
** a finite, deterministic dump can be compared between fast and paced runs.
*/
int
server_loop_run(void)
{
	unsigned int status;
	int tick;

	/* Each call drives one match; rebase the monotonic epoch so a reset
	   match starts a fresh 1/tick_hz schedule. */
	loop_reset();

	for (;;) {
		int net;

		tick = frame + 1;
		net = (server_net_active != NULL && server_net_active() != 0);
		if (net)
			(void) server_net_pre_step(tick);

		/* Count a missed deadline: we are about to run behind schedule. */
		if (loop_hz > 0 && !loop_fast &&
		    now_ns() > loop_deadline_ns(tick))
			late_ticks++;

		server_loop_pace(tick);

		if (loop_hz > 0) {
			long long a = now_ns();
			long long b;

			status = (unsigned int) server_step();
			if (net)
				server_net_post_step(frame);
			b = now_ns();
			loop_note_work(b - a);
		} else {
			status = (unsigned int) server_step();
			if (net)
				server_net_post_step(frame);
		}

		loop_ticks++;

		if (status != GAME_RUNNING)
			break;
		if (loop_tick_limit > 0 && loop_ticks >= loop_tick_limit) {
			loop_report();
			exit(0);
		}
		if (loop_match_limit > 0 && loop_ticks >= loop_match_limit) {
			/* Bounded match: finalize through the normal results path. */
			status = GAME_OVER;
			break;
		}
	}

	loop_report();
	return (int) status;
}
