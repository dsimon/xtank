/*
 * Client/platform_sdl.c -- SDL3 window/event/timer/renderer shell (P2.9).
 *
 * The only client translation unit that includes SDL3.  It creates a resizable
 * high-DPI window, a renderer with a software fallback, a nonblocking event
 * pump, a monotonic timer and the pure logical/drawable DPI mapping.  It holds
 * no protocol, transport or simulation state.
 *
 * Authority: docs/PHASE2-PLAN.md P2.9; docs/PHASE2-DECISIONS.md D2-A/D2-D.
 */

#define SDL_MAIN_HANDLED 1

#include <SDL3/SDL.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "platform_sdl.h"

#define CLIENT_INPUT_QUEUE 64

struct ClientPlatform {
	SDL_Window   *window;
	SDL_Renderer *renderer;
	SDL_Texture  *texture;		/* present target for the frame loop */
	int           tex_w, tex_h;

	ClientInputEvent inq[CLIENT_INPUT_QUEUE];
	int inq_head, inq_count;
};

static void inq_push(ClientPlatform *p, const ClientInputEvent *e)
{
	if (p->inq_count >= CLIENT_INPUT_QUEUE) {
		p->inq_head = (p->inq_head + 1) % CLIENT_INPUT_QUEUE;
		p->inq_count--;
	}
	p->inq[(p->inq_head + p->inq_count) % CLIENT_INPUT_QUEUE] = *e;
	p->inq_count++;
}

int client_platform_input_pop(ClientPlatform *p, ClientInputEvent *out)
{
	if (p == NULL || out == NULL || p->inq_count <= 0) {
		return 0;
	}
	*out = p->inq[p->inq_head];
	p->inq_head = (p->inq_head + 1) % CLIENT_INPUT_QUEUE;
	p->inq_count--;
	return 1;
}

/*
 * Physical SDL scancode -> neutral client control.  Keeping this table here
 * (the only SDL translation unit) guarantees that SDL scancodes never appear in
 * Client/input.c or reach the engine: input.c works only in XtkControl and wire
 * ops, the server maps wire ops to IntentOp.
 */
XtkControl xtk_sdl_scancode_to_control(int scancode)
{
	switch (scancode) {
	case SDL_SCANCODE_SPACE:	return XTK_CTL_FIRE;
	case SDL_SCANCODE_LCTRL:
	case SDL_SCANCODE_RCTRL:	return XTK_CTL_FIRE_HOLD;
	case SDL_SCANCODE_W:
	case SDL_SCANCODE_UP:		return XTK_CTL_DRIVE_FWD;
	case SDL_SCANCODE_S:
	case SDL_SCANCODE_DOWN:		return XTK_CTL_DRIVE_BACK;
	case SDL_SCANCODE_P:		return XTK_CTL_PAUSE;
	case SDL_SCANCODE_Q:		return XTK_CTL_QUIT;
	case SDL_SCANCODE_1:		return XTK_CTL_WEAPON_1;
	case SDL_SCANCODE_2:		return XTK_CTL_WEAPON_2;
	case SDL_SCANCODE_3:		return XTK_CTL_WEAPON_3;
	case SDL_SCANCODE_4:		return XTK_CTL_WEAPON_4;
	case SDL_SCANCODE_5:		return XTK_CTL_WEAPON_5;
	case SDL_SCANCODE_6:		return XTK_CTL_WEAPON_6;
	default:			return XTK_CTL_NONE;
	}
}

XtkControl xtk_sdl_button_to_control(int button)
{
	switch (button) {
	case SDL_BUTTON_LEFT:	return XTK_CTL_FIRE;
	case SDL_BUTTON_RIGHT:	return XTK_CTL_FIRE_HOLD;
	default:		return XTK_CTL_NONE;
	}
}

int xtk_dpi_logical_to_drawable(int logical, float scale)
{
	if (scale <= 0.0f) {
		return logical;
	}
	return (int)lroundf((float)logical * scale);
}

int xtk_dpi_drawable_to_logical(int drawable, float scale)
{
	if (scale <= 0.0f) {
		return drawable;
	}
	return (int)lroundf((float)drawable / scale);
}

