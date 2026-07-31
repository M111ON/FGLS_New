// test_contour_codec.c — full roundtrip test for contour_codec_20736.h
// ══════════════════════════════════════════════════════════════════════════
// Compile: gcc -O2 -std=c11 -o test_contour_codec.exe test_contour_codec.c -lm
// ══════════════════════════════════════════════════════════════════════════

#define CONTOUR_CODEC_IMPLEMENTATION
#include "contour_codec_20736.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// ── Test data generator ──────────────────────────────────────────────────

static contour_cell test_cells[CC_CELLS];

static void generate_test_data(int seed) {
    srand((unsigned)seed);
    for (int i = 0; i < CC_CELLS; i++) {
        int face, x, y, z;
        cell_from_idx(i, &face, &x, &y, &z);
        test_cells[i].face = face;
        test_cells[i].x = x;
        test_cells[i].y = y;
        test_cells[i].z = z;
        test_cells[i].global_idx = i;
        // Deterministic non-zero value based on position + seed
        int8_t v = (int8_t)((face * 1000 + z * 100 + y * 10 + x + seed) * 37 + 13) % 256 - 128;
        if (v == 0) v = 1;  // ensure no zero values for utilization check
        test_cells[i].value = v;
    }
}

// ── Test suite ───────────────────────────────────────────────────────────

static int total_tests = 0;
static int passed_tests = 0;

static void check(const char *label, int cond) {
    total_tests++;
    if (cond) {
        passed_tests++;
        printf("    [PASS] %s\n", label);
    } else {
        printf("    [FAIL] %s\n", label);
    }
}

// Test 1: Encode → Decode roundtrip for each strategy
static void test_roundtrip(CODEC_STRATEGY strategy) {
    printf("\n  ── Strategy: %s ──\n", codec_strategy_name(strategy));

    codec_ctx *ctx = codec_create(strategy);
    check("codec_create succeeds", ctx != NULL);

    int collisions = codec_encode(ctx, test_cells, CC_CELLS);
    check("zero collisions", collisions == 0);
    check("cell_count == CC_CELLS", ctx->cell_count == CC_CELLS);

    contour_cell decoded[CC_CELLS];
    int n = codec_decode(ctx, decoded, CC_CELLS);
    check("decoded count == CC_CELLS", n == CC_CELLS);

    // Verify every cell matches
    int mismatches = 0;
    for (int i = 0; i < CC_CELLS; i++) {
        if (decoded[i].face != test_cells[i].face ||
            decoded[i].x != test_cells[i].x ||
            decoded[i].y != test_cells[i].y ||
            decoded[i].z != test_cells[i].z ||
            decoded[i].value != test_cells[i].value) {
            mismatches++;
        }
    }
    check("zero mismatches in decoded cells", mismatches == 0);

    // codec_verify
    int errs = codec_verify(ctx, test_cells, CC_CELLS);
    check("codec_verify returns 0 errors", errs == 0);

    codec_free(ctx);
}

// Test 2: O(1) get/set
static void test_getset(CODEC_STRATEGY strategy) {
    printf("\n  ── Get/Set: %s ──\n", codec_strategy_name(strategy));

    codec_ctx *ctx = codec_create(strategy);
    codec_encode(ctx, test_cells, CC_CELLS);

    // Get must match original for every cell
    int get_ok = 1;
    for (int i = 0; i < CC_CELLS; i++) {
        int8_t got = codec_get(ctx, test_cells[i].face, test_cells[i].x,
                               test_cells[i].y, test_cells[i].z);
        if (got != test_cells[i].value) {
            get_ok = 0;
            printf("    [FAIL] get(%d,%d,%d,%d) = %d, expected %d\n",
                   test_cells[i].face, test_cells[i].x, test_cells[i].y,
                   test_cells[i].z, got, test_cells[i].value);
            break;
        }
    }
    check("get() matches all original values", get_ok);

    // Set a new value, then get it back
    codec_set(ctx, 3, 5, 7, 2, 42);
    check("set/get roundtrip", codec_get(ctx, 3, 5, 7, 2) == 42);

    codec_free(ctx);
}

// Test 3: Different seeds (exercises full int8 range)
static void test_seeds(void) {
    printf("\n  ── Multiple seeds ──\n");

    int seeds[] = {0, 1, 42, 137, 255, 1000, 6000};
    for (int s = 0; s < (int)(sizeof(seeds)/sizeof(seeds[0])); s++) {
        generate_test_data(seeds[s]);

        codec_ctx *ctx = codec_create(CODEC_SEQUENTIAL);
        codec_encode(ctx, test_cells, CC_CELLS);

        contour_cell decoded[CC_CELLS];
        codec_decode(ctx, decoded, CC_CELLS);

        int match = 1;
        for (int i = 0; i < CC_CELLS; i++) {
            if (decoded[i].value != test_cells[i].value) { match = 0; break; }
        }
        char label[64];
        snprintf(label, sizeof(label), "seed=%d roundtrip", seeds[s]);
        check(label, match);

        codec_free(ctx);
    }
}

