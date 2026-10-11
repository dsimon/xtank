/*
 * Tools/export-client-assets.c -- portable checked export of the original Xtank
 * visuals into the P2.10 client asset bundle.
 *
 * This host tool reads the read-only body descriptions in
 * Src/Display/Objects/<name>.obj (exactly the registry Display/objects.c uses),
 * derives the 16 vehicle rotations with the same 90/180-degree math as
 * Src/geometry.c, and emits:
 *
 *   --out BUNDLE      the portable little-endian asset bundle consumed by
 *                     Client/assets.c;
 *   --geometry TABLE  the explicit geometry table in the exact
 *                     Src/geometry.c geometry_dump() format, so the exported
 *                     bundle can be compared byte-for-byte with the frozen
 *                     X/server geometry evidence;
 *   --manifest FILE   a short human-readable inventory.
 *
 * It links no simulation, no graphics backend and no X11: the only engine
 * headers it includes are the data-only object descriptions.  The simulation
 * collision assets are read, never modified.
 *
 * Authority: docs/PHASE2-PLAN.md P2.10; Src/display.c and Src/geometry.c are
 * read-only references for the draw offsets/rotation indexing.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tanklimits.h"
#include "tanktypes.h"
#include "vehicleparts.h"
#include "object.h"

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

#include "../Client/render.h"

/* ---- The body registry (identical order/indices to objects.c) -------- */

typedef struct {
	int kind;		/* GEOM_* (reuse object.h constants) */
	Object *obj;
	Bits **bitmap;		/* NULL = alias (pulse laser) */
} GeoEntry;

#define GEOM_VEHICLE	0
#define GEOM_TURRET	1
#define GEOM_EXPLOSION	2
#define GEOM_BULLET	3
#define GEOM_LANDMARK	4
#define GEOM_RANDOM	5

static const GeoEntry registry[] = {
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

	{ GEOM_TURRET, &turret_sm_obj, turret_sm_bitmap },

	{ GEOM_EXPLOSION, &shock_obj, shock_bitmap },
	{ GEOM_EXPLOSION, &gleam_obj, gleam_bitmap },
	{ GEOM_EXPLOSION, &tink_obj, tink_bitmap },
	{ GEOM_EXPLOSION, &soft_obj, soft_bitmap },
	{ GEOM_EXPLOSION, &circle_obj, circle_bitmap },
	{ GEOM_EXPLOSION, &fiery_obj, fiery_bitmap },
	{ GEOM_EXPLOSION, &double_obj, double_bitmap },
	{ GEOM_EXPLOSION, &exhaust_obj, exhaust_bitmap },
	{ GEOM_EXPLOSION, &electric_obj, electric_bitmap },
	{ GEOM_EXPLOSION, &nuke_obj, nuke_bitmap },

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
	{ GEOM_BULLET, &lmg_obj, NULL },	/* pulse laser alias */
	{ GEOM_BULLET, &mine_obj, mine_bitmap },
	{ GEOM_BULLET, &slick_obj, slick_bitmap },
	{ GEOM_BULLET, &mort_obj, mort_bitmap },
	{ GEOM_BULLET, &tn_obj, tn_bitmap },
	{ GEOM_BULLET, &harm_obj, harm_bitmap },
	{ GEOM_BULLET, &disc_obj, disc_bitmap },

	{ GEOM_LANDMARK, &anim_landmarks_obj, anim_landmarks_bitmap },
	{ GEOM_LANDMARK, &map_landmarks_obj, map_landmarks_bitmap },
	{ GEOM_LANDMARK, &design_landmarks_obj, design_landmarks_bitmap },

	{ GEOM_RANDOM, &xtank_obj, xtank_bitmap },
	{ GEOM_RANDOM, &team_obj, team_bitmap },
	{ GEOM_RANDOM, &terp_obj, terp_bitmap },
};

static const int registry_count = (int)(sizeof(registry) / sizeof(registry[0]));

/* ---- Rotation math (verbatim from Src/geometry.c) -------------------- */