void xtk_dpi_mouse_to_drawable(float lx, float ly, float scale, float *dx,
			       float *dy)
{
	if (scale <= 0.0f) {
		scale = 1.0f;
	}
	if (dx != NULL) {
		*dx = lx * scale;
	}
	if (dy != NULL) {
		*dy = ly * scale;
	}
}

ClientPlatform *client_platform_create(const char *title, int logical_w,
				       int logical_h)
{
	ClientPlatform *p;
	SDL_WindowFlags flags;
	SDL_Renderer *r;

	if (!SDL_Init(SDL_INIT_VIDEO)) {
		SDL_Log("SDL_Init failed: %s", SDL_GetError());
		return NULL;
	}

	flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
	p = (ClientPlatform *)calloc(1, sizeof(*p));
	if (p == NULL) {
		SDL_Quit();
		return NULL;
	}

	p->window = SDL_CreateWindow(title != NULL ? title : "Xtank", logical_w,
				     logical_h, flags);
	if (p->window == NULL) {
		SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
		free(p);
		SDL_Quit();
		return NULL;
	}

	r = SDL_CreateRenderer(p->window, NULL);
	if (r == NULL) {
		r = SDL_CreateRenderer(p->window, SDL_SOFTWARE_RENDERER);
	}
	if (r == NULL) {
		SDL_Log("SDL_CreateRenderer failed: %s", SDL_GetError());
		SDL_DestroyWindow(p->window);
		free(p);
		SDL_Quit();
		return NULL;
	}
	p->renderer = r;
	SDL_SetRenderVSync(r, 1);
	return p;
}

void client_platform_destroy(ClientPlatform *p)
{
	if (p == NULL) {
		return;
	}
	if (p->texture != NULL) {
		SDL_DestroyTexture(p->texture);
		p->texture = NULL;
	}
	if (p->renderer != NULL) {
		SDL_DestroyRenderer(p->renderer);
	}
	if (p->window != NULL) {
		SDL_DestroyWindow(p->window);
	}
	free(p);
	SDL_Quit();
}

const char *client_platform_driver(const ClientPlatform *p)
{
	const char *name;
	(void)p;
	name = SDL_GetCurrentVideoDriver();
	return name != NULL ? name : "unknown";
}

const char *client_platform_renderer(const ClientPlatform *p)
{
	const char *name;
	if (p == NULL || p->renderer == NULL) {
		return "unknown";
	}
	name = SDL_GetRendererName(p->renderer);
	return name != NULL ? name : "unknown";
}

