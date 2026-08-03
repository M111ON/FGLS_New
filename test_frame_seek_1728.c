/*
 * test_frame_seek_1728.c — Full integration: data → flat_key → bridge_288 → frame_1728
 *
 * Tests the complete 288-cell pipeline:
 *   1. rdh_capture() → flat_key
 *   2. bridge_288(flat_key) → (face, direction, cell_pos)  — LOSSLESS
 *   3. frame_1728_at(flat_key % 1728) → Frame1728           — for decomposition only
 *   4. Round-trip via bridge_288_key (LOSSLESS)
 *   5. Comparison: 1440-cycle (lossy) vs 1728-cycle (lossless)
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "collection/rdh/rdh_288_bridge.h"
#include "collection/dgls/geo/include/geo_frame_seek_1728.h"
#include "collection/rdh/rdh_capture.h"

int main(void) {
    int pass = 0, fail = 0;

    #define CHECK(cond, msg) do { \
        if (cond) { printf("  PASS  %s\n", msg); pass++; } \
        else      { printf("  FAIL  %s\n", msg); fail++; } \
    } while(0)

    /* ═══════════════════════════════════════════════════════
       TEST 1: Number chain
       ═══════════════════════════════════════════════════════ */
    printf("=== Number Chain ===\n");
    CHECK(FRAME_1728_CYCLE == 1728, "FRAME_1728_CYCLE = 1728");
    CHECK(FRAME_1728_STRIDE == 37, "FRAME_1728_STRIDE = 37");
    CHECK(FACE_SLOTS_1728 == 144, "FACE_SLOTS_1728 = 144 (vs 120 in 1440)");
    CHECK(CELL_288 * CELL_DIRS * DODECA_FACES == GEO_FULL, "288×6×12 = 20736");
    CHECK(FRAME_1728_CYCLE * DODECA_FACES == GEO_FULL, "1728×12 = 20736");

    /* ═══════════════════════════════════════════════════════
       TEST 2: Stride-37 on 1728 — full bijection
       ═══════════════════════════════════════════════════════ */
    printf("\n=== Stride-37 on 1728 ===\n");
    CHECK(bridge_288_verify() == 0, "stride-37 full cycle on 1728");

    /* ═══════════════════════════════════════════════════════
       TEST 3: Bridge decomposition — all 20736 keys
       ═══════════════════════════════════════════════════════ */
    printf("\n=== Bridge Decomposition (20736 keys) ===\n");
    CHECK(bridge_288_full_verify() == 0, "all 20736 decompose uniquely");

    /* ═══════════════════════════════════════════════════════
       TEST 4: Direction & face distribution
       ═══════════════════════════════════════════════════════ */
    printf("\n=== Distribution ===\n");
    uint32_t dir_count[6] = {0};
    uint32_t face_count[12] = {0};
    for (int64_t k = 0; k < 20736; k++) {
        Cell288Addr a = bridge_288(k);
        dir_count[a.direction]++;
        face_count[a.face]++;
    }
    int dir_ok = 1, face_ok = 1;
    for (int d = 0; d < 6; d++) if (dir_count[d] != 3456) dir_ok = 0;
    for (int fc = 0; fc < 12; fc++) if (face_count[fc] != 1728) face_ok = 0;
    CHECK(dir_ok, "each direction gets 3456 keys");
    CHECK(face_ok, "each face gets 1728 keys");

    /* ═══════════════════════════════════════════════════════
       TEST 5: 1440-cycle (lossy) vs 1728-cycle (lossless)
       ═══════════════════════════════════════════════════════ */
    printf("\n=== 1440 vs 1728 Comparison ===\n");
    printf("  INFO  1440-cycle: 20736 → 1440 = %.1fx compression (LOSSY)\n", 20736.0/1440);
    printf("  INFO  1728-cycle: 20736 → 20736 = 1.00x (LOSSLESS via bridge)\n");
    CHECK(1, "comparison info printed");

    /* ═══════════════════════════════════════════════════════
       TEST 6: Round-trip via bridge_288 (LOSSLESS)
       ═══════════════════════════════════════════════════════ */
    printf("\n=== Round-trip via bridge_288 (20736 keys) ===\n");
    int rt_ok = 1;
    for (int64_t k = 0; k < GEO_FULL; k++) {
        Cell288Addr a = bridge_288(k);
        int64_t k2 = bridge_288_key(a);
        if (k2 != k) {
            printf("  FAIL  round-trip at key %ld: got %ld\n", (long)k, (long)k2);
            fail++; rt_ok = 0; break;
        }
    }
    CHECK(rt_ok, "all 20736 round-trips OK via bridge_288");

    /* ═══════════════════════════════════════════════════════
       TEST 7: frame_1728_at decomposition consistency
       (frame_1728_at works on 0..1727 enc values only)
       ═══════════════════════════════════════════════════════ */
    printf("\n=== frame_1728_at consistency (enc 0..1727) ===\n");
    int f1728_ok = 1;
    for (uint16_t enc = 0; enc < 1728; enc++) {
        Frame1728 f = frame_1728_at(enc);
        if (f.enc != enc || f.face > 11 || f.direction > 5 || f.cell_pos > 287) {
            printf("  FAIL  frame_1728_at(%u) mismatch\n", enc);
            f1728_ok = 0; fail++; break;
        }
    }
    CHECK(f1728_ok, "frame_1728_at consistent for all 1728 enc values");

    /* ═══════════════════════════════════════════════════════
       TEST 8: Sample decompositions — bridge_288 + frame_1728
       ═══════════════════════════════════════════════════════ */
    printf("\n=== Sample Decompositions ===\n");
    int64_t samples[] = {0, 287, 288, 1439, 1727, 1728, 20735};
    for (int i = 0; i < 7; i++) {
        int64_t k = samples[i];
        Cell288Addr ca = bridge_288(k);
        Frame1728 fr = frame_1728_at((uint16_t)(k % 1728));
        printf("  key=%5ld → bridge: face=%2d dir=%d cell=%3d | frame: enc=%4d face=%2d dir=%d cell=%3d\n",
               (long)k, ca.face, ca.direction, ca.cell_pos,
               fr.enc, fr.face, fr.direction, fr.cell_pos);
    }
    pass++; /* informational */

    /* ═══════════════════════════════════════════════════════
       TEST 9: Integration — rdh_capture → bridge_288 → frame_1728
       ═══════════════════════════════════════════════════════ */
    printf("\n=== Integration: rdh_capture → bridge_288 → frame_1728 ===\n");
    const char *test_data[] = {
        "hello world",
        "geometric compression",
        "288 cell architecture",
        "stride 37 decagram",
        "recursive diamond hierarch"
    };
    int n_tests = sizeof(test_data) / sizeof(test_data[0]);
    int int_ok = 1;
    for (int i = 0; i < n_tests; i++) {
        RDHConfig cfg = {144, 144, 1, 1, 1};
        int64_t fk = rdh_capture((const uint8_t *)test_data[i],
                                  strlen(test_data[i]), &cfg);
        Cell288Addr ca = bridge_288(fk);

        /* Verify: bridge_288_key(bridge_288(fk)) == fk */
        int64_t fk2 = bridge_288_key(ca);
        if (fk2 != fk) {
            printf("  FAIL  '%s': round-trip broken\n", test_data[i]);
            int_ok = 0;
        }
        printf("  '%s' → fk=%ld → face=%d dir=%d cell=%d\n",
               test_data[i], (long)fk, ca.face, ca.direction, ca.cell_pos);
    }
    CHECK(int_ok, "rdh_capture → bridge_288 round-trip consistent");

    /* ═══════════════════════════════════════════════════════
       TEST 10: Performance — 1M bridge_288 calls per second
       ═══════════════════════════════════════════════════════ */
    printf("\n=== Performance ===\n");
    clock_t start = clock();
    uint32_t count = 1000000;
    uint64_t checksum = 0;
    for (uint32_t i = 0; i < count; i++) {
        Cell288Addr a = bridge_288(i);
        checksum += a.cell_pos;
    }
    clock_t end = clock();
    double elapsed = (double)(end - start) / CLOCKS_PER_SEC;
    double mops = count / elapsed / 1e6;
    printf("  %.1f Mops/sec (%u bridge_288 calls in %.3fs, checksum=%lu)\n",
           mops, count, elapsed, (unsigned long)checksum);
    CHECK(mops > 1.0, "performance > 1 Mops/sec");

    /* ═══════════════════════════════════════════════════════
       SUMMARY
       ═══════════════════════════════════════════════════════ */
    printf("\n=== RESULTS: %d PASS, %d FAIL ===\n", pass, fail);
    return fail;
}
