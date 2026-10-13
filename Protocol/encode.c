/*
 * Protocol/encode.c -- bounded protocol-v1 encoder (P2.3).
 *
 * Serializes the decoded message shapes in Protocol/wire.h into the frozen
 * wire format of docs/PROTOCOL-v1.md: explicit big-endian fixed-width fields,
 * length-prefixed ASCII strings, IEEE-754 binary64 bit patterns, packed with
 * no padding.  It copies no native struct and never delegates byte order to
 * the host.
 *
 * The encoder is the counterpart of Protocol/decode.c and shares only the
 * primitive store routines declared in wire.h.  It uses no engine, SDL, ENet
 * or X11 header and performs no allocation.
 *
 * Authority: docs/PROTOCOL-v1.md; Tests/phase2/fixtures/protocol/.
 */

#include "wire.h"

/* Local byte copy: Protocol/ includes only stdint/stddef and protocol
 * headers, so <string.h> is unavailable here. */
static void copy_bytes(void *dstv, const void *srcv, size_t n)
{
    uint8_t *dst = (uint8_t *)dstv;
    const uint8_t *src = (const uint8_t *)srcv;
    size_t i;
    for (i = 0; i < n; ++i) {
        dst[i] = src[i];
    }
}

static size_t c_strlen(const char *s)
{
    size_t n = 0;
    while (s[n] != '\0') {
        ++n;
    }
    return n;
}

/* ---- primitive big-endian stores ------------------------------------- */

void xtk_store_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v);
}

void xtk_store_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v);
}

void xtk_store_i32(uint8_t *p, int32_t v)
{
    xtk_store_u32(p, (uint32_t)v);
}

void xtk_store_f64(uint8_t *p, double v)
{
    uint64_t bits = 0;
    int i;
    copy_bytes((uint8_t *)&bits, (const uint8_t *)&v, sizeof(bits));
    for (i = 0; i < 8; ++i) {
        p[i] = (uint8_t)(bits >> (56 - 8 * i));
    }
}

/* ---- bounded writer -------------------------------------------------- */

typedef struct {
    uint8_t *buf;
    size_t   cap;
    size_t   len;
    int      err;
} W;

static void w_init(W *w, uint8_t *buf, size_t cap)
{
    w->buf = buf;
    w->cap = cap;
    w->len = 0;
    w->err = XTK_ERR_NONE;
}

static int w_room(W *w, size_t n)
{
    if (w->err != XTK_ERR_NONE) {
        return 0;
    }
    if (w->len > w->cap || n > w->cap - w->len) {
        w->err = XTK_ERR_BAD_LENGTH;
        return 0;
    }
    return 1;
}

static void w_u8(W *w, uint8_t v)
{
    if (w_room(w, 1)) {
        w->buf[w->len++] = v;
    }
}

static void w_u16(W *w, uint16_t v)
{
    if (w_room(w, 2)) {
        xtk_store_u16(w->buf + w->len, v);
        w->len += 2;
    }
}

static void w_u32(W *w, uint32_t v)
{
    if (w_room(w, 4)) {
        xtk_store_u32(w->buf + w->len, v);
        w->len += 4;
    }
}

static void w_i32(W *w, int32_t v)
{
    if (w_room(w, 4)) {
        xtk_store_i32(w->buf + w->len, v);
        w->len += 4;
    }
}

static void w_f64(W *w, double v)
{
    if (w->err != XTK_ERR_NONE) {
        return;
    }
    if (!xtk_finite_f64(v)) {
        w->err = XTK_ERR_NONFINITE;
        return;
    }
    if (w_room(w, 8)) {
        xtk_store_f64(w->buf + w->len, v);
        w->len += 8;
    }
}

static void w_bytes(W *w, const uint8_t *p, size_t n)
{
    if (w_room(w, n)) {
        if (n != 0) {
            copy_bytes(w->buf + w->len, p, n);
        }
        w->len += n;
    }
}

/* Length-prefixed ASCII string.  The C string must be NUL-terminated, at
 * most maxlen octets and contain no embedded NUL and no byte above 0x7F. */
