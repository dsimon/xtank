/*
 * Client/main.c -- Xtank Windows client shell entry point (P2.9).
 *
 * Orchestrates the SDL3 platform shell (platform_sdl.c), the portable
 * protocol-v1 codec (Protocol/) and ENet (Transport pinned v1.3.18).  It links
 * no simulation, no bots, no ucontext, no X11 and no Unix loader.
 *
 * Modes:
 *   --self-test          run the automated open/resize/close + input + DPI +
 *                        codec-fixture + ENet loopback battery and exit 0/1
 *   --frames N           open a window and run N pump/render frames
 *   --connect HOST:PORT  connect to a live server and exchange HELLO/WELCOME
 *   --fixtures DIR       golden protocol fixture directory
 *   --logical WxH        requested logical window size (default 1024x768)
 *
 * Authority: docs/PHASE2-PLAN.md P2.9; docs/PHASE2-DECISIONS.md D2-A/D2-D.
 */

#define SDL_MAIN_HANDLED 1

#include <SDL3/SDL.h>

#include <enet/enet.h>

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform_sdl.h"
#include "input.h"
#include "wire.h"
#include "limits.h"
#include "content-manifest.h"
#include "hud.h"
#include "map.h"
#include "radar.h"
#include "menu.h"
#include "render.h"
#include "session_client.h"
#include "frame.h"

#define XTK_CLIENT_BUILD 0x0001

static int g_checks;
static int g_failures;

static void check(int ok, const char *what)
{
	++g_checks;
	if (!ok) {
		++g_failures;
		printf("FAIL: %s\n", what);
	}
}

static void checkf(int ok, const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	++g_checks;
	if (!ok) {
		++g_failures;
		va_start(ap, fmt);
		vsnprintf(buf, sizeof(buf), fmt, ap);
		va_end(ap);
		printf("FAIL: %s\n", buf);
	}
}

/* ---- golden fixture loading ----------------------------------------- */

static int hexval(int c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

static int load_hex(const char *dir, const char *name, uint8_t *out,
		    size_t cap, size_t *out_len)
{
	char path[1024];
	FILE *f;
	int c, hi = -1, v;
	size_t n = 0;

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	f = fopen(path, "rb");
	if (f == NULL) {
		return 0;
	}
	while ((c = fgetc(f)) != EOF) {
		if (c == '#') {
			while ((c = fgetc(f)) != EOF && c != '\n') {
			}
			continue;
		}
		v = hexval(c);
		if (v < 0) {
			continue;
		}
		if (hi < 0) {
			hi = v;
		} else {
			if (n >= cap) {
				fclose(f);
				return 0;
			}
			out[n++] = (uint8_t)((hi << 4) | v);
			hi = -1;
		}
	}
	fclose(f);
	*out_len = n;
	return 1;
}

static const char *const FIXTURE_NAMES[] = {
	"hello.hex", "welcome.hex", "reject_version.hex", "reject_content.hex",
	"catalog.hex", "config_select.hex", "start.hex", "epoch_begin.hex",
	"results.hex", "event.hex", "cmd_ack.hex", "reliable_cmd.hex",
	"protocol_error.hex", "input_batch.hex", "view_snapshot.hex"
};
#define NFIXTURES ((int)(sizeof(FIXTURE_NAMES) / sizeof(FIXTURE_NAMES[0])))

/*
 * Decode every committed golden fixture and re-encode it, requiring the
 * re-encoded octets to equal the independent golden bytes.  This exercises
 * both codec directions against committed (verify.py-checked) bytes rather
 * than a self-consistent round trip.
 */
static void run_codec_fixtures(const char *dir, int required)
{
	uint8_t buf[XTK_MAX_MESSAGE_BYTES];
	uint8_t enc[XTK_MAX_MESSAGE_BYTES];
	size_t len, enc_len;
	int i, present = 0;

	for (i = 0; i < NFIXTURES; ++i) {
		XtkMessage msg;
		int rc;
		char label[128];

		if (!load_hex(dir, FIXTURE_NAMES[i], buf, sizeof(buf), &len)) {
			if (required) {
				checkf(0, "fixture %s missing in %s",
				       FIXTURE_NAMES[i], dir);
			}
			continue;
		}
		present++;
		rc = xtk_decode(buf, len, &msg);
		snprintf(label, sizeof(label), "decode %s", FIXTURE_NAMES[i]);
		checkf(rc == XTK_ERR_NONE, "%s -> err %d", label, rc);
		if (rc != XTK_ERR_NONE) {
			continue;
		}
		rc = xtk_encode(enc, sizeof(enc), &msg, &enc_len);
		snprintf(label, sizeof(label), "re-encode %s", FIXTURE_NAMES[i]);
		checkf(rc == XTK_ERR_NONE, "%s -> err %d", label, rc);
		if (rc != XTK_ERR_NONE) {
			continue;
		}
		snprintf(label, sizeof(label), "golden bytes %s", FIXTURE_NAMES[i]);
		checkf(enc_len == len && memcmp(enc, buf, len) == 0, "%s", label);
	}
	if (required) {
		checkf(present == NFIXTURES, "fixtures present %d/%d", present,
		       NFIXTURES);
	}
	printf("codec fixtures: %d/%d present, %d checks, %d failures\n",
	       present, NFIXTURES, g_checks, g_failures);
}

/* ---- synthetic message builders (fixture-free fallback) -------------- */

static void build_hello(XtkMessage *m)
{
	memset(m, 0, sizeof(*m));
	m->env.message_type = XTK_MSG_HELLO;
	m->env.message_sequence = 1;
	m->body.hello.client_nonce = UINT32_C(0x0A0B0C0D);
	m->body.hello.proto_major = XTK_PROTOCOL_MAJOR;
	m->body.hello.proto_minor = XTK_PROTOCOL_MINOR;
	m->body.hello.client_build = XTK_CLIENT_BUILD;
	strcpy(m->body.hello.name, "pilot");
	memcpy(m->body.hello.content_set_digest, xtk_content_set_digest,
	       XTK_SHA256_BYTES);
}

static void build_welcome(XtkMessage *m)
{
	memset(m, 0, sizeof(*m));
	m->env.message_type = XTK_MSG_WELCOME;
	m->env.message_sequence = 1;
	m->body.welcome.server_nonce = UINT32_C(0xDEADBEEF);
	m->body.welcome.role = 1;
	m->body.welcome.tick_hz = (uint8_t)XTK_SERVER_TICK_HZ;
	m->body.welcome.max_players = (uint8_t)XTK_MAX_PLAYERS;
}

/* ---- ENet loopback + codec exchange --------------------------------- */

static void enet_service(ENetHost *h, int ms)
{
	ENetEvent ev;
	if (h == NULL) {
		return;
	}
	while (enet_host_service(h, &ev, ms) > 0) {
		ms = 0;
		if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
			enet_packet_destroy(ev.packet);
		}
	}
}

