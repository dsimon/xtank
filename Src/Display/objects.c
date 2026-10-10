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

#include "tanklimits.h"
#include "tanktypes.h"
#include "xtank.h"
#include "graphics.h"
#include "gr.h"
#include "object.h"
#include "proto.h"

#include "Objects/lightc.obj"
#include "Objects/hexo.obj"
#include "Objects/spider.obj"
#include "Objects/psycho.obj"
#include "Objects/tornado.obj"
#include "Objects/marauder.obj"
#include "Objects/tiger.obj"
#include "Objects/rhino.obj"
#include "Objects/medusa.obj"
#include "Objects/malice.obj"
#include "Objects/trike.obj"
#include "Objects/panzy.obj"
#include "Objects/disk.obj"
#include "Objects/delta.obj"

#include "Objects/turret_sm.obj"

#include "Objects/shock.obj"
#include "Objects/gleam.obj"
#include "Objects/tink.obj"
#include "Objects/soft.obj"
#include "Objects/circle.obj"
#include "Objects/fiery.obj"
#include "Objects/double.obj"
#include "Objects/exhaust.obj"
#include "Objects/electric.obj"
#include "Objects/bullets.obj"
#include "Objects/nuke.obj"

#include "Objects/anm_lmarks.obj"
#include "Objects/map_lmarks.obj"
#include "Objects/des_lmarks.obj"

#include "Objects/xtank.obj"
#include "Objects/team.obj"
#include "Objects/terp.obj"

int num_vehicle_objs;
Object *vehicle_obj[MAX_VEHICLE_OBJS];

int num_turret_objs;
Object *turret_obj[MAX_TURRET_OBJS];

int num_exp_objs;
Object *exp_obj[MAX_EXP_OBJS];

int num_bullet_objs;   /* NUM_BULLET_OBJS defined in bullets.obj */
Object *bullet_obj[NUM_BULLET_OBJS];

int num_landmark_objs;
Object *landmark_obj[MAX_LANDMARK_OBJS];

int num_random_objs;
Object *random_obj[MAX_RANDOM_OBJS];

int object_error;

/*
** P1.10 -- the body/bitmap registry.
**
** This is the single place the static Objects and bitmap arrays from
** Display/Objects/*.obj are visible.  Src/geometry.c consumes it to order
** the bodies, rotate the 16 vehicle views and dump the geometry, all without
** touching the video backend.  The sequence and indices below are exactly the
** legacy make_objects() ordering (P1.5 behaviour), including the pulse-laser
** bullet alias at index 18.
*/
const GeometryObject geometry_objects[] = {
	/* Make all of the vehicle objects (HAK 2/93 order) */
	{ GEOM_VEHICLE, &lightc_obj, lightc_bitmap },
	{ GEOM_VEHICLE, &trike_obj, trike_bitmap },
	{ GEOM_VEHICLE, &hexo_obj, hexo_bitmap },
	{ GEOM_VEHICLE, &spider_obj, spider_bitmap },
	{ GEOM_VEHICLE, &psycho_obj, psycho_bitmap },
	{ GEOM_VEHICLE, &tornado_obj, tornado_bitmap },
	{ GEOM_VEHICLE, &marauder_obj, marauder_bitmap },
	{ GEOM_VEHICLE, &tiger_obj, tiger_bitmap },
	{ GEOM_VEHICLE, &rhino_obj, rhino_bitmap },
	{ GEOM_VEHICLE, &medusa_obj, medusa_bitmap },
	{ GEOM_VEHICLE, &delta_obj, delta_bitmap },
	{ GEOM_VEHICLE, &disk_obj, disk_bitmap },
	{ GEOM_VEHICLE, &malice_obj, malice_bitmap },
	{ GEOM_VEHICLE, &panzy_obj, panzy_bitmap },

	/* Make all of the turret objects */
	{ GEOM_TURRET, &turret_sm_obj, turret_sm_bitmap },

	/* Make all of the explosion objects */
	{ GEOM_EXPLOSION, &shock_obj, shock_bitmap },
	{ GEOM_EXPLOSION, &gleam_obj, gleam_bitmap },
	{ GEOM_EXPLOSION, &tink_obj, tink_bitmap },
	{ GEOM_EXPLOSION, &soft_obj, soft_bitmap },
	{ GEOM_EXPLOSION, &circle_obj, circle_bitmap },
	{ GEOM_EXPLOSION, &fiery_obj, fiery_bitmap },
	{ GEOM_EXPLOSION, &double_obj, double_bitmap },
	{ GEOM_EXPLOSION, &exhaust_obj, exhaust_bitmap },
	{ GEOM_EXPLOSION, &electric_obj, electric_bitmap },	/*GHS*/
	{ GEOM_EXPLOSION, &nuke_obj, nuke_bitmap },

	/* Make the bullet objects */
	{ GEOM_BULLET, &lmg_obj, lmg_bitmap },
	{ GEOM_BULLET, &mg_obj, mg_bitmap },
	{ GEOM_BULLET, &hmg_obj, hmg_bitmap },
	{ GEOM_BULLET, &lac_obj, lac_bitmap },
	{ GEOM_BULLET, &ac_obj, ac_bitmap },
	{ GEOM_BULLET, &hac_obj, hac_bitmap },
	{ GEOM_BULLET, &lrl_obj, lrl_bitmap },
	{ GEOM_BULLET, &rl_obj, rl_bitmap },
	{ GEOM_BULLET, &hrl_obj, hrl_bitmap },
	{ GEOM_BULLET, &as_obj, as_bitmap },
	{ GEOM_BULLET, &ft_obj, ft_bitmap },
	{ GEOM_BULLET, &seek_obj, seek_bitmap },
	{ GEOM_BULLET, &pr_obj, pr_bitmap },
	{ GEOM_BULLET, &um_obj, um_bitmap },
	{ GEOM_BULLET, &tele_obj, tele_bitmap },
	{ GEOM_BULLET, &tow_obj, tow_bitmap },
	{ GEOM_BULLET, &ltorp_obj, ltorp_bitmap },
	{ GEOM_BULLET, &bc_obj, bc_bitmap },
	{ GEOM_BULLET, &lmg_obj, NULL },	/* pulse laser, no bitmap */
	{ GEOM_BULLET, &mine_obj, mine_bitmap },
	{ GEOM_BULLET, &slick_obj, slick_bitmap },
	{ GEOM_BULLET, &mort_obj, mort_bitmap },
	{ GEOM_BULLET, &tn_obj, tn_bitmap },
	{ GEOM_BULLET, &harm_obj, harm_bitmap },
	{ GEOM_BULLET, &disc_obj, disc_bitmap },

	/* Make the landmark objects */
	{ GEOM_LANDMARK, &anim_landmarks_obj, anim_landmarks_bitmap },
	{ GEOM_LANDMARK, &map_landmarks_obj, map_landmarks_bitmap },
	{ GEOM_LANDMARK, &design_landmarks_obj, design_landmarks_bitmap },

	/* Make the random objects */
	{ GEOM_RANDOM, &xtank_obj, xtank_bitmap },
	{ GEOM_RANDOM, &team_obj, team_bitmap },
	{ GEOM_RANDOM, &terp_obj, terp_bitmap },
};

