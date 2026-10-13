/*
 * codec_fixture.c -- P3.3 protocol-v1.1 codec acceptance driver.
 *
 * Portable (Linux gcc and MinGW-w64): it proves the codec in
 * Protocol/{encode.c,decode.c} against the frozen v1.1 golden fixtures and
 * against a battery of malformed inputs.  It is deliberately NOT a
 * round-trip-only test: the encoded octets are compared with the committed
 * golden bytes (which verify.py independently checks on the semantic side),
 * and every rejection is asserted to return a specific protocol error.
 *
 * It also proves that the codec accepts exactly version 1.1 and rejects a
 * 1.0 envelope with no fallback (D3-A).
 *
 * Usage: codec_fixture <fixture-dir>          (default ".")
 * Exit 0 iff every check passes.
 *
 * Authority: docs/PROTOCOL-v1.1.md; docs/PHASE3-PLAN.md P3.3;
 * Tests/phase3/fixtures/protocol/{*.hex,verify.py}.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wire.h"
#include "limits.h"
#include "content-manifest.h"

#define MAXBUF XTK_MAX_MESSAGE_BYTES

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
        printf("FAIL: cannot open %s\n", path);
        return 0;
    }
    while ((c = fgetc(f)) != EOF) {
        if (c == '#') {
            while ((c = fgetc(f)) != EOF && c != '\n') { /* skip */ }
            continue;
        }
        v = hexval(c);
        if (v < 0) continue;
        if (hi < 0) {
            hi = v;
        } else {
            if (n >= cap) { fclose(f); return 0; }
            out[n++] = (uint8_t)((hi << 4) | v);
            hi = -1;
        }
    }
    fclose(f);
    *out_len = n;
    return 1;
}

static void env_set(XtkMessage *m, uint8_t type, uint32_t sess, uint32_t epoch,
                    uint32_t seq, int32_t tick)
{
    memset(m, 0, sizeof(*m));
    m->env.message_type = type;
    m->env.session_id = sess;
    m->env.epoch = epoch;
    m->env.message_sequence = seq;
    m->env.tick = tick;
}

static void copy32(uint8_t *dst, const uint8_t *src)
{
    int i;
    for (i = 0; i < 32; ++i) dst[i] = src[i];
}

#define SESS UINT32_C(0x11223344)

/* ---- golden fixture builders (must match gen_fixtures.py) ------------ */

static void b_hello(XtkMessage *m)
{
    env_set(m, XTK_MSG_HELLO, 0, 0, 1, 0);
    m->body.hello.client_nonce = UINT32_C(0x0A0B0C0D);
    m->body.hello.proto_major = 1;
    m->body.hello.proto_minor = 1;
    m->body.hello.client_build = 0x0042;
    strcpy(m->body.hello.name, "pilot");
    copy32(m->body.hello.content_set_digest, xtk_content_set_digest);
}

static void b_welcome(XtkMessage *m)
{
    env_set(m, XTK_MSG_WELCOME, SESS, 0, 1, 0);
    m->body.welcome.server_nonce = UINT32_C(0xDEADBEEF);
    m->body.welcome.role = 1;
    m->body.welcome.tick_hz = 15;
    m->body.welcome.max_players = 4;
    m->body.welcome.human_capacity = 2;
}

static void b_reject_version(XtkMessage *m)
{
    env_set(m, XTK_MSG_REJECT, 0, 0, 0, 0);
    m->body.reject.error_code = XTK_ERR_VERSION_MISMATCH;
    strcpy(m->body.reject.text, "protocol version");
}

static void b_reject_content(XtkMessage *m)
{
    env_set(m, XTK_MSG_REJECT, 0, 0, 0, 0);
    m->body.reject.error_code = XTK_ERR_CONTENT_MISMATCH;
    strcpy(m->body.reject.text, "content mismatch");
}

static void b_reject_duplicate_name(XtkMessage *m)
{
    env_set(m, XTK_MSG_REJECT, SESS, 0, 2, 0);
    m->body.reject.error_code = XTK_ERR_DUPLICATE_NAME;
    strcpy(m->body.reject.text, "duplicate name");
}