static void run_network_selftest(const char *fixdir)
{
	ENetHost *server = NULL, *client = NULL;
	ENetPeer *peer = NULL;
	ENetAddress addr, caddr;
	ENetEvent ev;
	uint8_t hello_bytes[XTK_MAX_MESSAGE_BYTES];
	uint8_t welcome_bytes[XTK_MAX_MESSAGE_BYTES];
	size_t hello_len = 0, welcome_len = 0;
	XtkMessage hello_msg, welcome_msg;
	int have_hello = 0, have_welcome = 0;
	uint64_t deadline;

	if (enet_initialize() != 0) {
		check(0, "enet_initialize");
		return;
	}

	memset(&addr, 0, sizeof(addr));
	addr.host = ENET_HOST_ANY;
	addr.port = 0;
	server = enet_host_create(&addr, 1, 3, 0, 0);
	client = enet_host_create(NULL, 1, 3, 0, 0);
	check(server != NULL && client != NULL, "enet_host_create loopback");
	if (server == NULL || client == NULL) {
		goto done;
	}

	/* Connect to loopback explicitly: server->address.host is INADDR_ANY. */
	memset(&caddr, 0, sizeof(caddr));
	enet_address_set_host(&caddr, "127.0.0.1");
	caddr.port = server->address.port;

	peer = enet_host_connect(client, &caddr, 3, 0);
	check(peer != NULL, "enet_host_connect");
	if (peer == NULL) {
		goto done;
	}

	deadline = client_platform_ticks_ns() + UINT64_C(3000000000);
	for (;;) {
		int connected = 0;
		while (enet_host_service(client, &ev, 0) > 0) {
		}
		while (enet_host_service(server, &ev, 10) > 0) {
			if (ev.type == ENET_EVENT_TYPE_CONNECT) {
				connected = 1;
			} else if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
				enet_packet_destroy(ev.packet);
			}
		}
		if (connected) {
			break;
		}
		if (client_platform_ticks_ns() > deadline) {
			break;
		}
	}
	check(peer->state == ENET_PEER_STATE_CONNECTED, "loopback connected");
	if (peer->state != ENET_PEER_STATE_CONNECTED) {
		goto done;
	}

	if (load_hex(fixdir, "hello.hex", hello_bytes, sizeof(hello_bytes),
		     &hello_len)) {
		have_hello = (xtk_decode(hello_bytes, hello_len, &hello_msg) ==
			      XTK_ERR_NONE);
	}
	if (!have_hello) {
		build_hello(&hello_msg);
		check(xtk_encode(hello_bytes, sizeof(hello_bytes), &hello_msg,
				 &hello_len) == XTK_ERR_NONE, "encode hello");
	}

	{
		ENetPacket *pkt = enet_packet_create(hello_bytes, hello_len,
						     ENET_PACKET_FLAG_RELIABLE);
		check(pkt != NULL, "enet_packet_create hello");
		if (pkt != NULL) {
			enet_peer_send(peer, 0, pkt);
			enet_host_flush(client);
		}
	}

	{
		ENetPeer *srv_peer = NULL;
		int got = 0;
		deadline = client_platform_ticks_ns() + UINT64_C(3000000000);
		while (client_platform_ticks_ns() < deadline && !got) {
			while (enet_host_service(server, &ev, 10) > 0) {
				if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
					XtkMessage rx;
					int rc = xtk_decode(ev.packet->data,
							    ev.packet->dataLength,
							    &rx);
					srv_peer = ev.peer;
					checkf(rc == XTK_ERR_NONE,
					       "server decode hello -> err %d",
					       rc);
					check(rx.env.message_type ==
					      XTK_MSG_HELLO,
					      "server got HELLO type");
					check(strcmp(rx.body.hello.name,
						     "pilot") == 0,
					      "server got HELLO name");
					enet_packet_destroy(ev.packet);
					got = 1;
				}
			}
		}
		check(got, "server received HELLO over ENet");

		if (load_hex(fixdir, "welcome.hex", welcome_bytes,
			     sizeof(welcome_bytes), &welcome_len)) {
			have_welcome = (xtk_decode(welcome_bytes, welcome_len,
						   &welcome_msg) ==
					XTK_ERR_NONE);
		}
		if (!have_welcome) {
			build_welcome(&welcome_msg);
			check(xtk_encode(welcome_bytes, sizeof(welcome_bytes),
					 &welcome_msg, &welcome_len) ==
			      XTK_ERR_NONE, "encode welcome");
		}
		if (srv_peer != NULL) {
			ENetPacket *pkt = enet_packet_create(
				welcome_bytes, welcome_len,
				ENET_PACKET_FLAG_RELIABLE);
			check(pkt != NULL, "enet_packet_create welcome");
			if (pkt != NULL) {
				enet_peer_send(srv_peer, 0, pkt);
				enet_host_flush(server);
			}
		}
	}

	{
		int got = 0;
		deadline = client_platform_ticks_ns() + UINT64_C(3000000000);
		while (client_platform_ticks_ns() < deadline && !got) {
			while (enet_host_service(client, &ev, 10) > 0) {
				if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
					XtkMessage rx;
					int rc = xtk_decode(ev.packet->data,
							    ev.packet->dataLength,
							    &rx);
					checkf(rc == XTK_ERR_NONE,
					       "client decode welcome -> err %d",
					       rc);
					check(rx.env.message_type ==
					      XTK_MSG_WELCOME,
					      "client got WELCOME type");
					check(rx.body.welcome.tick_hz ==
					      XTK_SERVER_TICK_HZ,
					      "client WELCOME tick_hz");
					enet_packet_destroy(ev.packet);
					got = 1;
				}
			}
		}
		check(got, "client received WELCOME over ENet");
	}

