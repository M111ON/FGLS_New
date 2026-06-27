/*
 * test_coord_bridge.c — Integration test for coord_bridge.h
 * ═══════════════════════════════════════════════════════════════
 * Verifies that coord_bridge.h works correctly with:
 *   1. Bermuda → UnifiedCoord → Bermuda roundtrip
 *   2. TW → UnifiedCoord → TW roundtrip
 *   3. POGLS → UnifiedCoord → POGLS roundtrip
 *   4. Cross-system conversions (Bermuda ↔ TW)
 *   5. Utility functions
 *
 * Compile: gcc -std=c11 -Wall -I.. -I../core/core -o test_coord_bridge.exe test_coord_bridge.c
 * Run: ./test_coord_bridge.exe
 */

#include "coord_bridge.h"
#include "bermuda_export.h"
#include "tw_capture_int.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>

/* ═══════════════════════════════════════════════════════════════
   TEST 1: Bermuda roundtrip
   ═══════════════════════════════════════════════════════════════ */
static void test_bermuda_roundtrip(void) {
    printf("TEST 1: Bermuda roundtrip...\n");

    bermuda_init();

    /* Create a Bermuda route entry */
    BermudaRouteEntry orig;
    orig.idx_in    = 42;
    orig.zone      = 5;
    orig.pole      = 1;
    orig.shape     = 79;  /* 'O' */
    orig.polarity  = 1;
    orig.tring_slot = 200;

    /* Convert to UnifiedCoord */
    UnifiedCoord u = bermuda_to_coord(orig);
    assert(u.source == COORD_SOURCE_BERMUDA);
    assert(u.zone == 5);
    assert(u.slot == 200);
    assert(u.pole == 1);
    assert(u.shape == 79);

    /* Convert back to Bermuda */
    BermudaRouteEntry back = coord_to_bermuda(u);
    assert(back.zone == 5);
    assert(back.pole == 1);
    assert(back.shape == 79);
    assert(back.tring_slot == 200);

    printf("  PASS: Bermuda roundtrip preserves zone, pole, shape\n\n");
}

/* ═══════════════════════════════════════════════════════════════
   TEST 2: TW roundtrip
   ═══════════════════════════════════════════════════════════════ */
static void test_tw_roundtrip(void) {
    printf("TEST 2: TW roundtrip...\n");

    /* Create a TW capture */
    TWCaptureInt orig;
    orig.zone          = 3;
    orig.slot          = 15;
    orig.resid_x       = 12345;
    orig.resid_y       = -67890;
    orig.drain         = 0;
    orig.drain_zone    = 0;
    orig.drain_slot    = 0;
    orig.drain_resid_x = 0;
    orig.drain_resid_y = 0;

    /* Convert to UnifiedCoord */
    UnifiedCoord u = tw_to_coord(orig);
    assert(u.source == COORD_SOURCE_TW);
    assert(u.zone == 3);
    assert(u.slot == 15);
    assert(u.resid_x == 12345);
    assert(u.resid_y == -67890);

    /* Convert back to TW */
    TWCaptureInt back = coord_to_tw(u);
    assert(back.zone == 3);
    assert(back.slot == 15);
    assert(back.resid_x == 12345);
    assert(back.resid_y == -67890);
    assert(back.drain == 0);  /* drain is cleared */

    printf("  PASS: TW roundtrip preserves zone, slot, residual\n\n");
}

/* ═══════════════════════════════════════════════════════════════
   TEST 3: POGLS roundtrip
   ═══════════════════════════════════════════════════════════════ */
static void test_pogls_roundtrip(void) {
    printf("TEST 3: POGLS roundtrip...\n");

    /* Create a POGLS address */
    PoglsAddr orig;
    orig.face_id   = 12;
    orig.engine_id = 42;
    orig.vector_pos = 500;

    /* Convert to UnifiedCoord */
    UnifiedCoord u = pogls_to_coord(orig);
    assert(u.source == COORD_SOURCE_POGLS);
    assert(u.zone == 12);
    assert(u.slot == 500);

    /* Convert back to POGLS */
    PoglsAddr back = coord_to_pogls(u);
    assert(back.face_id == 12);
    assert(back.vector_pos == 500);
    /* engine_id is not preserved (set to 0) */

    printf("  PASS: POGLS roundtrip preserves face, vector_pos\n\n");
}

