/*
 * test_pogls_geopixel.c — Geopixel compression test suite
 *
 * Build:
 *   gcc -O2 -std=c11 -I. test_pogls_geopixel.c pogls_geopixel.c -o test_pogls_geopixel.exe
 *
 * Run:
 *   .\test_pogls_geopixel.exe
 */

#include "pogls_geopixel.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int pass = 0, fail = 0;

#define TEST(name, cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "  FAIL [%s] line %d\n", name, __LINE__); \
        fail++; \
    } else { \
        pass++; \
    } \
} while(0)

/* deterministic 32-bit PRNG (xorshift) */
static uint32_t xs32(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x; return x;
}

/* ═══════════════════════════════════════════════════════════════════════
   T1: FLAT block — 64 identical bytes → 2 bytes encode, decode matches
   ═══════════════════════════════════════════════════════════════════════ */
static void test_flat_block(void)
{
    uint8_t src[64];
    memset(src, 0x42, 64);

    uint8_t enc[128] = {0};
    uint32_t enc_sz = pogls_geopixel_encode_block(enc, sizeof(enc), src, 64);

    TEST("T1a flat encoded size = 2", enc_sz == 2);
    TEST("T1b flat tag = 0x00", enc[0] == POGLS_GEOPIXEL_FLAT);
    TEST("T1c flat value = 0x42", enc[1] == 0x42);

    uint8_t dec[64] = {0};
    uint32_t dec_sz = pogls_geopixel_decode_block(dec, sizeof(dec), enc, enc_sz);

    TEST("T1d flat decoded size = 64", dec_sz == 64);
    TEST("T1e flat roundtrip exact", memcmp(src, dec, 64) == 0);
}

/* ═══════════════════════════════════════════════════════════════════════
   T2: SMOOTH block — near-mean variation, intra-group noise → 10 bytes
   ═══════════════════════════════════════════════════════════════════════ */
static void test_smooth_block(void)
{
    uint8_t src[64];
    /* Small oscillation within each group — close to mean but not constant within group,
     * so GRADIENT won't match (groups aren't identical) */
    for (int i = 0; i < 64; i++)
        src[i] = (uint8_t)(100 + (i % 5));

    uint8_t enc[128] = {0};
    uint32_t enc_sz = pogls_geopixel_encode_block(enc, sizeof(enc), src, 64);

    TEST("T2a smooth encoded size = 10", enc_sz == 10);
    TEST("T2b smooth tag = 0x01", enc[0] == POGLS_GEOPIXEL_SMOOTH);

    uint8_t dec[64] = {0};
    uint32_t dec_sz = pogls_geopixel_decode_block(dec, sizeof(dec), enc, enc_sz);

    TEST("T2c smooth decoded size = 64", dec_sz == 64);
    /* Reconstruction is lossy within groups (all bytes in group get same value) */
    int max_err = 0;
    for (int i = 0; i < 64; i++) {
        int d = (int)dec[i] - (int)src[i];
        if (d < 0) d = -d;
        if (d > max_err) max_err = d;
    }
    TEST("T2d smooth max error ≤ 16", max_err <= 16);
}

/* ═══════════════════════════════════════════════════════════════════════
   T3: GRADIENT block — linear trend groups → 10 bytes
   ═══════════════════════════════════════════════════════════════════════ */
static void test_gradient_block(void)
{
    uint8_t src[64];
    /* groups: 50, 62, 74, 86, 98, 110, 122, 134 (+12 per group) */
    for (int g = 0; g < 8; g++)
        memset(src + g * 8, (uint8_t)(50 + g * 12), 8);

    uint8_t enc[128] = {0};
    uint32_t enc_sz = pogls_geopixel_encode_block(enc, sizeof(enc), src, 64);

    TEST("T3a gradient encoded size = 10", enc_sz == 10);
    TEST("T3b gradient tag = 0x02", enc[0] == POGLS_GEOPIXEL_GRADIENT);

    uint8_t dec[64] = {0};
    uint32_t dec_sz = pogls_geopixel_decode_block(dec, sizeof(dec), enc, enc_sz);

    TEST("T3c gradient decoded size = 64", dec_sz == 64);
    TEST("T3d gradient roundtrip exact", memcmp(src, dec, 64) == 0);
}

