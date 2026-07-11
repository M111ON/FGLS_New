#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pogls_kv.h"

static int n_pass = 0, n_fail = 0;

#define TEST(name) do { \
    printf("  %s ... ", #name); \
    if (test_##name() == 0) { \
        printf("PASS\n"); n_pass++; \
    } else { \
        printf("FAIL\n"); n_fail++; \
    } \
} while(0)

/* classify uses 4096-byte strides — test data must be large enough
 * for stride-aligned results */

static int test_classify_0pct(void) {
    uint8_t data[8192];
    memset(data, 0x42, sizeof(data));
    int pct = pogls_kv_classify(data, data, sizeof(data));
    return (pct == 0) ? 0 : 1;
}

static int test_classify_100pct(void) {
    uint8_t a[8192], b[8192];
    memset(a, 0x00, sizeof(a));
    memset(b, 0xFF, sizeof(b));
    int pct = pogls_kv_classify(a, b, sizeof(a));
    return (pct == 100) ? 0 : 1;
}

static int test_classify_50pct(void) {
    /* 8192 bytes: first 4096 same, second 4096 different → 50% */
    uint8_t a[8192], b[8192];
    memset(a,    0x00, 8192);
    memset(b,    0x00, 4096);
    memset(b+4096, 0xFF, 4096);
    int pct = pogls_kv_classify(a, b, 8192);
    return (pct == 50) ? 0 : 1;
}

static int test_entropy_roundtrip(void) {
    /* 32768 bytes: change only first stride (4096 bytes) → 12.5% → ENTROPY */
    uint8_t base[32768], cur[32768];
    memset(base, 0xAB, sizeof(base));
    memcpy(cur, base, sizeof(cur));
    for (size_t i = 0; i < 100; i++) cur[i] ^= 0xFF;

    PoglsKvDelta delta;
    int r = pogls_kv_encode(&delta, cur, base, sizeof(base));
    if (r != 0) { printf("(encode=%d) ", r); return 1; }
    if (delta.type != POGLS_KV_REMAP_ENTROPY) { printf("(type=%d) ", delta.type); return 1; }

    uint8_t decoded[32768];
    r = pogls_kv_decode(decoded, &delta, base, sizeof(base));
    if (r != 0) return 1;

    int ok = (memcmp(decoded, cur, sizeof(cur)) == 0);
    free(delta.entropy_data);
    return ok ? 0 : 1;
}

static int test_geo_roundtrip(void) {
    /* 32768 bytes: change strides 1,2,3 (3 of 8, 37.5%) → GEO */
    uint8_t base[32768], cur[32768];
    memset(base, 0xAB, sizeof(base));
    memcpy(cur, base, sizeof(cur));
    memset(cur + 4096,   0xCD, 4096);
    memset(cur + 8192,   0xEF, 4096);
    memset(cur + 12288,  0x99, 4096);

    PoglsKvDelta delta;
    int r = pogls_kv_encode(&delta, cur, base, sizeof(base));
    if (r != 0) { printf("(encode=%d) ", r); return 1; }
    if (delta.type != POGLS_KV_REMAP_GEO) { printf("(type=%d) ", delta.type); return 1; }
    if (delta.n_ranges == 0) return 1;

    uint8_t decoded[32768];
    r = pogls_kv_decode(decoded, &delta, base, sizeof(base));
    if (r != 0) return 1;

    int ok = (memcmp(decoded, cur, sizeof(cur)) == 0);
    free(delta.geo_data);
    return ok ? 0 : 1;
}

static int test_rle_roundtrip(void) {
    /* Encode non-trivial data against zero base → ENTROPY path tests RLE */
    uint8_t base[32768], cur[32768];
    memset(base, 0, sizeof(base));
    for (size_t i = 0; i < 32768; i++)
        cur[i] = (uint8_t)(i * 7);

    PoglsKvDelta delta;
    int r = pogls_kv_encode(&delta, cur, base, sizeof(base));
    /* Full non-zero data → classify sees all strides dirty → 100% → REBUILD */
    /* Skip the encode check — test RLE through decode directly */

    /* Manual test: compress XOR diff (which is just cur since base=0) via encode
     * but we need classify <85%. Use small change instead. */
    uint8_t base2[32768], cur2[32768];
    memset(base2, 0, sizeof(base2));
    memset(cur2, 0, sizeof(cur2));
    for (size_t i = 0; i < 100; i++) cur2[i] = (uint8_t)(i * 7);

    memset(&delta, 0, sizeof(delta));
    r = pogls_kv_encode(&delta, cur2, base2, sizeof(base2));
    if (r != 0) { printf("(encode=%d) ", r); return 1; }
    if (delta.type != POGLS_KV_REMAP_ENTROPY) { printf("(type=%d) ", delta.type); return 1; }

    uint8_t decoded[32768];
    memset(decoded, 0, sizeof(decoded));
    r = pogls_kv_decode(decoded, &delta, base2, sizeof(base2));
    if (r != 0) return 1;

    int ok = (memcmp(decoded, cur2, sizeof(cur2)) == 0);
    free(delta.entropy_data);
    return ok ? 0 : 1;
}