static void b_catalog(XtkMessage *m)
{
    XtkCatalogEntry *e;
    env_set(m, XTK_MSG_CONTENT_CATALOG, SESS, 0, 2, 0);
    copy32(m->body.catalog.content_set_digest, xtk_content_set_digest);
    m->body.catalog.entry_count = 3;
    e = &m->body.catalog.entry[0];
    e->id = 0x0101; e->kind = XTK_CONTENT_MAZE; strcpy(e->name, "combat1");
    copy32(e->source_sha256, xtk_content_entries[0].sha256);
    e = &m->body.catalog.entry[1];
    e->id = 0x0102; e->kind = XTK_CONTENT_MAZE; strcpy(e->name, "Arena");
    copy32(e->source_sha256, xtk_content_entries[1].sha256);
    e = &m->body.catalog.entry[2];
    e->id = 0x0201; e->kind = XTK_CONTENT_VEHICLE; strcpy(e->name, "Vanguard");
    copy32(e->source_sha256, xtk_content_entries[2].sha256);
}

static void b_config_select(XtkMessage *m)
{
    env_set(m, XTK_MSG_CONFIG_SELECT, SESS, 0, 3, 0);
    m->body.config_select.maze_id = 0x0101;
    m->body.config_select.vehicle_id = 0x0201;
    m->body.config_select.drone_count = 2;
}

static void b_config_status(XtkMessage *m)
{
    env_set(m, XTK_MSG_CONFIG_STATUS, SESS, 0, 4, 0);
    m->body.config_status.maze_id = 0x0101;
    m->body.config_status.vehicle_id = 0x0201;
    m->body.config_status.drone_count = 2;
    m->body.config_status.status = 0;
    m->body.config_status.error_code = XTK_ERR_NONE;
    m->body.config_status.config_revision = 7;
}

static void b_ready(XtkMessage *m)
{
    env_set(m, XTK_MSG_READY, SESS, 0, 5, 0);
    m->body.ready.ready = 1;
    m->body.ready.config_revision = 7;
}

static void b_start(XtkMessage *m)
{
    env_set(m, XTK_MSG_START, SESS, 1, 6, 1);
    m->body.start.start_tick = 1;
    m->body.start.maze_id = 0x0101;
    m->body.start.vehicle_id = 0x0201;
    m->body.start.drone_count = 2;
    m->body.start.config_revision = 7;
}

static void b_start_request(XtkMessage *m)
{
    env_set(m, XTK_MSG_START_REQUEST, SESS, 0, 7, 0);
    m->body.start_request.expected_config_revision = 7;
}

static void b_epoch_begin(XtkMessage *m)
{
    env_set(m, XTK_MSG_EPOCH_BEGIN, SESS, 1, 8, 1);
    m->body.epoch_begin.epoch = 1;
    m->body.epoch_begin.first_tick = 1;
}

static void b_lobby_state(XtkMessage *m)
{
    XtkRosterEntry *e;
    env_set(m, XTK_MSG_LOBBY_STATE, SESS, 0, 9, 0);
    m->body.lobby_state.config_revision = 7;
    m->body.lobby_state.session_state = XTK_SESSION_LOBBY;
    m->body.lobby_state.drone_count = 2;
    m->body.lobby_state.maze_id = 0x0101;
    m->body.lobby_state.vehicle_id = 0x0201;
    m->body.lobby_state.self_index = 0;
    m->body.lobby_state.admin_index = 0;
    m->body.lobby_state.human_count = 2;
    e = &m->body.lobby_state.entry[0];
    e->connection_id = UINT32_C(0x000000A1);
    e->controller_id = UINT32_C(0x00001001);
    e->slot = 0; e->ready = 1;
    strcpy(e->name, "pilot");
    e = &m->body.lobby_state.entry[1];
    e->connection_id = UINT32_C(0x000000A2);
    e->controller_id = UINT32_C(0x00001002);
    e->slot = 1; e->ready = 0;
    strcpy(e->name, "gunner");
}

static void b_results(XtkMessage *m)
{
    env_set(m, XTK_MSG_RESULTS, SESS, 1, 10, 231);
    m->body.results.end_tick = 231;
    m->body.results.result_kind = 0;
    m->body.results.winner_slot = 0;
    m->body.results.vehicle_count = 2;
    m->body.results.entry[0].slot = 0;
    m->body.results.entry[0].flags = 0;
    m->body.results.entry[0].generation = 1;
    m->body.results.entry[0].score = 1000;
    strcpy(m->body.results.entry[0].name, "pilot");
    m->body.results.entry[1].slot = 1;
    m->body.results.entry[1].flags = XTK_FLAG_DEPARTED;
    m->body.results.entry[1].generation = 2;
    m->body.results.entry[1].score = 100;
    strcpy(m->body.results.entry[1].name, "drone");
}

