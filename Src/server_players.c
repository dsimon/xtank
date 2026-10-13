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
** server_players.c -- explicit terminal-free human binding (P2.4).
**
** The legacy engine identifies a human player only through a Terminal: a
** settings "V#n" line matches terminal[i]->player_name, activate_vehicle()
** calls setup_terminal() on the owning player, and death clears
** terminal[player]->vehicle.  That is correct for the X front-end but wrong
** for a display-free server, where a human is a controller (a future
** connection) bound to a tank and no Terminal exists at all.
**
** This module owns that explicit binding and the only terminal accessor the
** simulation may use for a player.  It never allocates a Terminal and never
** falls back to a fake one.  The legacy X front-end is untouched: when
** terminals exist, server_player_terminal() returns the same terminal[] entry
** the engine used before, so the legacy code paths are byte-for-byte identical.
**
** Binding ownership is generation-checked: rebinding a slot bumps the
** displaced binding's generation and unbinds it, so a stale controller cannot
** drive a respawned or reassigned slot.
*/

#include <string.h>

#include "xtank.h"
#include "server.h"
#include "terminal.h"
#include "proto.h"

extern int num_terminals;
extern Terminal *terminal[];

static ServerHumanBinding humans[SERVER_MAX_HUMANS];
static int num_humans;

void
server_players_init(void)
{
	(void) memset((char *) humans, 0, sizeof(humans));
	num_humans = 0;
}

/* Find the binding with this controller id, or -1.  Full-width u32 identity:
 * a boundary value such as 0xFFFFFFFF is a distinct id, not a negative int. */
static int
find_controller(uint32_t controller)
{
	int i;

	for (i = 0; i < num_humans; i++)
		if (humans[i].used && humans[i].controller == controller)
			return i;
	return -1;
}

static int
find_name(const char *name)
{
	int i;

	if (name == NULL || name[0] == '\0')
		return -1;
	for (i = 0; i < num_humans; i++)
		if (humans[i].used && !strcmp(humans[i].name, name))
			return i;
	return -1;
}

int
server_players_add(const char *name, uint32_t controller)
{
	ServerHumanBinding *h;
	int i;

	if (name == NULL || name[0] == '\0')
		return -1;
	if (num_humans >= SERVER_MAX_HUMANS)
		return -1;
	if (find_name(name) >= 0 || find_controller(controller) >= 0)
		return -1;

	i = num_humans++;
	h = &humans[i];
	h->used = 1;
	h->controller = controller;
	h->generation = 1;
	h->slot = -1;
	(void) strncpy(h->name, name, MAX_STRING);
	h->name[MAX_STRING] = '\0';
	return i;
}

int
server_players_count(void)
{
	return num_humans;
}

const ServerHumanBinding *
server_players_binding(int index)
{
	if (index < 0 || index >= num_humans || !humans[index].used)
		return (const ServerHumanBinding *) NULL;
	return &humans[index];
}

int
server_players_bind_slot(int index, int slot)
{
	int i;

	if (index < 0 || index >= num_humans || !humans[index].used)
		return -1;
	if (slot < 0 || slot >= MAX_VEHICLES)
		return -1;

	/* Displace any other binding already owning the slot: bump its
	   generation and unbind it so the old controller is invalidated. */
	for (i = 0; i < num_humans; i++) {
		if (i != index && humans[i].used && humans[i].slot == slot) {
			humans[i].generation++;
			humans[i].slot = -1;
		}
	}

	humans[index].slot = slot;
	humans[index].generation++;
	return 0;
}

void
server_players_unbind_slot(int slot)
{
	int i;

	for (i = 0; i < num_humans; i++) {
		if (humans[i].used && humans[i].slot == slot) {
			humans[i].generation++;
			humans[i].slot = -1;
		}
	}
}

int
server_players_claim_name(const char *name, int slot)
{
	int i = find_name(name);

	if (i < 0)
		return -1;
	if (humans[i].slot != slot)
		(void) server_players_bind_slot(i, slot);
	return i;
}

int
server_players_index_for_player(int player)
{
	if (player < 0 || player >= num_humans || !humans[player].used)
		return -1;
	return player;
}

const char *
server_players_name_for_slot(int slot)
{
	int i;

	for (i = 0; i < num_humans; i++)
		if (humans[i].used && humans[i].slot == slot)
			return humans[i].name;
	return "";
}

int
server_players_validate(uint32_t controller, int slot, unsigned int generation)
{
	int i = find_controller(controller);

	if (i < 0)
		return 0;
	if (humans[i].slot != slot)
		return 0;
	if (generation != (unsigned int) -1 &&
		humans[i].generation != generation)
		return 0;
	return 1;
}

int
server_players_active(void)
{
	return num_humans > 0;
}

void *
server_player_terminal(int player_index)
{
	if (player_index >= 0 && player_index < num_terminals &&
		terminal[player_index] != (Terminal *) NULL)
		return (void *) terminal[player_index];
	return (void *) NULL;
}
