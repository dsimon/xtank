/*
 * Client/assets.c -- loader for the P2.10 portable original-visual asset bundle.
 *
 * Pure C (no SDL): parses the checked little-endian bundle emitted by
 * Tools/export-client-assets.c into an XtkAssets tree, validates every bound
 * and bitmap length, and can re-emit the explicit geometry table in the exact
 * Src/geometry.c format.  It reads no simulation, no X11 and no graphics
 * backend, and never allocates unbounded memory: every count is checked
 * against the frozen bundle caps before allocation.
 *
 * Authority: docs/PHASE2-PLAN.md P2.10; Client/render.h (the bundle format).
 */

#include <stdlib.h>
#include <string.h>

#include "render.h"

typedef struct {
	const uint8_t *p;
	size_t         len;
	size_t         pos;
	int            err;
} Reader;

static uint8_t rd_u8(Reader *r)
{
	if (r->pos + 1 > r->len) { r->err = 1; return 0; }
	return r->p[r->pos++];
}

static uint16_t rd_u16(Reader *r)
{
	uint16_t v;
	if (r->pos + 2 > r->len) { r->err = 1; return 0; }
	v = (uint16_t)(r->p[r->pos] | (r->p[r->pos + 1] << 8));
	r->pos += 2;
	return v;
}

static int16_t rd_i16(Reader *r)
{
	return (int16_t)rd_u16(r);
}

static uint32_t rd_u32(Reader *r)
{
	uint32_t v;
	if (r->pos + 4 > r->len) { r->err = 1; return 0; }
	v = (uint32_t)r->p[r->pos] | ((uint32_t)r->p[r->pos + 1] << 8) |
	    ((uint32_t)r->p[r->pos + 2] << 16) |
	    ((uint32_t)r->p[r->pos + 3] << 24);
	r->pos += 4;
	return v;
}

static int32_t rd_i32(Reader *r)
{
	return (int32_t)rd_u32(r);
}

static double rd_f64(Reader *r)
{
	uint64_t bits = 0;
	double d;
	int i;
	if (r->pos + 8 > r->len) { r->err = 1; return 0.0; }
	for (i = 0; i < 8; i++)
		bits |= (uint64_t)r->p[r->pos + i] << (8 * i);
	r->pos += 8;
	memcpy(&d, &bits, 8);
	return d;
}

static void rd_name(Reader *r, char *dst, size_t cap)
{
	if (r->pos + cap > r->len) { r->err = 1; return; }
	memcpy(dst, r->p + r->pos, cap);
	dst[cap - 1] = '\0';
	r->pos += cap;
}

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

static void set_err(char *err, size_t cap, const char *msg)
{
	if (err != NULL && cap > 0) {
		strncpy(err, msg, cap - 1);
		err[cap - 1] = '\0';
	}
}

void assets_free(XtkAssets *a)
{
	int c, i, r;
	if (a == NULL)
		return;
	for (c = 0; c < XTK_CAT_COUNT; c++) {
		XtkAssetObject *arr = NULL;
		int n = 0;
		switch (c) {
		case XTK_CAT_VEHICLE: arr = a->vehicle; n = a->num_vehicle; break;
		case XTK_CAT_TURRET: arr = a->turret; n = a->num_turret; break;
		case XTK_CAT_EXPLOSION: arr = a->explosion; n = a->num_explosion; break;
		case XTK_CAT_BULLET: arr = a->bullet; n = a->num_bullet; break;
		case XTK_CAT_LANDMARK: arr = a->landmark; n = a->num_landmark; break;
		case XTK_CAT_RANDOM: arr = a->random; n = a->num_random; break;
		}
		for (i = 0; i < n; i++)
			for (r = 0; r < arr[i].num_pics; r++)
				free(arr[i].pic[r].bitmap);
		free(arr);
	}
	memset(a, 0, sizeof(*a));
}

int assets_count(const XtkAssets *a, int cat)
{
	switch (cat) {
	case XTK_CAT_VEHICLE: return a->num_vehicle;
	case XTK_CAT_TURRET: return a->num_turret;
	case XTK_CAT_EXPLOSION: return a->num_explosion;
	case XTK_CAT_BULLET: return a->num_bullet;
	case XTK_CAT_LANDMARK: return a->num_landmark;
	case XTK_CAT_RANDOM: return a->num_random;
	}
	return 0;
}