static unsigned char reverse_byte[256] = {
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

static Byte *rot90(Picture *pic, Picture *rot_pic, Byte *bitmap)
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
	rot_bitmap = (Byte *)calloc((unsigned)(rot_num_bytes + 4), sizeof(Byte));

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
				d_ptr -= rot_by_line;
			for (itemp1 = itemp2; itemp1 >= 0; itemp1--) {
				if (*s_ptr & (1 << itemp1))
					*d_ptr |= d_mask;
				if (itemp1)
					d_ptr -= rot_by_line;
				else
					itemp2 = 7;
			}
		} while (--s_ptr > s_ptr_begin);

		if (++d_bit & 1 << 3) {
			d_bit = 0;
			d_ptr_begin++;
		}
	} while (s_ptr >= bitmap);

	return rot_bitmap;
}

static Byte *rot180(Picture *pic, Picture *rot_pic, Byte *bitmap)
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
	rot_bitmap = (Byte *)malloc((unsigned)(num_bytes * sizeof(Byte)));

	lshift = pic->width & 0x7;
	rshift = 8 - lshift;
	d_ptr = rot_bitmap;

	if (lshift == 0) {
		while (--num_bytes >= 0)
			*d_ptr++ = reverse_byte[bitmap[num_bytes]];
	} else {
		dest = reverse_byte[bitmap[--num_bytes]] >> rshift;
		while (--num_bytes >= 0) {
			rev = reverse_byte[bitmap[num_bytes]];
			dest |= rev << lshift;
			*d_ptr++ = dest;
			dest = rev >> rshift;
		}
		*d_ptr = dest;
	}
	return rot_bitmap;
}

/* ---- Little-endian writers ------------------------------------------ */

static int put_u16(FILE *f, uint16_t v)
{
	return fputc(v & 0xFF, f) == EOF || fputc((v >> 8) & 0xFF, f) == EOF;
}

static int put_i16(FILE *f, int16_t v)
{
	return put_u16(f, (uint16_t)v);
}

static int put_u32(FILE *f, uint32_t v)
{
	int i;
	for (i = 0; i < 4; i++)
		if (fputc((v >> (8 * i)) & 0xFF, f) == EOF)
			return 1;
	return 0;
}

static int put_i32(FILE *f, int32_t v)
{
	return put_u32(f, (uint32_t)v);
}

static int put_f64(FILE *f, double d)
{
	uint64_t bits;
	int i;
	memcpy(&bits, &d, 8);
	for (i = 0; i < 8; i++)
		if (fputc((bits >> (8 * i)) & 0xFF, f) == EOF)
			return 1;
	return 0;
}

static int put_bytes(FILE *f, const void *p, size_t n)
{
	return fwrite(p, 1, n, f) != n;
}

/* ---- Palette / font ------------------------------------------------- */

typedef struct { uint8_t idx, r, g, b; const char *name; } PalEntry;
typedef struct { uint8_t idx, w, h, asc; const char *name; } FontEntry;

/* Resolved sRGB of the legacy X11 color names (Src/Display/x11.c colorname[]),
 * cross-checked against /usr/share/X11/rgb.txt by Tests/phase2/assets.sh. */
static const PalEntry palette[] = {
	{ 0,  0,   0,   0,   "Black" },	/* BLACK     */
	{ 1,  255, 255, 255, "White" },	/* WHITE     */
	{ 2,  255, 0,   0,   "Red" },	/* RED       */
	{ 3,  255, 161, 68,  "#ffa144" },	/* ORANGE    */
	{ 4,  255, 255, 0,   "Yellow" },	/* YELLOW    */
	{ 5,  0,   255, 0,   "Green" },	/* GREEN     */
	{ 6,  135, 157, 255, "#879dff" },	/* BLUE      */
	{ 7,  218, 112, 214, "orchid" },	/* VIOLET    */
	{ 8,  181, 181, 181, "grey71" },	/* GREY      */
	{ 9,  0,   255, 255, "cyan" },	/* CUR_COLOR */
	{ 10, 255, 255, 255, "White" },	/* DASHED    */
};
static const int palette_count = (int)(sizeof(palette) / sizeof(palette[0]));

/*
 * Font identities from Src/Display/x11.c fontname[] (non-X11R2 selection).
 * Exact X font glyphs/metrics are backend-owned and not portable, so only the
 * identity and a nominal pixel height derived from the X font name are
 * exported; the client's text font is P2.13's concern.
 */
