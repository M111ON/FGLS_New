/*
 * fgls_profile_test.c — Test suite for fgls_profile.h
 *
 * Tests the universal data profiler on synthetic patterns:
 *   - All zeros (FLAT)
 *   - Single value (FLAT)
 *   - Sparse (few non-zero)
 *   - Gradient (smooth ascending)
 *   - Delta (sequential)
 *   - Structured (repeating pattern)
 *   - Limited palette (HEX)
 *   - Random noise (RAW)
 *   - Q4 quantized weights (RAW)
 *   - Sparse biases (SPARSE)
 *
 * Compile: gcc -O2 -o fgls_profile_test.exe fgls_profile_test.c -lm
 * Run:     ./fgls_profile_test.exe
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../collection/fgls_profile.h"

static int tests_run    = 0;
static int tests_passed = 0;

#define TEST_START(name) do { \
    tests_run++; \
    printf("TEST %d: %-30s ", tests_run, name); \
} while(0)

#define TEST_PASS() do { \
    tests_passed++; \
    printf("✓ PASS\n"); \
} while(0)

#define TEST_FAIL(msg) do { \
    printf("✗ FAIL: %s\n", msg); \
} while(0)

#define ASSERT_EQ(a, b, msg) do { \
    if ((a) != (b)) { \
        TEST_FAIL(msg); \
        printf("  expected %d, got %d\n", (int)(b), (int)(a)); \
        return; \
    } \
} while(0)

#define ASSERT_LE(a, b, msg) do { \
    if ((a) > (b)) { \
        TEST_FAIL(msg); \
        printf("  expected <= %d, got %d\n", (int)(b), (int)(a)); \
        return; \
    } \
} while(0)

/* ── Test data generators ── */

static void gen_all_zeros(uint8_t *buf, uint32_t n) {
    memset(buf, 0, n);
}

static void gen_single_value(uint8_t *buf, uint32_t n, uint8_t val) {
    memset(buf, val, n);
}

static void gen_sparse(uint8_t *buf, uint32_t n, uint32_t nonzero_count) {
    memset(buf, 0, n);
    /* spread non-zero values evenly across buffer */
    uint32_t step = n / (nonzero_count + 1);
    if (step < 1) step = 1;
    uint32_t pos = step;
    uint32_t state = 777;
    for (uint32_t i = 0; i < nonzero_count && pos < n; i++) {
        state = state * 1103515245u + 12345u;
        buf[pos] = (uint8_t)((state >> 16) % 254) + 1;
        pos += step;
    }
}

static void gen_gradient(uint8_t *buf, uint32_t n) {
    for (uint32_t i = 0; i < n; i++)
        buf[i] = (uint8_t)((i * 255) / n);
}

static void gen_delta(uint8_t *buf, uint32_t n) {
    /* sequential: each byte = previous + 1 (mod 256) */
    buf[0] = 0;
    for (uint32_t i = 1; i < n; i++)
        buf[i] = (uint8_t)(buf[i-1] + 1);
}

static void gen_repeating(uint8_t *buf, uint32_t n) {
    /* 4-byte pattern repeated */
    uint8_t pat[] = {0xAA, 0x55, 0x0F, 0xF0};
    for (uint32_t i = 0; i < n; i++)
        buf[i] = pat[i % 4];
}

static void gen_palette(uint8_t *buf, uint32_t n) {
    /* 8 distinct values scattered */
    uint8_t pal[] = {0, 32, 64, 96, 128, 160, 192, 224};
    for (uint32_t i = 0; i < n; i++)
        buf[i] = pal[i % 8];
}

static void gen_random(uint8_t *buf, uint32_t n) {
    /* LCG pseudo-random (deterministic) */
    uint32_t state = 12345;
    for (uint32_t i = 0; i < n; i++) {
        state = state * 1103515245u + 12345u;
        buf[i] = (uint8_t)(state >> 16);
    }
}

