/*
 * Protocol/wire.h -- Xtank wire protocol identifiers and enumerations.
 *
 * Frozen schema constants only.  This header includes no engine, SDL or ENet
 * header, copies no native struct and defines no wire I/O.  Every value that
 * appears on the wire is fixed-width and is encoded explicitly in network
 * byte order by the codec; the C types here are only convenient labels and are
 * never memcpy'd onto the wire.
 *
 * Version 1.0 was frozen by P2.2 (D2-C).  Version 1.1 is frozen by P3.3
 * (docs/PHASE3-DECISIONS.md D3-A..D3-F): exact-version 1.1, no dual stack.
 * The 1.1 additions are the multiplayer/session/lifetime extensions only;
 * gameplay ops and VIEW-v1 knowledge rules are unchanged.
 *
 * Authority: docs/PROTOCOL-v1.1.md; docs/PHASE3-DECISIONS.md D3-A..D3-F;
 * docs/PROTOCOL-v1.md (v1.0 base).
 */

#ifndef XTK_PROTOCOL_WIRE_H
#define XTK_PROTOCOL_WIRE_H

#include <stdint.h>
#include <stddef.h>
#include "limits.h"

/* ---- Envelope -------------------------------------------------------- */

/* "XTK1" as four octets; the low octet also tags the wire generation. */
#define XTK_WIRE_MAGIC        UINT32_C(0x58544B31)

#define XTK_PROTOCOL_MAJOR    1u
#define XTK_PROTOCOL_MINOR    1u

/* Envelope flag bits (envelope byte 7 is the channel; flags are per message
 * and carried in the first payload byte where a message defines them). */
#define XTK_FLAG_NONE         UINT8_C(0x00)
#define XTK_FLAG_KEYFRAME     UINT8_C(0x01)   /* VIEW_*: full-view barrier    */
#define XTK_FLAG_EDGE         UINT8_C(0x01)   /* command: one-shot edge       */
#define XTK_FLAG_HELD         UINT8_C(0x02)   /* command: held control        */
#define XTK_FLAG_RELIABLE     UINT8_C(0x04)   /* command: arrived on ch0      */

/* Per-record flag bits (roster/result entries). */
#define XTK_FLAG_DEPARTED     UINT8_C(0x01)   /* record: peer/departed marker */

/* Transport channels.  A message received on the wrong channel is discarded
 * and reported as PROTOCOL_ERROR/XTK_ERR_BAD_CHANNEL. */
#define XTK_CH_CONTROL        0u   /* reliable ordered   */
#define XTK_CH_SNAPSHOT       1u   /* unreliable sequenced */
#define XTK_CH_INPUT          2u   /* unreliable sequenced */

/* Message types.  Values are frozen; unknown gameplay types are rejected. */
enum XtkMessageType {
    XTK_MSG_HELLO          = 0x01,
    XTK_MSG_WELCOME        = 0x02,
    XTK_MSG_REJECT         = 0x03,
    XTK_MSG_PING           = 0x04,
    XTK_MSG_PONG           = 0x05,

    XTK_MSG_CONTENT_CATALOG= 0x10,
    XTK_MSG_CONFIG_SELECT  = 0x11,
    XTK_MSG_CONFIG_STATUS  = 0x12,
    XTK_MSG_READY          = 0x13,
    XTK_MSG_START          = 0x14,
    XTK_MSG_EPOCH_BEGIN    = 0x15,
    XTK_MSG_RESULTS        = 0x16,
    XTK_MSG_RESET          = 0x17,
    XTK_MSG_QUIT           = 0x18,
    XTK_MSG_LOBBY_STATE    = 0x19,   /* v1.1: reliable session/roster status */
    XTK_MSG_START_REQUEST  = 0x1A,   /* v1.1: reliable admin start request   */

    XTK_MSG_EVENT          = 0x20,
    XTK_MSG_CMD_ACK        = 0x21,
    XTK_MSG_PROTOCOL_ERROR = 0x22,
    XTK_MSG_RELIABLE_CMD   = 0x23,

    XTK_MSG_VIEW_KEYFRAME  = 0x30,   /* v1.1: reliable initial-view barrier  */
    XTK_MSG_VIEW_SNAPSHOT  = 0x40,