static const FontEntry fonts[] = {
	{ 0, 0, 8,  6,  "-misc-fixed-medium-r-*-*-8-*" },	/* T_FONT  */
	{ 1, 6, 10, 8,  "6x10" },				/* S_FONT  */
	{ 2, 8, 14, 11, "*-courier-bold-r-*-14-*" },		/* M_FONT  */
	{ 3, 9, 18, 14, "*-courier-bold-r-*-18-*" },		/* L_FONT  */
	{ 4, 12, 24, 19, "*-courier-bold-r-*-24-*" },		/* XL_FONT */
};
static const int font_count = (int)(sizeof(fonts) / sizeof(fonts[0]));

/* ---- Object assembly ------------------------------------------------- */

static void copy_pic(XtkPicture *dst, const Picture *p, const Byte *bitmap)
{
	dst->width = p->width;
	dst->height = p->height;
	dst->offset_x = p->offset_x;
	dst->offset_y = p->offset_y;
	if (bitmap != NULL && p->width > 0 && p->height > 0) {
		dst->bitmap_len = (uint32_t)(((p->width + 7) >> 3) * p->height);
		dst->bitmap = (uint8_t *)malloc(dst->bitmap_len);
		memcpy(dst->bitmap, bitmap, dst->bitmap_len);
	} else {
		dst->bitmap_len = 0;
		dst->bitmap = NULL;
	}
}

static void free_object(XtkAssetObject *o)
{
	int r;
	for (r = 0; r < o->num_pics; r++) {
		free(o->pic[r].bitmap);
		o->pic[r].bitmap = NULL;
	}
}

/*
 * Build one asset object from a registry entry.  Vehicle bodies rotate the
 * four base views into the 16 legacy views exactly as geometry.c does; all
 * other objects already carry every view.  Turret coordinates and collision
 * segments are copied verbatim from Object.picinfo.
 */
static void build_object(XtkAssetObject *o, const GeoEntry *e)
{
	Object *obj = e->obj;
	int r, t, k;

	memset(o, 0, sizeof(*o));
	strncpy(o->type, obj->type, XTK_ASSET_NAME - 1);
	o->type[XTK_ASSET_NAME - 1] = '\0';
	o->num_pics = obj->num_pics;
	o->num_turrets = (obj->picinfo != NULL) ? obj->num_turrets : 0;
	o->num_segs = (obj->picinfo != NULL) ? obj->num_segs : 0;

	if (o->num_pics > XTK_ASSET_MAX_VIEWS ||
	    o->num_turrets > XTK_ASSET_MAX_TURRETS ||
	    o->num_segs > XTK_ASSET_MAX_SEGS) {
		fprintf(stderr, "export: object '%s' exceeds bundle caps "
			"(pics %d turrets %d segs %d)\n", o->type, o->num_pics,
			o->num_turrets, o->num_segs);
		exit(1);
	}

	for (r = 0; r < o->num_pics; r++) {
		const Byte *bm = (e->bitmap != NULL) ?
			(const Byte *)e->bitmap[r] : NULL;
		copy_pic(&o->pic[r], &obj->pic[r], bm);
	}

	if (e->kind == GEOM_VEHICLE && e->bitmap != NULL) {
		/* bm[0..3] alias the read-only .obj bitmaps; bm[4..7] hold the
		 * 90-degree results and feed the 180-degree pass, exactly as
		 * the mutable g->bitmap array does in geometry.c. */
		Byte *bm[16];
		int b;
		for (b = 0; b < 4; b++)
			bm[b] = (Byte *)e->bitmap[b];
		for (b = 4; b < 16; b++)
			bm[b] = NULL;
		for (r = 0; r < 4; r++) {
			Byte *rb = rot90(&obj->pic[r], &obj->pic[r + 4],
					 (Byte *)e->bitmap[r]);
			bm[r + 4] = rb;
			copy_pic(&o->pic[r + 4], &obj->pic[r + 4], rb);
		}
		for (r = 0; r < 8; r++) {
			Byte *rb = rot180(&obj->pic[r], &obj->pic[r + 8], bm[r]);
			copy_pic(&o->pic[r + 8], &obj->pic[r + 8], rb);
			free(rb);
		}
		for (r = 4; r < 8; r++)
			free(bm[r]);
	}

	if (obj->picinfo != NULL) {
		for (r = 0; r < o->num_pics; r++) {
			for (t = 0; t < o->num_turrets; t++) {
				o->turret[r][t].x =
					obj->picinfo[r].turret_coord[t].x;
				o->turret[r][t].y =
					obj->picinfo[r].turret_coord[t].y;
			}
			for (k = 0; k < o->num_segs; k++) {
				const Segment *s = &obj->picinfo[r].segment[k];
				XtkSegment *d = &o->seg[r][k];
				d->x1 = s->x1; d->y1 = s->y1;
				d->x2 = s->x2; d->y2 = s->y2;
				d->dx = s->dx; d->dy = s->dy;
				d->minx = s->minx; d->miny = s->miny;
				d->maxx = s->maxx; d->maxy = s->maxy;
				d->intercept = s->intercept;
				d->slope = (double)s->slope;
			}
		}
	}
}