int client_platform_pump(ClientPlatform *p, ClientEvents *ev)
{
	SDL_Event e;

	if (p == NULL || ev == NULL) {
		return -1;
	}
	/* The bounded input queue is refilled each pump; frame.c drains it. */
	p->inq_head = 0;
	p->inq_count = 0;
	while (SDL_PollEvent(&e)) {
		switch (e.type) {
		case SDL_EVENT_QUIT:
			ev->quit = 1;
			break;
		case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
			ev->quit = 1;
			break;
		case SDL_EVENT_WINDOW_RESIZED:
		case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
			ev->resized++;
			break;
		case SDL_EVENT_WINDOW_FOCUS_GAINED: {
			ClientInputEvent ie;
			ev->focus_gained++;
			ie.type = CLIENT_IN_FOCUS;
			ie.control = XTK_CTL_NONE;
			ie.down = 0;
			ie.x = ie.y = 0.0f;
			ie.focused = 1;
			inq_push(p, &ie);
			break;
		}
		case SDL_EVENT_WINDOW_FOCUS_LOST: {
			ClientInputEvent ie;
			ev->focus_lost++;
			ie.type = CLIENT_IN_FOCUS;
			ie.control = XTK_CTL_NONE;
			ie.down = 0;
			ie.x = ie.y = 0.0f;
			ie.focused = 0;
			inq_push(p, &ie);
			break;
		}
		case SDL_EVENT_KEY_DOWN:
		case SDL_EVENT_KEY_UP: {
			XtkControl ctl;
			ClientInputEvent ie;
			int down = (e.type == SDL_EVENT_KEY_DOWN) ? 1 : 0;

			if (down) {
				ev->key_down++;
			} else {
				ev->key_up++;
			}
			ev->last_key = (int)e.key.key;
			ev->last_scancode = (int)e.key.scancode;
			ctl = xtk_sdl_scancode_to_control((int)e.key.scancode);
			if (ctl != XTK_CTL_NONE) {
				ie.type = CLIENT_IN_KEY;
				ie.control = (int)ctl;
				ie.down = down;
				ie.x = ie.y = 0.0f;
				ie.focused = 0;
				inq_push(p, &ie);
			}
			break;
		}
		case SDL_EVENT_MOUSE_MOTION: {
			ClientInputEvent ie;
			ev->mouse_motion++;
			ev->mouse_x = e.motion.x;
			ev->mouse_y = e.motion.y;
			ie.type = CLIENT_IN_MOTION;
			ie.control = XTK_CTL_NONE;
			ie.down = 0;
			ie.x = e.motion.x;
			ie.y = e.motion.y;
			ie.focused = 0;
			inq_push(p, &ie);
			break;
		}
		case SDL_EVENT_MOUSE_BUTTON_DOWN:
		case SDL_EVENT_MOUSE_BUTTON_UP: {
			XtkControl ctl;
			ClientInputEvent ie;
			int down = (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) ? 1 : 0;

			ev->mouse_button++;
			ev->last_button = (int)e.button.button;
			ev->mouse_x = e.button.x;
			ev->mouse_y = e.button.y;
			ctl = xtk_sdl_button_to_control((int)e.button.button);
			if (ctl != XTK_CTL_NONE) {
				ie.type = CLIENT_IN_BUTTON;
				ie.control = (int)ctl;
				ie.down = down;
				ie.x = e.button.x;
				ie.y = e.button.y;
				ie.focused = 0;
				inq_push(p, &ie);
			}
			break;
		}
		default:
			break;
		}
	}
	return 0;
}

void client_platform_metrics(const ClientPlatform *p, int *logical_w,
			     int *logical_h, int *drawable_w, int *drawable_h,
			     float *scale)
{
	int lw = 0, lh = 0, dw = 0, dh = 0;
	float s = 1.0f;

	if (p != NULL && p->window != NULL) {
		SDL_GetWindowSize(p->window, &lw, &lh);
		SDL_GetWindowSizeInPixels(p->window, &dw, &dh);
		s = SDL_GetWindowDisplayScale(p->window);
		if (s <= 0.0f) {
			s = 1.0f;
		}
	}
	if (logical_w != NULL)  *logical_w = lw;
	if (logical_h != NULL)  *logical_h = lh;
	if (drawable_w != NULL) *drawable_w = dw;
	if (drawable_h != NULL) *drawable_h = dh;
	if (scale != NULL)      *scale = s;
}

void client_platform_set_logical_size(ClientPlatform *p, int w, int h)
{
	if (p != NULL && p->window != NULL) {
		SDL_SetWindowSize(p->window, w, h);
	}
}

void client_platform_mouse(const ClientPlatform *p, float *x, float *y)
{
	float mx = 0.0f, my = 0.0f;
	(void)p;
	SDL_GetMouseState(&mx, &my);
	if (x != NULL) *x = mx;
	if (y != NULL) *y = my;
}

void client_platform_inject_key(ClientPlatform *p, int sdl_keycode, int down)
{
	SDL_Event e;
	SDL_zero(e);
	e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
	e.key.windowID = SDL_GetWindowID(p->window);
	e.key.key = (SDL_Keycode)sdl_keycode;
	e.key.scancode = SDL_SCANCODE_UNKNOWN;
	e.key.down = down ? true : false;
	SDL_PushEvent(&e);
}

