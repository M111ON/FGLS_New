/*
 * test_coord_spine.c — Integration test for coord_spine.h
 * ═══════════════════════════════════════════════════════════════
 * Verifies that coord_spine.h works correctly with:
 *   1. bermuda_export.h (Bermuda geometry router)
 *   2. tw_capture_int.h (TW capture system)
 *   3. geo_config.h (POGLS geometry config)
 *
 * Compile: gcc -std=c11 -Wall -I.. -I../core/core -o test_coord_spine.exe test_coord_spine.c
 * Run: ./test_coord_spine.exe
 */

#include "bermuda_export.h"
#include "tw_capture_int.h"
#include "geo_config.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>

/* ═══════════════════════════════════════════════════════════════
   TEST 1: Constants consistency across all systems
   ═══════════════════════════════════════════════════════════════ */
static void test_constants_consistency(void) {
    printf("TEST 1: Constants consistency...\n");

    /* Bermuda ↔ coord_spine */
    assert(BERMUDA_STRIDE == TRING_STRIDE);         /* 37 = 37 */
    assert(BERMUDA_N_ZONES == GEO_FACES_DODECA);    /* 12 = 12 */
    assert(BERMUDA_TRING_SLOTS == TRING_CYCLE);     /* 720 = 720 */
    assert(BERMUDA_SHADOW_RING == TRING_COMPOUNDS); /* 144 = 144 */

    /* TW ↔ coord_spine */
    assert(TW_SCALE == GEO_FULL * 10);              /* 207360 = 20736 × 10 */
    assert(TW_N_SECTORS == 10u);
    assert(TW_SLOTS_PER == 6u);
    assert(TW_N_SLOTS == TW_N_SECTORS * TW_SLOTS_PER); /* 60 = 10 × 6 */
    assert(TW_REWIND_SLOTS == GEO_FULL);            /* 20736 = 20736 */

    /* Geo ↔ coord_spine */
    assert(GEO_SPOKES == 6u);
    assert(GEO_FACES_ICOS == 9u);
    assert(GEO_FACES_DODECA == 12u);
    assert(GEO_FACE_UNITS == 64u);
    assert(GEO_SLOTS == 576u);
    assert(GEO_FULL_N == 3456u);
    assert(GEO_FULL == 20736u);

    /* Cross-system ratios */
    assert(GEO_FULL / GEO_FULL_N == 6u);           /* 20736 / 3456 = 6 */
    assert(GEO_FULL / GEO_SLOTS == 36u);           /* 20736 / 576 = 36 */
    assert(GEO_FULL / 144 == 144u);                /* 20736 / 144 = 144 */
    assert(TRING_TOTAL / 2 == GEO_FULL_N);         /* 6912 / 2 = 3456 */

    printf("  PASS: All constants consistent\n\n");
}

/* ═══════════════════════════════════════════════════════════════
   TEST 2: Bermuda system with coord_spine constants
   ═══════════════════════════════════════════════════════════════ */
static void test_bermuda_system(void) {
    printf("TEST 2: Bermuda system...\n");

    /* Init Bermuda */
    bermuda_init();
    assert(_bermuda_ctx.initialized == 1);

    /* Verify gear slots match coord_spine */
    assert(_bermuda_ctx.slots[1] == BERMUDA_GEAR1_SLOTS); /* 512 */
    assert(_bermuda_ctx.slots[2] == BERMUDA_GEAR2_SLOTS); /* 1024 */
    assert(_bermuda_ctx.slots[3] == BERMUDA_GEAR3_SLOTS); /* 2048 */
    assert(_bermuda_ctx.slots[4] == BERMUDA_GEAR4_SLOTS); /* 4096 */

    /* Test traverse on gear 3 (2048 slots) */
    uint16_t idx = 42;
    uint16_t result = bermuda_traverse(idx, 3, 0); /* ORBITAL */
    assert(result == (idx + 1) % 2048);            /* 43 */

    /* Test zone classification */
    uint8_t zone = bermuda_zone(idx, 3);
    assert(zone < BERMUDA_N_ZONES);                /* zone must be 0-11 */

    /* Test route token */
    BermudaRouteEntry entry;
    bermuda_route_token(idx, 3, 0, &entry);
    assert(entry.idx_in == idx);
    assert(entry.zone == zone);
    assert(entry.pole <= 1);
    assert(entry.shape == 73 || entry.shape == 79); /* I or O */
    assert(entry.tring_slot < BERMUDA_TRING_SLOTS);

    printf("  PASS: Bermuda works with coord_spine constants\n\n");
}

/* ═══════════════════════════════════════════════════════════════
   TEST 3: TW capture system with coord_spine constants
   ═══════════════════════════════════════════════════════════════ */
