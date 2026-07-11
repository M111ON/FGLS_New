/*
 * test_pogls_bermuda.c — Test suite for Bermuda Geometry Router Library
 *
 * Build:
 *   gcc -O2 -std=c11 -I. test_pogls_bermuda.c pogls_bermuda.c -o test_pogls_bermuda.exe
 */

#include "pogls_bermuda.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int n_pass = 0;
static int n_fail = 0;

#define TEST(name, cond) do { \
    if (!(cond)) { \
        printf("  FAIL [%s] (line %d)\n", name, __LINE__); \
        n_fail++; \
    } else { \
        printf("  PASS [%s]\n", name); \
        n_pass++; \
    } \
} while(0)

/* ═══════════════════════════════════════════════════════════════════
   Test: Stride-37 Routing
   ═══════════════════════════════════════════════════════════════════ */

static void test_route_basic(void)
{
    printf("\n=== Route: Basic ===\n");

    /* Route from 0, face 0, stride 1 — should give a valid address */
    uint32_t r = pogls_bermuda_route(0, 0, 1);
    TEST("route(0,0,1) < MAX_ADDR", r < POGLS_BERMUDA_MAX_ADDR);

    /* Route from same point with different faces gives different results */
    uint32_t r0 = pogls_bermuda_route(100, 0, 2);
    uint32_t r1 = pogls_bermuda_route(100, 1, 2);
    TEST("different faces -> different routes", r0 != r1);

    /* Route from different sources with same face gives different results */
    uint32_t r2 = pogls_bermuda_route(0, 3, 1);
    uint32_t r3 = pogls_bermuda_route(500, 3, 1);
    TEST("different sources -> different routes", r2 != r3);

    /* All strides produce valid addresses */
    for (uint32_t s = 1; s <= 4; s++) {
        uint32_t r4 = pogls_bermuda_route(42, 5, s);
        uint32_t max_slots = (uint32_t)512 << (s - 1);
        TEST("valid stride result", r4 < max_slots);
    }

    /* All 12 faces produce valid addresses */
    for (uint32_t f = 0; f < 12; f++) {
        uint32_t r5 = pogls_bermuda_route(42, f, 2);
        TEST("all faces valid", r5 < 1024);
    }
}

static void test_route_deterministic(void)
{
    printf("\n=== Route: Deterministic ===\n");

    /* Same inputs -> same outputs */
    for (int i = 0; i < 100; i++) {
        uint32_t from   = (uint32_t)(i * 73) % POGLS_BERMUDA_MAX_ADDR;
        uint32_t face   = (uint32_t)(i * 7) % 12;
        uint32_t stride = (uint32_t)(i % 4) + 1;

        uint32_t a = pogls_bermuda_route(from, face, stride);
        uint32_t b = pogls_bermuda_route(from, face, stride);
        TEST("deterministic routing", a == b);
    }
}

static void test_route_edge(void)
{
    printf("\n=== Route: Edge Cases ===\n");

    /* Boundary addresses */
    TEST("route from 0", pogls_bermuda_route(0, 0, 1) < 512);
    TEST("route from MAX-1", pogls_bermuda_route(POGLS_BERMUDA_MAX_ADDR - 1, 0, 1) < 512);
    TEST("route from MAX", pogls_bermuda_route(POGLS_BERMUDA_MAX_ADDR, 0, 1) < 512);

    /* Edge face values */
    TEST("face 11 valid", pogls_bermuda_route(100, 11, 2) < 1024);

    /* Face wrap: 12 -> 0 */
    uint32_t f0  = pogls_bermuda_route(100, 0, 2);
    uint32_t f12 = pogls_bermuda_route(100, 12, 2);
    TEST("face wraps mod 12", f0 == f12);

    /* Face wrap: 13 -> 1 */
    uint32_t f1  = pogls_bermuda_route(100, 1, 2);
    uint32_t f13 = pogls_bermuda_route(100, 13, 2);
    TEST("face 13 == face 1", f1 == f13);
}

/* ═══════════════════════════════════════════════════════════════════
   Test: Face Scores
   ═══════════════════════════════════════════════════════════════════ */

static void test_face_scores(void)
{
    printf("\n=== Face Scores ===\n");

    float scores[12];
    pogls_bermuda_face_scores(0, scores);

    /* Scores should sum to ~1.0 */
    float sum = 0;
    for (int i = 0; i < 12; i++) sum += scores[i];
    TEST("scores sum to 1.0", sum > 0.99f && sum < 1.01f);

    /* All scores should be non-negative */
    int all_nonneg = 1;
    for (int i = 0; i < 12; i++)
        if (scores[i] < 0) all_nonneg = 0;
    TEST("all scores non-negative", all_nonneg);

    /* Different bases produce different score distributions */
    float scores2[12];
    pogls_bermuda_face_scores(1234, scores2);
    int different = 0;
    for (int i = 0; i < 12; i++) {
        if (scores[i] != scores2[i]) { different = 1; break; }
    }
    TEST("different base -> different scores", different);
}