static void w_str(W *w, const char *s, size_t maxlen)
{
    size_t n;
    if (w->err != XTK_ERR_NONE) {
        return;
    }
    n = c_strlen(s);
    if (n > maxlen || n > 0xFFu) {
        w->err = XTK_ERR_MALFORMED;
        return;
    }
    if (n != 0) {
        size_t i;
        for (i = 0; i < n; ++i) {
            uint8_t c = (uint8_t)s[i];
            if (c == 0x00u || c > 0x7Fu) {
                w->err = XTK_ERR_MALFORMED;
                return;
            }
        }
    }
    w_u8(w, (uint8_t)n);
    w_bytes(w, (const uint8_t *)s, n);
}

/* ---- payload encoders ------------------------------------------------ */

static void enc_ping(W *w, const XtkPing *m)                 { w_u32(w, m->nonce); }

static void enc_hello(W *w, const XtkHello *m)
{
    w_u32(w, m->client_nonce);
    w_u8(w, m->proto_major);
    w_u8(w, m->proto_minor);
    w_u16(w, m->client_build);
    w_str(w, m->name, XTK_MAX_NAME_BYTES);
    w_bytes(w, m->content_set_digest, XTK_SHA256_BYTES);
}

static void enc_welcome(W *w, const XtkWelcome *m)
{
    w_u32(w, m->server_nonce);
    w_u8(w, m->role);
    w_u8(w, m->tick_hz);
    w_u8(w, m->max_players);
    w_u8(w, m->human_capacity);
    w_u8(w, 0);
}

static void enc_reject(W *w, const XtkReject *m)
{
    w_u16(w, m->error_code);
    w_u16(w, m->detail_code);
    w_str(w, m->text, XTK_MAX_ERROR_TEXT_BYTES);
}

static void enc_catalog(W *w, const XtkCatalog *m)
{
    uint16_t i;
    if (m->entry_count > XTK_MAX_CATALOG_ENTRIES) {
        w->err = XTK_ERR_BAD_LENGTH;
        return;
    }
    w_bytes(w, m->content_set_digest, XTK_SHA256_BYTES);
    w_u16(w, m->entry_count);
    for (i = 0; i < m->entry_count; ++i) {
        w_u16(w, m->entry[i].id);
        w_u8(w, m->entry[i].kind);
        w_str(w, m->entry[i].name, XTK_MAX_NAME_BYTES);
        w_bytes(w, m->entry[i].source_sha256, XTK_SHA256_BYTES);
    }
}

static void enc_config_select(W *w, const XtkConfigSelect *m)
{
    w_u16(w, m->maze_id);
    w_u16(w, m->vehicle_id);
    w_u8(w, m->drone_count);
    w_u8(w, 0);
    w_u16(w, 0);
    w_u32(w, 0);
}

static void enc_config_status(W *w, const XtkConfigStatus *m)
{
    w_u16(w, m->maze_id);
    w_u16(w, m->vehicle_id);
    w_u8(w, m->drone_count);
    w_u8(w, 0);
    w_u16(w, m->status);
    w_u16(w, m->error_code);
    w_u16(w, 0);
    w_u32(w, m->config_revision);
}

static void enc_ready(W *w, const XtkReady *m)
{
    w_u8(w, m->ready);
    w_u8(w, 0);
    w_u16(w, 0);
    w_u32(w, m->config_revision);
}

static void enc_start(W *w, const XtkStart *m)
{
    w_u32(w, m->start_tick);
    w_u16(w, m->maze_id);
    w_u16(w, m->vehicle_id);
    w_u8(w, m->drone_count);
    w_u8(w, 0);
    w_u16(w, 0);
    w_u32(w, m->config_revision);
}

static void enc_start_request(W *w, const XtkStartRequest *m)
{
    w_u32(w, m->expected_config_revision);
    w_u32(w, 0);
}

/* LOBBY_STATE carries its roster inline; this helper is the single source of
 * the entry layout. */
static void enc_roster_entry(W *w, const XtkRosterEntry *e)
{
    w_u32(w, e->connection_id);
    w_u32(w, e->controller_id);
    w_u8(w, e->slot);
    w_u8(w, e->ready);
    w_u8(w, e->flags);
    w_u8(w, e->departure_reason);
    w_str(w, e->name, XTK_MAX_NAME_BYTES);
}

static void enc_lobby_state(W *w, const XtkLobbyStatus *m)
{
    uint8_t i;
    if (m->human_count > XTK_LOBBY_ROSTER_MAX) {
        w->err = XTK_ERR_BAD_LENGTH;
        return;
    }
    w_u32(w, m->config_revision);
    w_u8(w, m->session_state);
    w_u8(w, m->drone_count);
    w_u16(w, m->maze_id);
    w_u16(w, m->vehicle_id);
    w_u8(w, m->self_index);
    w_u8(w, m->admin_index);
    w_u8(w, m->human_count);
    w_u8(w, 0);
    w_u16(w, 0);
    for (i = 0; i < m->human_count; ++i) {
        enc_roster_entry(w, &m->entry[i]);
    }
}

