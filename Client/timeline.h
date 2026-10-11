/*
 * Client/timeline.h -- monotonic estimated server timeline for the Xtank
 * Windows client (P2.11).
 *
 * The client owns presentation only: it receives full authorized snapshots at
 * 15 Hz, each tagged with an authoritative `server_tick`, and must render at an
 * estimated server time minus the approved one-tick interpolation buffer
 * (docs/PHASE2-DECISIONS.md D2-E).  This module turns a stream of
 * `(server_tick, receive_time)` observations into a monotonic, fractional
 * server-tick estimate.  It never extrapolates gameplay and never schedules an
 * engine tick; presentation speed cannot advance the world.
 *
 * Authority: docs/PHASE2-PLAN.md P2.11; docs/PHASE2-DECISIONS.md D2-E.
 */

#ifndef XTK_CLIENT_TIMELINE_H
#define XTK_CLIENT_TIMELINE_H

#include <stdint.h>

typedef struct {
	int      hz;		/* nominal server ticks per second          */
	double   tick_ns;	/* smoothed nanoseconds per server tick     */
	int      have;		/* an observation has been recorded         */
	int      have_rate;	/* tick_ns came from measured intervals     */
	int      anchor_tick;	/* server tick of the newest observation    */
	uint64_t anchor_ns;	/* receive time of the newest observation   */
	int      prev_tick;	/* previous observation tick (rate estimate) */
	uint64_t prev_ns;
	double   estimate;	/* last returned monotonic estimate         */
} XtkTimeline;

/* Initialise to the nominal rate (`hz` ticks/s) with no observations. */
void timeline_init(XtkTimeline *t, int hz);

/* Clear observations, keeping the nominal rate. */
void timeline_reset(XtkTimeline *t);

/*
 * Record one snapshot arrival.  `server_tick` is the snapshot's authoritative
 * tick and `recv_ns` is the local monotonic receive time in nanoseconds.  The
 * per-tick duration is smoothed from consecutive arrivals and clamped to a
 * sane band so a burst or a stall cannot distort the timeline; the anchor moves
 * to the newest tick so the estimate stays continuous.
 */
void timeline_observe(XtkTimeline *t, int server_tick, uint64_t recv_ns);

/*
 * Estimated fractional server tick at local time `now_ns`, monotonically
 * non-decreasing for a given timeline.  Returns 0 if no observation exists.
 */
double timeline_estimate(XtkTimeline *t, uint64_t now_ns);

/* True once at least one observation has been recorded. */
int timeline_have(const XtkTimeline *t);

#endif /* XTK_CLIENT_TIMELINE_H */