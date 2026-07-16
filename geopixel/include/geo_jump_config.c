/*
 * geo_jump_config.c — Test + CLI for GeoJump Multi-Config Gear Converter
 *
 * gcc -O2 -std=c11 -o geo_jump_config.exe geo_jump_config.c -lm
 * ./geo_jump_config.exe test
 * ./geo_jump_config.exe convert <input.gpf> <src_config> <dst_config> <output.gpf>
 * ./geo_jump_config.exe info
 */

#define GEO_JUMP_INLINE
#include "geo_jump_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* ═══════════════════════════════════════════════════════════════════════
   TESTS
   ═══════════════════════════════════════════════════════════════════════ */

static int test_config_info(void)
{
    printf("=== Config Info ===\n");

    GjConfigInfo a = gj_config_info(GJ_CONFIG_A);
    printf("Config A (4x4x3): floor=%ux%u=%u, floors=%u, tower=%u, group=%u, groups=%u, unit=%uB\n",
           a.floor_cols, a.floor_rows, a.floor_cells, a.floors,
           a.cells_per_tower, a.group_size, a.groups, a.unit_sz);

    GjConfigInfo b = gj_config_info(GJ_CONFIG_B);
    printf("Config B (8x8):   floor=%ux%u=%u, floors=%u, tower=%u, group=%u, groups=%u, unit=%uB\n",
           b.floor_cols, b.floor_rows, b.floor_cells, b.floors,
           b.cells_per_tower, b.group_size, b.groups, b.unit_sz);

    GjConfigInfo c = gj_config_info(GJ_CONFIG_C);
    printf("Config C (4x4x4): floor=%ux%u=%u, floors=%u, tower=%u, group=%u, groups=%u, unit=%uB\n",
           c.floor_cols, c.floor_rows, c.floor_cells, c.floors,
           c.cells_per_tower, c.group_size, c.groups, c.unit_sz);

    /* Verify all total 20736 */
    assert(a.groups * a.group_size == GEO_FULL);
    assert(b.groups * b.group_size == GEO_FULL);
    assert(c.groups * c.group_size == GEO_FULL);
    printf("OK: all configs total %u nodes\n", GEO_FULL);
    return 0;
}

static int test_decompose_compose(void)
{
    printf("\n=== Decompose/Compose Roundtrip ===\n");

    for (GjConfig cfg = GJ_CONFIG_A; cfg <= GJ_CONFIG_E; cfg++) {
        int errors = 0;
        for (uint32_t nid = 0; nid < GEO_FULL; nid++) {
            GjNodeAddr a = gj_decompose(nid, cfg);
            uint32_t back = gj_compose(&a, cfg);
            if (back != nid) {
                if (errors < 3)
                    printf("  FAIL cfg=%d node=%u → (%u,%u,%u,%u,%u) → %u\n",
                           cfg, nid, a.group, a.tower_in_group, a.tower, a.floor, a.cell, back);
                errors++;
            }
        }
        printf("Config %c: %d errors / %u nodes %s\n",
               'A' + cfg, errors, GEO_FULL, errors == 0 ? "OK" : "FAIL");
        if (errors > 0) return 1;
    }
    return 0;
}

static int test_gear_convert_same_size(void)
{
    printf("\n=== Gear Convert: B→C (same 64B, different grouping) ===\n");

    uint32_t n = 1024;
    uint8_t *src = (uint8_t *)calloc(n, 64);
    uint8_t *dst = (uint8_t *)calloc(n, 64);

    /* Fill source with known pattern */
    for (uint32_t i = 0; i < n; i++) {
        for (int j = 0; j < 64; j++)
            src[i * 64 + j] = (uint8_t)((i * 7 + j * 13) & 0xFF);
    }

    uint32_t converted = gj_gear_convert(src, GJ_CONFIG_B, dst, GJ_CONFIG_C, n);
    printf("  Converted %u nodes (B→C)\n", converted);

    /* Verify data is preserved */
    int diffs = 0;
    for (uint32_t i = 0; i < converted; i++) {
        if (memcmp(src + i * 64, dst + i * 64, 64) != 0) diffs++;
    }
    printf("  Diffs: %d / %u %s\n", diffs, converted, diffs == 0 ? "OK" : "FAIL");

    free(src);
    free(dst);
    return diffs;
}