const XtkAssetObject *assets_object(const XtkAssets *a, int cat, int index)
{
	const XtkAssetObject *arr = NULL;
	int n = 0;
	switch (cat) {
	case XTK_CAT_VEHICLE: arr = a->vehicle; n = a->num_vehicle; break;
	case XTK_CAT_TURRET: arr = a->turret; n = a->num_turret; break;
	case XTK_CAT_EXPLOSION: arr = a->explosion; n = a->num_explosion; break;
	case XTK_CAT_BULLET: arr = a->bullet; n = a->num_bullet; break;
	case XTK_CAT_LANDMARK: arr = a->landmark; n = a->num_landmark; break;
	case XTK_CAT_RANDOM: arr = a->random; n = a->num_random; break;
	}
	if (index < 0 || index >= n)
		return NULL;
	return &arr[index];
}

uint32_t assets_color_rgb(const XtkAssets *a, int palette_index)
{
	int i;
	for (i = 0; i < a->num_palette; i++)
		if (a->palette[i].index == (uint8_t)palette_index)
			return XTK_RGB(a->palette[i].r, a->palette[i].g,
				       a->palette[i].b);
	return 0;
}

int assets_validate(const XtkAssets *a, char *err, size_t errcap)
{
	int c, i, r;
	if (a->num_palette > XTK_ASSET_MAX_COLORS ||
	    a->num_font > XTK_ASSET_MAX_FONTS) {
		set_err(err, errcap, "palette/font count exceeds cap");
		return -1;
	}
	for (c = 0; c < XTK_CAT_COUNT; c++) {
		int n = assets_count(a, c);
		if (n < 0) { set_err(err, errcap, "negative object count"); return -1; }
		for (i = 0; i < n; i++) {
			const XtkAssetObject *o = assets_object(a, c, i);
			if (o == NULL) { set_err(err, errcap, "null object"); return -1; }
			if (o->num_pics < 0 || o->num_pics > XTK_ASSET_MAX_VIEWS) {
				set_err(err, errcap, "view count out of range");
				return -1;
			}
			if (o->num_turrets < 0 || o->num_turrets > XTK_ASSET_MAX_TURRETS ||
			    o->num_segs < 0 || o->num_segs > XTK_ASSET_MAX_SEGS) {
				set_err(err, errcap, "turret/segment count out of range");
				return -1;
			}
			for (r = 0; r < o->num_pics; r++) {
				const XtkPicture *p = &o->pic[r];
				uint32_t want = 0;
				if (p->width < 0 || p->height < 0) {
					set_err(err, errcap, "negative picture size");
					return -1;
				}
				if (p->width > 0 && p->height > 0)
					want = (uint32_t)(((p->width + 7) >> 3) * p->height);
				/* bitmap_len 0 is legal (the pulse-laser alias has
				 * geometry but no bitmap); otherwise it must match. */
				if (p->bitmap_len != 0 && p->bitmap_len != want) {
					set_err(err, errcap, "bitmap length mismatch");
					return -1;
				}
				if (p->bitmap_len > 0 && p->bitmap == NULL) {
					set_err(err, errcap, "missing bitmap data");
					return -1;
				}
			}
		}
	}
	return 0;
}

static int read_object(Reader *r, XtkAssetObject *o)
{
	int i, t, k;
	uint16_t np, nt, ns;

	memset(o, 0, sizeof(*o));
	rd_name(r, o->type, XTK_ASSET_NAME);
	np = rd_u16(r);
	nt = rd_u16(r);
	ns = rd_u16(r);
	(void)rd_u16(r);			/* reserved */
	if (r->err)
		return -1;
	if (np > XTK_ASSET_MAX_VIEWS || nt > XTK_ASSET_MAX_TURRETS ||
	    ns > XTK_ASSET_MAX_SEGS)
		return -1;
	o->num_pics = np;
	o->num_turrets = nt;
	o->num_segs = ns;

	for (i = 0; i < np; i++) {
		XtkPicture *p = &o->pic[i];
		p->width = rd_i16(r);
		p->height = rd_i16(r);
		p->offset_x = rd_i16(r);
		p->offset_y = rd_i16(r);
		p->bitmap_len = rd_u32(r);
		if (r->err)
			return -1;
		if (p->bitmap_len > (uint32_t)(1 << 20))
			return -1;
		if (p->bitmap_len > 0) {
			p->bitmap = (uint8_t *)malloc(p->bitmap_len);
			if (p->bitmap == NULL)
				return -1;
			if (r->pos + p->bitmap_len > r->len) {
				r->err = 1;
				return -1;
			}
			memcpy(p->bitmap, r->p + r->pos, p->bitmap_len);
			r->pos += p->bitmap_len;
		}
	}
	for (i = 0; i < np; i++)
		for (t = 0; t < nt; t++) {
			o->turret[i][t].x = rd_i16(r);
			o->turret[i][t].y = rd_i16(r);
		}
	for (i = 0; i < np; i++)
		for (k = 0; k < ns; k++) {
			XtkSegment *s = &o->seg[i][k];
			s->x1 = rd_i16(r); s->y1 = rd_i16(r);
			s->x2 = rd_i16(r); s->y2 = rd_i16(r);
			s->dx = rd_i16(r); s->dy = rd_i16(r);
			s->minx = rd_i16(r); s->miny = rd_i16(r);
			s->maxx = rd_i16(r); s->maxy = rd_i16(r);
			s->intercept = rd_i32(r);
			s->slope = rd_f64(r);
		}
	return r->err ? -1 : 0;
}