/* ═══════════════════════════════════════════════════════════════════════
   T4: EDGE block — random data → 65 bytes, roundtrip exact
   ═══════════════════════════════════════════════════════════════════════ */
static void test_edge_block(void)
{
    uint8_t src[64];
    uint32_t seed = 0xDEADBEEF;
    for (int i = 0; i < 64; i++) src[i] = (uint8_t)xs32(&seed);

    uint8_t enc[128] = {0};
    uint32_t enc_sz = pogls_geopixel_encode_block(enc, sizeof(enc), src, 64);

    TEST("T4a edge encoded size = 65", enc_sz == 65);
    TEST("T4b edge tag = 0x03", enc[0] == POGLS_GEOPIXEL_EDGE);

    uint8_t dec[64] = {0};
    uint32_t dec_sz = pogls_geopixel_decode_block(dec, sizeof(dec), enc, enc_sz);

    TEST("T4c edge decoded size = 64", dec_sz == 64);
    TEST("T4d edge roundtrip exact", memcmp(src, dec, 64) == 0);
}

/* ═══════════════════════════════════════════════════════════════════════
   T5: Full encode/decode roundtrip — mixed data (3 blocks: FLAT+FLAT+EDGE)
   ═══════════════════════════════════════════════════════════════════════ */
static void test_full_roundtrip(void)
{
    uint8_t src[192]; /* 3 blocks of 64 */
    /* block 0: FLAT */
    memset(src, 0xAB, 64);
    /* block 1: FLAT */
    memset(src + 64, 0xCD, 64);
    /* block 2: EDGE (random) */
    uint32_t seed = 0xCAFEBABE;
    for (int i = 0; i < 64; i++) src[128 + i] = (uint8_t)xs32(&seed);

    /* Encode */
    uint8_t enc[512] = {0};
    uint32_t enc_sz = pogls_geopixel_encode(enc, sizeof(enc), src, 192);
    TEST("T5a full encode success", enc_sz > 0);

    /* Decode */
    uint8_t dec[256] = {0};
    uint32_t dec_sz = pogls_geopixel_decode(dec, sizeof(dec), enc, enc_sz);
    TEST("T5b full decode success", dec_sz == 192);
    TEST("T5c full roundtrip exact", memcmp(src, dec, 192) == 0);
}

/* ═══════════════════════════════════════════════════════════════════════
   T6: Constant-group roundtrip — verify SMOOTH + GRADIENT both work
   ═══════════════════════════════════════════════════════════════════════ */
static void test_constant_group_roundtrip(void)
{
    /* Each group of 8 is constant; some are GRADIENT (linear), some are SMOOTH (non-linear) */
    /* Block that classifies as SMOOTH (non-linear group values, but within max_diff ≤ 16) */
    uint8_t src1[64];
    for (int g = 0; g < 8; g++)
        memset(src1 + g * 8, (uint8_t)(100 + g * 3 + (g % 2)), 8);
    /* values: 100, 104, 107, 111, 114, 118, 121, 125 — not perfectly linear */
    uint8_t enc1[128] = {0};
    uint32_t e1 = pogls_geopixel_encode_block(enc1, sizeof(enc1), src1, 64);
    uint8_t dec1[64] = {0};
    uint32_t d1 = pogls_geopixel_decode_block(dec1, sizeof(dec1), enc1, e1);
    TEST("T6a smooth-const encode size > 0", e1 > 0 && e1 < 64);
    TEST("T6b smooth-const decode size = 64", d1 == 64);
    TEST("T6c smooth-const roundtrip exact", memcmp(src1, dec1, 64) == 0);

    /* Block that classifies as GRADIENT (linear group values) */
    uint8_t src2[64];
    for (int g = 0; g < 8; g++)
        memset(src2 + g * 8, (uint8_t)(50 + g * 12), 8);
    uint8_t enc2[128] = {0};
    uint32_t e2 = pogls_geopixel_encode_block(enc2, sizeof(enc2), src2, 64);
    uint8_t dec2[64] = {0};
    uint32_t d2 = pogls_geopixel_decode_block(dec2, sizeof(dec2), enc2, e2);
    TEST("T6d gradient-const encoded size = 10", e2 == 10);
    TEST("T6e gradient-const tag = 0x02", enc2[0] == POGLS_GEOPIXEL_GRADIENT);
    TEST("T6f gradient-const decode size = 64", d2 == 64);
    TEST("T6g gradient-const roundtrip exact", memcmp(src2, dec2, 64) == 0);
}