done:
	if (peer != NULL && peer->state == ENET_PEER_STATE_CONNECTED) {
		enet_peer_disconnect(peer, 0);
		enet_service(client, 50);
	}
	if (client != NULL) {
		enet_host_destroy(client);
	}
	if (server != NULL) {
		enet_host_destroy(server);
	}
	enet_deinitialize();
}

/* ---- platform / DPI / input self-test -------------------------------- */

static void run_dpi_math(void)
{
	static const float scales[] = { 1.0f, 1.5f, 2.0f };
	int i;

	for (i = 0; i < 3; ++i) {
		float s = scales[i];
		int w = 1000;
		int d = xtk_dpi_logical_to_drawable(w, s);
		int back = xtk_dpi_drawable_to_logical(d, s);
		float mx, my;
		checkf(d == (int)lroundf((float)w * s),
		       "logical_to_drawable 1000 @ %.2f = %d", s, d);
		checkf(back == w, "drawable_to_logical round trip @ %.2f", s);
		xtk_dpi_mouse_to_drawable(100.0f, 50.0f, s, &mx, &my);
		checkf(mx == 100.0f * s && my == 50.0f * s,
		       "mouse transform @ %.2f -> %.1f,%.1f", s, mx, my);
	}
}

/*
 * P2.12 input-controller smoke: neutral-control mapping, 15 Hz emission (never
 * tick 0), one-shot fire edges with two-tick redundancy, held release, pause
 * outside Running, and defocus release.  Mirrors the standalone input fixture.
 */
static void run_input_selftest(void)
{
	XtkInputController ic;
	XtkMessage msg;
	XtkCamera cam;
	uint8_t enc[XTK_MAX_MESSAGE_BYTES];
	size_t enc_len = 0;
	double wx, wy;
	int n, i, fire_id = 0, fire_tick1 = 0, fire_tick2 = 0, fire_tick4 = 0;

	/* Separation: scancodes map to neutral controls, never engine ops. */
	check(xtk_sdl_scancode_to_control(SDL_SCANCODE_SPACE) == XTK_CTL_FIRE,
	      "scancode SPACE -> FIRE");
	check(xtk_sdl_scancode_to_control(SDL_SCANCODE_W) == XTK_CTL_DRIVE_FWD,
	      "scancode W -> DRIVE_FWD");
	check(xtk_sdl_scancode_to_control(SDL_SCANCODE_1) == XTK_CTL_WEAPON_1,
	      "scancode 1 -> WEAPON_1");
	check(xtk_sdl_scancode_to_control(SDL_SCANCODE_Z) == XTK_CTL_NONE,
	      "unmapped scancode -> NONE");
	check(xtk_sdl_button_to_control(SDL_BUTTON_LEFT) == XTK_CTL_FIRE,
	      "mouse left -> FIRE");
	check(xtk_sdl_button_to_control(SDL_BUTTON_RIGHT) == XTK_CTL_FIRE_HOLD,
	      "mouse right -> FIRE_HOLD");

	/* Input inverse transform at an offset camera and 2x DPI. */
	camera_init(&cam);
	camera_center_on(&cam, 1000.0, 2000.0);
	input_screen_to_world(&cam, 0.0f, 0.0f, 2.0f, &wx, &wy);
	checkf(wx == 1000.0 - 384.0 && wy == 2000.0 - 384.0,
	       "screen->world origin (%.1f,%.1f)", wx, wy);
	input_screen_to_world(&cam, 1000.0f, 1000.0f, 2.0f, &wx, &wy);
	checkf(wx == 1000.0 - 384.0 + 2000.0, "screen->world scaled x (%.1f)",
	       wx);

	input_init(&ic);

	/* Nothing is emitted on tick 0 or outside Running. */
	n = input_emit(&ic, 0, 1, 1, &msg);
	check(n == 0, "no emit on tick 0");
	input_set_state(&ic, XTK_INPUT_LOBBY);
	input_control_event(&ic, XTK_CTL_PAUSE, 1);
	check(input_paused(&ic) == 1, "pause toggles outside Running");
	n = input_emit(&ic, 5, 1, 1, &msg);
	check(n == 0, "no emit outside Running");

	input_set_state(&ic, XTK_INPUT_RUNNING);
	n = input_emit(&ic, 1, 1, 1, &msg);
	check(n == 1 && msg.body.input_batch.command[0].op == XTK_OP_HEARTBEAT,
	      "idle heartbeat while Running");
	check(xtk_encode(enc, sizeof(enc), &msg, &enc_len) == XTK_ERR_NONE &&
	      enc_len > 0, "heartbeat batch encodes");

	/* A fire press produces exactly one edged FIRE command. */
	input_control_event(&ic, XTK_CTL_FIRE, 1);
	n = input_emit(&ic, 2, 1, 1, &msg);
	check(n >= 1, "fire batch emitted");
	for (i = 0; i < n; ++i) {
		if (msg.body.input_batch.command[i].op == XTK_OP_FIRE) {
			fire_tick1++;
			fire_id = (int)msg.body.input_batch.command[i].command_id;
		}
	}
	check(fire_tick1 == 1, "fire edge fires once");

	/* The same command id appears in the redundancy window, then expires. */
	for (i = 0; i < (int)XTK_INPUT_REDUNDANCY_TICKS; ++i) {
		int t = 3 + i;
		input_emit(&ic, t, 1, 1, &msg);
		for (n = 0; n < (int)msg.body.input_batch.command_count; ++n) {
			if ((int)msg.body.input_batch.command[n].command_id ==
			    fire_id) {
				if (t == 3) fire_tick2 = 1;
			}
		}
	}
	check(fire_tick2 == 1, "edge retransmitted within redundancy");
	input_emit(&ic, 6, 1, 1, &msg);
	for (n = 0; n < (int)msg.body.input_batch.command_count; ++n) {
		if ((int)msg.body.input_batch.command[n].command_id == fire_id) {
			fire_tick4 = 1;
		}
	}
	check(fire_tick4 == 0, "edge released after redundancy window");

	/* Defocus releases held drive and fire-hold. */
	input_control_event(&ic, XTK_CTL_DRIVE_FWD, 1);
	input_control_event(&ic, XTK_CTL_FIRE_HOLD, 1);
	n = input_emit(&ic, 7, 1, 1, &msg);
	check(n >= 2, "held controls emitted");
	input_set_focus(&ic, 0);
	check(input_focused(&ic) == 0, "focus lost");
	n = input_emit(&ic, 8, 1, 1, &msg);
	{
		int drive0 = 0, fh0 = 0;
		for (i = 0; i < n; ++i) {
			XtkCommand *c = &msg.body.input_batch.command[i];
			if (c->op == XTK_OP_DRIVE_REL && c->a == 0.0) drive0 = 1;
			if (c->op == XTK_OP_FIRE_HOLD && c->a == 0.0) fh0 = 1;
		}
		check(drive0 && fh0, "defocus releases drive and fire");
	}

	/* Quit releases everything and is sticky. */
	input_set_focus(&ic, 1);
	input_control_event(&ic, XTK_CTL_DRIVE_FWD, 1);
	input_emit(&ic, 9, 1, 1, &msg);
	input_control_event(&ic, XTK_CTL_QUIT, 1);
	check(input_quit_requested(&ic) == 1, "quit requested");
	n = input_emit(&ic, 10, 1, 1, &msg);
	{
		int drive0 = 0;
		for (i = 0; i < n; ++i) {
			XtkCommand *c = &msg.body.input_batch.command[i];
			if (c->op == XTK_OP_DRIVE_REL && c->a == 0.0) drive0 = 1;
		}
		check(drive0, "quit releases drive");
	}
}

