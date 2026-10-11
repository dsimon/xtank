/*
 * Protocol/decode.c -- bounded, allocation-free protocol-v1 decoder (P2.3).
 *
 * Parses and fully validates the frozen wire format of docs/PROTOCOL-v1.md
 * into the decoded shapes in Protocol/wire.h.  Every read is bounds-checked,
 * every count is checked against its Protocol/limits.h bound before it is
 * used, strings are length-prefixed and ASCII-checked, and every f64 is
 * rejected unless finite.  The decoder never allocates and never copies a
 * native struct; it is the counterpart of Protocol/encode.c and shares only
 * the primitive load routines declared in wire.h.
 *
 * Uses no engine, SDL, ENet or X11 header.
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

/* ---- primitive big-endian loads -------------------------------------- */

uint16_t xtk_load_u16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

uint32_t xtk_load_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

int32_t xtk_load_i32(const uint8_t *p)
{
    return (int32_t)xtk_load_u32(p);
}

int xtk_load_f64(const uint8_t *p, double *out)
{
    uint64_t bits = 0;
    double v;
    int i;
    for (i = 0; i < 8; ++i) {
        bits = (bits << 8) | (uint64_t)p[i];
    }
    copy_bytes((uint8_t *)&v, (const uint8_t *)&bits, sizeof(v));
    if (!xtk_finite_f64(v)) {
        return 0;
    }
    *out = v;
    return 1;
}

uint8_t xtk_next_channel(uint8_t message_type)
{
    switch (message_type) {
    case XTK_MSG_HELLO:
    case XTK_MSG_WELCOME:
    case XTK_MSG_REJECT:
    case XTK_MSG_PING:
    case XTK_MSG_PONG:
    case XTK_MSG_CONTENT_CATALOG:
    case XTK_MSG_CONFIG_SELECT:
    case XTK_MSG_CONFIG_STATUS:
    case XTK_MSG_READY:
    case XTK_MSG_START:
    case XTK_MSG_EPOCH_BEGIN:
    case XTK_MSG_RESULTS:
    case XTK_MSG_RESET:
    case XTK_MSG_QUIT:
    case XTK_MSG_EVENT:
    case XTK_MSG_CMD_ACK:
    case XTK_MSG_PROTOCOL_ERROR:
    case XTK_MSG_RELIABLE_CMD:
        return XTK_CH_CONTROL;
    case XTK_MSG_VIEW_SNAPSHOT:
        return XTK_CH_SNAPSHOT;
    case XTK_MSG_INPUT_BATCH:
        return XTK_CH_INPUT;
    default:
        return 0xFFu;
    }
}

/* ---- bounded reader -------------------------------------------------- */

typedef struct {
    const uint8_t *buf;
    size_t         len;
    size_t         off;
    int            err;
} R;

static void r_init(R *r, const uint8_t *buf, size_t len)
{
    r->buf = buf;
    r->len = len;
    r->off = 0;
    r->err = XTK_ERR_NONE;
}

static int r_room(R *r, size_t n)
{
    if (r->err != XTK_ERR_NONE) {
        return 0;
    }
    if (r->off > r->len || n > r->len - r->off) {
        r->err = XTK_ERR_BAD_LENGTH;
        return 0;
    }
    return 1;
}

static uint8_t r_u8(R *r)
{
    if (!r_room(r, 1)) {
        return 0;
    }
    return r->buf[r->off++];
}

static uint16_t r_u16(R *r)
{
    uint16_t v;
    if (!r_room(r, 2)) {
        return 0;
    }
    v = xtk_load_u16(r->buf + r->off);
    r->off += 2;
    return v;
}

static uint32_t r_u32(R *r)
{
    uint32_t v;
    if (!r_room(r, 4)) {
        return 0;
    }
    v = xtk_load_u32(r->buf + r->off);
    r->off += 4;
    return v;
}

static double r_f64(R *r)
{
    double v = 0.0;
    if (!r_room(r, 8)) {
        return 0.0;
    }
    if (!xtk_load_f64(r->buf + r->off, &v)) {
        r->err = XTK_ERR_NONFINITE;
        return 0.0;
    }
    r->off += 8;
    return v;
}