static int test_gear_convert_48_64(void)
{
    printf("\n=== Gear Convert: A(48B)→B(64B) ===\n");

    uint32_t n = 1024;
    uint8_t *src48 = (uint8_t *)calloc(n, 48);
    uint8_t *dst64 = (uint8_t *)calloc(n, 64);

    /* Fill 48B source with pattern */
    for (uint32_t i = 0; i < n; i++) {
        for (int j = 0; j < 48; j++)
            src48[i * 48 + j] = (uint8_t)((i * 11 + j * 7) & 0xFF);
    }

    uint32_t converted = gj_gear_convert(src48, GJ_CONFIG_A, dst64, GJ_CONFIG_B, n);
    printf("  Converted %u nodes (48B→64B)\n", converted);

    /* Check: first 48 bytes should match, tail should be zero-padded */
    int errs = 0;
    for (uint32_t i = 0; i < converted && i < 10; i++) {
        if (memcmp(src48 + i * 48, dst64 + i * 64, 48) != 0) {
            printf("  DATA MISMATCH at node %u\n", i);
            errs++;
        }
        for (int j = 48; j < 64; j++) {
            if (dst64[i * 64 + j] != 0) {
                printf("  PADDING MISMATCH at node %u byte %d\n", i, j);
                errs++;
                break;
            }
        }
    }
    printf("  Errors: %d %s\n", errs, errs == 0 ? "OK" : "FAIL");

    free(src48);
    free(dst64);
    return errs;
}

static int test_gear_convert_64_48(void)
{
    printf("\n=== Gear Convert: B(64B)→A(48B) ===\n");

    uint32_t n = 1024;
    uint8_t *src64 = (uint8_t *)calloc(n, 64);
    uint8_t *dst48 = (uint8_t *)calloc(n, 48);

    /* Fill 64B source */
    for (uint32_t i = 0; i < n; i++) {
        for (int j = 0; j < 64; j++)
            src64[i * 64 + j] = (uint8_t)((i * 3 + j * 17) & 0xFF);
    }

    uint32_t converted = gj_gear_convert(src64, GJ_CONFIG_B, dst48, GJ_CONFIG_A, n);
    printf("  Converted %u nodes (64B→48B)\n", converted);

    /* Check: first 48 bytes of each src node should match dst */
    int errs = 0;
    for (uint32_t i = 0; i < converted && i < 10; i++) {
        if (memcmp(src64 + i * 64, dst48 + i * 48, 48) != 0) {
            printf("  TRUNCATION MISMATCH at node %u\n", i);
            errs++;
        }
    }
    printf("  Errors: %d %s\n", errs, errs == 0 ? "OK" : "FAIL");

    free(src64);
    free(dst48);
    return errs;
}

static int test_full_roundtrip(void)
{
    printf("\n=== Full Roundtrip: A→B→C→A (within common groups) ===\n");

    /* A has 144 groups, B/C have 162 groups. Only 144 groups overlap.
     * Convert only 144 × 144 = 20736 nodes (all of A's space). */
    uint32_t n = 144 * 144; /* = 20736, all A nodes */
    uint32_t a_unit = GJ_A_UNIT_SZ;  /* 48 */
    uint32_t b_unit = GJ_B_UNIT_SZ;  /* 64 */
    uint32_t c_unit = GJ_C_UNIT_SZ;  /* 64 */
    uint8_t *a48 = (uint8_t *)calloc(n, a_unit);
    uint8_t *b64 = (uint8_t *)calloc(n, b_unit);
    uint8_t *c64 = (uint8_t *)calloc(n, c_unit);
    uint8_t *a48_back = (uint8_t *)calloc(n, a_unit);

    /* Fill A with pattern */
    for (uint32_t i = 0; i < n; i++) {
        for (uint32_t j = 0; j < a_unit; j++)
            a48[i * a_unit + j] = (uint8_t)((i * 13 + j * 7) & 0xFF);
    }

    /* A→B: truncate test (48B→64B is lossless, just pads zeros) */
    gj_gear_convert(a48, GJ_CONFIG_A, b64, GJ_CONFIG_B, n);

    /* B→C: regroup (same 64B, different tower layout) */
    gj_gear_convert(b64, GJ_CONFIG_B, c64, GJ_CONFIG_C, n);

    /* C→A: truncate (64B→48B, keeps first 48 bytes) */
    gj_gear_convert(c64, GJ_CONFIG_C, a48_back, GJ_CONFIG_A, n);

    /* Verify: first 48 bytes of each node should survive roundtrip */
    int diffs = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (memcmp(a48 + i * a_unit, a48_back + i * a_unit, a_unit) != 0)
            diffs++;
    }
    printf("  A(48B)→B(64B)→C(64B)→A(48B): %d diffs / %u nodes %s\n",
           diffs, n, diffs == 0 ? "OK" : "FAIL");

    free(a48); free(b64); free(c64); free(a48_back);
    return diffs;
}