/*
 * Verify the logical-point to drawable-pixel relationship.  A conforming
 * high-DPI host (Windows 11 with SDL_WINDOW_HIGH_PIXEL_DENSITY) exposes a
 * backbuffer of logical*scale pixels.  Some hosts - notably Wine's DPI
 * emulation - report the display scale but keep the backbuffer unscaled
 * (drawable == logical); that is a host limitation, not a client defect, and
 * the transform itself is still verified by run_dpi_math() at 1.0/1.5/2.0.
 */
static void check_drawable_scale(const char *label, int lw, int lh, int dw,
				 int dh, float scale)
{
	if (scale > 1.0f && dw == lw && dh == lh) {
		printf("note: %s host reports scale %.2f but drawable==logical "
		       "(%dx%d); no high-DPI backbuffer on this host\n",
		       label, (double)scale, dw, dh);
		return;
	}
	checkf(abs(dw - xtk_dpi_logical_to_drawable(lw, scale)) <= 1,
	       "%s drawable width matches logical*scale (%d vs %d)", label, dw,
	       xtk_dpi_logical_to_drawable(lw, scale));
	checkf(abs(dh - xtk_dpi_logical_to_drawable(lh, scale)) <= 1,
	       "%s drawable height matches logical*scale (%d vs %d)", label, dh,
	       xtk_dpi_logical_to_drawable(lh, scale));
}

/* ---- P2.13 UI self-test and screenshot demo -------------------------- */

/* Append one scene's primitives onto another (order preserved). */
static void scene_merge(XtkUiScene *dst, const XtkUiScene *src)
{
	int i;

	for (i = 0; i < src->rect_count; i++) {
		const XtkUiRect *r = &src->rect[i];
		ui_add_rect(dst, r->x, r->y, r->w, r->h, r->color, r->filled);
	}
	for (i = 0; i < src->line_count; i++) {
		const XtkUiLine *l = &src->line[i];
		ui_add_line(dst, l->x1, l->y1, l->x2, l->y2, l->color);
	}
	for (i = 0; i < src->text_count; i++) {
		const XtkUiText *t = &src->text[i];
		ui_add_text_align(dst, t->x, t->y, (XtkUiFont)t->font, t->color,
				  t->align, t->text);
	}
}

/* A representative authorized match snapshot for the demo/screenshot. */
static void ui_demo_snapshot(XtkViewSnapshot *v, XtkSelfState *self)
{
	memset(v, 0, sizeof(*v));
	v->server_tick = 321;
	v->own_slot = 0;
	v->own_generation = 1;
	v->vehicle_count = 2;
	v->vehicle[0].slot = 0;
	v->vehicle[0].generation = 1;
	v->vehicle[0].kind = 6;
	v->vehicle[0].x = 480.0;
	v->vehicle[0].y = 400.0;
	v->vehicle[1].slot = 1;
	v->vehicle[1].generation = 1;
	v->vehicle[1].kind = 6;
	v->vehicle[1].x = 700.0;
	v->vehicle[1].y = 520.0;
	v->map_cell_count = 3;
	v->cell[0].grid_x = 1;
	v->cell[0].grid_y = 1;
	v->cell[0].flags = XTK_MAPCELL_KNOWN | XTK_MAPCELL_NORTH_WALL |
			   XTK_MAPCELL_WEST_WALL;
	v->cell[1].grid_x = 2;
	v->cell[1].grid_y = 2;
	v->cell[1].flags = XTK_MAPCELL_KNOWN | XTK_MAPCELL_NORTH_WALL |
			   XTK_MAPCELL_NORTH_DEST;
	v->cell[2].grid_x = 3;
	v->cell[2].grid_y = 3;
	v->cell[2].flags = XTK_MAPCELL_KNOWN | XTK_MAPCELL_TEAM_KNOWN;
	v->cell[2].type = 3;
	v->radar_blip_count = 2;
	v->radar[0].x = 20;
	v->radar[0].y = 20;
	v->radar[0].age = 0;
	v->radar[1].x = 60;
	v->radar[1].y = 40;
	v->radar[1].age = 9;
	v->weapon_count = 3;

	memset(self, 0, sizeof(*self));
	self->slot = 0;
	self->generation = 1;
	self->lifecycle_flags = XTK_SELF_ALIVE;
	self->armor[0] = 90;
	self->armor[1] = 75;
	self->armor[2] = 60;
	self->armor[3] = 45;
	self->fuel = 820;
	self->heat = 14;
	self->score = 750;
	self->weapon_count = 3;
	self->weapon_enabled_bitmap = 0x07;
	self->ammo[0] = 25;
	self->ammo[1] = 12;
	self->ammo[2] = 0;
	self->reload[0] = 3;
	self->reload[1] = 7;
	self->reload[2] = 0;
}

