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
** server_events.h -- generation-safe entity lifetimes, ticked events and
** immutable match results (P2.6).
**
** The protocol-v1 stable presentation key is (epoch, kind, id, generation).
** This module owns the export-side entity-lifetime counter that generation is
** drawn from (P2.5 used a provisional Combatant.deaths+1 proxy) and the
** read-only event stream that describes simulation transitions:
**
**   DEATH / RESPAWN   captured at the reviewed vehicle.c mutation sites
**   SCORE             derived from a read-only end-of-tick score comparison
**   EXPLOSION         derived from a read-only end-of-tick transient compare
**   MATCH_END         captured with the results, before scores are reset
**
** Events are never invented and never feed back into the simulation: the
** hooks only append to an export-side buffer, advance no RNG and change no
** gameplay rule.  Results are copied while the combatant scores still exist.
** No DTO carries a pointer.
**
** The wire type is included by relative path so the Phase 1 X-boundary checker
** (which preprocesses Src/ with -I Src -I Src/Include) can still resolve it.
*/

#ifndef _SERVER_EVENTS_H_
#define _SERVER_EVENTS_H_

#include "../../Protocol/wire.h"

/* Version of the derived event/lifetime rules. */
#define SERVER_EVENTS_RULES_VERSION	1u

/* Per-epoch event ring bound (matches XTK_MAX_PENDING_EVENTS). */
#define SERVER_EVENTS_MAX		XTK_MAX_PENDING_EVENTS

/* server_events_results() results. */
#define SERVER_EVENTS_OK		0
#define SERVER_EVENTS_ERR_NO_RESULTS	(-1)
#define SERVER_EVENTS_ERR_RANGE		(-2)

/*
** Lifecycle.  server_events_reset() starts a fresh epoch: it resets the event
** sequence, the pending event buffer, the results and the read-only diff
** baselines.  Entity-lifetime generations are monotonic across epochs (the
** epoch is part of the key, so a stale identity can never collide) and are NOT
** reset.  Calls are idempotent.
*/
void server_events_reset(uint32_t epoch, uint32_t first_tick);
uint32_t server_events_epoch(void);

/*
** Required read-only end-of-tick observation.  Called after server_step()
** completes (the view exporter also calls it) and is idempotent within a tick:
** repeated calls observe the same frame and emit nothing further.  It compares
** the authoritative records against the previous observation to derive the
** persistent SCORE transitions and the transient EXPLOSION appearances, and to
** advance bullet/explosion lifetime generations on slot reuse.  It writes only
** to this module's own state.
*/
void server_events_observe(void);

/*
** Entity-lifetime generation for the stable key (kind, id).  `id` is the
** vehicle slot for XTK_ENTITY_VEHICLE, the bullet array index for
** XTK_ENTITY_BULLET and the explosion array index for XTK_ENTITY_EXPLOSION.
** Returns 0 when the entity kind/id is not tracked.
*/
uint32_t server_events_generation(uint8_t kind, uint16_t id);

/* Pending event access, in event_sequence order. */
int server_events_count(void);
const XtkEvent *server_events_event(int index);

/* Non-zero when results have been captured for the current epoch. */
int server_events_results_valid(void);

/*
** Copy the immutable match results captured before score reset into `out`.
** Returns SERVER_EVENTS_OK or SERVER_EVENTS_ERR_NO_RESULTS / _ERR_RANGE.
*/
int server_events_results(XtkResults *out);

/*
** Reviewed read-only transient hooks.  These are called from the simulation at
** the existing mutation sites.  They are declared weak at each call site, so
** when this module is not linked (for example the frozen Phase 1 regression
** candidate) the calls are no-ops and the simulation is unchanged.
*/

/* A vehicle life ended (kill_vehicle).  `killer_slot` is -1 when no killer. */
void server_events_note_death(int victim_slot, int killer_slot);

/* A vehicle life began (activate_vehicle); x,y are its spawn Loc. */
void server_events_note_spawn(int slot, double x, double y);

/*
** Capture results while the combatant scores still exist (game.c
** game_finalize_scores, before the per-match zeroing).  `winner_slot` is -1
** when there is no winner.
*/
void server_events_capture_results(int status, int winner_slot);

#endif /* _SERVER_EVENTS_H_ */