static int load_payload(XtkAssets *a, const uint8_t *p, size_t len,
			char *err, size_t errcap)
{
	Reader r;
	int c, i;
	static const int cats[XTK_CAT_COUNT] = {
		XTK_CAT_VEHICLE, XTK_CAT_TURRET, XTK_CAT_EXPLOSION,
		XTK_CAT_BULLET, XTK_CAT_LANDMARK, XTK_CAT_RANDOM
	};

	memset(&r, 0, sizeof(r));
	r.p = p;
	r.len = len;

	for (i = 0; i < a->num_palette; i++) {
		a->palette[i].index = rd_u8(&r);
		a->palette[i].r = rd_u8(&r);
		a->palette[i].g = rd_u8(&r);
		a->palette[i].b = rd_u8(&r);
		rd_name(&r, a->palette[i].name, XTK_ASSET_NAME);
	}
	for (i = 0; i < a->num_font; i++) {
		a->font[i].index = rd_u8(&r);
		a->font[i].width = rd_u8(&r);
		a->font[i].height = rd_u8(&r);
		a->font[i].ascent = rd_u8(&r);
		(void)rd_u32(&r);		/* reserved */
		rd_name(&r, a->font[i].name, XTK_ASSET_FONT_NAME);
	}
	for (c = 0; c < XTK_CAT_COUNT; c++) {
		int n = assets_count(a, c);
		XtkAssetObject *arr = NULL;
		switch (cats[c]) {
		case XTK_CAT_VEHICLE: arr = a->vehicle; break;
		case XTK_CAT_TURRET: arr = a->turret; break;
		case XTK_CAT_EXPLOSION: arr = a->explosion; break;
		case XTK_CAT_BULLET: arr = a->bullet; break;
		case XTK_CAT_LANDMARK: arr = a->landmark; break;
		case XTK_CAT_RANDOM: arr = a->random; break;
		}
		for (i = 0; i < n; i++)
			if (read_object(&r, &arr[i]) != 0) {
				set_err(err, errcap, "malformed object record");
				return -1;
			}
	}
	if (r.err) {
		set_err(err, errcap, "truncated payload");
		return -1;
	}
	return 0;
}