    XTK_MSG_INPUT_BATCH    = 0x60
};

/* ---- Session states (LOBBY_STATE.session_state) --------------------- */

enum XtkSessionState {
    XTK_SESSION_LOBBY   = 0x00,
    XTK_SESSION_RUNNING = 0x01,
    XTK_SESSION_RESULTS = 0x02
};

/* ---- Commands (player input) ---------------------------------------- */

/* Wire op codes.  XTK_OP_* values are frozen protocol identifiers; they are
 * not the engine's IntentOp enum and must be mapped explicitly by the codec
 * and the (P2.7) admission adapter. */
enum XtkCommandOp {
    XTK_OP_NONE         = 0x00,
    XTK_OP_DRIVE_REL    = 0x01,  /* a in [-9, 9], integral            */
    XTK_OP_DRIVE_ABS    = 0x02,  /* a in [0, 9], integral             */
    XTK_OP_HEADING      = 0x03,  /* a = radians, finite               */
    XTK_OP_FIRE         = 0x04,  /* one-shot edge, no operands        */
    XTK_OP_WEAPON       = 0x05,  /* a = weapon 0..5, b in {0,1,2}     */
    XTK_OP_AIM_WORLD    = 0x06,  /* a = world x, b = world y, finite  */
    XTK_OP_AIM_TARGET   = 0x07,  /* TEST-ONLY until re-authorized     */
    XTK_OP_FIRE_HOLD    = 0x08,  /* a in {0,1}, held                  */
    XTK_OP_CHASE        = 0x09,  /* TEST-ONLY until re-authorized     */
    XTK_OP_STANDOFF     = 0x0A,  /* TEST-ONLY until re-authorized     */
    XTK_OP_HEARTBEAT    = 0x0F   /* idle keepalive, no operands       */
};

/* Ops the release network path accepts.  The rest are rejected on the wire
 * (test-only tick-indexed replay uses a separate controlled harness). */
#define XTK_OP_RELEASE_AUTHORIZED(op) \
    ((op) == XTK_OP_NONE        || (op) == XTK_OP_DRIVE_REL  || \
     (op) == XTK_OP_DRIVE_ABS   || (op) == XTK_OP_HEADING    || \
     (op) == XTK_OP_FIRE        || (op) == XTK_OP_WEAPON     || \
     (op) == XTK_OP_AIM_WORLD   || (op) == XTK_OP_FIRE_HOLD  || \
     (op) == XTK_OP_HEARTBEAT)

/* ---- Command acknowledgment status ---------------------------------- */

enum XtkAckStatus {
    XTK_ACK_ACCEPTED = 0x00,  /* queued for the assigned tick          */
    XTK_ACK_APPLIED  = 0x01,  /* consumed by an engine tick            */
    XTK_ACK_REJECTED = 0x02   /* never admitted; reason says why       */
};

/* ---- Events (channel 0) --------------------------------------------- */

enum XtkEventType {
    XTK_EV_DEATH        = 0x01,
    XTK_EV_RESPAWN      = 0x02,
    XTK_EV_SCORE        = 0x03,
    XTK_EV_MATCH_END    = 0x04,
    XTK_EV_EXPLOSION    = 0x05,
    XTK_EV_HIT          = 0x06
};

/* ---- Entity kinds --------------------------------------------------- */

enum XtkEntityKind {
    XTK_ENTITY_NONE     = 0x00,
    XTK_ENTITY_VEHICLE  = 0x01,
    XTK_ENTITY_BULLET   = 0x02,
    XTK_ENTITY_EXPLOSION= 0x03,
    XTK_ENTITY_LANDMARK = 0x04
};

/* ---- Content kinds -------------------------------------------------- */

enum XtkContentKind {
    XTK_CONTENT_MAZE    = 0x01,
    XTK_CONTENT_VEHICLE = 0x02,
    XTK_CONTENT_CONFIG  = 0x03,
    XTK_CONTENT_GEOMETRY= 0x04
};

/* ---- Error / rejection codes ---------------------------------------- */

