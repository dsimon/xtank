/*
 * Client/platform_sdl.h -- SDL3 platform shell for the Xtank Windows client (P2.9).
 *
 * This is the only client translation unit that includes SDL3.  It owns the
 * window, the renderer, the event pump, the monotonic timer and the DPI
 * coordinate mapping; it knows nothing about the wire protocol, ENet or the
 * simulation.  main.c orchestrates and links Protocol/ and ENet directly.
 *
 * Authority: docs/PHASE2-PLAN.md P2.9; docs/PHASE2-DECISIONS.md D2-A/D2-D.
 */

#ifndef XTK_CLIENT_PLATFORM_SDL_H
#define XTK_CLIENT_PLATFORM_SDL_H

#include <stdint.h>

#include "input.h"	/* neutral XtkControl vocabulary */

typedef struct ClientPlatform ClientPlatform;

/* Event/timer/drawable state accumulated by client_platform_pump(). */
typedef struct {
	int   quit;			/* SDL_EVENT_QUIT or close request seen   */
	int   key_down;			/* count of KEY_DOWN events               */
	int   key_up;			/* count of KEY_UP events                 */
	int   mouse_motion;		/* count of MOUSE_MOTION events           */
	int   mouse_button;		/* count of MOUSE_BUTTON_* events         */
	int   resized;			/* count of RESIZED / PIXEL_SIZE_CHANGED  */
	int   focus_lost;		/* count of WINDOW_FOCUS_LOST events      */
	int   focus_gained;		/* count of WINDOW_FOCUS_GAINED events    */
	int   last_key;			/* last SDL_Keycode seen                  */
	int   last_scancode;		/* last SDL scancode seen                 */
	int   last_button;		/* last mouse button index                */
	float mouse_x, mouse_y;		/* last mouse position, logical window pts */
} ClientEvents;

/*
 * Map a physical SDL scancode or mouse button to the neutral client control
 * vocabulary.  This is the ONLY place SDL scancodes meet client controls; the
 * neutral XtkControl values are then mapped to wire ops in Client/input.c and
 * the wire ops to engine intents on the server.  XTK_CTL_NONE means the input
 * is not a gameplay control.
 */
XtkControl xtk_sdl_scancode_to_control(int scancode);
XtkControl xtk_sdl_button_to_control(int button);

/*
 * Pure DPI/coordinate helpers (no SDL dependency) so the transform can be
 * unit-tested at 100/150/200% independently of the host display.  A logical
 * coordinate is a window point; a drawable coordinate is a physical pixel.
 * drawable = round(logical * scale) and logical = round(drawable / scale).
 */
int  xtk_dpi_logical_to_drawable(int logical, float scale);
int  xtk_dpi_drawable_to_logical(int drawable, float scale);
void xtk_dpi_mouse_to_drawable(float lx, float ly, float scale,
			       float *dx, float *dy);

/*
 * Create the SDL video subsystem, a resizable high-DPI window and a renderer
 * with a software fallback.  Returns NULL on failure.  `logical_w/h` are the
 * requested window size in points.
 */
ClientPlatform *client_platform_create(const char *title,
				       int logical_w, int logical_h);

/* Destroy the renderer, window and SDL video subsystem. */
void client_platform_destroy(ClientPlatform *p);

/* Current SDL video driver name ("windows", "x11", "dummy", ...). */
const char *client_platform_driver(const ClientPlatform *p);

/* Current renderer name ("software", "direct3d11", ...), or "unknown". */
const char *client_platform_renderer(const ClientPlatform *p);

/*
 * Drain all pending SDL events into `ev` (which the caller has zeroed).  The
 * counts accumulate; quit is sticky.  Returns 0, or -1 on a fatal SDL error.
 */
int client_platform_pump(ClientPlatform *p, ClientEvents *ev);

/* Fill logical/drawable sizes and display scale for the current window. */
void client_platform_metrics(const ClientPlatform *p, int *logical_w,
			     int *logical_h, int *drawable_w, int *drawable_h,
			     float *scale);

/* Request a new logical window size (used to exercise the resize path). */
void client_platform_set_logical_size(ClientPlatform *p, int w, int h);

/* Current mouse position in logical window points. */
void client_platform_mouse(const ClientPlatform *p, float *x, float *y);

/* Push a synthetic key event; `down` selects KEY_DOWN vs KEY_UP. */
void client_platform_inject_key(ClientPlatform *p, int sdl_keycode, int down);

/* Push a synthetic mouse-motion event at logical window coordinates. */
void client_platform_inject_mouse_motion(ClientPlatform *p, float x, float y);

/* Push a synthetic mouse-button event at logical window coordinates. */
void client_platform_inject_mouse_button(ClientPlatform *p, int button, int down,
					 float x, float y);

/* Push a synthetic SDL_EVENT_QUIT. */
void client_platform_inject_quit(ClientPlatform *p);

/* Push a synthetic window focus-gained/focus-lost event. */
void client_platform_inject_focus(ClientPlatform *p, int focused);

/* Draw one full frame (clear + simple placeholder geometry) and present. */
void client_platform_render(ClientPlatform *p);

/* Monotonic nanoseconds since an arbitrary epoch (SDL_GetTicksNS). */
uint64_t client_platform_ticks_ns(void);

/* Sleep for `ms` milliseconds. */
void client_platform_delay(uint32_t ms);

#endif /* XTK_CLIENT_PLATFORM_SDL_H */