static void b_reset(XtkMessage *m)
{
    env_set(m, XTK_MSG_RESET, SESS, 1, 11, 231);
    m->body.reset.prior_epoch = 1;
}

static void b_quit(XtkMessage *m)
{
    env_set(m, XTK_MSG_QUIT, SESS, 1, 12, 231);
}

static void b_event(XtkMessage *m)
{
    env_set(m, XTK_MSG_EVENT, SESS, 1, 13, 170);
    m->body.event.event_sequence = 5;
    m->body.event.capture_tick = 169;
    m->body.event.event_type = XTK_EV_HIT;
    m->body.event.entity_kind = XTK_ENTITY_VEHICLE;
    m->body.event.entity_id = 0;
    m->body.event.entity_generation = 1;
    m->body.event.aux0 = 9;
    m->body.event.aux1 = 6;
}

static void b_cmd_ack(XtkMessage *m)
{
    env_set(m, XTK_MSG_CMD_ACK, SESS, 1, 14, 171);
    m->body.cmd_ack.count = 2;
    m->body.cmd_ack.entry[0].command_id = 100;
    m->body.cmd_ack.entry[0].assigned_tick = 171;
    m->body.cmd_ack.entry[0].status = XTK_ACK_ACCEPTED;
    m->body.cmd_ack.entry[1].command_id = 101;
    m->body.cmd_ack.entry[1].assigned_tick = 171;
    m->body.cmd_ack.entry[1].status = XTK_ACK_REJECTED;
    m->body.cmd_ack.entry[1].reason = XTK_ERR_STALE_LIFE;
}

static void b_reliable_cmd(XtkMessage *m)
{
    env_set(m, XTK_MSG_RELIABLE_CMD, SESS, 1, 15, 171);
    m->body.reliable_cmd.last_observed_server_tick = 170;
    m->body.reliable_cmd.command_count = 1;
    m->body.reliable_cmd.command[0].command_id = 101;
    m->body.reliable_cmd.command[0].tick_hint = 171;
    m->body.reliable_cmd.command[0].life_generation = 1;
    m->body.reliable_cmd.command[0].op = XTK_OP_FIRE;
    m->body.reliable_cmd.command[0].flags = XTK_FLAG_EDGE;
}

static void b_protocol_error(XtkMessage *m)
{
    env_set(m, XTK_MSG_PROTOCOL_ERROR, SESS, 1, 16, 171);
    m->body.protocol_error.error_code = XTK_ERR_BAD_CHANNEL;
    m->body.protocol_error.offending_sequence = 7;
    m->body.protocol_error.offending_type = XTK_MSG_VIEW_SNAPSHOT;
}

static void b_input_batch(XtkMessage *m)
{
    env_set(m, XTK_MSG_INPUT_BATCH, SESS, 1, 17, 0);
    m->body.input_batch.packet_sequence = 10;
    m->body.input_batch.last_observed_server_tick = 170;
    m->body.input_batch.command_count = 2;
    m->body.input_batch.command[0].command_id = 100;
    m->body.input_batch.command[0].tick_hint = 171;
    m->body.input_batch.command[0].life_generation = 1;
    m->body.input_batch.command[0].op = XTK_OP_DRIVE_REL;
    m->body.input_batch.command[0].flags = XTK_FLAG_HELD;
    m->body.input_batch.command[0].a = 3.5;
    m->body.input_batch.command[1].command_id = 101;
    m->body.input_batch.command[1].tick_hint = 171;
    m->body.input_batch.command[1].life_generation = 1;
    m->body.input_batch.command[1].op = XTK_OP_FIRE;
    m->body.input_batch.command[1].flags = XTK_FLAG_EDGE;
}