enum XtkErrorCode {
    XTK_ERR_NONE             = 0x0000,
    XTK_ERR_BAD_MAGIC        = 0x0001,
    XTK_ERR_BAD_VERSION      = 0x0002,
    XTK_ERR_BAD_LENGTH       = 0x0003,
    XTK_ERR_BAD_CHANNEL      = 0x0004,
    XTK_ERR_BAD_TYPE         = 0x0005,
    XTK_ERR_MALFORMED        = 0x0006,
    XTK_ERR_VERSION_MISMATCH = 0x0010,
    XTK_ERR_CONTENT_MISMATCH = 0x0011,
    XTK_ERR_SESSION_FULL     = 0x0012,
    XTK_ERR_BAD_STATE        = 0x0013,
    XTK_ERR_BAD_CONTENT_ID   = 0x0014,
    XTK_ERR_NOT_ADMIN        = 0x0015,
    XTK_ERR_STALE_EPOCH      = 0x0016,
    XTK_ERR_STALE_SEQUENCE   = 0x0017,
    XTK_ERR_DUPLICATE        = 0x0018,
    XTK_ERR_FOREIGN_OWNER    = 0x0019,
    XTK_ERR_RANGE            = 0x001A,
    XTK_ERR_NONFINITE        = 0x001B,
    XTK_ERR_QUEUE_SATURATED  = 0x001C,
    XTK_ERR_TIMEOUT          = 0x001D,
    XTK_ERR_UNAUTHORIZED_OP  = 0x001E,

    /* v1.1 additions (P3.3). */
    XTK_ERR_STALE_LIFE       = 0x001F,  /* entity-life generation is stale  */
    XTK_ERR_DUPLICATE_NAME   = 0x0020,  /* lobby name already in use        */
    XTK_ERR_NOT_READY        = 0x0021,  /* start requested before all ready */
    XTK_ERR_STALE_CONFIG     = 0x0022   /* config revision has moved on     */
};

/* ======================================================================
 * Codec API (P2.3; extended to v1.1 by P3.3)
 *
 * The codec is a bounded, allocation-free serializer for the frozen v1.1
 * schema.  It uses only Protocol/ headers and never touches the engine,
 * SDL, ENet or the simulation.  All integers/floats are written explicitly
 * in network byte order (big-endian); no native struct is copied and the
 * host's byte order cannot change a single output octet.
 *
 * The codec emits and accepts exactly version 1.1.  A 1.0 envelope is
 * rejected (XTK_ERR_VERSION_MISMATCH) with no fallback; there is no
 * dual-version path (D3-A).
 * ====================================================================== */

/* ---- Primitive big-endian serialization ------------------------------ */

/* Finite test for an IEEE-754 binary64 value that needs no <math.h> and does
 * not depend on the FP exception mode.  NaN is the only value unequal to
 * itself; +-Inf fall outside the finite magnitude range. */
static inline int xtk_finite_f64(double v)
{
    return (v == v) && v <= 1.7976931348623157e308 &&
           v >= -1.7976931348623157e308;
}

/* Store writes exactly N octets and imposes no alignment requirement; Load
 * reads exactly N octets.  xtk_load_f64 returns 1 and writes *out for a
 * finite value, 0 for NaN/+-Inf. */
void     xtk_store_u16(uint8_t *p, uint16_t v);
void     xtk_store_u32(uint8_t *p, uint32_t v);
void     xtk_store_i32(uint8_t *p, int32_t v);
void     xtk_store_f64(uint8_t *p, double v);
uint16_t xtk_load_u16(const uint8_t *p);
uint32_t xtk_load_u32(const uint8_t *p);
int32_t  xtk_load_i32(const uint8_t *p);
int      xtk_load_f64(const uint8_t *p, double *out);

/* Canonical transport channel for a message type (0/1/2), or 0xFF if the
 * type is not part of protocol v1.1.  Used by encode to assign the channel
 * and by decode to reject a message delivered on the wrong channel. */
uint8_t  xtk_next_channel(uint8_t message_type);

/* ---- Decoded message shapes (host-native; never transmitted raw) ----- */

typedef struct {
    uint32_t magic;
    uint8_t  version_major;
    uint8_t  version_minor;
    uint8_t  message_type;
    uint8_t  channel;
    uint32_t payload_length;
    uint32_t session_id;
    uint32_t epoch;
    uint32_t message_sequence;
    int32_t  tick;
    uint32_t reserved;
} XtkEnvelope;

