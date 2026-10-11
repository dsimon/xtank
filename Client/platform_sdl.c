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

struct ClientPlatform {
	SDL_Window   *window;
	SDL_Renderer *renderer;
};

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
		case SDL_EVENT_KEY_DOWN:
			ev->key_down++;
			ev->last_key = (int)e.key.key;
			break;
		case SDL_EVENT_KEY_UP:
			ev->key_up++;
			ev->last_key = (int)e.key.key;
			break;
		case SDL_EVENT_MOUSE_MOTION:
			ev->mouse_motion++;
			ev->mouse_x = e.motion.x;
			ev->mouse_y = e.motion.y;
			break;
		case SDL_EVENT_MOUSE_BUTTON_DOWN:
		case SDL_EVENT_MOUSE_BUTTON_UP:
			ev->mouse_button++;
			ev->last_button = (int)e.button.button;
			ev->mouse_x = e.button.x;
			ev->mouse_y = e.button.y;
			break;
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

uint64_t client_platform_ticks_ns(void)
{
	return SDL_GetTicksNS();
}

void client_platform_delay(uint32_t ms)
{
	SDL_Delay(ms);
}