/* ---- Geometry table (exact geometry_dump() format) ------------------- */

static const char *kind_name(int kind)
{
	switch (kind) {
	case GEOM_VEHICLE:	return "vehicle";
	case GEOM_TURRET:	return "turret";
	case GEOM_EXPLOSION:	return "explosion";
	case GEOM_BULLET:	return "bullet";
	case GEOM_LANDMARK:	return "landmark";
	case GEOM_RANDOM:	return "random";
	}
	return "?";
}

static void dump_geometry(const XtkAssets *a, FILE *fp)
{
	static const int cats[XTK_CAT_COUNT] = {
		XTK_CAT_VEHICLE, XTK_CAT_TURRET, XTK_CAT_EXPLOSION,
		XTK_CAT_BULLET, XTK_CAT_LANDMARK, XTK_CAT_RANDOM
	};
	int c, i, r, t, k;

	fprintf(fp, "GEOMETRY-TABLE v1\n");
	fprintf(fp, "count vehicle %d turret %d explosion %d bullet %d "
		"landmark %d random %d\n", a->num_vehicle, a->num_turret,
		a->num_explosion, a->num_bullet, a->num_landmark, a->num_random);

	for (c = 0; c < XTK_CAT_COUNT; c++) {
		int n = assets_count(a, cats[c]);
		for (i = 0; i < n; i++) {
			const XtkAssetObject *o = assets_object(a, cats[c], i);
			fprintf(fp, "obj %s %d type %s pics %d turrets %d segs %d\n",
				kind_name(cats[c]), i, o->type, o->num_pics,
				o->num_turrets, o->num_segs);
			for (r = 0; r < o->num_pics; r++) {
				fprintf(fp, "  pic %d w %d h %d ox %d oy %d\n", r,
					o->pic[r].width, o->pic[r].height,
					o->pic[r].offset_x, o->pic[r].offset_y);
				for (t = 0; t < o->num_turrets; t++)
					fprintf(fp, "    turret %d x %d y %d\n", t,
						o->turret[r][t].x,
						o->turret[r][t].y);
				for (k = 0; k < o->num_segs; k++) {
					const XtkSegment *s = &o->seg[r][k];
					fprintf(fp, "    seg %d x1 %d y1 %d x2 %d "
						"y2 %d dx %d dy %d slope %.6f "
						"intercept %d minx %d miny %d "
						"maxx %d maxy %d\n",
						k, s->x1, s->y1, s->x2, s->y2,
						s->dx, s->dy, s->slope,
						s->intercept, s->minx, s->miny,
						s->maxx, s->maxy);
				}
			}
		}
	}
}

/* ---- Bundle writer --------------------------------------------------- */

static uint32_t fnv1a(const uint8_t *p, size_t n)
{
	uint32_t h = 2166136261u;
	size_t i;
	for (i = 0; i < n; i++) {
		h ^= p[i];
		h *= 16777619u;
	}
	return h;
}