static void r_bytes(R *r, uint8_t *out, size_t n)
{
    if (!r_room(r, n)) {
        return;
    }
    if (n != 0) {
        copy_bytes(out, r->buf + r->off, n);
    }
    r->off += n;
}

/* Length-prefixed ASCII string into a NUL-terminated buffer of maxlen+1. */
static void r_str(R *r, char *out, size_t maxlen)
{
    size_t n, i;
    if (r->err != XTK_ERR_NONE) {
        return;
    }
    if (!r_room(r, 1)) {
        return;
    }
    n = r->buf[r->off++];
    if (n > maxlen) {
        r->err = XTK_ERR_MALFORMED;
        return;
    }
    if (!r_room(r, n)) {
        return;
    }
    for (i = 0; i < n; ++i) {
        uint8_t c = r->buf[r->off + i];
        if (c == 0x00u || c > 0x7Fu) {
            r->err = XTK_ERR_MALFORMED;
            return;
        }
    }
    if (n != 0) {
        copy_bytes(out, r->buf + r->off, n);
    }
    out[n] = '\0';
    r->off += n;
}

static void r_count(R *r, uint32_t count, uint32_t max)
{
    if (r->err == XTK_ERR_NONE && count > max) {
        r->err = XTK_ERR_BAD_LENGTH;
    }
}

/* ---- envelope -------------------------------------------------------- */

static int dec_envelope(const uint8_t *buf, size_t len, XtkEnvelope *e,
                        size_t *payload_off, size_t *payload_len)
{
    uint8_t channel;

    if (len < XTK_ENVELOPE_BYTES) {
        return XTK_ERR_BAD_LENGTH;
    }
    e->magic = xtk_load_u32(buf);
    if (e->magic != XTK_WIRE_MAGIC) {
        return XTK_ERR_BAD_MAGIC;
    }
    e->version_major = buf[4];
    e->version_minor = buf[5];
    if (e->version_major != XTK_PROTOCOL_MAJOR) {
        return XTK_ERR_BAD_VERSION;
    }
    if (e->version_minor != XTK_PROTOCOL_MINOR) {
        return XTK_ERR_VERSION_MISMATCH;
    }
    e->message_type = buf[6];
    e->channel = buf[7];
    e->payload_length = xtk_load_u32(buf + 8);
    e->session_id = xtk_load_u32(buf + 12);
    e->epoch = xtk_load_u32(buf + 16);
    e->message_sequence = xtk_load_u32(buf + 20);
    e->tick = xtk_load_i32(buf + 24);
    e->reserved = xtk_load_u32(buf + 28);

    /* Check the declared length before any overflow-prone arithmetic. */
    if (e->payload_length > XTK_MAX_PAYLOAD_BYTES) {
        return XTK_ERR_BAD_LENGTH;
    }
    if (len != XTK_ENVELOPE_BYTES + (size_t)e->payload_length) {
        return XTK_ERR_BAD_LENGTH;
    }
    channel = xtk_next_channel(e->message_type);
    if (channel == 0xFFu) {
        return XTK_ERR_BAD_TYPE;
    }
    if (e->channel != channel) {
        return XTK_ERR_BAD_CHANNEL;
    }
    if (e->reserved != 0) {
        return XTK_ERR_MALFORMED;
    }
    if (e->tick < 0) {
        return XTK_ERR_MALFORMED;
    }

    *payload_off = XTK_ENVELOPE_BYTES;
    *payload_len = e->payload_length;
    return XTK_ERR_NONE;
}

/* ---- payload decoders ------------------------------------------------ */

static void dec_ping(R *r, XtkPing *m)          { m->nonce = r_u32(r); }

static void dec_hello(R *r, XtkHello *m)
{
    m->client_nonce = r_u32(r);
    m->proto_major = r_u8(r);
    m->proto_minor = r_u8(r);
    m->client_build = r_u16(r);
    r_str(r, m->name, XTK_MAX_NAME_BYTES);
    r_bytes(r, m->content_set_digest, XTK_SHA256_BYTES);
}

static void dec_welcome(R *r, XtkWelcome *m)
{
    m->server_nonce = r_u32(r);
    m->role = r_u8(r);
    m->tick_hz = r_u8(r);
    m->max_players = r_u8(r);
    m->reserved = r_u8(r);
}