const int geometry_num_objects =
	sizeof(geometry_objects) / sizeof(geometry_objects[0]);

/*
** Sets up the body ordering and all of the pixmaps in each object.
** The ordering and rotations (geometry.c) require no video backend; only the
** pixmap upload touches the backend and is fully no-op under the null build.
** Returns 1 if a pixmap could not be made.
*/
int
make_objects(void)
{
	/* Clear the error flag */
	object_error = 0;

	/* Order the bodies without allocating anything (P1.10). */
	geometry_order_objects();

	if (num_exp_objs != MAX_EXP_OBJS)
		printf("Yikes!!!!  MAX_EXP_OBJS mismatched!\n");
	if (num_bullet_objs != NUM_BULLET_OBJS)
		printf("Yikes!!!!  NUM_BULLET_OBJS mismatched!\n");

	/* Upload the pictures through the backend (X pixmaps; no-op under null). */
	geometry_upload_objects();

	/* Harness-only: emit the explicit geometry table if requested. */
	(void) geometry_test_dump();

	return object_error;
}

/*
** Makes all the pictures for the specified object.
*/
Object *
make_object(Object *obj, Bits **bitmap)
{
	int i;

	for (i = 0; i < obj->num_pics; i++) {
		if (make_picture(&obj->pic[i], (char *) bitmap[i])) {
			object_error = 1;
		}
	}
	return (obj);
}

/*
** Frees all the storage used by the objects.
*/
void
free_objects(void)
{
	int i;

	for (i = 0; i < num_vehicle_objs; i++)
		free_object(vehicle_obj[i]);

	for (i = 0; i < num_turret_objs; i++)
		free_object(turret_obj[i]);

	for (i = 0; i < num_exp_objs; i++)
		free_object(exp_obj[i]);

	free_object(bullet_obj);

	for (i = 0; i < num_landmark_objs; i++)
		free_object(landmark_obj[i]);

	for (i = 0; i < num_random_objs; i++)
		free_object(random_obj[i]);
}

/*
** Frees all the pictures used by the object.
*/
void
free_object(Object *obj)
{
	int i;

	for (i = 0; i < obj->num_pics; i++)
		free_picture(&obj->pic[i]);
}

/*
** Rotates all the vehicle objects so that all 16 rotations exist.
** The pure rotation math lives in Src/geometry.c (P1.10); this only enters
** the shared geometry module so the legacy call site in main() is unchanged.
*/
void
rotate_objects(void)
{
	geometry_rotate_objects();
}