/* ═══════════════════════════════════════════════════════════════
   TEST 4: Cross-system Bermuda ↔ TW
   ═══════════════════════════════════════════════════════════════ */
static void test_cross_system(void) {
    printf("TEST 4: Cross-system Bermuda ↔ TW...\n");

    /* Bermuda → TW */
    BermudaRouteEntry b;
    b.zone      = 7;
    b.tring_slot = 350;
    b.pole      = 0;
    b.shape     = 73;

    TWCaptureInt tw = bermuda_to_tw(b);
    assert(tw.zone == BERMUDA_TO_TW_ZONE[7]);  /* mapped zone */
    assert(tw.slot == 350 % TW_N_SLOTS);

    /* TW → Bermuda */
    TWCaptureInt tw2;
    tw2.zone = 4;
    tw2.slot = 25;

    BermudaRouteEntry b2 = tw_to_bermuda(tw2);
    assert(b2.zone == TW_TO_BERMUDA_ZONE[4]);
    assert(b2.tring_slot == 25);

    /* Verify mapping tables are consistent */
    assert(BERMUDA_TO_TW_ZONE[0] == 0);
    assert(BERMUDA_TO_TW_ZONE[1] == 1);
    assert(BERMUDA_TO_TW_ZONE[10] == 0);  /* wraps */
    assert(BERMUDA_TO_TW_ZONE[11] == 1);  /* wraps */

    printf("  PASS: Cross-system conversions work\n\n");
}

/* ═══════════════════════════════════════════════════════════════
   TEST 5: Utility functions
   ═══════════════════════════════════════════════════════════════ */
static void test_utilities(void) {
    printf("TEST 5: Utility functions...\n");

    UnifiedCoord a = {.zone=5, .slot=100, .resid_x=10, .resid_y=20};
    UnifiedCoord b = {.zone=5, .slot=100, .resid_x=10, .resid_y=20};
    UnifiedCoord c = {.zone=6, .slot=100, .resid_x=10, .resid_y=20};

    /* same_zone */
    assert(coord_same_zone(a, b) == 1);
    assert(coord_same_zone(a, c) == 0);

    /* same_slot */
    assert(coord_same_slot(a, b) == 1);
    assert(coord_same_slot(a, c) == 0);

    /* distance2 */
    assert(coord_distance2(a, b) == 0);     /* identical */
    assert(coord_distance2(a, c) == 1);     /* zone differs by 1 */

    printf("  PASS: Utility functions work\n\n");
}

/* ═══════════════════════════════════════════════════════════════
   TEST 6: Full pipeline (Bermuda → UnifiedCoord → TW)
   ═══════════════════════════════════════════════════════════════ */
static void test_full_pipeline(void) {
    printf("TEST 6: Full pipeline...\n");

    bermuda_init();

    /* Start with Bermuda index */
    uint16_t idx = 100;
    uint8_t gear = 3;

    /* Traverse in Bermuda */
    BermudaRouteEntry b;
    bermuda_route_token(idx, gear, 0, &b);
    assert(b.zone < BERMUDA_N_ZONES);

    /* Convert to UnifiedCoord */
    UnifiedCoord u = bermuda_to_coord(b);
    assert(u.source == COORD_SOURCE_BERMUDA);

    /* Convert to TW */
    TWCaptureInt tw = coord_to_tw(u);
    assert(tw.zone < TW_N_SECTORS);

    /* Convert to POGLS */
    PoglsAddr pogls = coord_to_pogls(u);
    assert(pogls.face_id < FACES_RAW);

    /* All conversions are deterministic */
    UnifiedCoord u2 = bermuda_to_coord(b);
    assert(coord_same_slot(u, u2));

    printf("  PASS: Full pipeline works\n\n");
}

/* ═══════════════════════════════════════════════════════════════
   MAIN
   ═══════════════════════════════════════════════════════════════ */
int main(void) {
    printf("=== coord_bridge.h Integration Test ===\n\n");

    test_bermuda_roundtrip();
    test_tw_roundtrip();
    test_pogls_roundtrip();
    test_cross_system();
    test_utilities();
    test_full_pipeline();

    printf("=== ALL TESTS PASS ===\n");
    return 0;
}