static void write_object(FILE *f, const XtkAssetObject *o)
{
	int r, t, k;
	char type[XTK_ASSET_NAME];

	memset(type, 0, sizeof(type));
	memcpy(type, o->type, XTK_ASSET_NAME);
	put_bytes(f, type, sizeof(type));
	put_u16(f, (uint16_t)o->num_pics);
	put_u16(f, (uint16_t)o->num_turrets);
	put_u16(f, (uint16_t)o->num_segs);
	put_u16(f, 0);

	for (r = 0; r < o->num_pics; r++) {
		const XtkPicture *p = &o->pic[r];
		put_i16(f, p->width);
		put_i16(f, p->height);
		put_i16(f, p->offset_x);
		put_i16(f, p->offset_y);
		put_u32(f, p->bitmap_len);
		if (p->bitmap_len)
			put_bytes(f, p->bitmap, p->bitmap_len);
	}
	for (r = 0; r < o->num_pics; r++)
		for (t = 0; t < o->num_turrets; t++) {
			put_i16(f, o->turret[r][t].x);
			put_i16(f, o->turret[r][t].y);
		}
	for (r = 0; r < o->num_pics; r++)
		for (k = 0; k < o->num_segs; k++) {
			const XtkSegment *s = &o->seg[r][k];
			put_i16(f, s->x1); put_i16(f, s->y1);
			put_i16(f, s->x2); put_i16(f, s->y2);
			put_i16(f, s->dx); put_i16(f, s->dy);
			put_i16(f, s->minx); put_i16(f, s->miny);
			put_i16(f, s->maxx); put_i16(f, s->maxy);
			put_i32(f, s->intercept);
			put_f64(f, s->slope);
		}
}

/* Serialize the payload into memory so its length and checksum are known.
 * The exporter is a Linux host tool, so POSIX open_memstream() is available. */
static uint8_t *build_payload(const XtkAssets *a, size_t *out_len)
{
	size_t len = 0;
	char *cbuf = NULL;
	uint8_t *buf;
	FILE *f;
	int c, i;
	static const int cats[XTK_CAT_COUNT] = {
		XTK_CAT_VEHICLE, XTK_CAT_TURRET, XTK_CAT_EXPLOSION,
		XTK_CAT_BULLET, XTK_CAT_LANDMARK, XTK_CAT_RANDOM
	};

	f = open_memstream(&cbuf, &len);
	if (f == NULL)
		return NULL;

	for (i = 0; i < a->num_palette; i++) {
		const XtkAssetColor *p = &a->palette[i];
		char name[XTK_ASSET_NAME];
		memset(name, 0, sizeof(name));
		memcpy(name, p->name, XTK_ASSET_NAME);
		fputc(p->index, f);
		fputc(p->r, f);
		fputc(p->g, f);
		fputc(p->b, f);
		fwrite(name, 1, sizeof(name), f);
	}
	for (i = 0; i < a->num_font; i++) {
		const XtkAssetFont *fn = &a->font[i];
		char name[XTK_ASSET_FONT_NAME];
		memset(name, 0, sizeof(name));
		memcpy(name, fn->name, XTK_ASSET_FONT_NAME);
		fputc(fn->index, f);
		fputc(fn->width, f);
		fputc(fn->height, f);
		fputc(fn->ascent, f);
		fputc(0, f); fputc(0, f); fputc(0, f); fputc(0, f);
		fwrite(name, 1, sizeof(name), f);
	}
	for (c = 0; c < XTK_CAT_COUNT; c++) {
		int n = assets_count(a, cats[c]);
		for (i = 0; i < n; i++)
			write_object(f, assets_object(a, cats[c], i));
	}
	fclose(f);
	*out_len = len;
	buf = (uint8_t *)cbuf;
	return buf;
}

/* ---- main ------------------------------------------------------------ */