typedef struct { uint32_t nonce; } XtkPing;   /* PING and PONG */

typedef struct {
    uint32_t client_nonce;
    uint8_t  proto_major;
    uint8_t  proto_minor;
    uint16_t client_build;
    char     name[XTK_MAX_NAME_BYTES + 1];
    uint8_t  content_set_digest[XTK_SHA256_BYTES];
} XtkHello;

typedef struct {
    uint32_t server_nonce;
    uint8_t  role;
    uint8_t  tick_hz;          /* XTK_SERVER_TICK_HZ                       */
    uint8_t  max_players;      /* vehicle preset bound (XTK_MAX_PLAYERS)   */
    uint8_t  human_capacity;   /* admitted human controllers (<= 2)        */
    uint8_t  reserved;
} XtkWelcome;

typedef struct {
    uint16_t error_code;
    uint16_t detail_code;
    char     text[XTK_MAX_ERROR_TEXT_BYTES + 1];
} XtkReject;

typedef struct {
    uint16_t id;
    uint8_t  kind;
    char     name[XTK_MAX_NAME_BYTES + 1];
    uint8_t  source_sha256[XTK_SHA256_BYTES];
} XtkCatalogEntry;

typedef struct {
    uint8_t         content_set_digest[XTK_SHA256_BYTES];
    uint16_t        entry_count;
    XtkCatalogEntry entry[XTK_MAX_CATALOG_ENTRIES];
} XtkCatalog;

typedef struct {
    uint16_t maze_id;
    uint16_t vehicle_id;
    uint8_t  drone_count;      /* 0 .. XTK_MAX_DRONES */
    uint8_t  reserved;
    uint16_t reserved2;
    uint32_t reserved3;
} XtkConfigSelect;

typedef struct {
    uint16_t maze_id;
    uint16_t vehicle_id;
    uint8_t  drone_count;
    uint8_t  reserved;
    uint16_t status;           /* 0 accepted                              */
    uint16_t error_code;       /* XTK_ERR_*                               */
    uint16_t reserved2;
    uint32_t config_revision;  /* server config revision                  */
} XtkConfigStatus;

typedef struct {
    uint8_t  ready;            /* 0 unready, 1 ready against revision      */
    uint8_t  reserved;
    uint16_t reserved2;
    uint32_t config_revision;
} XtkReady;

typedef struct {
    uint32_t start_tick;
    uint16_t maze_id;
    uint16_t vehicle_id;       /* full-width content id (v1.1)            */
    uint8_t  drone_count;
    uint8_t  reserved;
    uint16_t reserved2;
    uint32_t config_revision;
} XtkStart;

typedef struct { uint32_t expected_config_revision; uint32_t reserved; }
    XtkStartRequest;

typedef struct { uint32_t epoch; uint32_t first_tick; } XtkEpochBegin;

typedef struct {
    uint8_t  slot;
    uint8_t  flags;            /* XTK_FLAG_DEPARTED                       */
    uint16_t reserved;
    uint32_t generation;       /* u32 entity-life generation (v1.1)       */
    uint32_t score;
    char     name[XTK_MAX_NAME_BYTES + 1];
} XtkResultEntry;

typedef struct {
    uint32_t       end_tick;
    uint8_t        result_kind;
    uint8_t        winner_slot;
    uint8_t        vehicle_count;
    uint8_t        reserved;
    XtkResultEntry entry[XTK_MAX_PLAYERS];
} XtkResults;

typedef struct { uint32_t prior_epoch; uint32_t reserved; } XtkReset;

typedef struct {
    uint32_t connection_id;    /* stable connection identity              */
    uint32_t controller_id;    /* stable controller identity              */
    uint8_t  slot;             /* bound vehicle slot; 0xFF = unbound      */
    uint8_t  ready;
    uint8_t  flags;            /* XTK_FLAG_DEPARTED                       */
    uint8_t  departure_reason; /* XTK_ERR_*; 0 = none                     */
    char     name[XTK_MAX_NAME_BYTES + 1];
} XtkRosterEntry;

