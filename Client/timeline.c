/*
 * Client/timeline.c -- monotonic estimated server timeline (P2.11).
 *
 * See Client/timeline.h.  This translation unit is pure C: no SDL, no engine,
 * no RNG, no transport.  It reads only the tick numbers and receive times the
 * caller supplies.
 */

#include "timeline.h"

#include <string.h>

void timeline_init(XtkTimeline *t, int hz)
{
	if (t == NULL) {
		return;
	}
	memset(t, 0, sizeof(*t));
	t->hz = (hz > 0) ? hz : 1;
	t->tick_ns = 1.0e9 / (double)t->hz;
}

void timeline_reset(XtkTimeline *t)
{
	int hz;
	double tick_ns;

	if (t == NULL) {
		return;
	}
	hz = t->hz;
	tick_ns = t->tick_ns;
	memset(t, 0, sizeof(*t));
	t->hz = (hz > 0) ? hz : 1;
	t->tick_ns = (tick_ns > 0.0) ? tick_ns : (1.0e9 / (double)t->hz);
}

void timeline_observe(XtkTimeline *t, int server_tick, uint64_t recv_ns)
{
	double nominal;

	if (t == NULL) {
		return;
	}
	nominal = 1.0e9 / (double)t->hz;
	if (nominal <= 0.0) {
		nominal = 1.0;
	}

	if (!t->have) {
		t->have = 1;
		t->anchor_tick = server_tick;
		t->anchor_ns = recv_ns;
		t->prev_tick = server_tick;
		t->prev_ns = recv_ns;
		t->estimate = (double)server_tick;
		return;
	}

	/* Measure the interval between consecutive server ticks and smooth it. */
	if (server_tick > t->prev_tick && recv_ns > t->prev_ns) {
		double observed =
			(double)(recv_ns - t->prev_ns) /
			(double)(server_tick - t->prev_tick);
		if (observed < 0.25 * nominal) {
			observed = 0.25 * nominal;
		}
		if (observed > 4.0 * nominal) {
			observed = 4.0 * nominal;
		}
		if (t->have_rate) {
			t->tick_ns = 0.75 * t->tick_ns + 0.25 * observed;
		} else {
			t->tick_ns = observed;
			t->have_rate = 1;
		}
	}

	/* Re-anchor on the newest tick so the estimate is continuous. */
	if (server_tick >= t->prev_tick) {
		t->anchor_tick = server_tick;
		t->anchor_ns = recv_ns;
		t->prev_tick = server_tick;
		t->prev_ns = recv_ns;
	}
}

double timeline_estimate(XtkTimeline *t, uint64_t now_ns)
{
	double e;

	if (t == NULL || !t->have) {
		return 0.0;
	}
	if (now_ns <= t->anchor_ns) {
		e = (double)t->anchor_tick;
	} else {
		e = (double)t->anchor_tick +
		    (double)(now_ns - t->anchor_ns) / t->tick_ns;
	}
	if (e < t->estimate) {
		e = t->estimate;
	}
	t->estimate = e;
	return e;
}

int timeline_have(const XtkTimeline *t)
{
	return (t != NULL && t->have);
}