static int test_rebuild(void) {
    uint8_t base[64], cur[64];
    memset(base, 0x00, 64);
    memset(cur, 0xFF, 64);

    PoglsKvDelta delta;
    int r = pogls_kv_encode(&delta, cur, base, 64);
    return (r == -1 && delta.type == POGLS_KV_REMAP_REBUILD) ? 0 : 1;
}

static int test_rail(void) {
    uint8_t data[32768];
    memset(data, 0xAA, sizeof(data));

    PoglsKvSkeleton sk;
    if (pogls_kv_skeleton_init(&sk, data, sizeof(data)) != 0) return 1;

    PoglsKvRail rail;
    pogls_kv_rail_init(&rail, &sk, sizeof(data), NULL);
    rail.cur = data;

    int steps = 0;
    int r;
    do {
        r = pogls_kv_rail_step(&rail);
        steps++;
    } while (r == 0);

    pogls_kv_rail_free(&rail);
    pogls_kv_skeleton_destroy(&sk);

    if (r != 1) { printf("(r=%d) ", r); return 1; }
    if (rail.change_pct != 0) { printf("(pct=%d) ", rail.change_pct); return 1; }
    if (steps == 0) return 1;
    return 0;
}

static int test_rail_freeze(void) {
    uint8_t data[32768];
    memset(data, 0xAA, sizeof(data));

    PoglsKvSkeleton sk;
    if (pogls_kv_skeleton_init(&sk, data, sizeof(data)) != 0) return 1;

    PoglsKvRail rail;
    pogls_kv_rail_init(&rail, &sk, sizeof(data), NULL);
    rail.cur = data;

    int r;
    /* Step a few times so we're mid-scan */
    for (int i = 0; i < 2; i++) {
        r = pogls_kv_rail_step(&rail);
        if (r == 1) break;  /* completed early (unlikely with 32768B) */
    }

    if (rail.state != 1) { printf("(state=%d) ", rail.state); pogls_kv_rail_free(&rail); pogls_kv_skeleton_destroy(&sk); return 1; }

    pogls_kv_rail_freeze(&rail);
    if (rail.state != 2) { printf("(freeze state=%d) ", rail.state); pogls_kv_rail_free(&rail); pogls_kv_skeleton_destroy(&sk); return 1; }

    /* Step while frozen should return 0 */
    r = pogls_kv_rail_step(&rail);
    if (r != 0) { printf("(frozen step=%d) ", r); pogls_kv_rail_free(&rail); pogls_kv_skeleton_destroy(&sk); return 1; }

    pogls_kv_rail_resume(&rail);
    if (rail.state != 1) { printf("(resume state=%d) ", rail.state); pogls_kv_rail_free(&rail); pogls_kv_skeleton_destroy(&sk); return 1; }

    /* Complete the scan */
    do { r = pogls_kv_rail_step(&rail); } while (r == 0);

    pogls_kv_rail_free(&rail);
    pogls_kv_skeleton_destroy(&sk);
    if (r != 1) { printf("(final=%d) ", r); return 1; }
    return 0;
}

int main(void) {
    printf("pogls_kv test suite\n");
    printf("===================\n\n");

    TEST(classify_0pct);
    TEST(classify_100pct);
    TEST(classify_50pct);
    TEST(entropy_roundtrip);
    TEST(geo_roundtrip);
    TEST(rle_roundtrip);
    TEST(rebuild);
    TEST(rail);
    TEST(rail_freeze);

    printf("\nResults: %d pass, %d fail\n", n_pass, n_fail);
    return n_fail > 0 ? 1 : 0;
}
