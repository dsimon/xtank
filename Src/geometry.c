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
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
** geometry.c -- headless geometry initialization (P1.10).
**
** This translation unit owns the parts of object setup that the simulation
** needs but that have nothing to do with video or pixmaps:
**
**   - the body ordering (vehicle_obj/turret_obj/exp_obj/bullet_obj/
**     landmark_obj/random_obj and their counts);
**   - the 16 vehicle rotations and the resulting picture bounds/offsets;
**   - the turret coordinates and collision segments (read-only data from
**     Display/Objects/*.obj, exposed through Object.picinfo);
**   - a stable, backend-independent text dump of all of the above.
**
** The body/bitmap registry (geometry_objects[]) lives in Display/objects.c
** because that is the only translation unit in which the static .obj names
** are visible.  Everything here operates on that registry and allocates
** nothing but the rotated bitmaps; the X pixmap upload is performed
** separately by the front-end backend (Display/x11.c make_picture, or the
** null backend), never by this file.
**
** Behaviour is unchanged: the rotation math is moved verbatim from the
** front-end backend, and the ordering is the same sequence the legacy
** make_objects() used.
*/

#include <stdlib.h>
#include <string.h>

#include "tanklimits.h"
#include "tanktypes.h"
#include "xtank.h"
#include "graphics.h"
#include "gr.h"
#include "object.h"
#include "proto.h"

/*
** The legacy rotation loops were only ever run once per process; guard them
** so an explicit headless geometry init (P1.10) cannot rotate a body twice if
** the legacy main() path also runs.
*/
static int geometry_rotated = 0;

/* ------------------------------------------------------------------ */
/* Object ordering                                                     */
/* ------------------------------------------------------------------ */

void
geometry_order_objects(void)
{
	const GeometryObject *g;
	int i;
	int nv = 0, nt = 0, ne = 0, nb = 0, nl = 0, nr = 0;

	for (i = 0; i < geometry_num_objects; i++) {
		g = &geometry_objects[i];
		switch (g->kind) {
		  case GEOM_VEHICLE:
			vehicle_obj[nv++] = g->obj;
			break;
		  case GEOM_TURRET:
			turret_obj[nt++] = g->obj;
			break;
		  case GEOM_EXPLOSION:
			exp_obj[ne++] = g->obj;
			break;
		  case GEOM_BULLET:
			bullet_obj[nb++] = g->obj;
			break;
		  case GEOM_LANDMARK:
			landmark_obj[nl++] = g->obj;
			break;
		  case GEOM_RANDOM:
			random_obj[nr++] = g->obj;
			break;
		}
	}

	num_vehicle_objs = nv;
	num_turret_objs = nt;
	num_exp_objs = ne;
	num_bullet_objs = nb;
	num_landmark_objs = nl;
	num_random_objs = nr;
}

/* ------------------------------------------------------------------ */
/* Pure rotations                                                      */
/* ------------------------------------------------------------------ */

/* Lookup table to reverse the bits in a byte */
static unsigned char
geometry_reverse_byte[256] =
{
	0x00, 0x80, 0x40, 0xc0, 0x20, 0xa0, 0x60, 0xe0, 0x10, 0x90, 0x50, 0xd0, 0x30, 0xb0, 0x70,
	0xf0, 0x08, 0x88, 0x48, 0xc8, 0x28, 0xa8, 0x68, 0xe8, 0x18, 0x98, 0x58, 0xd8, 0x38, 0xb8,
	0x78, 0xf8, 0x04, 0x84, 0x44, 0xc4, 0x24, 0xa4, 0x64, 0xe4, 0x14, 0x94, 0x54, 0xd4, 0x34,
	0xb4, 0x74, 0xf4, 0x0c, 0x8c, 0x4c, 0xcc, 0x2c, 0xac, 0x6c, 0xec, 0x1c, 0x9c, 0x5c, 0xdc,
	0x3c, 0xbc, 0x7c, 0xfc, 0x02, 0x82, 0x42, 0xc2, 0x22, 0xa2, 0x62, 0xe2, 0x12, 0x92, 0x52,
	0xd2, 0x32, 0xb2, 0x72, 0xf2, 0x0a, 0x8a, 0x4a, 0xca, 0x2a, 0xaa, 0x6a, 0xea, 0x1a, 0x9a,
	0x5a, 0xda, 0x3a, 0xba, 0x7a, 0xfa, 0x06, 0x86, 0x46, 0xc6, 0x26, 0xa6, 0x66, 0xe6, 0x16,
	0x96, 0x56, 0xd6, 0x36, 0xb6, 0x76, 0xf6, 0x0e, 0x8e, 0x4e, 0xce, 0x2e, 0xae, 0x6e, 0xee,
	0x1e, 0x9e, 0x5e, 0xde, 0x3e, 0xbe, 0x7e, 0xfe, 0x01, 0x81, 0x41, 0xc1, 0x21, 0xa1, 0x61,
	0xe1, 0x11, 0x91, 0x51, 0xd1, 0x31, 0xb1, 0x71, 0xf1, 0x09, 0x89, 0x49, 0xc9, 0x29, 0xa9,
	0x69, 0xe9, 0x19, 0x99, 0x59, 0xd9, 0x39, 0xb9, 0x79, 0xf9, 0x05, 0x85, 0x45, 0xc5, 0x25,
	0xa5, 0x65, 0xe5, 0x15, 0x95, 0x55, 0xd5, 0x35, 0xb5, 0x75, 0xf5, 0x0d, 0x8d, 0x4d, 0xcd,
	0x2d, 0xad, 0x6d, 0xed, 0x1d, 0x9d, 0x5d, 0xdd, 0x3d, 0xbd, 0x7d, 0xfd, 0x03, 0x83, 0x43,
	0xc3, 0x23, 0xa3, 0x63, 0xe3, 0x13, 0x93, 0x53, 0xd3, 0x33, 0xb3, 0x73, 0xf3, 0x0b, 0x8b,
	0x4b, 0xcb, 0x2b, 0xab, 0x6b, 0xeb, 0x1b, 0x9b, 0x5b, 0xdb, 0x3b, 0xbb, 0x7b, 0xfb, 0x07,
	0x87, 0x47, 0xc7, 0x27, 0xa7, 0x67, 0xe7, 0x17, 0x97, 0x57, 0xd7, 0x37, 0xb7, 0x77, 0xf7,
	0x0f, 0x8f, 0x4f, 0xcf, 0x2f, 0xaf, 0x6f, 0xef, 0x1f, 0x9f, 0x5f, 0xdf, 0x3f, 0xbf, 0x7f,
	0xff
};

/*
** Puts a copy of pic rotated 90 degrees into rot_pic.
** Moved verbatim from the front-end backend; touches no video state.
*/
Byte *
geometry_rotate_pic_90(Picture *pic, Picture *rot_pic, Byte *bitmap)
{
	Byte *rot_bitmap;
	int by_line, rot_by_line, num_bytes, rot_num_bytes;
	int itemp1, itemp2;
	register Byte *s_ptr, *d_ptr, *s_ptr_begin, *d_ptr_begin;
	register Byte d_mask;
	register int s_bit_begin, d_bit;

	rot_pic->width = pic->height;
	rot_pic->height = pic->width;
	rot_pic->offset_x = pic->height - pic->offset_y - 1;
	rot_pic->offset_y = pic->offset_x;

	by_line = (pic->width + 7) >> 3;
	num_bytes = by_line * pic->height;
	rot_by_line = (rot_pic->width + 7) >> 3;
	rot_num_bytes = rot_by_line * rot_pic->height;
	rot_bitmap = (Byte *) calloc((unsigned) (rot_num_bytes + 4), sizeof(Byte));

	/* Scan across each source scanline in the bitmap starting from the bottom
	   right corner and working left and up. */
	s_ptr_begin = bitmap + num_bytes - 1;
	s_bit_begin = (pic->width - 1) & 0x7;
	d_ptr_begin = rot_bitmap + rot_num_bytes - rot_by_line;
	d_bit = 0;
	s_ptr = s_ptr_begin;
	do {
		d_ptr = d_ptr_begin;
		d_mask = 1 << d_bit;
		s_ptr_begin -= by_line;

		itemp1 = -99;
		itemp2 = s_bit_begin;
		do {
			if (itemp1 != -99)
				d_ptr -= rot_by_line;	/* happens after case 0 */

			for (itemp1 = itemp2; itemp1 >= 0; itemp1--) {
				if (*s_ptr & (1 << itemp1)) {
					*d_ptr |= d_mask;
				}
				if (itemp1) {
					d_ptr -= rot_by_line;	/* happens after case 0 */
				} else {
					itemp2 = 7;
				}
			}
		}
		while (--s_ptr > s_ptr_begin);

		if (++d_bit & 1 << 3) {
			d_bit = 0;
			d_ptr_begin++;
		}
	} while (s_ptr >= bitmap);

	return rot_bitmap;
}

/*
** Puts a copy of pic rotated 180 degrees into rot_pic.
** Moved verbatim from the front-end backend; touches no video state.
*/
Byte *
geometry_rotate_pic_180(Picture *pic, Picture *rot_pic, Byte *bitmap)
{
	Byte *rot_bitmap;
	int num_bytes;
	register Byte *d_ptr;
	register Byte rev, dest;
	register int rshift, lshift;

	rot_pic->width = pic->width;
	rot_pic->height = pic->height;
	rot_pic->offset_x = pic->width - pic->offset_x - 1;
	rot_pic->offset_y = pic->height - pic->offset_y - 1;

	num_bytes = ((pic->width + 7) >> 3) * pic->height;
	rot_bitmap = (Byte *) malloc((unsigned) (num_bytes * sizeof(Byte)));

	/* Scan across each source scanline in the bitmap starting from the
	   bottom right corner and working left and up. Reverse the source
	   bytes, shift, and OR into the destination. */
	lshift = pic->width & 0x7;
	rshift = 8 - lshift;
	d_ptr = rot_bitmap;

	/* Special case aligned rotation */
	if (lshift == 0) {
		while (--num_bytes >= 0)
			*d_ptr++ = geometry_reverse_byte[bitmap[num_bytes]];
	} else {
		dest = geometry_reverse_byte[bitmap[--num_bytes]] >> rshift;
		while (--num_bytes >= 0) {
			rev = geometry_reverse_byte[bitmap[num_bytes]];
			dest |= rev << lshift;
			*d_ptr++ = dest;
			dest = rev >> rshift;
		}
		*d_ptr = dest;
	}

	return rot_bitmap;
}

/*
** Creates the 12 missing rotations from the first 4 rotations for every
** vehicle object, by 90 and 180 degree rotations.
*/
void
geometry_rotate_object(Object *obj, Bits **bitmap)
{
	Picture *pic, *rot_pic;
	int dest, source;

	/* Create next 4 pictures by rotating the first 4 by 90 degrees */
	for (source = 0; source < 4; source++) {
		dest = source + 4;
		pic = &obj->pic[source];
		rot_pic = &obj->pic[dest];
		bitmap[dest] = (Bits *) geometry_rotate_pic_90(pic, rot_pic,
													   bitmap[source]);
	}

	/* Create last 8 pictures by rotating the first 8 by 180 degrees */
	for (source = 0; source < 8; source++) {
		dest = source + 8;
		pic = &obj->pic[source];
		rot_pic = &obj->pic[dest];
		bitmap[dest] = (Bits *) geometry_rotate_pic_180(pic, rot_pic,
														bitmap[source]);
	}
}

void
geometry_rotate_objects(void)
{
	const GeometryObject *g;
	int i;

	if (geometry_rotated)
		return;
	geometry_rotated = 1;

	for (i = 0; i < geometry_num_objects; i++) {
		g = &geometry_objects[i];
		if (g->kind == GEOM_VEHICLE && g->bitmap != NULL)
			geometry_rotate_object(g->obj, g->bitmap);
	}
}

/* ------------------------------------------------------------------ */
/* Backend pixmap upload (separated from the pure geometry above)      */
/* ------------------------------------------------------------------ */

void
geometry_upload_objects(void)
{
	const GeometryObject *g;
	int i, j;

	for (i = 0; i < geometry_num_objects; i++) {
		g = &geometry_objects[i];
		if (g->bitmap == NULL)
			continue;			/* pulse-laser alias: no pictures */
		for (j = 0; j < g->obj->num_pics; j++) {
			if (make_picture(&g->obj->pic[j], (char *) g->bitmap[j]))
				object_error = 1;
		}
	}
}

/* ------------------------------------------------------------------ */
/* Explicit, backend-independent geometry table                        */
/* ------------------------------------------------------------------ */

static const char *
geometry_kind_name(int kind)
{
	switch (kind) {
	  case GEOM_VEHICLE:	return "vehicle";
	  case GEOM_TURRET:		return "turret";
	  case GEOM_EXPLOSION:	return "explosion";
	  case GEOM_BULLET:		return "bullet";
	  case GEOM_LANDMARK:	return "landmark";
	  case GEOM_RANDOM:		return "random";
	}
	return "?";
}

void
geometry_dump(FILE *fp)
{
	const GeometryObject *g;
	const Picinfo *pi;
	const Segment *s;
	int i, r, t, k;
	int nv = 0, nt = 0, ne = 0, nb = 0, nl = 0, nr = 0;

	for (i = 0; i < geometry_num_objects; i++) {
		switch (geometry_objects[i].kind) {
		  case GEOM_VEHICLE:	nv++; break;
		  case GEOM_TURRET:		nt++; break;
		  case GEOM_EXPLOSION:	ne++; break;
		  case GEOM_BULLET:		nb++; break;
		  case GEOM_LANDMARK:	nl++; break;
		  case GEOM_RANDOM:		nr++; break;
		}
	}

	fprintf(fp, "GEOMETRY-TABLE v1\n");
	fprintf(fp, "count vehicle %d turret %d explosion %d bullet %d "
			"landmark %d random %d\n", nv, nt, ne, nb, nl, nr);

	int idx[6];
	memset(idx, 0, sizeof(idx));

	for (i = 0; i < geometry_num_objects; i++) {
		g = &geometry_objects[i];
		fprintf(fp, "obj %s %d type %s pics %d turrets %d segs %d\n",
				geometry_kind_name(g->kind), idx[g->kind],
				g->obj->type, g->obj->num_pics, g->obj->num_turrets,
				g->obj->num_segs);
		idx[g->kind]++;

		for (r = 0; r < g->obj->num_pics; r++) {
			Picture *p = &g->obj->pic[r];

			fprintf(fp, "  pic %d w %d h %d ox %d oy %d\n",
					r, p->width, p->height, p->offset_x, p->offset_y);

			if (g->obj->picinfo == NULL)
				continue;
			pi = &g->obj->picinfo[r];

			for (t = 0; t < g->obj->num_turrets; t++)
				fprintf(fp, "    turret %d x %d y %d\n", t,
						(int) pi->turret_coord[t].x,
						(int) pi->turret_coord[t].y);

			for (k = 0; k < g->obj->num_segs; k++) {
				s = &pi->segment[k];
				fprintf(fp, "    seg %d x1 %d y1 %d x2 %d y2 %d "
						"dx %d dy %d slope %.6f intercept %d "
						"minx %d miny %d maxx %d maxy %d\n",
						k, s->x1, s->y1, s->x2, s->y2, s->dx, s->dy,
						(double) s->slope, s->intercept,
						s->minx, s->miny, s->maxx, s->maxy);
			}
		}
	}
}

/*
** Harness hook: when XTANK_GEOMETRY_DUMP names a file, write the explicit
** geometry table there.  With XTANK_GEOMETRY_EXIT set the process stops right
** after the dump, so a front-end binary can be measured without entering a
** match.  Unset, this is a no-op and no behaviour changes.
*/
int
geometry_test_dump(void)
{
	const char *path = getenv("XTANK_GEOMETRY_DUMP");
	FILE *fp;

	if (path == NULL || *path == '\0')
		return 0;

	fp = fopen(path, "w");
	if (fp == NULL) {
		fprintf(stderr, "xtank: cannot open XTANK_GEOMETRY_DUMP=%s\n", path);
		return -1;
	}
	geometry_dump(fp);
	fclose(fp);

	if (getenv("XTANK_GEOMETRY_EXIT") != NULL)
		exit(0);

	return 0;
}