typedef struct {
    uint32_t config_revision;
    uint8_t  session_state;    /* enum XtkSessionState                    */
    uint8_t  drone_count;
    uint16_t maze_id;
    uint16_t vehicle_id;
    uint8_t  self_index;       /* recipient roster index; 0xFF = none     */
    uint8_t  admin_index;      /* admin roster index; 0xFF = none         */
    uint8_t  human_count;      /* 0 .. XTK_LOBBY_ROSTER_MAX               */
    uint8_t  reserved;
    uint16_t reserved2;
    XtkRosterEntry entry[XTK_LOBBY_ROSTER_MAX];
} XtkLobbyStatus;

typedef struct {
    uint32_t event_sequence;   /* per recipient, per epoch, from 0        */
    uint32_t capture_tick;     /* engine tick the event occurred          */
    uint8_t  event_type;
    uint8_t  entity_kind;
    uint16_t entity_id;
    uint32_t entity_generation;
    uint32_t aux0;
    uint32_t aux1;
    char     text[XTK_MAX_NAME_BYTES + 1];
} XtkEvent;

typedef struct {
    uint32_t command_id;
    uint32_t assigned_tick;
    uint8_t  status;
    uint8_t  reason;
    uint16_t reserved;
} XtkAckEntry;

typedef struct {
    uint16_t     count;
    uint16_t     reserved;
    XtkAckEntry  entry[XTK_ACK_MAX_ENTRIES];
} XtkCmdAck;

typedef struct {
    uint16_t error_code;
    uint16_t detail;
    uint32_t offending_sequence;
    uint16_t offending_type;
    uint16_t reserved;
} XtkProtocolError;

typedef struct {
    uint32_t command_id;
    uint32_t tick_hint;
    uint32_t life_generation;  /* controlled entity-life generation (v1.1) */
    uint8_t  op;
    uint8_t  flags;            /* XTK_FLAG_EDGE / _HELD / _RELIABLE        */
    uint16_t reserved;
    double   a;
    double   b;
} XtkCommand;

typedef struct {
    uint32_t   last_observed_server_tick;
    uint16_t   command_count;
    uint16_t   reserved;
    XtkCommand command[XTK_MAX_INPUT_COMMANDS_PER_BATCH];
} XtkReliableCmd;

typedef struct {
    uint32_t   packet_sequence;
    uint32_t   last_observed_server_tick;
    uint16_t   command_count;
    uint16_t   reserved;
    XtkCommand command[XTK_MAX_INPUT_COMMANDS_PER_BATCH];
} XtkInputBatch;

typedef struct {
    uint8_t  slot;
    uint8_t  reserved;
    uint16_t lifecycle_flags;
    uint16_t armor[XTK_ARMOR_SIDES];
    uint16_t fuel;
    uint16_t heat;
    uint32_t score;
    uint16_t ammo[XTK_MAX_WEAPONS];
    uint8_t  weapon_enabled_bitmap;
    uint8_t  weapon_count;
    uint16_t reload[XTK_MAX_WEAPONS];
    uint32_t generation;       /* u32 entity-life generation (v1.1)       */
    uint8_t  reserved2[2];
} XtkSelfState;

typedef struct {
    uint8_t  slot;
    uint8_t  reserved;
    uint8_t  kind;
    uint8_t  flags;
    double   x;
    double   y;
    double   body_angle;
    double   turret_angle;
    uint16_t body_picture;
    uint16_t turret_picture;
    uint32_t generation;       /* u32 entity-life generation (v1.1)       */
    uint8_t  reserved2[4];
} XtkVehicleView;

typedef struct {
    uint16_t id;
    uint16_t flags;
    double   x;
    double   y;
    double   heading;
    uint8_t  owner_slot;
    uint8_t  kind;
    uint16_t reserved;
    uint32_t generation;       /* bullet life generation (v1.1)           */
    uint32_t owner_generation; /* u32 owner-life generation (v1.1)        */
} XtkBulletView;

typedef struct {
    uint16_t id;
    uint8_t  kind;
    uint8_t  flags;
    double   x;
    double   y;
    uint32_t generation;       /* explosion life generation (v1.1)        */
    uint8_t  reserved[4];
} XtkExplosionView;