static void enc_epoch_begin(W *w, const XtkEpochBegin *m)
{
    w_u32(w, m->epoch);
    w_u32(w, m->first_tick);
}

static void enc_results(W *w, const XtkResults *m)
{
    uint8_t i;
    if (m->vehicle_count > XTK_MAX_PLAYERS) {
        w->err = XTK_ERR_BAD_LENGTH;
        return;
    }
    w_u32(w, m->end_tick);
    w_u8(w, m->result_kind);
    w_u8(w, m->winner_slot);
    w_u8(w, m->vehicle_count);
    w_u8(w, 0);
    for (i = 0; i < m->vehicle_count; ++i) {
        w_u8(w, m->entry[i].slot);
        w_u8(w, m->entry[i].flags);
        w_u16(w, 0);
        w_u32(w, m->entry[i].generation);
        w_u32(w, m->entry[i].score);
        w_str(w, m->entry[i].name, XTK_MAX_NAME_BYTES);
    }
}

static void enc_reset(W *w, const XtkReset *m)
{
    w_u32(w, m->prior_epoch);
    w_u32(w, 0);
}

static void enc_event(W *w, const XtkEvent *m)
{
    w_u32(w, m->event_sequence);
    w_u32(w, m->capture_tick);
    w_u8(w, m->event_type);
    w_u8(w, m->entity_kind);
    w_u16(w, m->entity_id);
    w_u32(w, m->entity_generation);
    w_u32(w, m->aux0);
    w_u32(w, m->aux1);
    w_str(w, m->text, XTK_MAX_NAME_BYTES);
}

static void enc_cmd_ack(W *w, const XtkCmdAck *m)
{
    uint16_t i;
    if (m->count > XTK_ACK_MAX_ENTRIES) {
        w->err = XTK_ERR_BAD_LENGTH;
        return;
    }
    w_u16(w, m->count);
    w_u16(w, 0);
    for (i = 0; i < m->count; ++i) {
        w_u32(w, m->entry[i].command_id);
        w_u32(w, m->entry[i].assigned_tick);
        w_u8(w, m->entry[i].status);
        w_u8(w, m->entry[i].reason);
        w_u16(w, 0);
    }
}

static void enc_protocol_error(W *w, const XtkProtocolError *m)
{
    w_u16(w, m->error_code);
    w_u16(w, m->detail);
    w_u32(w, m->offending_sequence);
    w_u16(w, m->offending_type);
    w_u16(w, 0);
}

static void enc_commands(W *w, const XtkCommand *c, uint16_t n)
{
    uint16_t i;
    for (i = 0; i < n; ++i) {
        w_u32(w, c[i].command_id);
        w_u32(w, c[i].tick_hint);
        w_u32(w, c[i].life_generation);
        w_u8(w, c[i].op);
        w_u8(w, c[i].flags);
        w_u16(w, 0);
        w_f64(w, c[i].a);
        w_f64(w, c[i].b);
    }
}

static void enc_reliable_cmd(W *w, const XtkReliableCmd *m)
{
    if (m->command_count > XTK_MAX_INPUT_COMMANDS_PER_BATCH) {
        w->err = XTK_ERR_BAD_LENGTH;
        return;
    }
    w_u32(w, m->last_observed_server_tick);
    w_u16(w, m->command_count);
    w_u16(w, 0);
    enc_commands(w, m->command, m->command_count);
}

static void enc_input_batch(W *w, const XtkInputBatch *m)
{
    if (m->command_count > XTK_MAX_INPUT_COMMANDS_PER_BATCH) {
        w->err = XTK_ERR_BAD_LENGTH;
        return;
    }
    w_u32(w, m->packet_sequence);
    w_u32(w, m->last_observed_server_tick);
    w_u16(w, m->command_count);
    w_u16(w, 0);
    enc_commands(w, m->command, m->command_count);
}