/* ═══════════════════════════════════════════════════════════════════
   Test: Diamond Shell Compress/Decompress
   ═══════════════════════════════════════════════════════════════════ */

static void test_diamond_allzero(void)
{
    printf("\n=== Diamond Shell: All-Zero ===\n");

    uint8_t src[64] = {0};
    uint8_t enc[128];
    uint8_t dec[64];

    uint32_t enc_sz = pogls_bermuda_diamond_compress(enc, sizeof(enc), src, 64);
    TEST("all-zero compress returns 2 (FLAT)", enc_sz == 2);
    TEST("all-zero flag is FLAT", enc[0] == POGLS_BERMUDA_FLAG_FLAT);

    uint32_t dec_sz = pogls_bermuda_diamond_decompress(dec, sizeof(dec), enc, enc_sz);
    TEST("all-zero decompress size", dec_sz == 64);
    TEST("all-zero roundtrip lossless", memcmp(src, dec, 64) == 0);
}

static void test_diamond_pattern(void)
{
    printf("\n=== Diamond Shell: Pattern ===\n");

    uint8_t src[64];
    for (int i = 0; i < 64; i++)
        src[i] = (uint8_t)(i * 17 + 31);

    uint8_t enc[256];
    uint8_t dec[64];

    uint32_t enc_sz = pogls_bermuda_diamond_compress(enc, sizeof(enc), src, 64);
    TEST("pattern compress returns 66 (DENSE)", enc_sz == 66);
    TEST("pattern flag is DENSE", enc[0] == POGLS_BERMUDA_FLAG_DENSE);

    uint32_t dec_sz = pogls_bermuda_diamond_decompress(dec, sizeof(dec), enc, enc_sz);
    TEST("pattern decompress size", dec_sz == 64);
    TEST("pattern roundtrip lossless", memcmp(src, dec, 64) == 0);
}

static void test_diamond_multi(void)
{
    printf("\n=== Diamond Shell: Multi-chunk ===\n");

    uint8_t src[192]; /* 3 chunks */
    for (int i = 0; i < 192; i++)
        src[i] = (uint8_t)(i * 13);

    uint8_t enc[512];
    uint8_t dec[192];

    uint32_t enc_sz = pogls_bermuda_diamond_compress(enc, sizeof(enc), src, 192);
    TEST("multi-chunk compress non-zero", enc_sz > 0);

    uint32_t dec_sz = pogls_bermuda_diamond_decompress(dec, sizeof(dec), enc, enc_sz);
    TEST("multi-chunk decompress size", dec_sz == 192);
    TEST("multi-chunk roundtrip lossless", memcmp(src, dec, 192) == 0);
}

static void test_diamond_mixed(void)
{
    printf("\n=== Diamond Shell: Mixed Zero/Nonzero ===\n");

    uint8_t src[128];
    memset(src, 0, 64);      /* first chunk: zero */
    for (int i = 0; i < 64; i++)
        src[64 + i] = (uint8_t)i; /* second chunk: pattern */

    uint8_t enc[256];
    uint8_t dec[128];

    uint32_t enc_sz = pogls_bermuda_diamond_compress(enc, sizeof(enc), src, 128);
    TEST("mixed compress non-zero", enc_sz > 0);

    uint32_t dec_sz = pogls_bermuda_diamond_decompress(dec, sizeof(dec), enc, enc_sz);
    TEST("mixed decompress size", dec_sz == 128);
    TEST("mixed roundtrip lossless", memcmp(src, dec, 128) == 0);
}

/* ═══════════════════════════════════════════════════════════════════
   Test: RLE Compress/Decompress
   ═══════════════════════════════════════════════════════════════════ */

static void test_rle_uniform(void)
{
    printf("\n=== RLE: Uniform ===\n");

    uint8_t src[100];
    memset(src, 0xAB, 100);

    uint8_t enc[256];
    uint8_t dec[100];

    uint32_t enc_sz = pogls_bermuda_rle_compress(enc, sizeof(enc), src, 100);
    TEST("uniform rle compress", enc_sz > 0 && enc_sz < 100);

    uint32_t dec_sz = pogls_bermuda_rle_decompress(dec, sizeof(dec), enc, enc_sz);
    TEST("uniform rle decompress size", dec_sz == 100);
    TEST("uniform rle roundtrip", memcmp(src, dec, 100) == 0);
}

static void test_rle_random(void)
{
    printf("\n=== RLE: Random ===\n");

    uint8_t src[200];
    for (int i = 0; i < 200; i++)
        src[i] = (uint8_t)(i * 31 + 17);

    uint8_t enc[512];
    uint8_t dec[200];

    uint32_t enc_sz = pogls_bermuda_rle_compress(enc, sizeof(enc), src, 200);
    TEST("random rle compress", enc_sz > 0);

    uint32_t dec_sz = pogls_bermuda_rle_decompress(dec, sizeof(dec), enc, enc_sz);
    TEST("random rle decompress size", dec_sz == 200);
    TEST("random rle roundtrip", memcmp(src, dec, 200) == 0);
}

