/*
 * Client/render.h -- portable original-visual asset bundle and snapshot-driven
 * drawing contract for the Xtank Windows client (P2.10).
 *
 * The client renders from the protocol-v1 VIEW_SNAPSHOT DTOs only; it links no
 * simulation, no bot, no X11 and no legacy world-reading renderer.  The art is
 * a checked, portable export of the original 1-bit body/turret/projectile/
 * explosion/landmark stencils (Src/Display/Objects/<name>.obj) plus the 16 body
 * rotations, the palette and the font identities.
 *
 * This header is deliberately SDL-free: assets.c and the exporter are pure C.
 * render_sdl.c is the only translation unit that includes SDL3, and it
 * implements DRAW_COPY *and* DRAW_XOR on an SDL_Surface.  The client never
 * reuses the legacy GraphicsBackend vtable, so no legacy caller's XOR
 * semantics are redefined.
 *
 * Authority: docs/PHASE2-PLAN.md P2.10; docs/VIEW-v1.md; Src/display.c
 * (read-only reference for the draw offsets, turret placement and rotation
 * indexing); Src/geometry.c (read-only reference for the 90/180 rotation).
 */

#ifndef XTK_CLIENT_RENDER_H
#define XTK_CLIENT_RENDER_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "wire.h"		/* XtkViewSnapshot (portable protocol DTO) */
#include "camera.h"		/* XtkCamera (P2.11 presentation camera)  */

/* ---- Bundle format -------------------------------------------------- */

#define XTK_ASSET_BUNDLE_MAGIC   "XTA1"
#define XTK_ASSET_BUNDLE_VERSION 1u
#define XTK_ASSET_HEADER_BYTES   64u

/* Structural caps, validated on load; the largest real object is the 39-view
 * mine bullet, the 32-view small turret and the 19-view design landmark. */
#define XTK_ASSET_MAX_VIEWS   64
#define XTK_ASSET_MAX_TURRETS 8
#define XTK_ASSET_MAX_SEGS    8
#define XTK_ASSET_NAME        24
#define XTK_ASSET_FONT_NAME   32
#define XTK_ASSET_MAX_COLORS  11
#define XTK_ASSET_MAX_FONTS   5

/* Category order in the bundle and in XtkAssets. */
enum {
	XTK_CAT_VEHICLE = 0,
	XTK_CAT_TURRET,
	XTK_CAT_EXPLOSION,
	XTK_CAT_BULLET,
	XTK_CAT_LANDMARK,
	XTK_CAT_RANDOM,
	XTK_CAT_COUNT
};

typedef struct {
	int16_t  width, height;	/* bitmap bounds in pixels                */
	int16_t  offset_x, offset_y;	/* hot-spot (draw point) within bitmap */
	uint32_t bitmap_len;	/* ((width+7)/8)*height, 0 = no bitmap    */
	uint8_t *bitmap;	/* 1 bit/pixel, MSB first, row-major      */
} XtkPicture;

typedef struct {
	int16_t x, y;		/* turret centre relative to body centre  */
} XtkTurretCoord;

typedef struct {
	int16_t  x1, y1, x2, y2;
	int16_t  dx, dy;
	int16_t  minx, miny, maxx, maxy;
	int32_t  intercept;
	double   slope;
} XtkSegment;

typedef struct {
	char     type[XTK_ASSET_NAME];	/* legacy Object.type string        */
	int      num_pics;
	int      num_turrets;
	int      num_segs;
	XtkPicture     pic[XTK_ASSET_MAX_VIEWS];
	XtkTurretCoord turret[XTK_ASSET_MAX_VIEWS][XTK_ASSET_MAX_TURRETS];
	XtkSegment     seg[XTK_ASSET_MAX_VIEWS][XTK_ASSET_MAX_SEGS];
} XtkAssetObject;

typedef struct {
	uint8_t index;		/* legacy palette index (BLACK..DASHED)     */
	uint8_t r, g, b;	/* resolved sRGB                            */
	char    name[XTK_ASSET_NAME];
} XtkAssetColor;

typedef struct {
	uint8_t index;		/* legacy font index (T_FONT..XL_FONT)      */
	uint8_t width, height, ascent;
	char    name[XTK_ASSET_FONT_NAME];
} XtkAssetFont;

