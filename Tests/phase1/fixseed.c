/*
 * fixseed - LD_PRELOAD seed shim for the frozen X reference build.
 *
 * The pinned baseline seeds its RNG from time() in util.c:init_random().  The
 * reference build must be reproducible, so this shim intercepts time() (and
 * gettimeofday(), which some scheduling paths consult) and returns the value
 * of XTANK_SEED (default 114336).  Reference-only: from P1.8 the candidate
 * takes its seed from server config and this shim is not preloaded.
 *
 * Build: gcc -shared -fPIC -O2 -o fixseed.so fixseed.c
 */
#define _GNU_SOURCE
#include <time.h>
#include <sys/time.h>
#include <stdlib.h>

static long
seed_value(void)
{
	char *s = getenv("XTANK_SEED");

	return s ? atol(s) : 114336L;
}

time_t
time(time_t *t)
{
	time_t v = (time_t) seed_value();

	if (t)
		*t = v;
	return v;
}

int
gettimeofday(struct timeval *tv, void *tz)
{
	(void) tz;
	if (tv) {
		tv->tv_sec = seed_value();
		tv->tv_usec = 0;
	}
	return 0;
}