/* ═══════════════════════════════════════════════════════════════════════
   T8: Hilbert XY→D→XY roundtrip for all 8x8 positions
   ═══════════════════════════════════════════════════════════════════════ */
static void test_hilbert_roundtrip(void)
{
    int ok = 1;
    for (uint32_t y = 0; y < 8 && ok; y++) {
        for (uint32_t x = 0; x < 8; x++) {
            uint32_t d  = pogls_geopixel_hilbert_xy_to_d(x, y, 3);
            uint32_t x2, y2;
            pogls_geopixel_hilbert_d_to_xy(d, 3, &x2, &y2);
            if (x != x2 || y != y2) { ok = 0; break; }
        }
    }
    TEST("T7 hilbert xy->d->xy roundtrip for 8x8", ok);

    /* Verify Hilbert indices are a permutation of 0..63 */
    uint32_t seen[64] = {0};
    for (uint32_t y = 0; y < 8; y++) {
        for (uint32_t x = 0; x < 8; x++) {
            uint32_t d = pogls_geopixel_hilbert_xy_to_d(x, y, 3);
            if (d < 64) seen[d] = 1;
        }
    }
    int all_seen = 1;
    for (uint32_t i = 0; i < 64; i++) {
        if (!seen[i]) { all_seen = 0; break; }
    }
    TEST("T7b hilbert covers 0..63 exactly once", all_seen);
}

/* ═══════════════════════════════════════════════════════════════════════
   T8: Session init + feed
   ═══════════════════════════════════════════════════════════════════════ */
static void test_session(void)
{
    PoglsGeopixelSession s;
    int r = pogls_geopixel_session_init(&s, 10);
    TEST("T8a session init ok", r == 0);
    TEST("T8b session frame_seq=0", s.frame_seq == 0);
    TEST("T8c session block_count=10", s.block_count == 10);

    /* Feed one block */
    uint8_t src[64];
    memset(src, 0x42, 64);
    uint8_t enc[128] = {0};
    uint32_t written = pogls_geopixel_session_feed(&s, enc, sizeof(enc), src, 64);
    TEST("T8d session feed wrote >0", written > 0);
    TEST("T8e session frame_seq=1", s.frame_seq == 1);
    TEST("T8f session total_bytes=64", s.total_bytes == 64);
    TEST("T8g session compressed_bytes==written", s.compressed_bytes == written);

    /* Feed a second block */
    memset(src, 0x7F, 64);
    written = pogls_geopixel_session_feed(&s, enc, sizeof(enc), src, 64);
    TEST("T8h session feed2 wrote >0", written > 0);
    TEST("T8i session frame_seq=2", s.frame_seq == 2);
    TEST("T8j session total_bytes=128", s.total_bytes == 128);

    /* Stats should print without crash */
    pogls_geopixel_session_stats(&s);
}

/* ═══════════════════════════════════════════════════════════════════════
   MAIN
   ═══════════════════════════════════════════════════════════════════════ */

int main(void)
{
    fprintf(stdout, "[geopixel] test suite\n"
                    "────────────────────────────────────────\n");

    test_flat_block();
    test_smooth_block();
    test_gradient_block();
    test_edge_block();
    test_full_roundtrip();
    test_constant_group_roundtrip();
    test_hilbert_roundtrip();
    test_session();

    fprintf(stdout, "────────────────────────────────────────\n"
                    "[geopixel] %d/%d passed, %d failed\n",
                    pass, pass + fail, fail);
    return fail ? 1 : 0;
}