typedef struct {
	XtkAssetObject *vehicle;	int num_vehicle;
	XtkAssetObject *turret;		int num_turret;
	XtkAssetObject *explosion;	int num_explosion;
	XtkAssetObject *bullet;		int num_bullet;
	XtkAssetObject *landmark;	int num_landmark;
	XtkAssetObject *random;		int num_random;
	XtkAssetColor palette[XTK_ASSET_MAX_COLORS];
	int           num_palette;
	XtkAssetFont  font[XTK_ASSET_MAX_FONTS];
	int           num_font;
	uint32_t      payload_checksum;	/* FNV-1a of the bundle payload     */
} XtkAssets;

/*
 * Load a bundle from a file or from memory.  On success returns 0 and fills
 * `a`; the caller must eventually call assets_free().  On failure returns -1,
 * writes a diagnostic to `err` and leaves `a` safe to pass to assets_free().
 */
int assets_load_file(XtkAssets *a, const char *path, char *err, size_t errcap);
int assets_load_memory(XtkAssets *a, const uint8_t *data, size_t len,
		       char *err, size_t errcap);
void assets_free(XtkAssets *a);

/* Re-check internal consistency (counts, bitmap lengths, view caps). */
int assets_validate(const XtkAssets *a, char *err, size_t errcap);

/* Category/object accessors.  Return NULL if out of range. */
const XtkAssetObject *assets_object(const XtkAssets *a, int cat, int index);
int assets_count(const XtkAssets *a, int cat);

/* Resolve a legacy palette index to 0xRRGGBB, or 0 if not present. */
uint32_t assets_color_rgb(const XtkAssets *a, int palette_index);

/* Emit the explicit geometry table in the exact Src/geometry.c
 * geometry_dump() format, from the loaded bundle (so the client's own view of
 * the art can be compared byte-for-byte with the frozen X/server evidence). */
int assets_dump_geometry(const XtkAssets *a, FILE *fp);

/* ---- Snapshot-driven drawing ---------------------------------------- */

/*
 * Presentation camera/view.  `center_x/center_y` is the world coordinate at
 * the centre of the animation viewport (legacy term->loc + half extent); the
 * viewport is in logical pixels and one world unit maps to one pixel, exactly
 * as the legacy 768x768 animation window (NUM_BOXES * BOX_WIDTH).  Camera
 * policy (follow/interpolation) is P2.11; P2.10 only draws the given pose.
 */
typedef struct {
	double   center_x, center_y;
	int      viewport_w, viewport_h;
	uint32_t color_bg;
	uint32_t color_own;
	uint32_t color_vehicle;
	uint32_t color_bullet;
	uint32_t color_explosion;
	uint32_t color_landmark;
	int      explosion_frame;	/* -1 = last frame of the object   */
} XtkRenderView;

struct SDL_Surface;

/*
 * Draw one full frame of the authorized snapshot onto an ARGB8888 SDL_Surface.
 * The whole surface is cleared first, so a redraw after a resize or an expose
 * cannot leave stale pixels (no trails).  Returns 0, or -1 with a diagnostic.
 */
int render_frame_surface(const XtkAssets *a, const XtkViewSnapshot *v,
			 const XtkRenderView *rv, struct SDL_Surface *surf,
			 char *err, size_t errcap);

/*
 * P2.11 presentation glue: draw one full frame of an authorized (already
 * interpolated) snapshot through the fixed camera of Client/camera.h, with the
 * default presentation colors.  The camera viewport and the 1:1 world scale are
 * invariant; only the camera centre follows the displayed own pose.  Returns 0,
 * or -1 with a diagnostic.
 */
int world_render_present(const XtkAssets *a, const XtkViewSnapshot *v,
			 const XtkCamera *cam, struct SDL_Surface *surf,
			 char *err, size_t errcap);

/* Low-level surface primitives.  DRAW_XOR is a true per-channel XOR, matching
 * the legacy X11 GXxor semantics for opaque 1-bit stencils. */
#define XTK_DRAW_COPY 0
#define XTK_DRAW_XOR  1
int render_draw_picture(struct SDL_Surface *surf, int x, int y,
			const XtkPicture *pic, int func, uint32_t color);
void render_surface_clear(struct SDL_Surface *surf, uint32_t color);
uint32_t render_surface_get(const struct SDL_Surface *surf, int x, int y);

/* Presentation rotation index for directional bullets, replicated from the
 * legacy find_rot() macro (Src/Include/bullet.h).  No gameplay state is read. */
int render_find_rot(double angle, int num_views);

/* 0xAARRGGBB helper. */
#define XTK_RGB(r, g, b) ((uint32_t)0xFF000000u | \
	((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))

#endif /* XTK_CLIENT_RENDER_H */