// Test 4: Address space utilization
static void test_utilization(CODEC_STRATEGY strategy) {
    printf("\n  ── Utilization: %s ──\n", codec_strategy_name(strategy));

    codec_ctx *ctx = codec_create(strategy);
    codec_encode(ctx, test_cells, CC_CELLS);

    int used = 0;
    for (int i = 0; i < CC_GEO_FULL; i++) {
        if (ctx->geo[i] != 0) used++;
    }
    printf("    Addresses used: %d / %d (%.1f%%)\n",
           used, CC_GEO_FULL, 100.0 * used / CC_GEO_FULL);
    check("used <= CC_CELLS (no overflow)", used <= CC_CELLS);
    check("used == CC_CELLS (all cells placed)", used == CC_CELLS);

    codec_free(ctx);
}

// Test 5: Edge case — zero-size encode/decode
static void test_edge_cases(void) {
    printf("\n  ── Edge cases ──\n");

    codec_ctx *ctx = codec_create(CODEC_SEQUENTIAL);
    check("encode NULL returns error", codec_encode(NULL, test_cells, 1) == -1);
    check("encode zero cells returns error", codec_encode(ctx, test_cells, 0) == -1);

    contour_cell out[1];
    check("decode NULL returns error", codec_decode(ctx, NULL, 1) == -1);
    check("decode zero max returns error", codec_decode(ctx, out, 0) == -1);

    check("create invalid strategy returns NULL", codec_create((CODEC_STRATEGY)99) == NULL);

    codec_free(ctx);
}

// Test 6: Verify all strategies are distinct (different address layouts)
static void test_distinct_mappings(void) {
    printf("\n  ── Distinct mappings ──\n");

    contour_cell c = {3, 5, 7, 2, 0, cell_global_idx(3,5,7,2)};

    uint32_t addrs[CODEC_COUNT];
    for (int s = 0; s < CODEC_COUNT; s++) {
        cc_mapfn map = map_func_for((CODEC_STRATEGY)s);
        addrs[s] = map(&c);
    }

    int distinct = 1;
    for (int i = 0; i < CODEC_COUNT; i++) {
        for (int j = i+1; j < CODEC_COUNT; j++) {
            if (addrs[i] == addrs[j]) {
                printf("    [INFO] %s and %s map cell (3,5,7,2) to same addr %u\n",
                       codec_strategy_name((CODEC_STRATEGY)i),
                       codec_strategy_name((CODEC_STRATEGY)j), addrs[i]);
                // Not a failure — different strategies can collide on one cell
            }
        }
    }
    // Just verify at least 2 different address layouts exist across strategies
    int unique_addrs = 0;
    for (int i = 0; i < CODEC_COUNT; i++) {
        int is_new = 1;
        for (int j = 0; j < i; j++) {
            if (addrs[j] == addrs[i]) { is_new = 0; break; }
        }
        if (is_new) unique_addrs++;
    }
    check("at least 2 strategies produce distinct address layouts", unique_addrs >= 2);
}

// ── Main ─────────────────────────────────────────────────────────────────

int main(void) {
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║  Contour Codec 20736 — Full Test Suite                 ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n");
    printf("  Cells:   %d (6×10×10×10)\n", CC_CELLS);
    printf("  Target:  %d (144×144)\n", CC_GEO_FULL);
    printf("  Unused:  %d (%.1f%%)\n\n",
           CC_GEO_FULL - CC_CELLS, 100.0*(CC_GEO_FULL - CC_CELLS)/CC_GEO_FULL);

    generate_test_data(42);

    // Run all tests
    printf("═══ Roundtrip Tests (all strategies) ═══\n");
    for (int s = 0; s < CODEC_COUNT; s++) {
        test_roundtrip((CODEC_STRATEGY)s);
    }

    printf("\n═══ O(1) Get/Set Tests ═══\n");
    for (int s = 0; s < CODEC_COUNT; s++) {
        test_getset((CODEC_STRATEGY)s);
    }

    test_seeds();

    printf("\n═══ Utilization Tests ═══\n");
    for (int s = 0; s < CODEC_COUNT; s++) {
        test_utilization((CODEC_STRATEGY)s);
    }

    test_edge_cases();
    test_distinct_mappings();

    // ── Summary ──
    printf("\n══════════════════════════════════════════════════════════\n");
    printf("  RESULTS: %d / %d tests passed\n", passed_tests, total_tests);
    printf("══════════════════════════════════════════════════════════\n");

    if (passed_tests == total_tests) {
        printf("  ALL TESTS PASSED ✓\n");
    } else {
        printf("  SOME TESTS FAILED ✗\n");
    }

    return (passed_tests == total_tests) ? 0 : 1;
}