static void enc_self_state(W *w, const XtkSelfState *m)
{
    int i;
    w_u8(w, m->slot);
    w_u8(w, 0);
    w_u16(w, m->lifecycle_flags);
    for (i = 0; i < (int)XTK_ARMOR_SIDES; ++i) w_u16(w, m->armor[i]);
    w_u16(w, m->fuel);
    w_u16(w, m->heat);
    w_u32(w, m->score);
    for (i = 0; i < (int)XTK_MAX_WEAPONS; ++i) w_u16(w, m->ammo[i]);
    w_u8(w, m->weapon_enabled_bitmap);
    w_u8(w, m->weapon_count);
    for (i = 0; i < (int)XTK_MAX_WEAPONS; ++i) w_u16(w, m->reload[i]);
    w_u32(w, m->generation);
    w_u8(w, 0);
    w_u8(w, 0);
}

static void enc_view_snapshot(W *w, const XtkViewSnapshot *m)
{
    uint16_t i;
    if (m->vehicle_count > XTK_MAX_VEHICLES ||
        m->bullet_count > XTK_MAX_BULLETS ||
        m->explosion_count > XTK_MAX_EXPLOSIONS ||
        m->map_cell_count > XTK_MAX_MAP_CELLS ||
        m->landmark_count > XTK_MAX_LANDMARKS ||
        m->radar_blip_count > XTK_MAX_RADAR_BLIPS ||
        m->weapon_count > XTK_MAX_WEAPONS) {
        w->err = XTK_ERR_BAD_LENGTH;
        return;
    }
    w_u32(w, m->snapshot_sequence);
    w_u32(w, m->server_tick);
    w_u16(w, m->flags);
    w_u8(w, m->own_slot);
    w_u8(w, 0);
    w_u32(w, m->own_generation);
    w_u16(w, m->vehicle_count);
    w_u16(w, m->bullet_count);
    w_u16(w, m->explosion_count);
    w_u16(w, m->map_cell_count);
    w_u16(w, m->landmark_count);
    w_u8(w, m->radar_blip_count);
    w_u8(w, m->weapon_count);
    w_u32(w, m->map_revision);

    enc_self_state(w, &m->self);

    for (i = 0; i < m->vehicle_count; ++i) {
        const XtkVehicleView *v = &m->vehicle[i];
        w_u8(w, v->slot);
        w_u8(w, 0);
        w_u8(w, v->kind);
        w_u8(w, v->flags);
        w_f64(w, v->x);
        w_f64(w, v->y);
        w_f64(w, v->body_angle);
        w_f64(w, v->turret_angle);
        w_u16(w, v->body_picture);
        w_u16(w, v->turret_picture);
        w_u32(w, v->generation);
        w_bytes(w, (const uint8_t[4]){0, 0, 0, 0}, 4);
    }
    for (i = 0; i < m->bullet_count; ++i) {
        const XtkBulletView *b = &m->bullet[i];
        w_u16(w, b->id);
        w_u16(w, b->flags);
        w_f64(w, b->x);
        w_f64(w, b->y);
        w_f64(w, b->heading);
        w_u8(w, b->owner_slot);
        w_u8(w, b->kind);
        w_u16(w, 0);
        w_u32(w, b->generation);
        w_u32(w, b->owner_generation);
    }
    for (i = 0; i < m->explosion_count; ++i) {
        const XtkExplosionView *e = &m->explosion[i];
        w_u16(w, e->id);
        w_u8(w, e->kind);
        w_u8(w, e->flags);
        w_f64(w, e->x);
        w_f64(w, e->y);
        w_u32(w, e->generation);
        w_bytes(w, (const uint8_t[4]){0, 0, 0, 0}, 4);
    }
    for (i = 0; i < m->map_cell_count; ++i) {
        const XtkMapCell *c = &m->cell[i];
        w_u8(w, c->grid_x);
        w_u8(w, c->grid_y);
        w_u8(w, c->flags);
        w_u8(w, c->type);
    }
    for (i = 0; i < m->landmark_count; ++i) {
        const XtkLandmarkView *l = &m->landmark[i];
        w_u8(w, l->kind);
        w_u8(w, l->grid_x);
        w_u8(w, l->grid_y);
        w_u8(w, 0);
        w_f64(w, l->x);
        w_f64(w, l->y);
        w_u16(w, l->id);
        w_u16(w, 0);
        w_bytes(w, (const uint8_t[4]){0, 0, 0, 0}, 4);
    }
    for (i = 0; i < m->radar_blip_count; ++i) {
        const XtkRadarBlip *r = &m->radar[i];
        w_f64(w, r->x);
        w_f64(w, r->y);
        w_u8(w, r->age);
        w_u8(w, r->classification);
        w_u16(w, 0);
    }
}