static void dec_reject(R *r, XtkReject *m)
{
    m->error_code = r_u16(r);
    m->detail_code = r_u16(r);
    r_str(r, m->text, XTK_MAX_ERROR_TEXT_BYTES);
}

static void dec_catalog(R *r, XtkCatalog *m)
{
    uint16_t i;
    r_bytes(r, m->content_set_digest, XTK_SHA256_BYTES);
    m->entry_count = r_u16(r);
    r_count(r, m->entry_count, XTK_MAX_CATALOG_ENTRIES);
    for (i = 0; i < m->entry_count && r->err == XTK_ERR_NONE; ++i) {
        m->entry[i].id = r_u16(r);
        m->entry[i].kind = r_u8(r);
        r_str(r, m->entry[i].name, XTK_MAX_NAME_BYTES);
        r_bytes(r, m->entry[i].source_sha256, XTK_SHA256_BYTES);
    }
}

static void dec_config_select(R *r, XtkConfigSelect *m)
{
    m->maze_id = r_u16(r);
    m->vehicle_id = r_u16(r);
    m->reserved = r_u32(r);
}

static void dec_config_status(R *r, XtkConfigStatus *m)
{
    m->maze_id = r_u16(r);
    m->vehicle_id = r_u16(r);
    m->status = r_u16(r);
    m->error_code = r_u16(r);
}

static void dec_start(R *r, XtkStart *m)
{
    m->start_tick = r_u32(r);
    m->maze_id = r_u16(r);
    m->vehicle_id = r_u8(r);
    m->reserved = r_u8(r);
}

static void dec_epoch_begin(R *r, XtkEpochBegin *m)
{
    m->epoch = r_u32(r);
    m->first_tick = r_u32(r);
}

static void dec_results(R *r, XtkResults *m)
{
    uint8_t i;
    m->end_tick = r_u32(r);
    m->result_kind = r_u8(r);
    m->winner_slot = r_u8(r);
    m->vehicle_count = r_u8(r);
    m->reserved = r_u8(r);
    r_count(r, m->vehicle_count, XTK_MAX_PLAYERS);
    for (i = 0; i < m->vehicle_count && r->err == XTK_ERR_NONE; ++i) {
        m->entry[i].slot = r_u8(r);
        m->entry[i].generation = r_u8(r);
        m->entry[i].reserved = r_u16(r);
        m->entry[i].score = r_u32(r);
        r_str(r, m->entry[i].name, XTK_MAX_NAME_BYTES);
    }
}

static void dec_reset(R *r, XtkReset *m)
{
    m->prior_epoch = r_u32(r);
    m->reserved = r_u32(r);
}

static void dec_event(R *r, XtkEvent *m)
{
    m->event_sequence = r_u32(r);
    m->event_type = r_u8(r);
    m->entity_kind = r_u8(r);
    m->entity_id = r_u16(r);
    m->entity_generation = r_u32(r);
    m->aux0 = r_u32(r);
    m->aux1 = r_u32(r);
    r_str(r, m->text, XTK_MAX_NAME_BYTES);
}

static void dec_cmd_ack(R *r, XtkCmdAck *m)
{
    uint16_t i;
    m->count = r_u16(r);
    m->reserved = r_u16(r);
    r_count(r, m->count, XTK_ACK_MAX_ENTRIES);
    for (i = 0; i < m->count && r->err == XTK_ERR_NONE; ++i) {
        m->entry[i].command_id = r_u32(r);
        m->entry[i].assigned_tick = r_u32(r);
        m->entry[i].status = r_u8(r);
        m->entry[i].reason = r_u8(r);
        m->entry[i].reserved = r_u16(r);
    }
}

static void dec_protocol_error(R *r, XtkProtocolError *m)
{
    m->error_code = r_u16(r);
    m->detail = r_u16(r);
    m->offending_sequence = r_u32(r);
    m->offending_type = r_u16(r);
    m->reserved = r_u16(r);
}

static void dec_commands(R *r, XtkCommand *c, uint16_t n)
{
    uint16_t i;
    for (i = 0; i < n && r->err == XTK_ERR_NONE; ++i) {
        c[i].command_id = r_u32(r);
        c[i].tick_hint = r_u32(r);
        c[i].op = r_u8(r);
        c[i].flags = r_u8(r);
        c[i].reserved = r_u16(r);
        c[i].a = r_f64(r);
        c[i].b = r_f64(r);
    }
}