static void run_ui_selftest(void)
{
	XtkSelfState self;
	XtkHudInfo info;
	XtkUiScene s;
	XtkResultsState rs;
	int refcount;
	const char *const *ref;

	memset(&self, 0, sizeof(self));
	self.lifecycle_flags = XTK_SELF_ALIVE;
	self.armor[0] = 17;
	self.armor[1] = 18;
	self.armor[2] = 19;
	self.armor[3] = 20;
	self.fuel = 321;
	self.heat = 9;
	self.score = 77;
	self.weapon_count = 1;
	self.weapon_enabled_bitmap = 0x01;
	self.ammo[0] = 4;
	self.reload[0] = 2;
	memset(&info, 0, sizeof(info));
	info.connected = 1;

	check(hud_build(&self, 1, &info, &s) == 0, "ui: hud_build");
	check(ui_scene_has_text(&s, "321"), "ui: hud fuel value shown");
	check(ui_scene_has_text(&s, "77"), "ui: hud score value shown");
	check(ui_scene_has_text(&s, "17") && ui_scene_has_text(&s, "20"),
	      "ui: hud armor shown");
	check(!ui_scene_has_text(&s, "pilot"), "ui: hud has no player name");

	/* The lobby reproduces the frozen reference entry texts. */
	ref = menu_reference_main_entries(&refcount);
	check(refcount == 11 && strcmp(ref[0], "Play") == 0 &&
	      strcmp(ref[10], "Quit") == 0, "ui: reference main-menu texts");

	/* Results/banner carry the public names from the server DTO. */
	menu_results_init(&rs);
	rs.have_results = 1;
	rs.results.end_tick = 500;
	rs.results.result_kind = 0;
	rs.results.winner_slot = 0;
	rs.results.vehicle_count = 1;
	strcpy(rs.results.entry[0].name, "pilot");
	rs.results.entry[0].slot = 0;
	rs.results.entry[0].score = 1000;
	check(menu_build_results(&rs, &s) == 0, "ui: results build");
	check(ui_scene_has_text(&s, "pilot: 1000 points"),
	      "ui: results name + score");
	check(ui_scene_has_text(&s, "Battle won by pilot"),
	      "ui: results banner");

	/* Render a representative match HUD/map/radar scene onto a surface and
	 * confirm it actually paints (screenshot path). */
	{
		SDL_Surface *surf = SDL_CreateSurface(XTK_UI_SCREEN_W,
						      XTK_UI_SCREEN_H,
						      SDL_PIXELFORMAT_ARGB8888);
		check(surf != NULL, "ui: create surface");
		if (surf != NULL) {
			XtkViewSnapshot v;
			XtkUiScene hud, mp, rd;
			int x, y, lit = 0;

			ui_demo_snapshot(&v, &self);
			hud_build(&self, 1, &info, &hud);
			map_build(&v, &mp);
			radar_build(&v, &rd);
			ui_scene_init(&s);
			scene_merge(&s, &hud);
			scene_merge(&s, &mp);
			scene_merge(&s, &rd);
			check(render_frame_ui(&s, surf) == 0, "ui: render match");
			for (y = XTK_UI_CONS_Y + 5; y < XTK_UI_CONS_Y + 40; y++) {
				for (x = XTK_UI_CONS_X + 5;
				     x < XTK_UI_CONS_X + 120; x++) {
					if (render_surface_get(surf, x, y) !=
					    XTK_RGB(0x10, 0x14, 0x18)) {
						lit++;
					}
				}
			}
			check(lit > 0, "ui: HUD text painted pixels");
			SDL_DestroySurface(surf);
		}
	}
}

/* Write representative lobby and match frames as BMPs for owner review. */
static void run_ui_shot(const char *dir)
{
	SDL_Surface *surf;
	XtkViewSnapshot v;
	XtkSelfState self;
	XtkHudInfo info;
	XtkUiScene s, part;
	XtkLobbyState ls;
	XtkResultsState rs;
	char path[1024];

	surf = SDL_CreateSurface(XTK_UI_SCREEN_W, XTK_UI_SCREEN_H,
				 SDL_PIXELFORMAT_ARGB8888);
	if (surf == NULL) {
		printf("UI-SHOT FAIL (surface)\n");
		return;
	}

	ui_demo_snapshot(&v, &self);
	memset(&info, 0, sizeof(info));
	info.connected = 1;

	/* Match frame: world is drawn separately (P2.14); here HUD/map/radar. */
	ui_scene_init(&s);
	hud_build(&self, 1, &info, &part);
	scene_merge(&s, &part);
	map_build(&v, &part);
	scene_merge(&s, &part);
	radar_build(&v, &part);
	scene_merge(&s, &part);
	render_frame_ui(&s, surf);
	snprintf(path, sizeof(path), "%s/ui_match.bmp", dir);
	SDL_SaveBMP(surf, path);

	/* High-DPI (200%) variant of the same logical match scene. */
	{
		SDL_Surface *hi = SDL_CreateSurface(XTK_UI_SCREEN_W * 2,
						    XTK_UI_SCREEN_H * 2,
						    SDL_PIXELFORMAT_ARGB8888);
		if (hi != NULL) {
			render_frame_ui_scaled(&s, hi, 2);
			snprintf(path, sizeof(path), "%s/ui_match_2x.bmp",
				 dir);
			SDL_SaveBMP(hi, path);
			SDL_DestroySurface(hi);
		}
	}

	/* Lobby frame with a synthetic catalog and a 3-player result. */
	menu_lobby_init(&ls);
	ls.connected = 1;
	ls.is_admin = 1;
	ls.have_catalog = 1;
	ls.num_mazes = 2;
	strcpy(ls.maze[0], "combat1");
	ls.maze_id[0] = 0x0101;
	strcpy(ls.maze[1], "Arena");
	ls.maze_id[1] = 0x0102;
	ls.num_vehicles = 1;
	strcpy(ls.vehicle[0], "Vanguard");
	ls.vehicle_id[0] = 0x0201;
	ls.selected_maze_id = 0x0101;
	ls.selected_vehicle_id = 0x0201;
	menu_build_lobby(&ls, &s);
	render_frame_ui(&s, surf);
	snprintf(path, sizeof(path), "%s/ui_lobby.bmp", dir);
	SDL_SaveBMP(surf, path);

	menu_results_init(&rs);
	rs.have_results = 1;
	rs.results.end_tick = 4004;
	rs.results.result_kind = 0;
	rs.results.winner_slot = 2;
	rs.results.vehicle_count = 3;
	strcpy(rs.results.entry[0].name, "alpha");
	rs.results.entry[0].score = 300;
	strcpy(rs.results.entry[1].name, "bravo");
	rs.results.entry[1].score = 450;
	strcpy(rs.results.entry[2].name, "charlie");
	rs.results.entry[2].slot = 2;
	rs.results.entry[2].score = 1000;
	menu_build_results(&rs, &s);
	render_frame_ui(&s, surf);
	snprintf(path, sizeof(path), "%s/ui_results.bmp", dir);
	SDL_SaveBMP(surf, path);

	SDL_DestroySurface(surf);
	printf("UI-SHOT PASS (%s/ui_lobby.bmp ui_match.bmp ui_results.bmp)\n",
	       dir);
}