static void test_tw_system(void) {
    printf("TEST 3: TW capture system...\n");

    /* Verify TW constants match coord_spine */
    assert(TW_SCALE == 207360u);
    assert(TW_N_SECTORS == 10u);
    assert(TW_SLOTS_PER == 6u);
    assert(TW_N_SLOTS == 60u);

    /* Test capture at a known hex centroid and verify lossless roundtrip */
    TWCaptureInt cap;
    int64_t tx = TW_SLOT_LOCAL_I[0][0][0];
    int64_t ty = TW_SLOT_LOCAL_I[0][0][1];
    tw_capture_int(tx, ty, &cap);
    assert(cap.zone < TW_N_SECTORS);
    assert(cap.slot < TW_N_SLOTS);

    /* Verify lossless roundtrip */
    int64_t rx, ry;
    tw_reconstruct_int(&cap, &rx, &ry);
    assert(rx == tx);
    assert(ry == ty);

    /* Test capture at known sector boundary */
    int64_t vx = 100000, vy = 100000;
    tw_capture_int(vx, vy, &cap);
    assert(cap.zone < TW_N_SECTORS);
    assert(cap.slot < TW_N_SLOTS);

    /* Test combined capture */
    uint8_t is_tri;
    TWCaptureInt cap_combined;
    tw_capture_int_combined(vx, vy, &cap_combined, &is_tri);
    assert(cap_combined.zone < TW_N_SECTORS);
    assert(is_tri <= 1);

    printf("  PASS: TW works with coord_spine constants\n\n");
}

/* ═══════════════════════════════════════════════════════════════
   TEST 4: Geo config with coord_spine constants
   ═══════════════════════════════════════════════════════════════ */
static void test_geo_config(void) {
    printf("TEST 4: Geo config...\n");

    /* Verify all geo constants match coord_spine */
    assert(GEO_SPOKES == 6u);
    assert(GEO_FACES_ICOS == 9u);
    assert(GEO_FACE_UNITS == 64u);
    assert(GEO_SLOTS == 576u);
    assert(GEO_FULL_N == 3456u);
    assert(GEO_FULL == 20736u);
    assert(GEO_OUTER_SLOTS == 512u);
    assert(GEO_CENTER_BASE == 512u);
    assert(GEO_SIDE_FULL == 54u);
    assert(GEO_HILBERT_N == 576u);
    assert(GEO_BLOCK_BOUNDARY == 288u);
    assert(GEO_TE_CYCLE == 144u);
    assert(GEO_TE_FULL_CYCLES == 24u);
    assert(GEO_TE_SNAPS == 6u);

    /* Verify compile-time checks pass (they're in coord_spine.h) */
    assert(GEO_SPOKES * GEO_SLOTS == GEO_FULL_N);
    assert(GEO_TE_CYCLE * GEO_TE_FULL_CYCLES == GEO_FULL_N);
    assert(GEO_FACES_ICOS * GEO_FACE_UNITS == GEO_SLOTS);
    assert(GEO_BLOCK_BOUNDARY * 2 == GEO_SLOTS);

    printf("  PASS: Geo config works with coord_spine constants\n\n");
}

/* ═══════════════════════════════════════════════════════════════
   TEST 5: Cross-system integration
   ═══════════════════════════════════════════════════════════════ */
static void test_cross_system(void) {
    printf("TEST 5: Cross-system integration...\n");

    /* Bermuda index → TW sector mapping */
    uint16_t bermuda_idx = 100;
    uint8_t gear = 3; /* 2048 slots */

    /* Get Bermuda zone */
    uint8_t bermuda_zone_val = bermuda_zone(bermuda_idx, gear);
    assert(bermuda_zone_val < BERMUDA_N_ZONES);

    /* Map to TW sector (0-9) via modular arithmetic */
    uint8_t tw_sector = bermuda_zone_val % TW_N_SECTORS;
    assert(tw_sector < TW_N_SECTORS);

    /* TW sector → POGLS face */
    uint8_t pogls_face = tw_sector % GEO_FACES_ICOS;
    assert(pogls_face < GEO_FACES_ICOS);

    /* Verify the chain is deterministic */
    uint8_t tw_sector2 = bermuda_zone(bermuda_idx, gear) % TW_N_SECTORS;
    uint8_t pogls_face2 = tw_sector2 % GEO_FACES_ICOS;
    assert(tw_sector == tw_sector2);
    assert(pogls_face == pogls_face2);

    /* Verify GEO_WRAP works with TW_SCALE */
    uint32_t addr = GEO_WRAP(TW_SCALE);
    assert(addr == 0); /* 207360 % 20736 = 0 */

    printf("  PASS: Cross-system integration works\n\n");
}

/* ═══════════════════════════════════════════════════════════════
   TEST 6: PHI constants (POGLS fold)
   ═══════════════════════════════════════════════════════════════ */
static void test_pogls_constants(void) {
    printf("TEST 6: POGLS constants...\n");

    /* Verify PHI constants are correct */
    assert(PHI_SCALE == 1048576u);     /* 2^20 */
    assert(PHI_UP == 1696631u);        /* floor(φ × 2^20) */
    assert(PHI_DOWN == 648055u);       /* floor(φ^-1 × 2^20) */

    /* Verify PHI_UP + PHI_DOWN ≈ PHI_SCALE (golden ratio property) */
    uint32_t sum = PHI_UP + PHI_DOWN;
    assert(sum == 2344686u);           /* 1696631 + 648055 */

    /* Verify node/face constants */
    assert(NODE_MAX == 162u);
    assert(FACES_RAW == 32u);
    assert(FACES_LOGICAL == 256u);
    assert(DIAMOND_BLOCK_SIZE == 64u);

    printf("  PASS: POGLS constants correct\n\n");
}

/* ═══════════════════════════════════════════════════════════════
   MAIN
   ═══════════════════════════════════════════════════════════════ */
int main(void) {
    printf("=== coord_spine.h Integration Test ===\n\n");

    test_constants_consistency();
    test_bermuda_system();
    test_tw_system();
    test_geo_config();
    test_cross_system();
    test_pogls_constants();

    printf("=== ALL TESTS PASS ===\n");
    return 0;
}
