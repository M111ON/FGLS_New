/*
 * test_rdh_288_bridge.c — Verify rdh_288_bridge.h
 * Tests: bijection, round-trip, stride-37 on 1728, bridge decomposition
 */
#include <stdio.h>
#include <string.h>
#include "collection/rdh/rdh_288_bridge.h"

int main(void) {
    int pass = 0, fail = 0;

    /* Test 1: Number chain */
    printf("=== Number Chain ===\n");
    #define CHECK(cond, msg) do { \
        if (cond) { printf("  PASS  %s\n", msg); pass++; } \
        else      { printf("  FAIL  %s\n", msg); fail++; } \
    } while(0)

    CHECK(144 * 144 == GEO_FULL, "144² = 20736");
    CHECK(12 * 12 * 12 * 12 == GEO_FULL, "12⁴ = 20736");
    CHECK(CELL_288 * CELL_DIRS * DODECA_FACES == GEO_FULL, "288×6×12 = 20736");
    CHECK(CELL_288 * 72 == GEO_FULL, "288×72 = 20736");
    CHECK(CELL_PER_FACE * DODECA_FACES == GEO_FULL, "1728×12 = 20736");
    CHECK(CELL_PER_FACE == CELL_288 * CELL_DIRS, "1728 = 288×6");

    /* Test 2: Bridge bijection — all 20736 keys decompose uniquely */
    printf("\n=== Bridge Bijection (20736 keys) ===\n");
    int bijection_ok = 1;
    for (int64_t k = 0; k < GEO_FULL; k++) {
        Cell288Addr a = bridge_288(k);
        if (a.face > 11 || a.direction > 5 || a.cell_pos > 287) {
            printf("  FAIL  key %ld out of range\n", (long)k);
            fail++; bijection_ok = 0; break;
        }
    }
    if (bijection_ok) { printf("  PASS  all 20736 decompose in range\n"); pass++; }

    /* Test 3: Round-trip — bridge_288_key(bridge_288(k)) == k */
    printf("\n=== Round-trip (20736 keys) ===\n");
    int rt_ok = 1;
    for (int64_t k = 0; k < GEO_FULL; k++) {
        Cell288Addr a = bridge_288(k);
        int64_t k2 = bridge_288_key(a);
        if (k2 != k) {
            printf("  FAIL  round-trip failed at key %ld\n", (long)k);
            fail++; rt_ok = 0; break;
        }
    }
    if (rt_ok) { printf("  PASS  all 20736 round-trips OK\n"); pass++; }

    /* Test 4: Stride-37 on 1728 — full bijection */
    printf("\n=== Stride-37 on 1728 ===\n");
    int s37_ok = bridge_288_verify();
    if (s37_ok == 0) { printf("  PASS  stride-37 full cycle on 1728\n"); pass++; }
    else { printf("  FAIL  stride-37 verify returned %d\n", s37_ok); fail++; }

    /* Test 5: Direction distribution — each direction gets equal keys */
    printf("\n=== Direction Distribution ===\n");
    uint32_t dir_count[6] = {0};
    for (int64_t k = 0; k < GEO_FULL; k++) {
        Cell288Addr a = bridge_288(k);
        dir_count[a.direction]++;
    }
    int dir_eq = 1;
    for (int d = 0; d < 6; d++) {
        if (dir_count[d] != 3456) { dir_eq = 0; break; }
    }
    if (dir_eq) { printf("  PASS  each direction gets 3456 keys\n"); pass++; }
    else {
        printf("  FAIL  direction counts:");
        for (int d = 0; d < 6; d++) printf(" %u", dir_count[d]);
        printf("\n"); fail++;
    }

    /* Test 6: Face distribution — each face gets 1728 keys */
    printf("\n=== Face Distribution ===\n");
    uint32_t face_count[12] = {0};
    for (int64_t k = 0; k < GEO_FULL; k++) {
        Cell288Addr a = bridge_288(k);
        face_count[a.face]++;
    }
    int face_eq = 1;
    for (int f = 0; f < 12; f++) {
        if (face_count[f] != 1728) { face_eq = 0; break; }
    }
    if (face_eq) { printf("  PASS  each face gets 1728 keys\n"); pass++; }
    else {
        printf("  FAIL  face counts:");
        for (int f = 0; f < 12; f++) printf(" %u", face_count[f]);
        printf("\n"); fail++;
    }

    /* Test 7: Sample decompositions */
    printf("\n=== Sample Decompositions ===\n");
    int64_t samples[] = {0, 287, 288, 1727, 1728, 20735};
    for (int i = 0; i < 6; i++) {
        Cell288Addr a = bridge_288(samples[i]);
        printf("  key=%5ld → face=%2d dir=%d cell=%3d\n",
               (long)samples[i], a.face, a.direction, a.cell_pos);
    }
    pass++; /* informational */

    /* Summary */
    printf("\n=== RESULTS: %d PASS, %d FAIL ===\n", pass, fail);
    return fail;
}