static void b_view_snapshot_fill(XtkViewSnapshot *v)
{
    XtkSelfState *s;
    XtkVehicleView *ve;
    XtkBulletView *b;
    XtkExplosionView *x;
    XtkMapCell *c;
    XtkLandmarkView *l;
    XtkRadarBlip *r;

    v->snapshot_sequence = 42;
    v->server_tick = 170;
    v->flags = XTK_FLAG_KEYFRAME;
    v->own_slot = 0;
    v->own_generation = 1;
    v->vehicle_count = 1;
    v->bullet_count = 1;
    v->explosion_count = 1;
    v->map_cell_count = 1;
    v->landmark_count = 1;
    v->radar_blip_count = 1;
    v->weapon_count = 2;
    v->map_revision = 3;

    s = &v->self;
    s->slot = 0; s->lifecycle_flags = 1;
    s->armor[0] = 100; s->armor[1] = 90; s->armor[2] = 80; s->armor[3] = 70;
    s->fuel = 500; s->heat = 20; s->score = 1000;
    s->ammo[0] = 10; s->ammo[1] = 20;
    s->weapon_enabled_bitmap = 0x03; s->weapon_count = 2;
    s->reload[1] = 5;
    s->generation = 1;

    ve = &v->vehicle[0];
    ve->slot = 0; ve->kind = 6; ve->flags = 0x01;
    ve->x = 100.5; ve->y = 200.25;
    ve->body_angle = 1.5707963267948966;
    ve->turret_angle = 3.141592653589793;
    ve->generation = 1;

    b = &v->bullet[0];
    b->id = 7; b->flags = 0; b->x = 101.5; b->y = 200.25;
    b->heading = 1.5707963267948966;
    b->owner_slot = 0; b->kind = 0;
    b->generation = 9; b->owner_generation = 1;

    x = &v->explosion[0];
    x->id = 11; x->kind = 3; x->flags = 0;
    x->x = 105.0; x->y = 200.0; x->generation = 5;

    c = &v->cell[0];
    c->grid_x = 15; c->grid_y = 10; c->flags = 0x03; c->type = 0;

    l = &v->landmark[0];
    l->kind = 2; l->grid_x = 15; l->grid_y = 10;
    l->x = 64.0; l->y = 32.0; l->id = 300;

    r = &v->radar[0];
    r->x = 200.0; r->y = 100.0; r->age = 4; r->classification = 1;
}

static void b_view_snapshot(XtkMessage *m)
{
    env_set(m, XTK_MSG_VIEW_SNAPSHOT, SESS, 1, 18, 170);
    b_view_snapshot_fill(&m->body.view_snapshot);
}

static void b_view_keyframe(XtkMessage *m)
{
    env_set(m, XTK_MSG_VIEW_KEYFRAME, SESS, 1, 19, 170);
    b_view_snapshot_fill(&m->body.view_snapshot);
}

typedef void (*Builder)(XtkMessage *);

static const struct {
    const char *name;
    Builder     build;
} FIXTURES[] = {
    { "hello.hex",                 b_hello },
    { "welcome.hex",               b_welcome },
    { "reject_version.hex",        b_reject_version },
    { "reject_content.hex",        b_reject_content },
    { "reject_duplicate_name.hex", b_reject_duplicate_name },
    { "catalog.hex",               b_catalog },
    { "config_select.hex",         b_config_select },
    { "config_status.hex",         b_config_status },
    { "ready.hex",                 b_ready },
    { "start.hex",                 b_start },
    { "start_request.hex",         b_start_request },
    { "epoch_begin.hex",           b_epoch_begin },
    { "lobby_state.hex",           b_lobby_state },
    { "results.hex",               b_results },
    { "reset.hex",                 b_reset },
    { "quit.hex",                  b_quit },
    { "event.hex",                 b_event },
    { "cmd_ack.hex",               b_cmd_ack },
    { "reliable_cmd.hex",          b_reliable_cmd },
    { "protocol_error.hex",        b_protocol_error },
    { "input_batch.hex",           b_input_batch },
    { "view_snapshot.hex",         b_view_snapshot },
    { "view_keyframe.hex",         b_view_keyframe },
};

#define NFIX ((int)(sizeof(FIXTURES) / sizeof(FIXTURES[0])))

