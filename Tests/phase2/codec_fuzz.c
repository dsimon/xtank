/*
 * codec_fuzz.c -- deterministic decoder fuzz harness for the P2.3 codec.
 *
 * Feeds xtk_decode() a reproducible stream of mutated valid messages, raw
 * random octets and truncations for a fixed wall-clock duration, then exits
 * 0.  It asserts nothing about individual decodes -- the property under test
 * is memory safety and bounded work, checked by building this driver with
 * AddressSanitizer + UndefinedBehaviorSanitizer (Linux) and by the absence of
 * any allocation in decode.o.  The seed and duration are printed so a run is
 * reproducible.
 *
 * Usage: codec_fuzz <seed> <seconds> <fixture-dir>
 *
 * Authority: docs/PHASE2-PLAN.md P2.3.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "wire.h"
#include "limits.h"

#define MAXBUF   XTK_MAX_MESSAGE_BYTES
#define MAXCORP  16

static uint64_t g_rng;

static uint64_t rng_next(void)
{
    /* xorshift64* : deterministic across hosts. */
    uint64_t x = g_rng;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    g_rng = x;
    return x * UINT64_C(0x2545F4914F6CDD1D);
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

static const char *CORPUS_NAMES[] = {
    "hello.hex", "welcome.hex", "reject_version.hex", "reject_content.hex",
    "catalog.hex", "config_select.hex", "start.hex", "epoch_begin.hex",
    "results.hex", "event.hex", "cmd_ack.hex", "reliable_cmd.hex",
    "protocol_error.hex", "input_batch.hex", "view_snapshot.hex"
};
#define NCORP ((int)(sizeof(CORPUS_NAMES) / sizeof(CORPUS_NAMES[0])))

static uint8_t g_corpus[MAXCORP][MAXBUF];
static size_t  g_corpus_len[MAXCORP];
static int     g_corpus_n;
static uint8_t g_buf[MAXBUF];
static XtkMessage g_msg;

static void mutate(uint8_t *buf, size_t *len)
{
    int mutations = 1 + (int)(rng_next() % 8);
    size_t i;
    for (i = 0; i < (size_t)mutations; ++i) {
        size_t pos;
        if (*len == 0) break;
        pos = (size_t)(rng_next() % *len);
        switch (rng_next() % 4) {
        case 0: buf[pos] ^= (uint8_t)(1u << (rng_next() % 8)); break;
        case 1: buf[pos] = (uint8_t)rng_next(); break;
        case 2: buf[pos] = 0xFF; break;
        default: buf[pos] = 0x00; break;
        }
    }
    /* sometimes truncate */
    if ((rng_next() % 5) == 0 && *len > 0) {
        *len = (size_t)(rng_next() % (*len + 1));
    }
    /* sometimes leave the declared length unchanged but cut the buffer in a
     * way that exercises the envelope-vs-actual-length check */
    if ((rng_next() % 7) == 0 && *len > XTK_ENVELOPE_BYTES) {
        size_t n = *len - XTK_ENVELOPE_BYTES;
        if (n <= XTK_MAX_PAYLOAD_BYTES) {
            xtk_store_u32(buf + 8, (uint32_t)n);
        }
    }
}

int main(int argc, char **argv)
{
    uint64_t seed = (argc > 1) ? strtoull(argv[1], NULL, 0)
                               : UINT64_C(0xC0FFEE);
    long seconds = (argc > 2) ? strtol(argv[2], NULL, 0) : 20;
    const char *dir = (argc > 3) ? argv[3] : ".";
    time_t start, now;
    unsigned long long iterations = 0;
    unsigned long long accepted = 0;
    int i;

    for (i = 0; i < NCORP; ++i) {
        if (load_hex(dir, CORPUS_NAMES[i], g_corpus[g_corpus_n],
                     sizeof(g_corpus[0]), &g_corpus_len[g_corpus_n])) {
            ++g_corpus_n;
        }
    }
    printf("fuzz: seed=%llu seconds=%ld corpus=%d\n",
           (unsigned long long)seed, seconds, g_corpus_n);
    if (g_corpus_n == 0) {
        printf("FUZZ SKIP: no corpus loaded from %s\n", dir);
        return 0;
    }

    g_rng = seed ? seed : UINT64_C(0xC0FFEE);
    start = time(NULL);

    for (;;) {
        int it;
        for (it = 0; it < 4096; ++it) {
            size_t len;
            int src = (int)(rng_next() % (uint64_t)(g_corpus_n + 1));
            if (src == g_corpus_n) {
                /* pure random octets */
                len = (size_t)(rng_next() % 256);
                {
                    size_t k;
                    for (k = 0; k < len; ++k) {
                        g_buf[k] = (uint8_t)rng_next();
                    }
                }
            } else {
                len = g_corpus_len[src];
                memcpy(g_buf, g_corpus[src], len);
                mutate(g_buf, &len);
            }
            if (xtk_decode(g_buf, len, &g_msg) == XTK_ERR_NONE) {
                ++accepted;
            }
            ++iterations;
        }
        now = time(NULL);
        if (seconds <= 0) {
            if (iterations >= 200000) break;
        } else if ((long)(now - start) >= seconds) {
            break;
        }
    }

    printf("FUZZ PASS seed=%llu seconds=%ld iterations=%llu accepted=%llu\n",
           (unsigned long long)seed, seconds, iterations, accepted);
    return 0;
}
