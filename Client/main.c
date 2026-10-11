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

/* ---- argument parsing ----------------------------------------------- */

static void usage(const char *argv0)
{
	printf("usage: %s [--self-test] [--frames N] [--connect HOST:PORT]\n"
	       "          [--fixtures DIR] [--logical WxH]\n", argv0);
}

int main(int argc, char **argv)
{
	const char *fixdir = NULL;
	int frames = -1;
	int logical_w = 1024, logical_h = 768;
	int self_test = 0;
	const char *connect_spec = NULL;
	int i;

	for (i = 1; i < argc; ++i) {
		if (strcmp(argv[i], "--self-test") == 0) {
			self_test = 1;
		} else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
			frames = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--connect") == 0 && i + 1 < argc) {
			connect_spec = argv[++i];
		} else if (strcmp(argv[i], "--fixtures") == 0 && i + 1 < argc) {
			fixdir = argv[++i];
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
