/*
 * codec.c -- P2.3 protocol-v1 codec acceptance driver.
 *
 * Portable (Linux gcc and MinGW-w64): it proves the codec in
 * Protocol/{encode.c,decode.c} against the frozen golden fixtures and against
 * a battery of malformed inputs.  It is deliberately NOT a round-trip-only
 * test: the encoded octets are compared with the committed golden bytes
 * (which verify.py independently checks on the semantic side), and every
 * rejection is asserted to return a specific protocol error.
 *
 * Usage: codec <fixture-dir>          (default ".")
 * Exit 0 iff every check passes.
 *
 * Authority: docs/PROTOCOL-v1.md; docs/PHASE2-PLAN.md P2.3;
 * Tests/phase2/fixtures/protocol/{*.hex,verify.py}.
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

/* Fixture loader: lowercase/uppercase hex octets, whitespace and '#' comments
 * ignored -- the same format verify.py consumes. */
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
            while ((c = fgetc(f)) != EOF && c != '\n') {
                /* skip comment */
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

/* ---- envelope helpers ------------------------------------------------ */

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

#define SESS   UINT32_C(0x11223344)

/* ---- golden fixture builders ---------------------------------------- */

static void b_hello(XtkMessage *m)
{
    env_set(m, XTK_MSG_HELLO, 0, 0, 1, 0);
    m->body.hello.client_nonce = UINT32_C(0x0A0B0C0D);
    m->body.hello.proto_major = 1;
    m->body.hello.proto_minor = 0;
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
}

static void b_reject_version(XtkMessage *m)
{
    env_set(m, XTK_MSG_REJECT, 0, 0, 0, 0);
    m->body.reject.error_code = XTK_ERR_BAD_VERSION;
    m->body.reject.detail_code = 0;
    strcpy(m->body.reject.text, "protocol version");
}

static void b_reject_content(XtkMessage *m)
{
    env_set(m, XTK_MSG_REJECT, 0, 0, 0, 0);
    m->body.reject.error_code = XTK_ERR_CONTENT_MISMATCH;
    m->body.reject.detail_code = 0;
    strcpy(m->body.reject.text, "content mismatch");
}

static void b_catalog(XtkMessage *m)
{
    XtkCatalogEntry *e;
    env_set(m, XTK_MSG_CONTENT_CATALOG, SESS, 0, 2, 0);
    copy32(m->body.catalog.content_set_digest, xtk_content_set_digest);
    m->body.catalog.entry_count = 3;
    e = &m->body.catalog.entry[0];
    e->id = 0x0101; e->kind = XTK_CONTENT_MAZE;
    strcpy(e->name, "combat1");
    copy32(e->source_sha256, xtk_content_entries[0].sha256);
    e = &m->body.catalog.entry[1];
    e->id = 0x0102; e->kind = XTK_CONTENT_MAZE;
    strcpy(e->name, "Arena");
    copy32(e->source_sha256, xtk_content_entries[1].sha256);
    e = &m->body.catalog.entry[2];
    e->id = 0x0201; e->kind = XTK_CONTENT_VEHICLE;
    strcpy(e->name, "Vanguard");
    copy32(e->source_sha256, xtk_content_entries[2].sha256);
}

static void b_config_select(XtkMessage *m)
{
    env_set(m, XTK_MSG_CONFIG_SELECT, SESS, 0, 3, 0);
    m->body.config_select.maze_id = 0x0101;
    m->body.config_select.vehicle_id = 0x0201;
}

static void b_start(XtkMessage *m)
{
    env_set(m, XTK_MSG_START, SESS, 1, 4, 1);
    m->body.start.start_tick = 1;
    m->body.start.maze_id = 0x0101;
    m->body.start.vehicle_id = 1;
}

static void b_epoch_begin(XtkMessage *m)
{
    env_set(m, XTK_MSG_EPOCH_BEGIN, SESS, 1, 5, 1);
    m->body.epoch_begin.epoch = 1;
    m->body.epoch_begin.first_tick = 1;
}

static void b_results(XtkMessage *m)
{
    env_set(m, XTK_MSG_RESULTS, SESS, 1, 6, 231);
    m->body.results.end_tick = 231;
    m->body.results.result_kind = 0;
    m->body.results.winner_slot = 0;
    m->body.results.vehicle_count = 2;
    m->body.results.entry[0].slot = 0;
    m->body.results.entry[0].generation = 1;
    m->body.results.entry[0].score = 1000;
    strcpy(m->body.results.entry[0].name, "pilot");
    m->body.results.entry[1].slot = 1;
    m->body.results.entry[1].generation = 2;
    m->body.results.entry[1].score = 100;
    strcpy(m->body.results.entry[1].name, "drone");
}

static void b_event(XtkMessage *m)
{
    env_set(m, XTK_MSG_EVENT, SESS, 1, 7, 170);
    m->body.event.event_sequence = 5;
    m->body.event.event_type = XTK_EV_HIT;
    m->body.event.entity_kind = XTK_ENTITY_VEHICLE;
    m->body.event.entity_id = 0;
    m->body.event.entity_generation = 1;
    m->body.event.aux0 = 9;
    m->body.event.aux1 = 6;
}

static void b_cmd_ack(XtkMessage *m)
{
    env_set(m, XTK_MSG_CMD_ACK, SESS, 1, 8, 171);
    m->body.cmd_ack.count = 2;
    m->body.cmd_ack.entry[0].command_id = 100;
    m->body.cmd_ack.entry[0].assigned_tick = 171;
    m->body.cmd_ack.entry[0].status = XTK_ACK_ACCEPTED;
    m->body.cmd_ack.entry[1].command_id = 101;
    m->body.cmd_ack.entry[1].assigned_tick = 171;
    m->body.cmd_ack.entry[1].status = XTK_ACK_REJECTED;
    m->body.cmd_ack.entry[1].reason = XTK_ERR_UNAUTHORIZED_OP;
}

static void b_reliable_cmd(XtkMessage *m)
{
    env_set(m, XTK_MSG_RELIABLE_CMD, SESS, 1, 9, 171);
    m->body.reliable_cmd.last_observed_server_tick = 170;
    m->body.reliable_cmd.command_count = 1;
    m->body.reliable_cmd.command[0].command_id = 101;
    m->body.reliable_cmd.command[0].tick_hint = 171;
    m->body.reliable_cmd.command[0].op = XTK_OP_FIRE;
    m->body.reliable_cmd.command[0].flags = XTK_FLAG_EDGE;
    m->body.reliable_cmd.command[0].a = 0.0;
    m->body.reliable_cmd.command[0].b = 0.0;
}

static void b_protocol_error(XtkMessage *m)
{
    env_set(m, XTK_MSG_PROTOCOL_ERROR, SESS, 1, 10, 171);
    m->body.protocol_error.error_code = XTK_ERR_BAD_CHANNEL;
    m->body.protocol_error.detail = 0;
    m->body.protocol_error.offending_sequence = 7;
    m->body.protocol_error.offending_type = XTK_MSG_VIEW_SNAPSHOT;
}

static void b_input_batch(XtkMessage *m)
{
    env_set(m, XTK_MSG_INPUT_BATCH, SESS, 1, 11, 0);
    m->body.input_batch.packet_sequence = 10;
    m->body.input_batch.last_observed_server_tick = 170;
    m->body.input_batch.command_count = 2;
    m->body.input_batch.command[0].command_id = 100;
    m->body.input_batch.command[0].tick_hint = 171;
    m->body.input_batch.command[0].op = XTK_OP_DRIVE_REL;
    m->body.input_batch.command[0].flags = XTK_FLAG_HELD;
    m->body.input_batch.command[0].a = 3.5;
    m->body.input_batch.command[0].b = 0.0;
    m->body.input_batch.command[1].command_id = 101;
    m->body.input_batch.command[1].tick_hint = 171;
    m->body.input_batch.command[1].op = XTK_OP_FIRE;
    m->body.input_batch.command[1].flags = XTK_FLAG_EDGE;
    m->body.input_batch.command[1].a = 0.0;
    m->body.input_batch.command[1].b = 0.0;
}

static void b_view_snapshot(XtkMessage *m)
{
    XtkSelfState *s;
    XtkVehicleView *v;
    XtkBulletView *b;
    XtkMapCell *c;
    env_set(m, XTK_MSG_VIEW_SNAPSHOT, SESS, 1, 12, 170);
    m->body.view_snapshot.snapshot_sequence = 42;
    m->body.view_snapshot.server_tick = 170;
    m->body.view_snapshot.flags = XTK_FLAG_KEYFRAME;
    m->body.view_snapshot.own_slot = 0;
    m->body.view_snapshot.own_generation = 1;
    m->body.view_snapshot.vehicle_count = 1;
    m->body.view_snapshot.bullet_count = 1;
    m->body.view_snapshot.explosion_count = 0;
    m->body.view_snapshot.map_cell_count = 1;
    m->body.view_snapshot.landmark_count = 0;
    m->body.view_snapshot.radar_blip_count = 0;
    m->body.view_snapshot.weapon_count = 2;
    m->body.view_snapshot.map_revision = 3;
    s = &m->body.view_snapshot.self;
    s->slot = 0; s->generation = 1; s->lifecycle_flags = 1;
    s->armor[0] = 100; s->armor[1] = 90; s->armor[2] = 80; s->armor[3] = 70;
    s->fuel = 500; s->heat = 20; s->score = 1000;
    s->ammo[0] = 10; s->ammo[1] = 20;
    s->weapon_enabled_bitmap = 0x03; s->weapon_count = 2;
    s->reload[1] = 5;
    v = &m->body.view_snapshot.vehicle[0];
    v->slot = 0; v->generation = 1; v->kind = 6; v->flags = 0x01;
    v->x = 100.5; v->y = 200.25;
    v->body_angle = 1.5707963267948966;
    v->turret_angle = 3.141592653589793;
    v->body_picture = 0; v->turret_picture = 0;
    b = &m->body.view_snapshot.bullet[0];
    b->id = 7; b->flags = 0; b->x = 101.5; b->y = 200.25;
    b->heading = 1.5707963267948966;
    b->owner_slot = 0; b->owner_generation = 1; b->kind = 0;
    c = &m->body.view_snapshot.cell[0];
    c->grid_x = 15; c->grid_y = 10; c->flags = 0x03; c->type = 0;
}

typedef void (*Builder)(XtkMessage *);

static const struct {
    const char *name;
    Builder     build;
} FIXTURES[] = {
    { "hello.hex",          b_hello },
    { "welcome.hex",        b_welcome },
    { "reject_version.hex", b_reject_version },
    { "reject_content.hex", b_reject_content },
    { "catalog.hex",        b_catalog },
    { "config_select.hex",  b_config_select },
    { "start.hex",          b_start },
    { "epoch_begin.hex",    b_epoch_begin },
    { "results.hex",        b_results },
    { "event.hex",          b_event },
    { "cmd_ack.hex",        b_cmd_ack },
    { "reliable_cmd.hex",   b_reliable_cmd },
    { "protocol_error.hex", b_protocol_error },
    { "input_batch.hex",    b_input_batch },
    { "view_snapshot.hex",  b_view_snapshot },
};

#define NFIX ((int)(sizeof(FIXTURES) / sizeof(FIXTURES[0])))

static uint8_t g_golden[MAXBUF];
static uint8_t g_out[MAXBUF];
static uint8_t g_tmp[MAXBUF];
static XtkMessage g_msg;

static void first_diff(const char *tag, const uint8_t *a, const uint8_t *b,
                       size_t n)
{
    size_t i;
    for (i = 0; i < n; ++i) {
        if (a[i] != b[i]) {
            printf("FAIL: %s first differs at octet %zu: got %02x want %02x\n",
                   tag, i, (unsigned)a[i], (unsigned)b[i]);
            return;
        }
    }
}

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : ".";
    int i;
    size_t glen, olen, l;

    printf("codec driver: fixtures in %s, sizeof(XtkMessage)=%zu\n",
           dir, sizeof(XtkMessage));

    /* 1. Golden byte reproduction + independent decode. */
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
            if (olen == glen && memcmp(g_out, g_golden, glen) != 0) {
                first_diff(FIXTURES[i].name, g_out, g_golden, glen);
                check(0, FIXTURES[i].name);
            } else {
                check(1, FIXTURES[i].name);
            }
        }

        /* decode the golden bytes, then re-encode and require byte identity */
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

        /* 2a. Truncation at every prefix: raw (envelope length check). */
        for (l = 0; l < glen; ++l) {
            rc = xtk_decode(g_golden, l, &g_msg);
            if (rc == XTK_ERR_NONE) {
                snprintf(what, sizeof(what),
                         "%s raw truncation at %zu accepted", FIXTURES[i].name, l);
                check(0, what);
                break;
            }
        }
        check(1, FIXTURES[i].name);

        /* 2b. Truncation with a matching declared payload_length, so envelope
         * validation passes and the inner field readers must reject. */
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

    /* 3. Cross-endian primitives: fixed big-endian regardless of host. */
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
            check(elen == 18 && memcmp(e, g_golden, 18) == 0,
                  "endian encode");
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

    /* 4. Envelope rejection tests (mutating a valid WELCOME). */
    {
        size_t n = 0;
        int rc;
        if (!load_hex(dir, "welcome.hex", g_golden, sizeof(g_golden), &n)) {
            check(0, "welcome.hex for rejections");
        } else {
            memcpy(g_tmp, g_golden, n);
            g_tmp[0] ^= 0xFF;                       /* bad magic */
            rc = xtk_decode(g_tmp, n, &g_msg);
            check(rc == XTK_ERR_BAD_MAGIC, "reject bad magic");

            memcpy(g_tmp, g_golden, n);
            g_tmp[4] = 2;                           /* wrong major */
            rc = xtk_decode(g_tmp, n, &g_msg);
            check(rc == XTK_ERR_BAD_VERSION, "reject bad major version");

            memcpy(g_tmp, g_golden, n);
            g_tmp[5] = 1;                           /* wrong minor */
            rc = xtk_decode(g_tmp, n, &g_msg);
            check(rc == XTK_ERR_VERSION_MISMATCH, "reject bad minor version");

            memcpy(g_tmp, g_golden, n);
            g_tmp[6] = 0x7F;                        /* unknown type */
            rc = xtk_decode(g_tmp, n, &g_msg);
            check(rc == XTK_ERR_BAD_TYPE, "reject unknown message type");

            memcpy(g_tmp, g_golden, n);
            g_tmp[7] = XTK_CH_INPUT;                /* wrong channel */
            rc = xtk_decode(g_tmp, n, &g_msg);
            check(rc == XTK_ERR_BAD_CHANNEL, "reject wrong channel");

            memcpy(g_tmp, g_golden, n);
            g_tmp[31] = 1;                          /* nonzero reserved */
            rc = xtk_decode(g_tmp, n, &g_msg);
            check(rc == XTK_ERR_MALFORMED, "reject nonzero reserved");

            memcpy(g_tmp, g_golden, n);
            xtk_store_i32(g_tmp + 24, -1);          /* negative tick */
            rc = xtk_decode(g_tmp, n, &g_msg);
            check(rc == XTK_ERR_MALFORMED, "reject negative tick");

            memcpy(g_tmp, g_golden, n);
            xtk_store_u32(g_tmp + 8, UINT32_C(0xFFFFFFFF));
            rc = xtk_decode(g_tmp, n, &g_msg);
            check(rc == XTK_ERR_BAD_LENGTH, "reject payload_length wrap");

            memcpy(g_tmp, g_golden, n);
            xtk_store_u32(g_tmp + 8, 0);            /* length != actual */
            rc = xtk_decode(g_tmp, n, &g_msg);
            check(rc == XTK_ERR_BAD_LENGTH, "reject inconsistent length");

            memcpy(g_tmp, g_golden, n + 1);         /* trailing octet */
            g_tmp[n] = 0;
            rc = xtk_decode(g_tmp, n + 1, &g_msg);
            check(rc == XTK_ERR_BAD_LENGTH, "reject trailing octet");
        }
    }

    /* 5. Delegated per-type payload rejections. */
    {
        static const struct {
            const char *fix;
            size_t      off;      /* payload-relative */
            int         width;    /* 1 = u8, 2 = u16 */
            uint16_t    value;
            int         want;
            const char *what;
        } cases[] = {
            { "input_batch.hex", 8,  2, 33,        XTK_ERR_BAD_LENGTH,
              "oversized INPUT_BATCH count" },
            { "reliable_cmd.hex", 4, 2, 33,        XTK_ERR_BAD_LENGTH,
              "oversized RELIABLE_CMD count" },
            { "cmd_ack.hex",     0,  2, 33,        XTK_ERR_BAD_LENGTH,
              "oversized CMD_ACK count" },
            { "catalog.hex",     32, 2, 9,         XTK_ERR_BAD_LENGTH,
              "oversized CATALOG count" },
            { "results.hex",     6,  1, 5,         XTK_ERR_BAD_LENGTH,
              "oversized RESULTS count" },
            { "view_snapshot.hex", 12, 2, 17,      XTK_ERR_BAD_LENGTH,
              "oversized snapshot vehicle count" },
            { "hello.hex",       8,  1, 33,        XTK_ERR_MALFORMED,
              "oversized name length" },
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
            } else {
                xtk_store_u16(g_tmp + XTK_ENVELOPE_BYTES + cases[k].off,
                              cases[k].value);
            }
            rc = xtk_decode(g_tmp, n, &g_msg);
            check(rc == cases[k].want, cases[k].what);
        }

        /* malformed string byte: HELLO name is 5 bytes at payload offset 9 */
        {
            size_t n = 0;
            if (load_hex(dir, "hello.hex", g_golden, sizeof(g_golden), &n)) {
                int rc;
                memcpy(g_tmp, g_golden, n);
                g_tmp[XTK_ENVELOPE_BYTES + 9] = 0x80;
                rc = xtk_decode(g_tmp, n, &g_msg);
                check(rc == XTK_ERR_MALFORMED, "reject non-ASCII string byte");
            }
        }

        /* NaN and +Inf f64 in INPUT_BATCH command[0].a (payload offset 24) */
        {
            size_t n = 0;
            static const uint8_t nan_bits[8] =
                { 0x7F, 0xF8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
            static const uint8_t inf_bits[8] =
                { 0x7F, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
            if (load_hex(dir, "input_batch.hex", g_golden, sizeof(g_golden), &n)) {
                int rc;
                memcpy(g_tmp, g_golden, n);
                memcpy(g_tmp + XTK_ENVELOPE_BYTES + 24, nan_bits, 8);
                rc = xtk_decode(g_tmp, n, &g_msg);
                check(rc == XTK_ERR_NONFINITE, "reject NaN f64");

                memcpy(g_tmp, g_golden, n);
                memcpy(g_tmp + XTK_ENVELOPE_BYTES + 24, inf_bits, 8);
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

        env_set(&m, XTK_MSG_INPUT_BATCH, 1, 1, 1, 0);
        m.body.input_batch.command_count = 1;
        m.body.input_batch.command[0].a = 0.0 / 0.0; /* NaN */
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