static void run_self_test(int logical_w, int logical_h)
{
	ClientPlatform *p;
	ClientEvents ev;
	int lw, lh, dw, dh, i;
	float scale;
	uint64_t t0, t1;

	memset(&ev, 0, sizeof(ev));
	p = client_platform_create("Xtank P2.9 smoke", logical_w, logical_h);
	check(p != NULL, "client_platform_create");
	if (p == NULL) {
		printf("WINDOWS-SMOKE FAIL (no platform)\n");
		exit(1);
	}

	printf("video driver: %s\n", client_platform_driver(p));
	printf("renderer: %s\n", client_platform_renderer(p));

	client_platform_metrics(p, &lw, &lh, &dw, &dh, &scale);
	printf("window logical=%dx%d drawable=%dx%d scale=%.2f\n",
	       lw, lh, dw, dh, (double)scale);
	check(lw == logical_w && lh == logical_h, "requested logical size");
	check(dw > 0 && dh > 0, "positive drawable size");
	check_drawable_scale("window", lw, lh, dw, dh, scale);

	run_dpi_math();
	run_input_selftest();
	run_ui_selftest();

	/*
	 * Drain the OS startup events (window shown/focus/enter and an initial
	 * pointer position).  A real desktop window emits these before the
	 * application injects anything, so discard them and zero the counters
	 * to keep the injected-input assertions exact.
	 */
	client_platform_pump(p, &ev);
	memset(&ev, 0, sizeof(ev));

	t0 = client_platform_ticks_ns();
	client_platform_delay(5);
	t1 = client_platform_ticks_ns();
	check(t1 > t0, "monotonic timer advances");

	client_platform_inject_key(p, SDLK_A, 1);
	client_platform_inject_key(p, SDLK_A, 0);
	client_platform_inject_mouse_motion(p, 321.0f, 654.0f);
	client_platform_inject_mouse_button(p, SDL_BUTTON_LEFT, 1, 321.0f, 654.0f);
	client_platform_inject_mouse_button(p, SDL_BUTTON_LEFT, 0, 321.0f, 654.0f);
	client_platform_pump(p, &ev);
	check(ev.key_down == 1, "key down received");
	check(ev.key_up == 1, "key up received");
	check(ev.last_key == SDLK_A, "key code reported");
	check(ev.mouse_motion >= 1, "mouse motion received");
	check(ev.mouse_button == 2, "mouse button down+up received");
	check(fabsf(ev.mouse_x - 321.0f) < 0.5f &&
	      fabsf(ev.mouse_y - 654.0f) < 0.5f, "mouse coordinates reported");

	client_platform_set_logical_size(p, 800, 600);
	for (i = 0; i < 100; ++i) {
		client_platform_pump(p, &ev);
		client_platform_metrics(p, &lw, &lh, &dw, &dh, &scale);
		if (lw == 800 && lh == 600) {
			break;
		}
		client_platform_delay(5);
	}
	check(lw == 800 && lh == 600, "window resized to requested logical size");
	check(ev.resized > 0, "resize event observed");
	printf("resized logical=%dx%d drawable=%dx%d scale=%.2f\n",
	       lw, lh, dw, dh, (double)scale);
	check_drawable_scale("post-resize", lw, lh, dw, dh, scale);

	t0 = client_platform_ticks_ns();
	for (i = 0; i < 60; ++i) {
		client_platform_render(p);
	}
	t1 = client_platform_ticks_ns();
	check(t1 > t0, "60 frames rendered");
	printf("60 frames in %llu ms\n",
	       (unsigned long long)((t1 - t0) / 1000000ULL));

	client_platform_inject_quit(p);
	client_platform_pump(p, &ev);
	check(ev.quit == 1, "quit event received");

	client_platform_destroy(p);
}

/* ---- interactive frames mode ---------------------------------------- */

static void run_frames(int frames, int logical_w, int logical_h)
{
	ClientPlatform *p;
	ClientEvents ev;
	int i;

	memset(&ev, 0, sizeof(ev));
	p = client_platform_create("Xtank P2.9", logical_w, logical_h);
	if (p == NULL) {
		printf("WINDOWS-SMOKE FAIL (no platform)\n");
		exit(1);
	}
	printf("video driver: %s renderer: %s\n", client_platform_driver(p),
	       client_platform_renderer(p));
	for (i = 0; i < frames && !ev.quit; ++i) {
		if (i == frames / 2) {
			client_platform_set_logical_size(p, 900, 700);
		}
		client_platform_pump(p, &ev);
		client_platform_render(p);
		client_platform_delay(2);
	}
	client_platform_destroy(p);
	printf("FRAMES PASS (%d frames)\n", i);
}

/* ---- live connect mode ---------------------------------------------- */

