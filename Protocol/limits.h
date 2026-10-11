/*
 * Protocol/limits.h -- Xtank wire protocol v1 bounds (P2.2).
 *
 * These are the frozen numeric limits of the version 1 wire protocol.  They
 * are deliberately self-contained: nothing in Protocol/ may include engine,
 * SDL or ENet headers, and these values are independent copies of any
 * simulation or transport constant of the same number.  A change here is a
 * protocol change and requires a new owner decision (D2-C).
 *
 * Authority: docs/PROTOCOL-v1.md, docs/PHASE2-PLAN.md section 3.
 */

#ifndef XTK_PROTOCOL_LIMITS_H
#define XTK_PROTOCOL_LIMITS_H

/* ---- Framing --------------------------------------------------------- */

/* Fixed envelope size; see docs/PROTOCOL-v1.md section 3. */
#define XTK_ENVELOPE_BYTES              32u

/* Largest complete application message (envelope + payload), 64 KiB. */
#define XTK_MAX_MESSAGE_BYTES           65536u

/* Largest payload an envelope may declare. */
#define XTK_MAX_PAYLOAD_BYTES           (XTK_MAX_MESSAGE_BYTES - XTK_ENVELOPE_BYTES)

/* ---- Transport service budgets -------------------------------------- */

/* Reliable ordered output queued per peer before the peer is aborted. */
#define XTK_MAX_RELIABLE_QUEUE_BYTES    262144u

/* Nonblocking ENet service work admitted per simulation tick. */
#define XTK_MAX_SERVICE_WORK_PER_TICK   64u

/* ---- Input admission ------------------------------------------------ */

/* Commands in one channel-2 INPUT_BATCH (after 2-tick redundancy). */
#define XTK_MAX_INPUT_COMMANDS_PER_BATCH 32u

/* Prior tick batches repeated in each input batch (freshness, no guarantee). */
#define XTK_INPUT_REDUNDANCY_TICKS       2u

/* A command older than this many ticks past its assigned tick is stale. */
#define XTK_CMD_EXPIRY_TICKS             15u

/* Client command-id replay window remembered per session. */
#define XTK_CMD_DEDUP_WINDOW             256u

/* Mirror of the engine intent queue bound (Src/Include/intent.h). */
#define XTK_INTENT_QUEUE_MAX             128u

/* Command entries in one CMD_ACK message. */
#define XTK_ACK_MAX_ENTRIES              32u

/* ---- Catalog / content identity ------------------------------------- */

#define XTK_MAX_CATALOG_ENTRIES          8u
#define XTK_MAX_NAME_BYTES               32u
#define XTK_MAX_ERROR_TEXT_BYTES         128u
#define XTK_SHA256_BYTES                 32u

/* ---- Snapshot entity bounds ----------------------------------------- */

/* Independent copies of the engine array bounds (Src/Include/tanklimits.h).
 * The wire encoder must never emit more than these; the decoder rejects any
 * count above them rather than allocating. */
#define XTK_MAX_VEHICLES                 16u
#define XTK_MAX_BULLETS                  511u
#define XTK_MAX_EXPLOSIONS               255u
#define XTK_MAP_GRID_WIDTH               30u
#define XTK_MAP_GRID_HEIGHT              30u
#define XTK_MAX_MAP_CELLS                (XTK_MAP_GRID_WIDTH * XTK_MAP_GRID_HEIGHT)
#define XTK_MAX_LANDMARKS                50u
#define XTK_MAX_RADAR_BLIPS              22u
#define XTK_MAX_WEAPONS                  6u
#define XTK_ARMOR_SIDES                  4u

/* ---- Session / client model ----------------------------------------- */

/* Snapshots retained for interpolation on the client. */
#define XTK_SNAPSHOT_RING                8u

/* Snapshot/event pending bounds applied before an epoch change. */
#define XTK_MAX_PENDING_EVENTS           256u

/* Approved solo-abort policy: no valid traffic for two seconds (D2-F). */
#define XTK_NEUTRAL_TIMEOUT_MS           2000U

/* Frozen presentation tick rate (D2-E). */
#define XTK_SERVER_TICK_HZ               15u

/* One human + one drone in the Phase 2 slice, up to the release maximum. */
#define XTK_MAX_PLAYERS                  4u

#endif /* XTK_PROTOCOL_LIMITS_H */