static void dec_reliable_cmd(R *r, XtkReliableCmd *m)
{
    m->last_observed_server_tick = r_u32(r);
    m->command_count = r_u16(r);
    m->reserved = r_u16(r);
    r_count(r, m->command_count, XTK_MAX_INPUT_COMMANDS_PER_BATCH);
    dec_commands(r, m->command, m->command_count);
}

static void dec_input_batch(R *r, XtkInputBatch *m)
{
    m->packet_sequence = r_u32(r);
    m->last_observed_server_tick = r_u32(r);
    m->command_count = r_u16(r);
    m->reserved = r_u16(r);
    r_count(r, m->command_count, XTK_MAX_INPUT_COMMANDS_PER_BATCH);
    dec_commands(r, m->command, m->command_count);
}

static void dec_view_snapshot(R *r, XtkViewSnapshot *m)
{
    uint16_t i;
    m->snapshot_sequence = r_u32(r);
    m->server_tick = r_u32(r);
    m->flags = r_u16(r);
    m->own_slot = r_u8(r);
    m->own_generation = r_u8(r);
    m->vehicle_count = r_u16(r);
    m->bullet_count = r_u16(r);
    m->explosion_count = r_u16(r);
    m->map_cell_count = r_u16(r);
    m->landmark_count = r_u16(r);
    m->radar_blip_count = r_u8(r);
    m->weapon_count = r_u8(r);
    m->map_revision = r_u32(r);
    r_count(r, m->vehicle_count, XTK_MAX_VEHICLES);
    r_count(r, m->bullet_count, XTK_MAX_BULLETS);
    r_count(r, m->explosion_count, XTK_MAX_EXPLOSIONS);
    r_count(r, m->map_cell_count, XTK_MAX_MAP_CELLS);
    r_count(r, m->landmark_count, XTK_MAX_LANDMARKS);
    r_count(r, m->radar_blip_count, XTK_MAX_RADAR_BLIPS);
    r_count(r, m->weapon_count, XTK_MAX_WEAPONS);
    if (r->err != XTK_ERR_NONE) {
        return;
    }

    m->self.slot = r_u8(r);
    m->self.generation = r_u8(r);
    m->self.lifecycle_flags = r_u16(r);
    for (i = 0; i < XTK_ARMOR_SIDES; ++i) m->self.armor[i] = r_u16(r);
    m->self.fuel = r_u16(r);
    m->self.heat = r_u16(r);
    m->self.score = r_u32(r);
    for (i = 0; i < XTK_MAX_WEAPONS; ++i) m->self.ammo[i] = r_u16(r);
    m->self.weapon_enabled_bitmap = r_u8(r);
    m->self.weapon_count = r_u8(r);
    for (i = 0; i < XTK_MAX_WEAPONS; ++i) m->self.reload[i] = r_u16(r);
    r_bytes(r, m->self.reserved, 2);

    for (i = 0; i < m->vehicle_count && r->err == XTK_ERR_NONE; ++i) {
        XtkVehicleView *v = &m->vehicle[i];
        v->slot = r_u8(r);
        v->generation = r_u8(r);
        v->kind = r_u8(r);
        v->flags = r_u8(r);
        v->x = r_f64(r);
        v->y = r_f64(r);
        v->body_angle = r_f64(r);
        v->turret_angle = r_f64(r);
        v->body_picture = r_u16(r);
        v->turret_picture = r_u16(r);
        r_bytes(r, v->reserved, 4);
    }
    for (i = 0; i < m->bullet_count && r->err == XTK_ERR_NONE; ++i) {
        XtkBulletView *b = &m->bullet[i];
        b->id = r_u16(r);
        b->flags = r_u16(r);
        b->x = r_f64(r);
        b->y = r_f64(r);
        b->heading = r_f64(r);
        b->owner_slot = r_u8(r);
        b->owner_generation = r_u8(r);
        b->kind = r_u8(r);
        b->reserved = r_u8(r);
    }
    for (i = 0; i < m->explosion_count && r->err == XTK_ERR_NONE; ++i) {
        XtkExplosionView *e = &m->explosion[i];
        e->id = r_u16(r);
        e->kind = r_u8(r);
        e->flags = r_u8(r);
        e->x = r_f64(r);
        e->y = r_f64(r);
        r_bytes(r, e->reserved, 4);
    }
    for (i = 0; i < m->map_cell_count && r->err == XTK_ERR_NONE; ++i) {
        XtkMapCell *c = &m->cell[i];
        c->grid_x = r_u8(r);
        c->grid_y = r_u8(r);
        c->flags = r_u8(r);
        c->type = r_u8(r);
    }
    for (i = 0; i < m->landmark_count && r->err == XTK_ERR_NONE; ++i) {
        XtkLandmarkView *l = &m->landmark[i];
        l->kind = r_u8(r);
        l->id = r_u8(r);
        l->reserved = r_u16(r);
        l->x = r_f64(r);
        l->y = r_f64(r);
        r_bytes(r, l->reserved2, 4);
    }
    for (i = 0; i < m->radar_blip_count && r->err == XTK_ERR_NONE; ++i) {
        XtkRadarBlip *b = &m->radar[i];
        b->x = r_f64(r);
        b->y = r_f64(r);
        b->age = r_u8(r);
        b->classification = r_u8(r);
        b->reserved = r_u16(r);
    }
}