int main(int argc, char **argv)
{
	const char *out_path = NULL, *geom_path = NULL, *manifest_path = NULL;
	XtkAssets assets;
	uint8_t *payload;
	size_t payload_len = 0;
	uint32_t checksum;
	FILE *f;
	int i;
	uint8_t hdr[XTK_ASSET_HEADER_BYTES];

	memset(&assets, 0, sizeof(assets));

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--out") == 0 && i + 1 < argc)
			out_path = argv[++i];
		else if (strcmp(argv[i], "--geometry") == 0 && i + 1 < argc)
			geom_path = argv[++i];
		else if (strcmp(argv[i], "--manifest") == 0 && i + 1 < argc)
			manifest_path = argv[++i];
		else {
			fprintf(stderr, "usage: %s --out BUNDLE [--geometry TABLE] "
				"[--manifest FILE]\n", argv[0]);
			return 2;
		}
	}
	if (out_path == NULL) {
		fprintf(stderr, "export-client-assets: --out is required\n");
		return 2;
	}

	/* Allocate one XtkAssetObject per registry entry, bucketed by kind. */
	{
		int counts[XTK_CAT_COUNT] = { 0, 0, 0, 0, 0, 0 };
		int idx[XTK_CAT_COUNT] = { 0, 0, 0, 0, 0, 0 };
		int kind_to_cat[6];

		kind_to_cat[GEOM_VEHICLE] = XTK_CAT_VEHICLE;
		kind_to_cat[GEOM_TURRET] = XTK_CAT_TURRET;
		kind_to_cat[GEOM_EXPLOSION] = XTK_CAT_EXPLOSION;
		kind_to_cat[GEOM_BULLET] = XTK_CAT_BULLET;
		kind_to_cat[GEOM_LANDMARK] = XTK_CAT_LANDMARK;
		kind_to_cat[GEOM_RANDOM] = XTK_CAT_RANDOM;

		for (i = 0; i < registry_count; i++)
			counts[kind_to_cat[registry[i].kind]]++;

		assets.num_vehicle = counts[XTK_CAT_VEHICLE];
		assets.num_turret = counts[XTK_CAT_TURRET];
		assets.num_explosion = counts[XTK_CAT_EXPLOSION];
		assets.num_bullet = counts[XTK_CAT_BULLET];
		assets.num_landmark = counts[XTK_CAT_LANDMARK];
		assets.num_random = counts[XTK_CAT_RANDOM];

		assets.vehicle = (XtkAssetObject *)calloc((size_t)assets.num_vehicle, sizeof(XtkAssetObject));
		assets.turret = (XtkAssetObject *)calloc((size_t)assets.num_turret, sizeof(XtkAssetObject));
		assets.explosion = (XtkAssetObject *)calloc((size_t)assets.num_explosion, sizeof(XtkAssetObject));
		assets.bullet = (XtkAssetObject *)calloc((size_t)assets.num_bullet, sizeof(XtkAssetObject));
		assets.landmark = (XtkAssetObject *)calloc((size_t)assets.num_landmark, sizeof(XtkAssetObject));
		assets.random = (XtkAssetObject *)calloc((size_t)assets.num_random, sizeof(XtkAssetObject));

		for (i = 0; i < registry_count; i++) {
			int cat = kind_to_cat[registry[i].kind];
			XtkAssetObject *arr = (cat == XTK_CAT_VEHICLE) ? assets.vehicle :
				(cat == XTK_CAT_TURRET) ? assets.turret :
				(cat == XTK_CAT_EXPLOSION) ? assets.explosion :
				(cat == XTK_CAT_BULLET) ? assets.bullet :
				(cat == XTK_CAT_LANDMARK) ? assets.landmark : assets.random;
			build_object(&arr[idx[cat]], &registry[i]);
			idx[cat]++;
		}
	}

	for (i = 0; i < palette_count; i++) {
		assets.palette[i].index = palette[i].idx;
		assets.palette[i].r = palette[i].r;
		assets.palette[i].g = palette[i].g;
		assets.palette[i].b = palette[i].b;
		strncpy(assets.palette[i].name, palette[i].name, XTK_ASSET_NAME - 1);
	}
	assets.num_palette = palette_count;
	for (i = 0; i < font_count; i++) {
		assets.font[i].index = fonts[i].idx;
		assets.font[i].width = fonts[i].w;
		assets.font[i].height = fonts[i].h;
		assets.font[i].ascent = fonts[i].asc;
		strncpy(assets.font[i].name, fonts[i].name, XTK_ASSET_FONT_NAME - 1);
	}
	assets.num_font = font_count;

	if (assets_validate(&assets, NULL, 0) != 0) {
		fprintf(stderr, "export-client-assets: assembled assets invalid\n");
		return 1;
	}

	payload = build_payload(&assets, &payload_len);
	if (payload == NULL) {
		fprintf(stderr, "export-client-assets: out of memory\n");
		return 1;
	}
	checksum = fnv1a(payload, payload_len);
	assets.payload_checksum = checksum;

	/* 64-byte header. */
	memset(hdr, 0, sizeof(hdr));
	memcpy(hdr, XTK_ASSET_BUNDLE_MAGIC, 4);
	hdr[4] = XTK_ASSET_BUNDLE_VERSION & 0xFF;
	hdr[5] = (XTK_ASSET_BUNDLE_VERSION >> 8) & 0xFF;
	hdr[6] = 0; hdr[7] = 0;			/* flags */
	{
		uint16_t counts[8];
		int off = 8;
		counts[0] = (uint16_t)assets.num_vehicle;
		counts[1] = (uint16_t)assets.num_turret;
		counts[2] = (uint16_t)assets.num_explosion;
		counts[3] = (uint16_t)assets.num_bullet;
		counts[4] = (uint16_t)assets.num_landmark;
		counts[5] = (uint16_t)assets.num_random;
		counts[6] = (uint16_t)assets.num_palette;
		counts[7] = (uint16_t)assets.num_font;
		for (i = 0; i < 8; i++) {
			hdr[off++] = counts[i] & 0xFF;
			hdr[off++] = (counts[i] >> 8) & 0xFF;
		}
	}
	/* reserved u32 at 24; payload_len at 28; checksum at 32. */
	hdr[28] = payload_len & 0xFF;
	hdr[29] = (payload_len >> 8) & 0xFF;
	hdr[30] = (payload_len >> 16) & 0xFF;
	hdr[31] = (payload_len >> 24) & 0xFF;
	hdr[32] = checksum & 0xFF;
	hdr[33] = (checksum >> 8) & 0xFF;
	hdr[34] = (checksum >> 16) & 0xFF;
	hdr[35] = (checksum >> 24) & 0xFF;

	f = fopen(out_path, "wb");
	if (f == NULL) {
		perror(out_path);
		return 1;
	}
	if (fwrite(hdr, 1, sizeof(hdr), f) != sizeof(hdr) ||
	    fwrite(payload, 1, payload_len, f) != payload_len) {
		fprintf(stderr, "export-client-assets: short write to %s\n", out_path);
		fclose(f);
		return 1;
	}
	fclose(f);

	if (geom_path != NULL) {
		f = fopen(geom_path, "w");
		if (f == NULL) {
			perror(geom_path);
			return 1;
		}
		dump_geometry(&assets, f);
		fclose(f);
	}

	if (manifest_path != NULL) {
		f = fopen(manifest_path, "w");
		if (f == NULL) {
			perror(manifest_path);
			return 1;
		}
		fprintf(f, "XTANK-CLIENT-ASSETS v1\n");
		fprintf(f, "bundle %s\n", out_path);
		fprintf(f, "header_bytes %u\n", XTK_ASSET_HEADER_BYTES);
		fprintf(f, "payload_bytes %zu\n", payload_len);
		fprintf(f, "payload_fnv1a %08x\n", checksum);
		fprintf(f, "counts vehicle %d turret %d explosion %d bullet %d "
			"landmark %d random %d palette %d font %d\n",
			assets.num_vehicle, assets.num_turret,
			assets.num_explosion, assets.num_bullet,
			assets.num_landmark, assets.num_random,
			assets.num_palette, assets.num_font);
		for (i = 0; i < assets.num_vehicle; i++)
			fprintf(f, "vehicle %d %s pics %d\n", i,
				assets.vehicle[i].type, assets.vehicle[i].num_pics);
		for (i = 0; i < assets.num_palette; i++)
			fprintf(f, "color %d %s %d %d %d\n", assets.palette[i].index,
				assets.palette[i].name, assets.palette[i].r,
				assets.palette[i].g, assets.palette[i].b);
		for (i = 0; i < assets.num_font; i++)
			fprintf(f, "font %d %s %d %d %d\n", assets.font[i].index,
				assets.font[i].name, assets.font[i].width,
				assets.font[i].height, assets.font[i].ascent);
		fclose(f);
	}

	printf("export-client-assets: %d objects (%d vehicles), payload %zu "
	       "bytes, fnv1a %08x\n", registry_count, assets.num_vehicle,
	       payload_len, checksum);

	free(payload);
	/* Free every object's bitmaps. */
	for (i = 0; i < assets.num_vehicle; i++) free_object(&assets.vehicle[i]);
	for (i = 0; i < assets.num_turret; i++) free_object(&assets.turret[i]);
	for (i = 0; i < assets.num_explosion; i++) free_object(&assets.explosion[i]);
	for (i = 0; i < assets.num_bullet; i++) free_object(&assets.bullet[i]);
	for (i = 0; i < assets.num_landmark; i++) free_object(&assets.landmark[i]);
	for (i = 0; i < assets.num_random; i++) free_object(&assets.random[i]);
	free(assets.vehicle);
	free(assets.turret);
	free(assets.explosion);
	free(assets.bullet);
	free(assets.landmark);
	free(assets.random);
	return 0;
}