/* ---- whole-message encode -------------------------------------------- */

int xtk_encode(uint8_t *out, size_t cap, const XtkMessage *msg, size_t *out_len)
{
    W w;
    uint8_t type = msg->env.message_type;
    uint8_t channel = xtk_next_channel(type);
    size_t payload_off;

    if (channel == 0xFFu) {
        return XTK_ERR_BAD_TYPE;
    }
    if (out == NULL || out_len == NULL || cap < XTK_ENVELOPE_BYTES) {
        return XTK_ERR_BAD_LENGTH;
    }

    w_init(&w, out, cap);
    w_u32(&w, XTK_WIRE_MAGIC);
    w_u8(&w, XTK_PROTOCOL_MAJOR);
    w_u8(&w, XTK_PROTOCOL_MINOR);
    w_u8(&w, type);
    w_u8(&w, channel);
    w_u32(&w, 0);                            /* payload_length, patched below */
    w_u32(&w, msg->env.session_id);
    w_u32(&w, msg->env.epoch);
    w_u32(&w, msg->env.message_sequence);
    w_i32(&w, msg->env.tick);
    w_u32(&w, 0);                            /* reserved */
    if (w.err != XTK_ERR_NONE) {
        return w.err;
    }

    payload_off = w.len;
    switch (type) {
    case XTK_MSG_PING:
    case XTK_MSG_PONG:       enc_ping(&w, &msg->body.ping); break;
    case XTK_MSG_HELLO:      enc_hello(&w, &msg->body.hello); break;
    case XTK_MSG_WELCOME:    enc_welcome(&w, &msg->body.welcome); break;
    case XTK_MSG_REJECT:     enc_reject(&w, &msg->body.reject); break;
    case XTK_MSG_CONTENT_CATALOG:
                             enc_catalog(&w, &msg->body.catalog); break;
    case XTK_MSG_CONFIG_SELECT:
                             enc_config_select(&w, &msg->body.config_select); break;
    case XTK_MSG_CONFIG_STATUS:
                             enc_config_status(&w, &msg->body.config_status); break;
    case XTK_MSG_READY:      enc_ready(&w, &msg->body.ready); break;
    case XTK_MSG_START:      enc_start(&w, &msg->body.start); break;
    case XTK_MSG_START_REQUEST:
                             enc_start_request(&w, &msg->body.start_request); break;
    case XTK_MSG_EPOCH_BEGIN:
                             enc_epoch_begin(&w, &msg->body.epoch_begin); break;
    case XTK_MSG_LOBBY_STATE:
                             enc_lobby_state(&w, &msg->body.lobby_state); break;
    case XTK_MSG_RESULTS:    enc_results(&w, &msg->body.results); break;
    case XTK_MSG_RESET:      enc_reset(&w, &msg->body.reset); break;
    case XTK_MSG_QUIT:       break;                 /* zero-length payload */
    case XTK_MSG_EVENT:      enc_event(&w, &msg->body.event); break;
    case XTK_MSG_CMD_ACK:    enc_cmd_ack(&w, &msg->body.cmd_ack); break;
    case XTK_MSG_PROTOCOL_ERROR:
                             enc_protocol_error(&w, &msg->body.protocol_error); break;
    case XTK_MSG_RELIABLE_CMD:
                             enc_reliable_cmd(&w, &msg->body.reliable_cmd); break;
    case XTK_MSG_INPUT_BATCH:
                             enc_input_batch(&w, &msg->body.input_batch); break;
    case XTK_MSG_VIEW_SNAPSHOT:
    case XTK_MSG_VIEW_KEYFRAME:
                             enc_view_snapshot(&w, &msg->body.view_snapshot); break;
    default:
        return XTK_ERR_BAD_TYPE;
    }
    if (w.err != XTK_ERR_NONE) {
        return w.err;
    }
    if (w.len - payload_off > XTK_MAX_PAYLOAD_BYTES ||
        w.len > XTK_MAX_MESSAGE_BYTES ||
        w.len - payload_off > 0xFFFFFFFFu) {
        return XTK_ERR_BAD_LENGTH;
    }

    xtk_store_u32(out + 8, (uint32_t)(w.len - payload_off));
    *out_len = w.len;
    return XTK_ERR_NONE;
}