static uint8_t g_golden[MAXBUF];
static uint8_t g_out[MAXBUF];
static uint8_t g_tmp[MAXBUF];
static XtkMessage g_msg;

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : ".";
    int i;
    size_t glen, olen, l;

    printf("codec driver v1.1: fixtures in %s, sizeof(XtkMessage)=%zu\n",
           dir, sizeof(XtkMessage));

    /* 1. Golden byte reproduction + independent decode + re-encode. */
    for (i = 0; i < NFIX; ++i) {
        char what[256];
        int rc;

        if (!load_hex(dir, FIXTURES[i].name, g_golden, sizeof(g_golden), &glen)) {
            check(0, FIXTURES[i].name);
            continue;
        }
        FIXTURES[i].build(&g_msg);
        rc = xtk_encode(g_out, sizeof(g_out), &g_msg, &olen);
        snprintf(what, sizeof(what), "%s encode rc=%d", FIXTURES[i].name, rc);
        check(rc == XTK_ERR_NONE, what);
        if (rc == XTK_ERR_NONE) {
            snprintf(what, sizeof(what), "%s encoded length %zu want %zu",
                     FIXTURES[i].name, olen, glen);
            check(olen == glen, what);
            check(olen == glen && memcmp(g_out, g_golden, glen) == 0,
                  FIXTURES[i].name);
            /* the codec must always stamp exact version 1.1 */
            check(g_out[4] == XTK_PROTOCOL_MAJOR &&
                  g_out[5] == XTK_PROTOCOL_MINOR,
                  "encoded version is 1.1");
        }

        rc = xtk_decode(g_golden, glen, &g_msg);
        snprintf(what, sizeof(what), "%s decode rc=%d", FIXTURES[i].name, rc);
        check(rc == XTK_ERR_NONE, what);
        if (rc == XTK_ERR_NONE) {
            size_t olen2 = 0;
            rc = xtk_encode(g_out, sizeof(g_out), &g_msg, &olen2);
            check(rc == XTK_ERR_NONE && olen2 == glen &&
                  memcmp(g_out, g_golden, glen) == 0,
                  FIXTURES[i].name);
        }

        /* 2a. Raw truncation at every prefix. */
        for (l = 0; l < glen; ++l) {
            rc = xtk_decode(g_golden, l, &g_msg);
            if (rc == XTK_ERR_NONE) {
                snprintf(what, sizeof(what),
                         "%s raw truncation at %zu accepted",
                         FIXTURES[i].name, l);
                check(0, what);
                break;
            }
        }
        check(1, FIXTURES[i].name);

        /* 2b. Truncation with a matching declared payload_length so the inner
         * field readers must reject. */
        if (glen > XTK_ENVELOPE_BYTES) {
            for (l = XTK_ENVELOPE_BYTES; l < glen; ++l) {
                size_t n = l - XTK_ENVELOPE_BYTES;
                if (n > XTK_MAX_PAYLOAD_BYTES) break;
                memcpy(g_tmp, g_golden, l);
                xtk_store_u32(g_tmp + 8, (uint32_t)n);
                rc = xtk_decode(g_tmp, l, &g_msg);
                if (rc == XTK_ERR_NONE) {
                    snprintf(what, sizeof(what),
                             "%s field truncation at %zu accepted",
                             FIXTURES[i].name, l);
                    check(0, what);
                    break;
                }
            }
        }
        check(1, FIXTURES[i].name);
    }

    /* 3. Cross-endian primitives. */
    {
        size_t elen = 0;
        uint16_t u16;
        uint32_t u32;
        int32_t i32;
        double d = 0.0;
        uint8_t e[18];

        if (load_hex(dir, "endian.hex", g_golden, sizeof(g_golden), &elen)) {
            xtk_store_u16(e, 0x0102);
            xtk_store_u32(e + 2, UINT32_C(0x01020304));
            xtk_store_i32(e + 6, -2);
            xtk_store_f64(e + 10, 1.0);
            check(elen == 18 && memcmp(e, g_golden, 18) == 0, "endian encode");
            u16 = xtk_load_u16(g_golden);
            u32 = xtk_load_u32(g_golden + 2);
            i32 = xtk_load_i32(g_golden + 6);
            check(u16 == 0x0102, "endian u16");
            check(u32 == UINT32_C(0x01020304), "endian u32");
            check(i32 == -2, "endian i32 (two's-complement wrap)");
            check(xtk_load_f64(g_golden + 10, &d) == 1 && d == 1.0,
                  "endian f64");
        } else {
            check(0, "endian.hex");
        }
    }

    /* 4. Envelope rejection tests (mutating a valid LOBBY_STATE). */
    {
        size_t n = 0;
        if (!load_hex(dir, "lobby_state.hex", g_golden, sizeof(g_golden), &n)) {
            check(0, "lobby_state.hex for rejections");
        } else {
            memcpy(g_tmp, g_golden, n);
            g_tmp[0] ^= 0xFF;
            check(xtk_decode(g_tmp, n, &g_msg) == XTK_ERR_BAD_MAGIC,
                  "reject bad magic");

            memcpy(g_tmp, g_golden, n);
            g_tmp[4] = 2;
            check(xtk_decode(g_tmp, n, &g_msg) == XTK_ERR_BAD_VERSION,
                  "reject bad major version 2.1");

            /* explicit 1.0 rejection: exact-version 1.1, no fallback (D3-A) */
            memcpy(g_tmp, g_golden, n);
            g_tmp[5] = 0;
            check(xtk_decode(g_tmp, n, &g_msg) == XTK_ERR_VERSION_MISMATCH,
                  "reject 1.0 envelope (no fallback)");

            memcpy(g_tmp, g_golden, n);
            g_tmp[5] = 2;
            check(xtk_decode(g_tmp, n, &g_msg) == XTK_ERR_VERSION_MISMATCH,
                  "reject 1.2 envelope");

            memcpy(g_tmp, g_golden, n);
            g_tmp[6] = 0x7F;
            check(xtk_decode(g_tmp, n, &g_msg) == XTK_ERR_BAD_TYPE,
                  "reject unknown message type");

            memcpy(g_tmp, g_golden, n);
            g_tmp[7] = XTK_CH_SNAPSHOT;
            check(xtk_decode(g_tmp, n, &g_msg) == XTK_ERR_BAD_CHANNEL,
                  "reject wrong channel (lobby_state on ch1)");

            memcpy(g_tmp, g_golden, n);
            g_tmp[31] = 1;
            check(xtk_decode(g_tmp, n, &g_msg) == XTK_ERR_MALFORMED,
                  "reject nonzero envelope reserved");

            memcpy(g_tmp, g_golden, n);
            xtk_store_i32(g_tmp + 24, -1);
            check(xtk_decode(g_tmp, n, &g_msg) == XTK_ERR_MALFORMED,
                  "reject negative tick");

            memcpy(g_tmp, g_golden, n);
            xtk_store_u32(g_tmp + 8, UINT32_C(0xFFFFFFFF));
            check(xtk_decode(g_tmp, n, &g_msg) == XTK_ERR_BAD_LENGTH,
                  "reject payload_length wrap");

            memcpy(g_tmp, g_golden, n);
            xtk_store_u32(g_tmp + 8, 0);
            check(xtk_decode(g_tmp, n, &g_msg) == XTK_ERR_BAD_LENGTH,
                  "reject inconsistent length");

            memcpy(g_tmp, g_golden, n + 1);
            g_tmp[n] = 0;
            check(xtk_decode(g_tmp, n + 1, &g_msg) == XTK_ERR_BAD_LENGTH,
                  "reject trailing octet");
        }
    }

    /* 4b. VIEW_KEYFRAME is a ch0 message; on ch1 it is rejected. */
    {
        size_t n = 0;
        if (load_hex(dir, "view_keyframe.hex", g_golden, sizeof(g_golden), &n)) {
            memcpy(g_tmp, g_golden, n);
            g_tmp[7] = XTK_CH_SNAPSHOT;
            check(xtk_decode(g_tmp, n, &g_msg) == XTK_ERR_BAD_CHANNEL,
                  "reject view_keyframe on ch1");
        }
    }

    /* 5. Delegated field rejections: over-bound counts, nonzero reserved on
     * every new field, malformed strings and non-finite operands. */
    {
        static const struct {
            const char *fix;
            size_t      off;   /* payload-relative */
            int         width; /* 1/2/4 = integer store; 0 = single byte 0x80 */
            uint32_t    value;
            int         want;
            const char *what;
        } cases[] = {
            /* counts */
            { "input_batch.hex",    8,  2, 33, XTK_ERR_BAD_LENGTH,
              "oversized INPUT_BATCH command count" },
            { "reliable_cmd.hex",   4,  2, 33, XTK_ERR_BAD_LENGTH,
              "oversized RELIABLE_CMD command count" },
            { "cmd_ack.hex",        0,  2, 33, XTK_ERR_BAD_LENGTH,
              "oversized CMD_ACK count" },
            { "catalog.hex",       32,  2, 9,  XTK_ERR_BAD_LENGTH,
              "oversized CATALOG entry count" },
            { "results.hex",        6,  1, 5,  XTK_ERR_BAD_LENGTH,
              "oversized RESULTS vehicle count" },
            { "view_snapshot.hex", 16,  2, 17, XTK_ERR_BAD_LENGTH,
              "oversized snapshot vehicle count" },
            { "lobby_state.hex",   12,  1, 3,  XTK_ERR_BAD_LENGTH,
              "oversized LOBBY_STATE roster count" },
            { "hello.hex",          8,  1, 33, XTK_ERR_MALFORMED,
              "oversized HELLO name length" },
            { "lobby_state.hex",   28,  1, 33, XTK_ERR_MALFORMED,
              "oversized roster name length" },
            /* reserved fields must be zero */
            { "welcome.hex",        8,  1, 1,  XTK_ERR_MALFORMED,
              "nonzero WELCOME reserved" },
            { "config_select.hex",  5,  1, 1,  XTK_ERR_MALFORMED,
              "nonzero CONFIG_SELECT reserved" },
            { "config_select.hex",  6,  2, 1,  XTK_ERR_MALFORMED,
              "nonzero CONFIG_SELECT reserved2" },
            { "config_select.hex",  8,  4, 1,  XTK_ERR_MALFORMED,
              "nonzero CONFIG_SELECT reserved3" },
            { "config_status.hex",  5,  1, 1,  XTK_ERR_MALFORMED,
              "nonzero CONFIG_STATUS reserved" },
            { "config_status.hex", 10,  2, 1,  XTK_ERR_MALFORMED,
              "nonzero CONFIG_STATUS reserved2" },
            { "ready.hex",          1,  1, 1,  XTK_ERR_MALFORMED,
              "nonzero READY reserved" },
            { "ready.hex",          2,  2, 1,  XTK_ERR_MALFORMED,
              "nonzero READY reserved2" },
            { "start.hex",          9,  1, 1,  XTK_ERR_MALFORMED,
              "nonzero START reserved" },
            { "start.hex",         10,  2, 1,  XTK_ERR_MALFORMED,
              "nonzero START reserved2" },
            { "start_request.hex",  4,  4, 1,  XTK_ERR_MALFORMED,
              "nonzero START_REQUEST reserved" },
            { "lobby_state.hex",   13,  1, 1,  XTK_ERR_MALFORMED,
              "nonzero LOBBY_STATE reserved" },
            { "lobby_state.hex",   14,  2, 1,  XTK_ERR_MALFORMED,
              "nonzero LOBBY_STATE reserved2" },
            { "results.hex",        7,  1, 1,  XTK_ERR_MALFORMED,
              "nonzero RESULTS header reserved" },
            { "results.hex",       10,  2, 1,  XTK_ERR_MALFORMED,
              "nonzero RESULTS entry reserved" },
            { "reset.hex",          4,  4, 1,  XTK_ERR_MALFORMED,
              "nonzero RESET reserved" },
            { "cmd_ack.hex",        2,  2, 1,  XTK_ERR_MALFORMED,
              "nonzero CMD_ACK reserved" },
            { "protocol_error.hex",10,  2, 1,  XTK_ERR_MALFORMED,
              "nonzero PROTOCOL_ERROR reserved" },
            { "reliable_cmd.hex",   6,  2, 1,  XTK_ERR_MALFORMED,
              "nonzero RELIABLE_CMD reserved" },
            { "reliable_cmd.hex",  22,  2, 1,  XTK_ERR_MALFORMED,
              "nonzero COMMAND reserved (reliable)" },
            { "input_batch.hex",   10,  2, 1,  XTK_ERR_MALFORMED,
              "nonzero INPUT_BATCH reserved" },
            { "input_batch.hex",   26,  2, 1,  XTK_ERR_MALFORMED,
              "nonzero COMMAND reserved (input)" },
            { "view_snapshot.hex", 11,  1, 1,  XTK_ERR_MALFORMED,
              "nonzero snapshot header reserved" },
            { "view_snapshot.hex", 33,  1, 1,  XTK_ERR_MALFORMED,
              "nonzero SelfState reserved" },
            { "view_snapshot.hex", 82,  2, 1,  XTK_ERR_MALFORMED,
              "nonzero SelfState reserved2" },
            { "view_snapshot.hex", 85,  1, 1,  XTK_ERR_MALFORMED,
              "nonzero VehicleView reserved" },
            { "view_snapshot.hex",128,  4, 1,  XTK_ERR_MALFORMED,
              "nonzero VehicleView reserved2" },
            { "view_snapshot.hex",162,  2, 1,  XTK_ERR_MALFORMED,
              "nonzero BulletView reserved" },
            { "view_snapshot.hex",196,  4, 1,  XTK_ERR_MALFORMED,
              "nonzero ExplosionView reserved" },
            { "view_snapshot.hex",207,  1, 1,  XTK_ERR_MALFORMED,
              "nonzero LandmarkView reserved" },
            { "view_snapshot.hex",226,  2, 1,  XTK_ERR_MALFORMED,
              "nonzero LandmarkView reserved2" },
            { "view_snapshot.hex",228,  4, 1,  XTK_ERR_MALFORMED,
              "nonzero LandmarkView reserved3" },
            { "view_snapshot.hex",250,  2, 1,  XTK_ERR_MALFORMED,
              "nonzero RadarBlip reserved" },
            /* malformed strings */
            { "hello.hex",          9,  0, 0x80, XTK_ERR_MALFORMED,
              "non-ASCII HELLO name byte" },
            { "lobby_state.hex",   29,  0, 0x80, XTK_ERR_MALFORMED,
              "non-ASCII roster name byte" },
        };
        size_t k;
        for (k = 0; k < sizeof(cases) / sizeof(cases[0]); ++k) {
            size_t n = 0;
            int rc;
            if (!load_hex(dir, cases[k].fix, g_golden, sizeof(g_golden), &n)) {
                check(0, cases[k].what);
                continue;
            }
            memcpy(g_tmp, g_golden, n);
            if (cases[k].width == 1) {
                g_tmp[XTK_ENVELOPE_BYTES + cases[k].off] =
                    (uint8_t)cases[k].value;
            } else if (cases[k].width == 2) {
                xtk_store_u16(g_tmp + XTK_ENVELOPE_BYTES + cases[k].off,
                              (uint16_t)cases[k].value);
            } else if (cases[k].width == 4) {
                xtk_store_u32(g_tmp + XTK_ENVELOPE_BYTES + cases[k].off,
                              cases[k].value);
            } else {
                g_tmp[XTK_ENVELOPE_BYTES + cases[k].off] = 0x80;
            }
            rc = xtk_decode(g_tmp, n, &g_msg);
            check(rc == cases[k].want, cases[k].what);
        }

        /* NaN and +Inf f64 in INPUT_BATCH command[0].a (payload offset 28) */
        {
            size_t n = 0;
            static const uint8_t nan_bits[8] =
                { 0x7F, 0xF8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
            static const uint8_t inf_bits[8] =
                { 0x7F, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
            if (load_hex(dir, "input_batch.hex", g_golden, sizeof(g_golden), &n)) {
                int rc;
                memcpy(g_tmp, g_golden, n);
                memcpy(g_tmp + XTK_ENVELOPE_BYTES + 28, nan_bits, 8);
                rc = xtk_decode(g_tmp, n, &g_msg);
                check(rc == XTK_ERR_NONFINITE, "reject NaN f64");

                memcpy(g_tmp, g_golden, n);
                memcpy(g_tmp + XTK_ENVELOPE_BYTES + 28, inf_bits, 8);
                rc = xtk_decode(g_tmp, n, &g_msg);
                check(rc == XTK_ERR_NONFINITE, "reject +Inf f64");
            }
        }
    }

    /* 6. Encoder rejects its own invalid inputs. */
    {
        int rc;
        XtkMessage m;
        size_t olen = 0;

        env_set(&m, XTK_MSG_INPUT_BATCH, 1, 1, 1, 0);
        m.body.input_batch.command_count = 33;
        rc = xtk_encode(g_out, sizeof(g_out), &m, &olen);
        check(rc == XTK_ERR_BAD_LENGTH, "encode rejects oversized count");

        env_set(&m, XTK_MSG_LOBBY_STATE, 1, 0, 1, 0);
        m.body.lobby_state.human_count = 3;
        rc = xtk_encode(g_out, sizeof(g_out), &m, &olen);
        check(rc == XTK_ERR_BAD_LENGTH, "encode rejects oversized roster");

        env_set(&m, XTK_MSG_INPUT_BATCH, 1, 1, 1, 0);
        m.body.input_batch.command_count = 1;
        m.body.input_batch.command[0].a = 0.0 / 0.0;
        rc = xtk_encode(g_out, sizeof(g_out), &m, &olen);
        check(rc == XTK_ERR_NONFINITE, "encode rejects NaN operand");

        env_set(&m, (uint8_t)0x7F, 0, 0, 0, 0);
        rc = xtk_encode(g_out, sizeof(g_out), &m, &olen);
        check(rc == XTK_ERR_BAD_TYPE, "encode rejects unknown type");
    }

    printf("CODEC %s (%d checks, %d failures)\n",
           g_failures == 0 ? "PASS" : "FAIL", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