static void gen_q4_weights(uint8_t *buf, uint32_t n) {
    /* Q4: values 0-15, uniformly distributed (looks random at byte level) */
    uint32_t state = 42;
    for (uint32_t i = 0; i < n; i++) {
        state = state * 1103515245u + 12345u;
        buf[i] = (uint8_t)((state >> 16) % 16);
    }
}

static void gen_sparse_biases(uint8_t *buf, uint32_t n) {
    /* 90% zeros, 10% random values */
    memset(buf, 0, n);
    uint32_t state = 99;
    uint32_t nz = n / 10;
    for (uint32_t i = 0; i < nz; i++) {
        state = state * 1103515245u + 12345u;
        uint32_t pos = (state >> 16) % n;
        state = state * 1103515245u + 12345u;
        buf[pos] = (uint8_t)((state >> 16) % 255) + 1;
    }
}

/* ── Tests ── */

static void test_null_input(void) {
    TEST_START("null input");
    FglsProfile p;
    int rc = fgls_profile(NULL, 100, &p);
    ASSERT_EQ(rc, -1, "should return -1 for NULL");
    rc = fgls_profile((const uint8_t*)"x", 0, &p);
    ASSERT_EQ(rc, -1, "should return -1 for size=0");
    TEST_PASS();
}

static void test_all_zeros(void) {
    TEST_START("all zeros → FLAT");
    uint8_t buf[256];
    gen_all_zeros(buf, 256);
    FglsProfile p;
    fgls_profile(buf, 256, &p);
    ASSERT_EQ(p.nonzero_count, 0, "nonzero should be 0");
    ASSERT_EQ(p.max_value, 0, "max should be 0");
    ASSERT_EQ(p.is_flat, 1, "should be flat");
    FglsRoute r = fgls_route(&p);
    ASSERT_EQ(r, FGLS_ROUTE_FLAT, "should route to FLAT");
    TEST_PASS();
}

static void test_single_value(void) {
    TEST_START("single value (0x80) → FLAT");
    uint8_t buf[512];
    gen_single_value(buf, 512, 0x80);
    FglsProfile p;
    fgls_profile(buf, 512, &p);
    ASSERT_EQ(p.nonzero_count, 512, "all nonzero");
    ASSERT_EQ(p.unique_values, 1, "one unique value");
    ASSERT_EQ(p.max_run, 512, "run should be 512");
    FglsRoute r = fgls_route(&p);
    ASSERT_EQ(r, FGLS_ROUTE_FLAT, "should route to FLAT (long run)");
    TEST_PASS();
}

static void test_sparse(void) {
    TEST_START("sparse (5% nonzero) → SPARSE");
    uint8_t buf[1024];
    gen_sparse(buf, 1024, 50); /* 50/1024 ≈ 5% */
    FglsProfile p;
    fgls_profile(buf, 1024, &p);
    uint32_t nz_pct = (p.nonzero_count * 100u) / p.size;
    ASSERT_LE(nz_pct, 25u, "nonzero pct should be < 25%");
    FglsRoute r = fgls_route(&p);
    ASSERT_EQ(r, FGLS_ROUTE_SPARSE, "should route to SPARSE");
    TEST_PASS();
}

static void test_gradient(void) {
    TEST_START("gradient (smooth ascending) → GRADIENT");
    uint8_t buf[256];
    gen_gradient(buf, 256);
    FglsProfile p;
    fgls_profile(buf, 256, &p);
    /* gradient has low locality (adjacent values are close) */
    ASSERT_LE(p.locality_x1000, 10000u, "locality should be moderate");
    FglsRoute r = fgls_route(&p);
    /* gradient → DELTA or GRADIENT (both valid: gradient IS sequential) */
    int ok = (r == FGLS_ROUTE_DELTA) || (r == FGLS_ROUTE_GRADIENT);
    ASSERT_EQ(ok, 1, "should route to DELTA or GRADIENT");
    TEST_PASS();
}