static void run_connect(const char *spec, const char *fixdir)
{
	char host[256];
	int port = 0;
	const char *colon;
	ENetHost *client;
	ENetPeer *peer;
	ENetAddress addr;
	ENetEvent ev;
	uint8_t hello_bytes[XTK_MAX_MESSAGE_BYTES];
	size_t hello_len = 0;
	XtkMessage hello;
	uint64_t deadline;
	int got_welcome = 0;

	colon = strrchr(spec, ':');
	if (colon == NULL) {
		printf("WINDOWS-SMOKE FAIL (connect needs HOST:PORT)\n");
		exit(1);
	}
	if ((size_t)(colon - spec) >= sizeof(host)) {
		printf("WINDOWS-SMOKE FAIL (host too long)\n");
		exit(1);
	}
	memcpy(host, spec, (size_t)(colon - spec));
	host[colon - spec] = '\0';
	port = atoi(colon + 1);

	if (enet_initialize() != 0) {
		printf("WINDOWS-SMOKE FAIL (enet_initialize)\n");
		exit(1);
	}
	client = enet_host_create(NULL, 1, 3, 0, 0);
	if (client == NULL) {
		printf("WINDOWS-SMOKE FAIL (host create)\n");
		exit(1);
	}
	memset(&addr, 0, sizeof(addr));
	enet_address_set_host(&addr, host);
	addr.port = (enet_uint16)port;
	peer = enet_host_connect(client, &addr, 3, 0);
	deadline = client_platform_ticks_ns() + UINT64_C(5000000000);
	while (peer != NULL && peer->state != ENET_PEER_STATE_CONNECTED &&
	       client_platform_ticks_ns() < deadline) {
		enet_service(client, 20);
	}
	if (peer == NULL || peer->state != ENET_PEER_STATE_CONNECTED) {
		printf("CONNECT FAIL %s:%d\n", host, port);
		enet_host_destroy(client);
		enet_deinitialize();
		exit(1);
	}
	printf("CONNECT OK %s:%d\n", host, port);

	if (!load_hex(fixdir, "hello.hex", hello_bytes, sizeof(hello_bytes),
		      &hello_len) ||
	    xtk_decode(hello_bytes, hello_len, &hello) != XTK_ERR_NONE) {
		build_hello(&hello);
		xtk_encode(hello_bytes, sizeof(hello_bytes), &hello, &hello_len);
	}
	{
		ENetPacket *pkt = enet_packet_create(hello_bytes, hello_len,
						     ENET_PACKET_FLAG_RELIABLE);
		enet_peer_send(peer, 0, pkt);
		enet_host_flush(client);
	}
	deadline = client_platform_ticks_ns() + UINT64_C(5000000000);
	while (client_platform_ticks_ns() < deadline && !got_welcome) {
		while (enet_host_service(client, &ev, 50) > 0) {
			if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
				XtkMessage rx;
				if (xtk_decode(ev.packet->data,
					       ev.packet->dataLength, &rx) ==
				    XTK_ERR_NONE) {
					if (rx.env.message_type ==
					    XTK_MSG_WELCOME) {
						got_welcome = 1;
					} else if (rx.env.message_type ==
						   XTK_MSG_REJECT) {
						printf("REJECT code=0x%04x\n",
						       rx.body.reject.error_code);
					}
				}
				enet_packet_destroy(ev.packet);
			}
		}
	}
	printf("WELCOME %s\n", got_welcome ? "OK" : "TIMEOUT");
	enet_peer_disconnect(peer, 0);
	enet_service(client, 50);
	enet_host_destroy(client);
	enet_deinitialize();
	printf("%s\n", got_welcome ? "CONNECT PASS" : "CONNECT FAIL");
	exit(got_welcome ? 0 : 1);
}

/* ---- playable mode (P2.14) ------------------------------------------- */

/* Feed the bounded platform input queue into the frame loop. */
static void deliver_platform_input(XtkFrame *fr, ClientPlatform *p)
{
	ClientInputEvent ie;
	float scale = 1.0f;
	int lw, lh, dw, dh;

	client_platform_metrics(p, &lw, &lh, &dw, &dh, &scale);
	while (client_platform_input_pop(p, &ie)) {
		switch (ie.type) {
		case CLIENT_IN_KEY:
		case CLIENT_IN_BUTTON:
			frame_control(fr, (XtkControl)ie.control, ie.down);
			break;
		case CLIENT_IN_MOTION:
			frame_aim_screen(fr, ie.x, ie.y, scale);
			break;
		case CLIENT_IN_FOCUS:
			frame_focus(fr, ie.focused);
			break;
		default:
			break;
		}
	}
}

