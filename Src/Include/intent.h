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
** intent.h -- bounded, tick/player/sequence-tagged player-intent queue
** (P1.7).
**
** The headless/server path no longer reaches into the simulation directly
** from terminal events.  A controller (one player, one tank) submits tagged
** intents and the server consumes exactly the intents whose tick has arrived,
** once per simulation tick, after bot programs and before movement.
**
** Intents are tagged with:
**   - tick      the simulation frame the intent applies to;
**   - player    the controller/tank the intent belongs to;
**   - sequence  a within-tick ordering key chosen by the producer.
**
** The queue is bounded (INTENT_QUEUE_MAX); a push that would exceed the bound
** is rejected without changing any state.  Invalid, stale (already-consumed
** tick) and foreign-target (nonexistent vehicle) intents are rejected too.
** Duplicate (player, tick, sequence) intents are rejected.  Held controls
** (fire, target aim, chase and standoff) persist across ticks until changed or
** released, and are cleared by intent_reset().
*/

#ifndef _INTENT_H_
#define _INTENT_H_

#include "tanktypes.h"
#include "tanklimits.h"

/* Explicit bound on the number of intents waiting to be consumed. */
#define INTENT_QUEUE_MAX	128

/* One controller per tank. */
#define INTENT_MAX_PLAYERS	MAX_VEHICLES

typedef enum {
	INTENT_NONE = 0,

	/* One-shot controls. */
	INTENT_DRIVE_REL,	/* a = relative drive in [-9, 9]        */
	INTENT_DRIVE_ABS,	/* a = absolute drive                   */
	INTENT_HEADING,		/* a = desired heading (radians)        */
	INTENT_FIRE,		/* fire all weapons once                */
	INTENT_WEAPON,		/* a = weapon, b = on/off/fire          */

	/* Held controls. */
	INTENT_AIM_WORLD,	/* a = world x, b = world y (one-shot)  */
	INTENT_AIM_TARGET,	/* a = target player, -1 releases (held)*/
	INTENT_FIRE_HOLD,	/* a != 0 hold, 0 release (held)        */
	INTENT_CHASE,		/* a = target, -1 releases; b = drive   */
	INTENT_STANDOFF		/* a = target, -1 releases; b = distance*/
} IntentOp;

typedef enum {
	INTENT_WEAPON_ON = 0,
	INTENT_WEAPON_OFF,
	INTENT_WEAPON_FIRE
} IntentWeaponAction;

/* intent_push() results: 0 accepted, negative rejected. */
typedef enum {
	INTENT_OK = 0,
	INTENT_REJECT_FULL = -1,	/* bounded overflow                */
	INTENT_REJECT_BADOP = -2,	/* unknown op / bad arguments      */
	INTENT_REJECT_BADPLAYER = -3,	/* no such controller/tank         */
	INTENT_REJECT_STALE = -4,	/* tick already consumed           */
	INTENT_REJECT_DUPLICATE = -5,	/* (player,tick,sequence) pending  */
	INTENT_REJECT_FOREIGN = -6	/* target is not a vehicle here    */
} IntentStatus;

typedef struct {
	int tick;
	int player;
	unsigned int sequence;
	IntentOp op;
	double a, b;
} Intent;

/* One-time initialisation (safe to call repeatedly). */
void intent_init(void);

/* Clear the queue and every held control; used at match init/reset. */
void intent_reset(void);

/*
 * Cancel one controller's pending and held input (P3.7 departure).  Drops every
 * queued intent tagged with `player`, clears that controller's held aim / chase
 * / standoff / fire and its anti-wedge bookkeeping, and leaves every other
 * controller's queue and held state untouched.  A peer-scoped cancel is
 * required on departure so a removed tank cannot fire or drive from stale
 * input; the all-or-nothing intent_reset() would also wipe the survivor.
 */
void intent_cancel_player(int player);

/*
 * Submit an intent.  Returns INTENT_OK or a negative IntentStatus; a rejected
 * intent never changes queue or held-control state.
 */
int intent_push(const Intent *in);

/* Number of queued intents (not yet consumed) tagged with `tick`. */
int intent_pending(int tick);

/*
 * Consume the intents for `tick` in stable (player, sequence) order, applying
 * held controls first and held fire last, exactly at the legacy input phase.
 * Intents for later ticks stay queued.  Returns the number of queued intents
 * applied.
 */
int intent_consume(int tick);

/* Inspection helpers (used by the P1.7 acceptance fixture). */
int intent_held_fire(int player);
int intent_held_aim(int player);
int intent_queued(void);

#endif /* _INTENT_H_ */