typedef struct {
    uint8_t grid_x;
    uint8_t grid_y;
    uint8_t flags;
    uint8_t type;
} XtkMapCell;

typedef struct {
    uint8_t  kind;
    uint8_t  grid_x;           /* full grid key (v1.1)                    */
    uint8_t  grid_y;
    uint8_t  reserved;
    double   x;
    double   y;
    uint16_t id;               /* full-width landmark id (v1.1)           */
    uint16_t reserved2;
    uint8_t  reserved3[4];
} XtkLandmarkView;

typedef struct {
    double   x;
    double   y;
    uint8_t  age;
    uint8_t  classification;
    uint16_t reserved;
} XtkRadarBlip;

typedef struct {
    uint32_t snapshot_sequence;
    uint32_t server_tick;
    uint16_t flags;            /* XTK_FLAG_KEYFRAME                       */
    uint8_t  own_slot;
    uint8_t  reserved;
    uint32_t own_generation;   /* u32 entity-life generation (v1.1)       */
    uint16_t vehicle_count;
    uint16_t bullet_count;
    uint16_t explosion_count;
    uint16_t map_cell_count;
    uint16_t landmark_count;
    uint8_t  radar_blip_count;
    uint8_t  weapon_count;
    uint32_t map_revision;     /* recipient-local mapper metadata (D3-C)  */
    XtkSelfState      self;
    XtkVehicleView    vehicle[XTK_MAX_VEHICLES];
    XtkBulletView     bullet[XTK_MAX_BULLETS];
    XtkExplosionView  explosion[XTK_MAX_EXPLOSIONS];
    XtkMapCell        cell[XTK_MAX_MAP_CELLS];
    XtkLandmarkView   landmark[XTK_MAX_LANDMARKS];
    XtkRadarBlip      radar[XTK_MAX_RADAR_BLIPS];
} XtkViewSnapshot;

/* One decoded message: the envelope plus a typed body selected by
 * env.message_type.  The body is stored inline (no pointer, no allocation);
 * the fixed array bounds make the struct a little under 34 KiB. */
typedef union {
    XtkPing          ping;
    XtkHello         hello;
    XtkWelcome       welcome;
    XtkReject        reject;
    XtkCatalog       catalog;
    XtkConfigSelect  config_select;
    XtkConfigStatus  config_status;
    XtkReady         ready;
    XtkStart         start;
    XtkStartRequest  start_request;
    XtkEpochBegin    epoch_begin;
    XtkLobbyStatus    lobby_state;
    XtkResults       results;
    XtkReset         reset;
    XtkEvent         event;
    XtkCmdAck        cmd_ack;
    XtkProtocolError protocol_error;
    XtkReliableCmd   reliable_cmd;
    XtkInputBatch    input_batch;
    XtkViewSnapshot  view_snapshot;   /* also VIEW_KEYFRAME */
} XtkMessageBody;

typedef struct {
    XtkEnvelope    env;
    XtkMessageBody body;
} XtkMessage;

/* ---- Whole-message codec -------------------------------------------- */

/* Encode a complete message (envelope + payload) into out[0..cap).  The
 * envelope magic/version/channel/reserved are assigned by the codec; the
 * payload_length is computed.  msg->env.message_type selects the body.
 * Returns XTK_ERR_NONE and sets *out_len, or an XtkErrorCode.  On error the
 * buffer holds no usable message.  A caller encoding the largest authorized
 * view must provide at least XTK_MAX_MESSAGE_BYTES. */
int xtk_encode(uint8_t *out, size_t cap, const XtkMessage *msg, size_t *out_len);

/* Decode and fully validate one complete message of exactly len octets.
 * Rejects bad magic, wrong major/minor version, a payload length over bound
 * or inconsistent with len, a wrong/unknown channel, an unknown message
 * type, a non-zero reserved field, a negative tick, over-bound counts,
 * truncated or trailing octets, malformed string fields and non-finite f64
 * values.  Never allocates.  Returns XTK_ERR_NONE on success. */
int xtk_decode(const uint8_t *buf, size_t len, XtkMessage *msg);

#endif /* XTK_PROTOCOL_WIRE_H */