static int test_hilbert_consistency(void)
{
    printf("\n=== Hilbert Consistency Across Configs ===\n");

    /* Config A: Hilbert order within 4×4 floor (order 2) */
    uint32_t order_a = 2; /* 4×4 = 2^2 */
    printf("  Config A (4x4, order %u): ", order_a);
    for (uint32_t d = 0; d < 16; d++) {
        uint32_t x, y;
        gj_hilbert_d2xy(d, order_a, &x, &y);
        uint32_t back = gj_hilbert_xy2d(x, y, order_a);
        if (back != d) { printf("FAIL at d=%u (got %u)\n", d, back); return 1; }
    }
    printf("OK\n");

    /* Config B: Hilbert order within 8×8 floor (order 3) */
    uint32_t order_b = 3; /* 8×8 = 2^3 */
    printf("  Config B (8x8, order %u): ", order_b);
    for (uint32_t d = 0; d < 64; d++) {
        uint32_t x, y;
        gj_hilbert_d2xy(d, order_b, &x, &y);
        uint32_t back = gj_hilbert_xy2d(x, y, order_b);
        if (back != d) { printf("FAIL at d=%u (got %u, xy=%u,%u)\n", d, back, x, y); return 1; }
    }
    printf("OK\n");

    /* Config C: Hilbert order within 4×4 floor (order 2), repeated 4 floors */
    printf("  Config C (4x4, order %u, 4 floors): ", order_a);
    for (uint32_t f = 0; f < 4; f++) {
        for (uint32_t d = 0; d < 16; d++) {
            uint32_t x, y;
            gj_hilbert_d2xy(d, order_a, &x, &y);
            uint32_t back = gj_hilbert_xy2d(x, y, order_a);
            if (back != d) { printf("FAIL floor=%u d=%u\n", f, d); return 1; }
        }
    }
    printf("OK\n");

    /* Config D: 9×9 floor — Peano curve instead of Hilbert (non-power-of-2) */
    printf("  Config D (9x9, Peano): ");
    {
        int peano_ok = 1;
        for (uint32_t i = 0; i < 81; i++) {
            uint32_t x = i / 9;
            uint32_t y = i % 9;
            uint32_t d = gj_peano_xy2d(x, y, 9, 9);
            if (d >= 81) { peano_ok = 0; break; }
        }
        printf("%s\n", peano_ok ? "OK (Peano 9x9)" : "FAIL");
        if (!peano_ok) return 1;
    }

    /* Config E: Hilbert order within 16×16 floor (order 4) */
    uint32_t order_e = 4; /* 16×16 = 2^4 */
    printf("  Config E (16x16, order %u): ", order_e);
    for (uint32_t d = 0; d < 256; d++) {
        uint32_t x, y;
        gj_hilbert_d2xy(d, order_e, &x, &y);
        uint32_t back = gj_hilbert_xy2d(x, y, order_e);
        if (back != d) { printf("FAIL at d=%u (got %u)\n", d, back); return 1; }
    }
    printf("OK\n");

    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
   MAIN
   ═══════════════════════════════════════════════════════════════════════ */

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("GeoJump Multi-Config Gear Converter\n");
        printf("Usage:\n");
        printf("  %s test          Run all tests\n", argv[0]);
        printf("  %s info          Show config details\n", argv[0]);
        return 0;
    }

    if (strcmp(argv[1], "info") == 0) {
        return test_config_info();
    }

    if (strcmp(argv[1], "test") == 0) {
        int fail = 0;
        fail += test_config_info();
        fail += test_decompose_compose();
        fail += test_hilbert_consistency();
        fail += test_gear_convert_same_size();
        fail += test_gear_convert_48_64();
        fail += test_gear_convert_64_48();
        fail += test_full_roundtrip();

        printf("\n=== RESULT: %s (%d errors) ===\n",
               fail == 0 ? "ALL PASS" : "FAILED", fail);
        return fail;
    }

    fprintf(stderr, "Unknown command: %s\n", argv[1]);
    return 1;
}
