/*
 * Protocol/wire.h -- Xtank wire protocol v1 identifiers and enumerations (P2.2).
 *
 * Frozen schema constants only.  This header includes no engine, SDL or ENet
 * header, copies no native struct and defines no wire I/O: the codec lands in
 * P2.3.  Every value that appears on the wire is fixed-width and is encoded
 * explicitly in network byte order by the codec; the C types here are only
 * convenient labels and are never memcpy'd onto the wire.
 *
 * Authority: docs/PROTOCOL-v1.md; docs/PHASE2-DECISIONS.md D2-C.
 */

#ifndef XTK_PROTOCOL_WIRE_H
#define XTK_PROTOCOL_WIRE_H

#include <stdint.h>
#include "limits.h"

/* ---- Envelope -------------------------------------------------------- */

/* "XTK1" as four octets; the low octet also tags the wire generation. */
#define XTK_WIRE_MAGIC        UINT32_C(0x58544B31)

#define XTK_PROTOCOL_MAJOR    1u
#define XTK_PROTOCOL_MINOR    0u

/* Envelope flag bits (envelope byte 7 is the channel; flags are per message
 * and carried in the first payload byte where a message defines them). */
#define XTK_FLAG_NONE         UINT8_C(0x00)
#define XTK_FLAG_KEYFRAME     UINT8_C(0x01)   /* VIEW_SNAPSHOT: full barrier */
#define XTK_FLAG_EDGE         UINT8_C(0x01)   /* command: one-shot edge       */
#define XTK_FLAG_HELD         UINT8_C(0x02)   /* command: held control        */
#define XTK_FLAG_RELIABLE     UINT8_C(0x04)   /* command: arrived on ch0      */

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

    XTK_MSG_EVENT          = 0x20,
    XTK_MSG_CMD_ACK        = 0x21,
    XTK_MSG_PROTOCOL_ERROR = 0x22,
    XTK_MSG_RELIABLE_CMD   = 0x23,

    XTK_MSG_VIEW_SNAPSHOT  = 0x40,

    XTK_MSG_INPUT_BATCH    = 0x60
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
    XTK_ERR_UNAUTHORIZED_OP  = 0x001E
};

#endif /* XTK_PROTOCOL_WIRE_H */