/* ---- whole-message decode -------------------------------------------- */

int xtk_decode(const uint8_t *buf, size_t len, XtkMessage *msg)
{
    size_t payload_off = 0;
    size_t payload_len = 0;
    int err;
    R r;

    if (buf == NULL || msg == NULL) {
        return XTK_ERR_BAD_LENGTH;
    }
    err = dec_envelope(buf, len, &msg->env, &payload_off, &payload_len);
    if (err != XTK_ERR_NONE) {
        return err;
    }

    r_init(&r, buf + payload_off, payload_len);
    switch (msg->env.message_type) {
    case XTK_MSG_PING:
    case XTK_MSG_PONG:       dec_ping(&r, &msg->body.ping); break;
    case XTK_MSG_HELLO:      dec_hello(&r, &msg->body.hello); break;
    case XTK_MSG_WELCOME:    dec_welcome(&r, &msg->body.welcome); break;
    case XTK_MSG_REJECT:     dec_reject(&r, &msg->body.reject); break;
    case XTK_MSG_CONTENT_CATALOG:
                             dec_catalog(&r, &msg->body.catalog); break;
    case XTK_MSG_CONFIG_SELECT:
                             dec_config_select(&r, &msg->body.config_select); break;
    case XTK_MSG_CONFIG_STATUS:
                             dec_config_status(&r, &msg->body.config_status); break;
    case XTK_MSG_READY:      break;
    case XTK_MSG_START:      dec_start(&r, &msg->body.start); break;
    case XTK_MSG_EPOCH_BEGIN:
                             dec_epoch_begin(&r, &msg->body.epoch_begin); break;
    case XTK_MSG_RESULTS:    dec_results(&r, &msg->body.results); break;
    case XTK_MSG_RESET:      dec_reset(&r, &msg->body.reset); break;
    case XTK_MSG_QUIT:       break;
    case XTK_MSG_EVENT:      dec_event(&r, &msg->body.event); break;
    case XTK_MSG_CMD_ACK:    dec_cmd_ack(&r, &msg->body.cmd_ack); break;
    case XTK_MSG_PROTOCOL_ERROR:
                             dec_protocol_error(&r, &msg->body.protocol_error); break;
    case XTK_MSG_RELIABLE_CMD:
                             dec_reliable_cmd(&r, &msg->body.reliable_cmd); break;
    case XTK_MSG_INPUT_BATCH:
                             dec_input_batch(&r, &msg->body.input_batch); break;
    case XTK_MSG_VIEW_SNAPSHOT:
                             dec_view_snapshot(&r, &msg->body.view_snapshot); break;
    default:
        return XTK_ERR_BAD_TYPE;
    }
    if (r.err != XTK_ERR_NONE) {
        return r.err;
    }
    if (r.off != payload_len) {
        return XTK_ERR_MALFORMED;
    }
    return XTK_ERR_NONE;
}
