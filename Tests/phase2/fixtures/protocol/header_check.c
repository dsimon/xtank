/*
 * header_check.c -- proves Protocol/ compiles and links standalone (P2.2).
 *
 * It includes only the protocol headers (no engine, SDL or ENet header) and
 * prints the frozen identity digests so the harness can compare them against
 * the tree's Phase 1 goldens.  Build:
 *
 *   gcc -std=c11 -Wall -Wextra -Werror -I Protocol \
 *       Tests/phase2/fixtures/protocol/header_check.c \
 *       Protocol/content-manifest.c -o <outside-checkout>
 */

#include <stdio.h>
#include <stddef.h>
#include "wire.h"
#include "limits.h"
#include "content-manifest.h"

static void print_hex(const char *tag, const uint8_t *p, size_t n)
{
    size_t i;
    printf("%s ", tag);
    for (i = 0; i < n; ++i) {
        printf("%02x", (unsigned)p[i]);
    }
    printf("\n");
}

int main(void)
{
    const XtkContentEntry *e;

    if (XTK_ENVELOPE_BYTES != 32u) {
        fprintf(stderr, "unexpected envelope size\n");
        return 1;
    }
    if (XTK_MAX_MESSAGE_BYTES > 65536u) {
        fprintf(stderr, "message bound too large\n");
        return 1;
    }
    if (xtk_content_entry_count != 3u) {
        fprintf(stderr, "unexpected catalog size\n");
        return 1;
    }
    e = xtk_content_lookup(XTK_CONTENT_ID_COMBAT1);
    if (e == NULL || e->kind != XTK_CONTENT_MAZE) {
        fprintf(stderr, "combat1 lookup failed\n");
        return 1;
    }
    if (xtk_content_lookup((uint16_t)0x9999) != NULL) {
        fprintf(stderr, "unknown id was accepted\n");
        return 1;
    }

    printf("HEADER-OK %zu\n", xtk_content_entry_count);
    print_hex("CONTENT-SET", xtk_content_set_digest, XTK_SHA256_BYTES);
    print_hex("PRESET", xtk_release_preset_sha256, XTK_SHA256_BYTES);
    print_hex("GEOM", xtk_geometry_sha256, XTK_SHA256_BYTES);
    print_hex("COMBAT1", xtk_content_entries[0].sha256, XTK_SHA256_BYTES);
    print_hex("ARENA", xtk_content_entries[1].sha256, XTK_SHA256_BYTES);
    print_hex("VANGUARD", xtk_content_entries[2].sha256, XTK_SHA256_BYTES);
    return 0;
}