static void test_rle_mixed(void)
{
    printf("\n=== RLE: Mixed Runs and Random ===\n");

    uint8_t src[256];
    size_t pos = 0;
    /* Run of 50 zeros */
    memset(src + pos, 0, 50); pos += 50;
    /* 20 varied bytes */
    for (int i = 0; i < 20; i++) src[pos++] = (uint8_t)(i * 7);
    /* Run of 80 0xFF */
    memset(src + pos, 0xFF, 80); pos += 80;
    /* 30 varied bytes */
    for (int i = 0; i < 30; i++) src[pos++] = (uint8_t)(i * 13 + 5);
    /* Run of 76 0x42 */
    memset(src + pos, 0x42, 76); pos += 76;

    uint8_t enc[512];
    uint8_t dec[256];

    uint32_t enc_sz = pogls_bermuda_rle_compress(enc, sizeof(enc), src, pos);
    TEST("mixed rle compress", enc_sz > 0);

    uint32_t dec_sz = pogls_bermuda_rle_decompress(dec, sizeof(dec), enc, enc_sz);
    TEST("mixed rle decompress size", dec_sz == (uint32_t)pos);
    TEST("mixed rle roundtrip", memcmp(src, dec, pos) == 0);
}

static void test_rle_edge(void)
{
    printf("\n=== RLE: Edge Cases ===\n");

    /* Single byte */
    uint8_t src1[1] = {0x42};
    uint8_t enc1[16];
    uint8_t dec1[1];
    uint32_t es1 = pogls_bermuda_rle_compress(enc1, sizeof(enc1), src1, 1);
    uint32_t ds1 = pogls_bermuda_rle_decompress(dec1, sizeof(dec1), enc1, es1);
    TEST("rle single byte", ds1 == 1 && memcmp(src1, dec1, 1) == 0);

    /* Two bytes */
    uint8_t src2[2] = {0xAA, 0xBB};
    uint8_t enc2[16];
    uint8_t dec2[2];
    uint32_t es2 = pogls_bermuda_rle_compress(enc2, sizeof(enc2), src2, 2);
    uint32_t ds2 = pogls_bermuda_rle_decompress(dec2, sizeof(dec2), enc2, es2);
    TEST("rle two bytes", ds2 == 2 && memcmp(src2, dec2, 2) == 0);

    /* Empty */
    uint8_t enc3[4];
    uint32_t es3 = pogls_bermuda_rle_compress(enc3, sizeof(enc3), NULL, 0);
    uint32_t ds3 = pogls_bermuda_rle_decompress(NULL, 0, enc3, es3);
    TEST("rle empty", es3 == 0 && ds3 == 0);
}

/* ═══════════════════════════════════════════════════════════════════
   Test: Shadow Bond
   ═══════════════════════════════════════════════════════════════════ */

static void test_shadow_bond(void)
{
    printf("\n=== Shadow Bond ===\n");

    uint8_t data[64];
    for (int i = 0; i < 64; i++)
        data[i] = (uint8_t)(i);

    int w = pogls_bermuda_shadow_write("test-key", data, 64);
    TEST("shadow write succeeds", w == 1);

    uint8_t readback[64];
    memset(readback, 0, 64);
    int r = pogls_bermuda_shadow_read("test-key", readback, 64);
    TEST("shadow read succeeds", r == 1);
    TEST("shadow read matches write", memcmp(data, readback, 64) == 0);

    /* Read with wrong key fails */
    uint8_t junk[64];
    int r2 = pogls_bermuda_shadow_read("wrong-key", junk, 64);
    TEST("shadow wrong key fails", r2 == 0);

    /* Overwrite same key */
    uint8_t data2[64];
    memset(data2, 0x42, 64);
    pogls_bermuda_shadow_write("test-key", data2, 64);
    memset(readback, 0, 64);
    pogls_bermuda_shadow_read("test-key", readback, 64);
    TEST("shadow overwrite works", memcmp(data2, readback, 64) == 0);
}

/* ═══════════════════════════════════════════════════════════════════
   Main
   ═══════════════════════════════════════════════════════════════════ */

int main(void)
{
    printf("POGLS Bermuda Test Suite\n");
    printf("=======================\n");

    test_route_basic();
    test_route_deterministic();
    test_route_edge();
    test_face_scores();
    test_diamond_allzero();
    test_diamond_pattern();
    test_diamond_multi();
    test_diamond_mixed();
    test_rle_uniform();
    test_rle_random();
    test_rle_mixed();
    test_rle_edge();
    test_shadow_bond();

    printf("\n=======================\n");
    printf("Results: %d PASS, %d FAIL out of %d tests\n",
           n_pass, n_fail, n_pass + n_fail);

    return n_fail > 0 ? 1 : 0;
}