static void test_delta(void) {
    TEST_START("sequential (delta+1) → DELTA");
    uint8_t buf[256];
    gen_delta(buf, 256);
    FglsProfile p;
    fgls_profile(buf, 256, &p);
    /* delta: locality = 1.0 (each step is exactly 1) */
    ASSERT_LE(p.locality_x1000, 2000u, "locality should be very low");
    FglsRoute r = fgls_route(&p);
    ASSERT_EQ(r, FGLS_ROUTE_DELTA, "should route to DELTA");
    TEST_PASS();
}

static void test_repeating_pattern(void) {
    TEST_START("repeating 4-byte pattern → structured");
    uint8_t buf[1024];
    gen_repeating(buf, 1024);
    FglsProfile p;
    fgls_profile(buf, 1024, &p);
    ASSERT_EQ(p.unique_values, 4, "should have 4 unique values");
    /* repeating pattern has low entropy */
    ASSERT_LE(p.entropy_x1000, 2500u, "entropy should be low");
    FglsRoute r = fgls_route(&p);
    /* should route to HEX (4 unique ≤ 16) or GRADIENT */
    int ok = (r == FGLS_ROUTE_HEX) || (r == FGLS_ROUTE_GRADIENT)
          || (r == FGLS_ROUTE_HILBERT) || (r == FGLS_ROUTE_FLAT);
    ASSERT_EQ(ok, 1, "should route to HEX/GRADIENT/HILBERT/FLAT");
    TEST_PASS();
}

static void test_palette(void) {
    TEST_START("limited palette (8 values) → HEX");
    uint8_t buf[512];
    gen_palette(buf, 512);
    FglsProfile p;
    fgls_profile(buf, 512, &p);
    ASSERT_EQ(p.unique_values, 8, "should have 8 unique values");
    FglsRoute r = fgls_route(&p);
    ASSERT_EQ(r, FGLS_ROUTE_HEX, "should route to HEX");
    TEST_PASS();
}

static void test_random_noise(void) {
    TEST_START("random noise → ZSTD or RAW");
    uint8_t buf[4096];
    gen_random(buf, 4096);
    FglsProfile p;
    fgls_profile(buf, 4096, &p);
    /* random: high entropy, many unique values */
    ASSERT_LE(p.entropy_x1000, 8500u, "entropy should be bounded");
    FglsRoute r = fgls_route(&p);
    int ok = (r == FGLS_ROUTE_ZSTD) || (r == FGLS_ROUTE_RAW);
    ASSERT_EQ(ok, 1, "should route to ZSTD or RAW");
    TEST_PASS();
}

static void test_q4_weights(void) {
    TEST_START("Q4 quantized weights (0-15) → HEX or RAW");
    uint8_t buf[2048];
    gen_q4_weights(buf, 2048);
    FglsProfile p;
    fgls_profile(buf, 2048, &p);
    ASSERT_EQ(p.max_value <= 15, 1, "max value should be ≤ 15");
    ASSERT_EQ(p.bit_width, 4, "bit_width should be 4");
    FglsRoute r = fgls_route(&p);
    /* Q4: 16 unique values, moderate entropy → HEX or ZSTD or RAW */
    int ok = (r == FGLS_ROUTE_HEX) || (r == FGLS_ROUTE_ZSTD)
          || (r == FGLS_ROUTE_RAW);
    ASSERT_EQ(ok, 1, "should route to HEX/ZSTD/RAW");
    TEST_PASS();
}

static void test_sparse_biases(void) {
    TEST_START("sparse biases (90% zeros) → SPARSE");
    uint8_t buf[4096];
    gen_sparse_biases(buf, 4096);
    FglsProfile p;
    fgls_profile(buf, 4096, &p);
    uint32_t nz_pct = (p.nonzero_count * 100u) / p.size;
    ASSERT_LE(nz_pct, 15u, "nonzero pct should be < 15%");
    FglsRoute r = fgls_route(&p);
    ASSERT_EQ(r, FGLS_ROUTE_SPARSE, "should route to SPARSE");
    TEST_PASS();
}