int assets_load_memory(XtkAssets *a, const uint8_t *data, size_t len,
		       char *err, size_t errcap)
{
	uint16_t counts[8];
	uint32_t payload_len, checksum;
	int i;

	memset(a, 0, sizeof(*a));
	if (len < XTK_ASSET_HEADER_BYTES) {
		set_err(err, errcap, "bundle shorter than header");
		return -1;
	}
	if (memcmp(data, XTK_ASSET_BUNDLE_MAGIC, 4) != 0) {
		set_err(err, errcap, "bad bundle magic");
		return -1;
	}
	if ((data[4] | (data[5] << 8)) != XTK_ASSET_BUNDLE_VERSION) {
		set_err(err, errcap, "unsupported bundle version");
		return -1;
	}
	for (i = 0; i < 8; i++)
		counts[i] = (uint16_t)(data[8 + 2 * i] |
				       (data[9 + 2 * i] << 8));
	payload_len = (uint32_t)data[28] | ((uint32_t)data[29] << 8) |
		      ((uint32_t)data[30] << 16) | ((uint32_t)data[31] << 24);
	checksum = (uint32_t)data[32] | ((uint32_t)data[33] << 8) |
		   ((uint32_t)data[34] << 16) | ((uint32_t)data[35] << 24);

	if (XTK_ASSET_HEADER_BYTES + (size_t)payload_len != len) {
		set_err(err, errcap, "payload length mismatch");
		return -1;
	}
	if (fnv1a(data + XTK_ASSET_HEADER_BYTES, payload_len) != checksum) {
		set_err(err, errcap, "payload checksum mismatch");
		return -1;
	}

	a->num_vehicle = counts[0];
	a->num_turret = counts[1];
	a->num_explosion = counts[2];
	a->num_bullet = counts[3];
	a->num_landmark = counts[4];
	a->num_random = counts[5];
	a->num_palette = counts[6];
	a->num_font = counts[7];
	a->payload_checksum = checksum;

	if (a->num_palette > XTK_ASSET_MAX_COLORS ||
	    a->num_font > XTK_ASSET_MAX_FONTS) {
		set_err(err, errcap, "palette/font count exceeds cap");
		return -1;
	}

	if (a->num_vehicle > 0) {
		a->vehicle = (XtkAssetObject *)calloc((size_t)a->num_vehicle,
						      sizeof(XtkAssetObject));
		if (a->vehicle == NULL) goto oom;
	}
	if (a->num_turret > 0) {
		a->turret = (XtkAssetObject *)calloc((size_t)a->num_turret,
						     sizeof(XtkAssetObject));
		if (a->turret == NULL) goto oom;
	}
	if (a->num_explosion > 0) {
		a->explosion = (XtkAssetObject *)calloc((size_t)a->num_explosion,
							sizeof(XtkAssetObject));
		if (a->explosion == NULL) goto oom;
	}
	if (a->num_bullet > 0) {
		a->bullet = (XtkAssetObject *)calloc((size_t)a->num_bullet,
						     sizeof(XtkAssetObject));
		if (a->bullet == NULL) goto oom;
	}
	if (a->num_landmark > 0) {
		a->landmark = (XtkAssetObject *)calloc((size_t)a->num_landmark,
						       sizeof(XtkAssetObject));
		if (a->landmark == NULL) goto oom;
	}
	if (a->num_random > 0) {
		a->random = (XtkAssetObject *)calloc((size_t)a->num_random,
						     sizeof(XtkAssetObject));
		if (a->random == NULL) goto oom;
	}

	if (load_payload(a, data + XTK_ASSET_HEADER_BYTES, payload_len,
			 err, errcap) != 0)
		return -1;
	if (assets_validate(a, err, errcap) != 0)
		return -1;
	return 0;

oom:
	set_err(err, errcap, "out of memory");
	assets_free(a);
	return -1;
}

int assets_load_file(XtkAssets *a, const char *path, char *err, size_t errcap)
{
	FILE *f;
	uint8_t *buf;
	long sz;
	size_t n;
	int rc;

	f = fopen(path, "rb");
	if (f == NULL) {
		set_err(err, errcap, "cannot open asset bundle");
		return -1;
	}
	if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0) {
		fclose(f);
		set_err(err, errcap, "cannot size asset bundle");
		return -1;
	}
	rewind(f);
	buf = (uint8_t *)malloc((size_t)sz);
	if (buf == NULL) {
		fclose(f);
		set_err(err, errcap, "out of memory");
		return -1;
	}
	n = fread(buf, 1, (size_t)sz, f);
	fclose(f);
	if (n != (size_t)sz) {
		free(buf);
		set_err(err, errcap, "short read");
		return -1;
	}
	rc = assets_load_memory(a, buf, n, err, errcap);
	free(buf);
	return rc;
}

/* ---- Geometry table (exact geometry_dump() format) ------------------- */

static const char *cat_kind_name(int cat)
{
	switch (cat) {
	case XTK_CAT_VEHICLE: return "vehicle";
	case XTK_CAT_TURRET: return "turret";
	case XTK_CAT_EXPLOSION: return "explosion";
	case XTK_CAT_BULLET: return "bullet";
	case XTK_CAT_LANDMARK: return "landmark";
	case XTK_CAT_RANDOM: return "random";
	}
	return "?";
}

int assets_dump_geometry(const XtkAssets *a, FILE *fp)
{
	int c, i, r, t, k;

	fprintf(fp, "GEOMETRY-TABLE v1\n");
	fprintf(fp, "count vehicle %d turret %d explosion %d bullet %d "
		"landmark %d random %d\n", a->num_vehicle, a->num_turret,
		a->num_explosion, a->num_bullet, a->num_landmark, a->num_random);

	for (c = 0; c < XTK_CAT_COUNT; c++) {
		int n = assets_count(a, c);
		for (i = 0; i < n; i++) {
			const XtkAssetObject *o = assets_object(a, c, i);
			fprintf(fp, "obj %s %d type %s pics %d turrets %d segs %d\n",
				cat_kind_name(c), i, o->type, o->num_pics,
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
	return 0;
}
