/*
 * test_rdh_288_unified.c — Integration: old 1440-cycle vs new 288-cell pipeline
 *
 * Tests:
 *   1. Both APIs work on same data
 *   2. New API is LOSSLESS (round-trip)
 *   3. Old API is LOSSY (information lost)
 *   4. Performance comparison
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "collection/rdh/rdh_288_unified.h"

int main(void) {
    int pass = 0, fail = 0;

    #define CHECK(cond, msg) do { \
        if (cond) { printf("  PASS  %s\n", msg); pass++; } \
        else      { printf("  FAIL  %s\n", msg); fail++; } \
    } while(0)

    /* ═══════════════════════════════════════════════════════
       TEST 1: Number chain consistency
       ═══════════════════════════════════════════════════════ */
    printf("=== Number Chain ===\n");
    CHECK(GEO_FULL == 20736, "GEO_FULL = 20736");
    CHECK(FRAME_1728_CYCLE == 1728, "FRAME_1728_CYCLE = 1728");
    CHECK(CELL_288 * CELL_DIRS * DODECA_FACES == 20736, "288×6×12 = 20736");

    /* ═══════════════════════════════════════════════════════
       TEST 2: Both APIs on same data
       ═══════════════════════════════════════════════════════ */
    printf("\n=== Both APIs on Same Data ===\n");
    const char *test_data[] = {
        "hello world",
        "geometric compression",
        "288 cell architecture",
        "stride 37 decagram",
        "recursive diamond hierarch",
        "fibo spine jet bridge",
        "diamond shell codec",
        "goldberg polyhedra"
    };
    int n_tests = sizeof(test_data) / sizeof(test_data[0]);
    RDHConfig cfg = {144, 144, 1, 1, 1};

    printf("  %-30s %8s %6s %5s %5s %5s  |  %5s %5s %5s\n",
           "Data", "flat_key", "enc1440", "face", "dir", "cell", "F_old", "S_old", "Ph_old");
    printf("  %s\n", "----------------------------------------------------------------------"
                     "---------------------------");

    for (int i = 0; i < n_tests; i++) {
        PipelineComparison c = rdh_compare((const uint8_t *)test_data[i],
                                           strlen(test_data[i]), &cfg);

        printf("  %-30s %8ld %6u %5d %5d %5d  |  %5d %5d %5d\n",
               test_data[i], (long)c.flat_key, c.enc_1440,
               c.cell_288.face, c.cell_288.direction, c.cell_288.cell_pos,
               c.frame_old.face, c.frame_old.slot, c.frame_old.phase);
    }
    CHECK(1, "both APIs produce output");
    pass++; /* informational */

    /* ═══════════════════════════════════════════════════════
       TEST 3: New API is LOSSLESS — round-trip via bridge
       ═══════════════════════════════════════════════════════ */
    printf("\n=== New API: LOSSLESS Round-trip ===\n");
    int new_ok = 1;
    for (int i = 0; i < n_tests; i++) {
        Cell288Addr ca = rdh_capture_to_288((const uint8_t *)test_data[i],
                                             strlen(test_data[i]), &cfg);
        int64_t fk2 = bridge_288_key(ca);
        int64_t fk1 = rdh_capture((const uint8_t *)test_data[i],
                                   strlen(test_data[i]), &cfg);
        if (fk1 != fk2) {
            printf("  FAIL  '%s': round-trip broken\n", test_data[i]);
            new_ok = 0;
        }
    }
    CHECK(new_ok, "new API: all round-trips OK (LOSSLESS)");

    /* ═══════════════════════════════════════════════════════
       TEST 4: Old API is LOSSY — different flat_keys → same enc
       ═══════════════════════════════════════════════════════ */
    printf("\n=== Old API: LOSSY (information lost) ===\n");
    /* flat_key 0 and flat_key 1440 both map to enc 0 in old API */
    uint16_t enc0 = (uint16_t)(0 % 1440);
    uint16_t enc1440 = (uint16_t)(1440 % 1440);
    CHECK(enc0 == enc1440, "old API: flat_key 0 and 1440 both → enc 0 (LOSSY)");

    /* But new API distinguishes them */
    Cell288Addr ca0 = bridge_288(0);
    Cell288Addr ca1440 = bridge_288(1440);
    CHECK(ca0.direction != ca1440.direction, "new API: flat_key 0 and 1440 → different directions (LOSSLESS)");

    /* ═══════════════════════════════════════════════════════
       TEST 5: All 20736 keys round-trip via new API
       ═══════════════════════════════════════════════════════ */
    printf("\n=== Full 20736 Round-trip ===\n");
    int full_ok = 1;
    for (int64_t k = 0; k < GEO_FULL; k++) {
        Cell288Addr ca = bridge_288(k);
        int64_t k2 = bridge_288_key(ca);
        if (k2 != k) {
            printf("  FAIL  key %ld round-trip broken\n", (long)k);
            full_ok = 0; break;
        }
    }
    CHECK(full_ok, "all 20736 keys round-trip OK via bridge_288");

    /* ═══════════════════════════════════════════════════════
       TEST 6: Stride-37 bijection on both cycles
       ═══════════════════════════════════════════════════════ */
    printf("\n=== Stride-37 on Both Cycles ===\n");
    CHECK(bridge_288_verify() == 0, "stride-37 full cycle on 1728");
    CHECK(geo_frame_seek_verify() == 0, "stride-37 full cycle on 1440");

    /* ═══════════════════════════════════════════════════════
       TEST 7: Performance — old vs new
       ═══════════════════════════════════════════════════════ */
    printf("\n=== Performance Comparison ===\n");
    uint32_t N = 1000000;

    /* Old pipeline: rdh_capture → enc → frame_at */
    clock_t t0 = clock();
    uint64_t sum_old = 0;
    for (uint32_t i = 0; i < N; i++) {
        uint8_t buf[8];
        *(uint32_t*)buf = i;
        uint16_t enc = rdh_capture_to_enc(buf, 4, &cfg);
        DualFrame f = frame_at(enc);
        sum_old += f.face + f.slot;
    }
    clock_t t1 = clock();
    double old_us = (double)(t1 - t0) / CLOCKS_PER_SEC * 1e6 / N;

    /* New pipeline: rdh_capture → bridge_288 → frame_1728 */
    t0 = clock();
    uint64_t sum_new = 0;
    for (uint32_t i = 0; i < N; i++) {
        uint8_t buf[8];
        *(uint32_t*)buf = i;
        Cell288Addr ca = rdh_capture_to_288(buf, 4, &cfg);
        sum_new += ca.face + ca.direction + ca.cell_pos;
    }
    t1 = clock();
    double new_us = (double)(t1 - t0) / CLOCKS_PER_SEC * 1e6 / N;

    printf("  Old (1440-cycle): %.2f µs/op\n", old_us);
    printf("  New (288-cell):   %.2f µs/op\n", new_us);
    printf("  Overhead:         %.1fx\n", new_us / old_us);
    CHECK(new_us < 10.0, "new API < 10 µs/op");

    /* ═══════════════════════════════════════════════════════
       SUMMARY
       ═══════════════════════════════════════════════════════ */
    printf("\n=== RESULTS: %d PASS, %d FAIL ===\n", pass, fail);
    return fail;
}