void client_platform_inject_scancode(ClientPlatform *p, int scancode, int down)
{
	SDL_Event e;
	SDL_zero(e);
	e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
	e.key.windowID = SDL_GetWindowID(p->window);
	e.key.scancode = (SDL_Scancode)scancode;
	e.key.key = SDL_GetKeyFromScancode((SDL_Scancode)scancode, SDL_KMOD_NONE,
					   false);
	e.key.down = down ? true : false;
	SDL_PushEvent(&e);
}

void client_platform_inject_mouse_motion(ClientPlatform *p, float x, float y)
{
	SDL_Event e;
	SDL_zero(e);
	e.type = SDL_EVENT_MOUSE_MOTION;
	e.motion.windowID = SDL_GetWindowID(p->window);
	e.motion.x = x;
	e.motion.y = y;
	SDL_PushEvent(&e);
}

void client_platform_inject_mouse_button(ClientPlatform *p, int button, int down,
					 float x, float y)
{
	SDL_Event e;
	SDL_zero(e);
	e.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
	e.button.windowID = SDL_GetWindowID(p->window);
	e.button.button = (Uint8)button;
	e.button.down = down ? true : false;
	e.button.x = x;
	e.button.y = y;
	SDL_PushEvent(&e);
}

void client_platform_inject_quit(ClientPlatform *p)
{
	SDL_Event e;
	(void)p;
	SDL_zero(e);
	e.type = SDL_EVENT_QUIT;
	SDL_PushEvent(&e);
}

void client_platform_inject_focus(ClientPlatform *p, int focused)
{
	SDL_Event e;
	SDL_zero(e);
	e.type = focused ? SDL_EVENT_WINDOW_FOCUS_GAINED
			 : SDL_EVENT_WINDOW_FOCUS_LOST;
	e.window.windowID = SDL_GetWindowID(p->window);
	SDL_PushEvent(&e);
}

void client_platform_render(ClientPlatform *p)
{
	int w = 0, h = 0;
	SDL_FRect border;
	float t;

	if (p == NULL || p->renderer == NULL) {
		return;
	}
	SDL_GetRenderOutputSize(p->renderer, &w, &h);

	SDL_SetRenderDrawColor(p->renderer, 0x10, 0x14, 0x18, 0xFF);
	SDL_RenderClear(p->renderer);

	SDL_SetRenderDrawColor(p->renderer, 0x3C, 0xC8, 0x78, 0xFF);
	border.x = 8.0f;
	border.y = 8.0f;
	border.w = (float)w - 16.0f;
	border.h = (float)h - 16.0f;
	SDL_RenderRect(p->renderer, &border);

	t = (float)((client_platform_ticks_ns() / 1000000ULL) % 1000ULL) / 1000.0f;
	SDL_SetRenderDrawColor(p->renderer, 0xC8, 0xC8, 0x3C, 0xFF);
	SDL_RenderLine(p->renderer, 0.0f, t * (float)h, (float)w, t * (float)h);

	SDL_RenderPresent(p->renderer);
}

void client_platform_present(ClientPlatform *p, struct SDL_Surface *surf)
{
	if (p == NULL || p->renderer == NULL || surf == NULL) {
		return;
	}
	if (p->texture == NULL || p->tex_w != surf->w || p->tex_h != surf->h) {
		if (p->texture != NULL) {
			SDL_DestroyTexture(p->texture);
			p->texture = NULL;
		}
		p->texture = SDL_CreateTexture(p->renderer,
					       SDL_PIXELFORMAT_ARGB8888,
					       SDL_TEXTUREACCESS_STREAMING,
					       surf->w, surf->h);
		p->tex_w = surf->w;
		p->tex_h = surf->h;
	}
	if (p->texture == NULL) {
		return;
	}
	SDL_UpdateTexture(p->texture, NULL, surf->pixels, surf->pitch);
	SDL_SetRenderDrawColor(p->renderer, 0x00, 0x00, 0x00, 0xFF);
	SDL_RenderClear(p->renderer);
	SDL_RenderTexture(p->renderer, p->texture, NULL, NULL);
	SDL_RenderPresent(p->renderer);
}

uint64_t client_platform_ticks_ns(void)
{
	return SDL_GetTicksNS();
}

void client_platform_delay(uint32_t ms)
{
	SDL_Delay(ms);
}
