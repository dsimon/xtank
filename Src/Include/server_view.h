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
** server_view.h -- authorized read-only per-player view exporter (P2.5).
**
** Derives the field/visibility authorization table documented in
** docs/VIEW-v1.md and populates the frozen protocol-v1 XtkViewSnapshot from the
** authoritative simulation records, without ever mutating them.  The exporter
** is called after server_step() completes; it never invokes a draw callback, a
** special model action, the RNG or any control function.
**
** The wire type is included by relative path so that the Phase 1 X-boundary
** checker, which preprocesses Src/ with only -I Src -I Src/Include, can still
** resolve it; Protocol/ is two directories above Src/Include/.
*/

#ifndef _SERVER_VIEW_H_
#define _SERVER_VIEW_H_

#include "../../Protocol/wire.h"

/* Version of the derived view rules (docs/VIEW-v1.md). */
#define SERVER_VIEW_RULES_VERSION	1u

/* server_view_export() results. */
#define SERVER_VIEW_OK			0
#define SERVER_VIEW_ERR_NO_SLOT		(-1)	/* slot out of range           */
#define SERVER_VIEW_ERR_NO_VEHICLE	(-2)	/* no vehicle for the slot     */
#define SERVER_VIEW_ERR_GENERATION	(-3)	/* stale generation rejected   */
#define SERVER_VIEW_ERR_OVERFLOW	(-4)	/* a record bound was exceeded */

/* MapCell.flags bits (docs/VIEW-v1.md section 4.6). */
#define SERVER_MAPCELL_KNOWN		(1u << 0)
#define SERVER_MAPCELL_NORTH_WALL	(1u << 1)
#define SERVER_MAPCELL_WEST_WALL	(1u << 2)
#define SERVER_MAPCELL_NORTH_DEST	(1u << 3)
#define SERVER_MAPCELL_WEST_DEST	(1u << 4)
#define SERVER_MAPCELL_TEAM_KNOWN	(1u << 5)

/* RadarBlip.classification values (docs/VIEW-v1.md section 4.7). */
#define SERVER_RADAR_ANONYMOUS		0u
#define SERVER_RADAR_KNOWN		1u

/*
** Radar blip life at acquisition, mirroring the private RADAR_born constant in
** Src/radar.c.  age = SERVER_RADAR_BORN - Blip.life, clamped to >= 0.
*/
#define SERVER_RADAR_BORN		23

/*
** Export one authorized snapshot for the controlled player `player_slot`.
**
** `generation`, when non-zero, must equal the current provisional lifetime
** value, otherwise the export is refused (SERVER_VIEW_ERR_GENERATION).  The
** snapshot header's own_slot/own_generation/server_tick and every record array
** plus its count are fully populated on success.
**
** Returns SERVER_VIEW_OK or a SERVER_VIEW_ERR_* code.  Read-only.
*/
int server_view_export(int player_slot, unsigned int generation,
		       uint32_t snapshot_sequence, uint16_t flags,
		       XtkViewSnapshot *out);

/*
** Reference animation viewport half-extent in world units (per side), derived
** from the legacy ANIM_WIN_WIDTH and NUM_BOXES (docs/VIEW-v1.md section 3).
*/
int server_view_half_extent(void);

#endif /* _SERVER_VIEW_H_ */