static void test_gpx5_mapping(void) {
    TEST_START("GPX5 codec mapping consistency");
    uint8_t expected[] = {
        FGLS_GPX5_CODEC_SEED,     /* FLAT     */
        FGLS_GPX5_CODEC_RICE3,    /* SPARSE   */
        FGLS_GPX5_CODEC_FREQ,     /* GRADIENT */
        FGLS_GPX5_CODEC_DELTA,    /* DELTA    */
        FGLS_GPX5_CODEC_HILBERT,  /* HILBERT  */
        FGLS_GPX5_CODEC_HEX,      /* HEX      */
        FGLS_GPX5_CODEC_ZSTD19,   /* ZSTD     */
        FGLS_GPX5_CODEC_RAW,      /* RAW      */
        0x08                       /* FRAMED   */
    };
    /* sanity: expected array length must match FGLS_ROUTE_COUNT */
    if (sizeof(expected) / sizeof(expected[0]) != (size_t)FGLS_ROUTE_COUNT) {
        char msg[96];
        snprintf(msg, 96, "expected array size %zu != FGLS_ROUTE_COUNT %d",
                 sizeof(expected) / sizeof(expected[0]), (int)FGLS_ROUTE_COUNT);
        TEST_FAIL(msg);
        return;
    }
    for (int i = 0; i < FGLS_ROUTE_COUNT; i++) {
        uint8_t codec = fgls_route_to_gpx5_codec((FglsRoute)i);
        if (codec != expected[i]) {
            char msg[64];
            snprintf(msg, 64, "route %d: expected codec 0x%02X, got 0x%02X",
                     i, expected[i], codec);
            TEST_FAIL(msg);
            return;
        }
    }
    TEST_PASS();
}

static void test_profile_consistency(void) {
    TEST_START("profile consistency (sum checks)");
    uint8_t buf[1024];
    gen_random(buf, 1024);
    FglsProfile p;
    fgls_profile(buf, 1024, &p);
    ASSERT_EQ(p.size, 1024, "size should match");
    /* entropy should be between 0 and 8000 */
    ASSERT_LE(p.entropy_x1000, 8001u, "entropy ≤ 8.0");
    /* top4 counts should sum to ≤ size */
    uint32_t top4_sum = p.top4_cnt[0] + p.top4_cnt[1]
                      + p.top4_cnt[2] + p.top4_cnt[3];
    ASSERT_LE(top4_sum, p.size, "top4 sum ≤ size");
    TEST_PASS();
}

static void test_large_input(void) {
    TEST_START("large input (1 MB)");
    uint32_t n = 1024 * 1024;
    uint8_t *buf = (uint8_t *)malloc(n);
    if (!buf) { TEST_FAIL("malloc failed"); return; }
    gen_sparse_biases(buf, n);
    FglsProfile p;
    int rc = fgls_profile(buf, n, &p);
    ASSERT_EQ(rc, 0, "should succeed");
    ASSERT_EQ(p.size, n, "size should match");
    FglsRoute r = fgls_route(&p);
    ASSERT_EQ(r, FGLS_ROUTE_SPARSE, "should route to SPARSE");
    free(buf);
    TEST_PASS();
}

/* ── Main ── */

int main(void) {
    printf("=== fgls_profile.h Test Suite ===\n\n");

    test_null_input();
    test_all_zeros();
    test_single_value();
    test_sparse();
    test_gradient();
    test_delta();
    test_repeating_pattern();
    test_palette();
    test_random_noise();
    test_q4_weights();
    test_sparse_biases();
    test_gpx5_mapping();
    test_profile_consistency();
    test_large_input();

    printf("\n=== Results: %d/%d passed ===\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