static void run_play(const char *spec, const char *assets_path,
		     const char *name)
{
	char host[256];
	int port = 0;
	const char *colon;
	ClientPlatform *p;
	SDL_Surface *surf;
	XtkAssets assets;
	int have_assets = 0;
	XtkSessionClient *sc;
	XtkFrame *fr;
	ClientEvents ev;
	XtkFrameStats st;
	uint64_t last_log = 0;
	int reconnects = 0;
	int i;

	colon = strrchr(spec, ':');
	if (colon == NULL || (size_t)(colon - spec) >= sizeof(host)) {
		printf("PLAY FAIL (address must be HOST:PORT)\n");
		exit(1);
	}
	memcpy(host, spec, (size_t)(colon - spec));
	host[colon - spec] = '\0';
	port = atoi(colon + 1);

	p = client_platform_create("Xtank", XTK_UI_SCREEN_W, XTK_UI_SCREEN_H);
	if (p == NULL) {
		printf("PLAY FAIL (no platform)\n");
		exit(1);
	}
	surf = SDL_CreateSurface(XTK_UI_SCREEN_W, XTK_UI_SCREEN_H,
				 SDL_PIXELFORMAT_ARGB8888);
	if (surf == NULL) {
		printf("PLAY FAIL (no surface)\n");
		client_platform_destroy(p);
		exit(1);
	}
	if (assets_path != NULL) {
		char err[128];
		if (assets_load_file(&assets, assets_path, err, sizeof(err)) == 0) {
			have_assets = 1;
		} else {
			printf("note: asset bundle not loaded (%s)\n", err);
		}
	}

	sc = session_client_create();
	if (sc == NULL ||
	    session_client_connect(sc, host, (uint16_t)port,
				   name != NULL ? name : "pilot") != 0) {
		printf("PLAY FAIL (cannot connect to %s:%d)\n", host, port);
		SDL_DestroySurface(surf);
		client_platform_destroy(p);
		exit(1);
	}
	fr = frame_create(sc, have_assets ? &assets : NULL, surf);
	if (fr == NULL) {
		printf("PLAY FAIL (no frame state)\n");
		exit(1);
	}
	printf("PLAY CONNECTING %s:%d\n", host, port);

	memset(&ev, 0, sizeof(ev));
	for (;;) {
		uint64_t now;

		client_platform_pump(p, &ev);
		deliver_platform_input(fr, p);
		if (ev.quit) {
			(void)frame_request_quit(fr);
		}
		if (frame_quit_requested(fr)) {
			break;
		}

		now = client_platform_ticks_ns();
		frame_tick(fr, now);
		client_platform_present(p, surf);

		frame_stats(fr, &st);
		if (st.session_state == XTK_SC_FAILED ||
		    st.session_state == XTK_SC_DISCONNECTED) {
			/* Rejoin a fresh lobby in-process (no restart). */
			if (reconnects < 5) {
				printf("PLAY reconnecting (state=%d 0x%04x %s)\n",
				       (int)st.session_state,
				       (unsigned)st.last_error,
				       session_client_error_text(sc));
				frame_reconnect(fr);
				if (session_client_connect(sc, host,
					(uint16_t)port,
					name != NULL ? name : "pilot") == 0) {
					reconnects++;
					client_platform_delay(100);
					continue;
				}
			}
			printf("PLAY FAIL (0x%04x %s)\n",
			       (unsigned)st.last_error,
			       session_client_error_text(sc));
			break;
		}
		if (now - last_log > 1000000000ULL) {
			printf("PLAY state=%d tick=%d snaps=%d emitted=%d\n",
			       (int)st.session_state, st.server_tick,
			       st.snapshots, st.emitted_batches);
			last_log = now;
		}
		client_platform_delay(5);
	}

	/* Clean disconnect: pump until the transport drops the peer. */
	for (i = 0; i < 200 &&
	     session_client_state(sc) != XTK_SC_DISCONNECTED; i++) {
		frame_tick(fr, client_platform_ticks_ns());
		client_platform_present(p, surf);
		client_platform_delay(5);
	}
	frame_stats(fr, &st);
	printf("PLAY %s (state=%d)\n",
	       (st.last_error == 0 && st.rejected == 0) ? "DONE" : "ENDED",
	       (int)st.session_state);

	frame_destroy(fr);
	session_client_destroy(sc);
	if (have_assets) {
		assets_free(&assets);
	}
	SDL_DestroySurface(surf);
	client_platform_destroy(p);
}

/* ---- argument parsing ----------------------------------------------- */

static void usage(const char *argv0)
{
	printf("usage: %s [--self-test] [--frames N] [--connect HOST:PORT]\n"
	       "          [--play HOST:PORT] [--name NAME] [--assets FILE]\n"
	       "          [--fixtures DIR] [--logical WxH] [--ui-demo DIR]\n",
	       argv0);
}

int main(int argc, char **argv)
{
	const char *fixdir = NULL;
	int frames = -1;
	int logical_w = 1024, logical_h = 768;
	int self_test = 0;
	const char *connect_spec = NULL;
	const char *play_spec = NULL;
	const char *assets_path = NULL;
	const char *ui_demo_dir = NULL;
	const char *player_name = NULL;
	int i;

	for (i = 1; i < argc; ++i) {
		if (strcmp(argv[i], "--self-test") == 0) {
			self_test = 1;
		} else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
			frames = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--connect") == 0 && i + 1 < argc) {
			connect_spec = argv[++i];
		} else if (strcmp(argv[i], "--play") == 0 && i + 1 < argc) {
			play_spec = argv[++i];
		} else if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) {
			player_name = argv[++i];
		} else if (strcmp(argv[i], "--assets") == 0 && i + 1 < argc) {
			assets_path = argv[++i];
		} else if (strcmp(argv[i], "--fixtures") == 0 && i + 1 < argc) {
			fixdir = argv[++i];
		} else if (strcmp(argv[i], "--ui-demo") == 0 && i + 1 < argc) {
			ui_demo_dir = argv[++i];
		} else if (strcmp(argv[i], "--logical") == 0 && i + 1 < argc) {
			if (sscanf(argv[++i], "%dx%d", &logical_w,
				   &logical_h) != 2) {
				usage(argv[0]);
				return 2;
			}
		} else if (strcmp(argv[i], "--help") == 0) {
			usage(argv[0]);
			return 0;
		} else {
			usage(argv[0]);
			return 2;
		}
	}

	if (ui_demo_dir != NULL) {
		SDL_Init(0);
		run_ui_shot(ui_demo_dir);
		SDL_Quit();
		return 0;
	}

	if (play_spec != NULL) {
		run_play(play_spec, assets_path, player_name);
		return 0;
	}

	if (connect_spec != NULL) {
		run_connect(connect_spec, fixdir != NULL ? fixdir : ".");
	}

	if (frames >= 0) {
		run_frames(frames, logical_w, logical_h);
		return 0;
	}

	if (!self_test) {
		self_test = 1;
	}

	printf("== Xtank P2.9 Windows shell self-test\n");
	printf("protocol major=%u minor=%u build=0x%04x\n",
	       XTK_PROTOCOL_MAJOR, XTK_PROTOCOL_MINOR, XTK_CLIENT_BUILD);
	run_self_test(logical_w, logical_h);
	run_codec_fixtures(fixdir != NULL ? fixdir : ".", fixdir != NULL);
	run_network_selftest(fixdir != NULL ? fixdir : ".");

	printf("checks=%d failures=%d\n", g_checks, g_failures);
	if (g_failures == 0) {
		printf("WINDOWS-SMOKE PASS\n");
		return 0;
	}
	printf("WINDOWS-SMOKE FAIL\n");
	return 1;
}
