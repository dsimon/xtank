/*
 * Protocol/content-manifest.h -- Xtank protocol v1 content identity (P2.2).
 *
 * The release/MVP content set is exactly two supplied mazes (combat1, Arena)
 * and one supplied vehicle (Vanguard, Marauder body 6), plus the approved
 * release-preset configuration and the exported geometry table.  Each is
 * identified by a stable, explicit content ID and the SHA-256 of its exact
 * bytes.  IDs are stable protocol identifiers, never list indices.
 *
 * Hashes establish version agreement, not secrecy: the supplied maps are
 * historically public.  The client ships visual resources and this manifest,
 * not raw .m files; a content mismatch fails before start and never
 * auto-downloads.
 *
 * This header includes no engine, SDL or ENet header.
 *
 * Authority: docs/PROTOCOL-v1.md section 10; docs/PHASE2-DECISIONS.md D2-C;
 * Tests/phase1/content.golden.sha256; Tests/phase1/geometry.golden.sha256.
 */

#ifndef XTK_PROTOCOL_CONTENT_MANIFEST_H
#define XTK_PROTOCOL_CONTENT_MANIFEST_H

#include <stdint.h>
#include <stddef.h>
#include "wire.h"
#include "limits.h"

/* Stable content IDs.  The high byte is the kind. */
#define XTK_CONTENT_ID_COMBAT1   ((uint16_t)0x0101)
#define XTK_CONTENT_ID_ARENA     ((uint16_t)0x0102)
#define XTK_CONTENT_ID_VANGUARD  ((uint16_t)0x0201)

typedef struct {
    uint16_t    id;
    uint8_t     kind;                       /* enum XtkContentKind          */
    const char *name;                        /* stable name, not an index    */
    uint8_t     sha256[XTK_SHA256_BYTES];    /* SHA-256 of the exact bytes   */
} XtkContentEntry;

/* The catalogue entries (mazes and vehicle). */
extern const XtkContentEntry xtk_content_entries[];
extern const size_t          xtk_content_entry_count;

/* SHA-256 of the canonical release-preset bytes
 * (Tests/phase2/fixtures/protocol/release-preset.txt). */
extern const uint8_t xtk_release_preset_sha256[XTK_SHA256_BYTES];

/* SHA-256 of the frozen geometry table (Tests/phase1/geometry.golden.sha256). */
extern const uint8_t xtk_geometry_sha256[XTK_SHA256_BYTES];

/* SHA-256 over the canonical content-set text:
 *
 *   XTK-CONTENT-SET-v1\n
 *   maze combat1 <hex>\n
 *   maze Arena <hex>\n
 *   vehicle Vanguard <hex>\n
 *   config release <hex>\n
 *   geometry table <hex>\n
 *
 * with lower-case hex digests.  This is the digest sent in HELLO and in the
 * CONTENT_CATALOG header; equality is the version-agreement test. */
extern const uint8_t xtk_content_set_digest[XTK_SHA256_BYTES];

/* Look up an entry by stable ID; NULL if the ID is not in the allowlist. */
const XtkContentEntry *xtk_content_lookup(uint16_t id);

#endif /* XTK_PROTOCOL_CONTENT_MANIFEST_H */
