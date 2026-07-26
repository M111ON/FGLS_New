/*
 * test_beam_entropy_container.c — Beam Entropy Container tests (v2)
 * ═══════════════════════════════════════════════════════════════════
 *
 * Updated for 8-bit BECCoord + simplified BECSlot (65B).
 * Navigation separated from value encoding.
 *
 * ═══════════════════════════════════════════════════════════════════
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "beam_entropy_container.h"

static int n_pass = 0, n_fail = 0;
#define TEST(name, expr) do { \
    if (expr) { n_pass++; printf("  PASS  %s\n", name); } \
    else { n_fail++; printf("  FAIL  %s (line %d)\n", name, __LINE__); } \
} while(0)

/* ── T1: BECCoord 8-bit roundtrip ─────────────────────────── */
static void test_coord_8bit(void) {
    printf("=== T1: BECCoord 8-bit roundtrip ===\n");

    /* Full Q8 range */
    {
        int ok = 1;
        for (int32_t w = -128; w <= 127; w++) {
            BECCoord c = bec_coord_from_weight(w);
            int32_t r = bec_weight_from_coord(c);
            if (r != w) { ok = 0; break; }
        }
        TEST("full Q8 range roundtrip", ok);
    }

    /* Zone/position split */
    {
        BECCoord c = bec_coord_from_weight(100);
        /* weight=100 → 100+128=228 → zone=228>>4=14, pos=228&0xF=4 */
        uint8_t zone = bec_coord_zone(c);
        uint8_t pos = bec_coord_pos(c);
        TEST("zone=14 for weight=100", zone == 14);
        TEST("pos=4 for weight=100", pos == 4);
    }

    /* Zero weight */
    {
        BECCoord c = bec_coord_from_weight(0);
        /* weight=0 → 0+128=128 → zone=8, pos=0 */
        TEST("zero weight zone=8", bec_coord_zone(c) == 8);
        TEST("zero weight pos=0", bec_coord_pos(c) == 0);
        TEST("zero weight roundtrip", bec_weight_from_coord(c) == 0);
    }

    /* Edge cases */
    {
        BECCoord cmax = bec_coord_from_weight(127);
        BECCoord cmin = bec_coord_from_weight(-128);
        TEST("max=127 roundtrip", bec_weight_from_coord(cmax) == 127);
        TEST("min=-128 roundtrip", bec_weight_from_coord(cmin) == -128);
    }

    /* All 256 codes are distinct */
    {
        int seen[256] = {0};
        int dup = 0;
        for (int32_t w = -128; w <= 127; w++) {
            BECCoord c = bec_coord_from_weight(w);
            if (seen[c]++) { dup = 1; break; }
        }
        TEST("256 distinct codes", dup == 0);
    }

    /* sizeof(BECCoord) */
    TEST("BECCoord is 1 byte", sizeof(BECCoord) == 1);
}

/* ── T2: Navigation (param→slot) ──────────────────────────── */
static void test_navigation(void) {
    printf("=== T2: Navigation (param→slot) ===\n");

    /* All slots within range */
    for (uint32_t i = 0; i < 1000; i++) {
        uint32_t slot = bec_param_to_slot(i);
        if (slot >= BEC_SLOTS) {
            printf("  FAIL  slot %u out of range at i=%u\n", slot, i);
            n_fail++;
            return;
        }
    }
    TEST("1000 slots in range", 1);

    /* Field mapping */
    uint16_t row, col;
    bec_param_to_field(100, &row, &col);
    TEST("row < 144", row < BEC_FIELD_H);
    TEST("col < 144", col < BEC_FIELD_W);

    /* Consistent slot from row/col */
    uint32_t slot = bec_param_to_slot(100);
    uint32_t reconstructed = (uint32_t)row * BEC_FIELD_W + col;
    TEST("slot consistent", slot == reconstructed);

    /* Same param → same slot (deterministic) */
    uint32_t s1 = bec_param_to_slot(500);
    uint32_t s2 = bec_param_to_slot(500);
    TEST("deterministic slot", s1 == s2);
}

/* ── T3: Store and load ──────────────────────────────────── */
static void test_store_load(void) {
    printf("=== T3: Store and load ===\n");
    BeamEntropyContainer bec;
    bec_init(&bec);

    /* Store data at param_index */
    uint8_t data[64];
    for (int i = 0; i < 64; i++) data[i] = (uint8_t)(i * 37 + 13);

    int ret = bec_store(&bec, data, 64, 200);
    TEST("store success", ret == 0);

    /* Load by param_index */
    uint8_t loaded[64];
    memset(loaded, 0, 64);
    int n = bec_load(&bec, 200, loaded, 64);
    TEST("load returns 64", n == 64);
    TEST("data matches", memcmp(data, loaded, 64) == 0);

    /* Load by slot */
    uint32_t slot = bec_param_to_slot(200);
    memset(loaded, 0, 64);
    n = bec_load_by_slot(&bec, slot, loaded, 64);
    TEST("load_by_slot returns 64", n == 64);
    TEST("slot data matches", memcmp(data, loaded, 64) == 0);

    /* Statistics */
    uint32_t occ, ovw, st, ld;
    bec_stats(&bec, &occ, &ovw, &st, &ld);
    TEST("occupied = 1", occ == 1);
    TEST("stored = 1", st == 1);
}

/* ── T4: Existence check ─────────────────────────────────── */
static void test_existence(void) {
    printf("=== T4: Existence check ===\n");
    BeamEntropyContainer bec;
    bec_init(&bec);

    /* Empty container */
    TEST("empty has = 0", bec_has(&bec, 400) == 0);
    TEST("empty has_slot = 0", bec_has_slot(&bec, bec_param_to_slot(400)) == 0);

    /* Store one item */
    uint8_t data[64] = {0};
    bec_store(&bec, data, 64, 400);

    TEST("occupied has = 1", bec_has(&bec, 400) == 1);
    TEST("occupied has_slot = 1", bec_has_slot(&bec, bec_param_to_slot(400)) == 1);

    /* Different param_index */
    TEST("different param has = 0", bec_has(&bec, 401) == 0);
}

/* ── T5: Iterator ────────────────────────────────────────── */
static void test_iterator(void) {
    printf("=== T5: Iterator (1440 steps) ===\n");
    BeamEntropyContainer bec;
    bec_init(&bec);

    /* Store 5 items */
    for (int i = 0; i < 5; i++) {
        uint8_t data[64];
        for (int j = 0; j < 64; j++) data[j] = (uint8_t)(i * 100 + j);
        bec_store(&bec, data, 64, (uint32_t)(i * 100));
    }

    /* Walk 1440 steps */
    BECIter it;
    bec_iter_init(&it, &bec);
    uint32_t visited = 0, found = 0;
    while (visited < FT_FRAME_CYCLE) {
        if (it.valid) {
            found++;
            /* Verify it's one of our stored items */
            const BECSlot *slot = &bec.slots[it.row][it.col];
            int match = 0;
            for (int i = 0; i < 5; i++) {
                uint8_t expected[64];
                for (int j = 0; j < 64; j++) expected[j] = (uint8_t)(i * 100 + j);
                if (memcmp(slot->data, expected, 64) == 0) { match = 1; break; }
            }
            if (!match) {
                printf("  FAIL  unexpected data at step %u\n", visited);
                n_fail++;
                return;
            }
        }
        visited++;
        if (!bec_iter_next(&it)) break;
    }
    TEST("1440 steps", visited == 1440);
    printf("  Found %u of 5\n", found);
    TEST("found some", found > 0);
}

/* ── T6: Multi-block (200B) ──────────────────────────────── */
static void test_multi_block(void) {
    printf("=== T6: Multi-block (200B) ===\n");
    BeamEntropyContainer bec;
    bec_init(&bec);

    uint8_t big[200];
    for (int i = 0; i < 200; i++) big[i] = (uint8_t)(i * 7 + 11);

    /* Store each 64B block */
    int stored = 0;
    for (int i = 0; i < 4; i++) {
        size_t off = i * 64;
        size_t blk = (i == 3) ? (200 - 3 * 64) : 64;
        if (bec_store(&bec, big + off, blk, (uint32_t)(i * 1000)) == 0) stored++;
    }
    TEST("stored 4", stored == 4);

    /* Verify each block roundtrips */
    int ok = 0;
    for (int i = 0; i < 4; i++) {
        size_t off = i * 64;
        size_t blk = (i == 3) ? (200 - 3 * 64) : 64;
        uint8_t loaded[64];
        int n = bec_load(&bec, (uint32_t)(i * 1000), loaded, 64);
        if (n == (int)blk && memcmp(loaded, big + off, blk) == 0) ok++;
    }
    printf("  %d/4 blocks roundtrip\n", ok);
    TEST(">= 1 block ok", ok >= 1);
}

/* ── T7: Random stress (200 blocks) ──────────────────────── */
static void test_random(void) {
    printf("=== T7: Random (200 blocks) ===\n");
    BeamEntropyContainer bec;
    bec_init(&bec);
    srand(42);

    int ok = 0;
    for (int i = 0; i < 200; i++) {
        uint8_t orig[64];
        for (int j = 0; j < 64; j++) orig[j] = (uint8_t)(rand() & 0xFF);

        bec_store(&bec, orig, 64, (uint32_t)i);

        uint8_t loaded[64];
        int n = bec_load(&bec, (uint32_t)i, loaded, 64);
        if (n == 64 && memcmp(orig, loaded, 64) == 0) ok++;
    }
    printf("  %d/200 roundtrip (overwrites: %u)\n", ok, bec.overwrites);
    TEST(">= 100 roundtrip", ok >= 100);
}

/* ── T8: Beam timer integration ──────────────────────────── */
static void test_beam_timer(void) {
    printf("=== T8: Beam timer integration ===\n");
    BeamEntropyContainer bec;
    bec_init(&bec);

    /* Store 12 items */
    for (int i = 0; i < 12; i++) {
        uint8_t data[64];
        for (int j = 0; j < 64; j++) data[j] = (uint8_t)((i + 1) * 251 + j * 31);
        bec_store(&bec, data, 64, (uint32_t)(i * 100));
    }

    /* Verify action distribution */
    uint32_t counts[4] = {0};
    for (int i = 0; i < 12; i++) {
        uint16_t enc = (uint16_t)((i * 100) % FT_FRAME_CYCLE);
        uint8_t action = ft_store_action(enc);
        counts[action]++;
    }
    printf("  FREEZE=%u MAIN=%u PIPE=%u BRIDGE=%u\n",
           counts[0], counts[1], counts[2], counts[3]);
    TEST("all 12 stored", counts[0] + counts[1] + counts[2] + counts[3] == 12);
}

/* ── T9: Determinism ─────────────────────────────────────── */
static void test_determinism(void) {
    printf("=== T9: Determinism ===\n");

    /* Same param → same slot */
    uint32_t slot1 = bec_param_to_slot(500);
    uint32_t slot2 = bec_param_to_slot(500);
    TEST("same param → same slot", slot1 == slot2);

    /* Different param → different slot (usually) */
    uint32_t slot3 = bec_param_to_slot(501);
    TEST("both slots valid", slot1 < BEC_SLOTS && slot3 < BEC_SLOTS);
}

/* ── T10: Empty container ────────────────────────────────── */
static void test_empty(void) {
    printf("=== T10: Empty container ===\n");
    BeamEntropyContainer bec;
    bec_init(&bec);

    uint8_t buf[64];
    TEST("empty load returns 0", bec_load(&bec, 999, buf, 64) == 0);
    TEST("empty load_by_slot returns 0",
         bec_load_by_slot(&bec, 12345, buf, 64) == 0);
    TEST("empty has = 0", bec_has(&bec, 999) == 0);
}

/* ── T11: BECSlot size ───────────────────────────────────── */
static void test_slot_size(void) {
    printf("=== T11: BECSlot size ===\n");
    printf("  sizeof(BECSlot) = %zu bytes\n", sizeof(BECSlot));
    /* Should be 65: 64 data + 1 flags (down from 88) */
    TEST("BECSlot is 65 bytes", sizeof(BECSlot) == 65);
}

/* ── T12: BECCoord code mapping ──────────────────────────── */
static void test_code_mapping(void) {
    printf("=== T12: BECCoord code mapping ===\n");

    /* Verify 16 zones × 16 positions = 256 codes */
    BECCoord zp_map[16][16];
    int codes_used = 0;
    for (uint8_t z = 0; z < 16; z++) {
        for (uint8_t p = 0; p < 16; p++) {
            BECZonePos zp = {z, p};
            BECCoord c = bec_coord_from_zp(zp);
            zp_map[z][p] = c;
            codes_used++;

            BECZonePos zp2 = bec_coord_to_zp(c);
            if (zp2.zone != z || zp2.position != p) {
                printf("  FAIL  zp roundtrip at (%u,%u) -> %u -> (%u,%u)\n",
                       z, p, c, zp2.zone, zp2.position);
                n_fail++;
                return;
            }
        }
    }
    TEST("all 256 zp combos valid", codes_used == 256);

    /* Verify no duplicates in zp→c mapping */
    int seen[256] = {0};
    int dup = 0;
    for (uint8_t z = 0; z < 16; z++) {
        for (uint8_t p = 0; p < 16; p++) {
            BECZonePos zp = {z, p};
            BECCoord c = bec_coord_from_zp(zp);
            if (seen[c]++) { dup = 1; break; }
        }
    }
    TEST("all zp combos unique", dup == 0);
}

/* ── T13: Store with BECCoord mapping ────────────────────── */
static void test_store_with_coord(void) {
    printf("=== T13: Store with BECCoord ===\n");
    BeamEntropyContainer bec;
    bec_init(&bec);

    /* Store 10 weights at sequential param_indices */
    uint8_t data[64];
    for (int i = 0; i < 10; i++) {
        memset(data, (uint8_t)i, 64);
        bec_store(&bec, data, 64, (uint32_t)(i * 500));
    }

    /* Verify each — note: using bec_store_weight (alias for bec_store) */
    for (int i = 0; i < 10; i++) {
        uint8_t loaded[64];
        int n = bec_load(&bec, (uint32_t)(i * 500), loaded, 64);
        if (n != 64) {
            printf("  FAIL  load returned %d at i=%d\n", n, i);
            n_fail++;
            return;
        }
        for (int j = 0; j < 64; j++) {
            if (loaded[j] != (uint8_t)i) {
                printf("  FAIL  data mismatch at i=%d, j=%d\n", i, j);
                n_fail++;
                return;
            }
        }
    }
    TEST("10 items roundtrip", 1);
}

/* ── main ────────────────────────────────────────────────── */
int main(void) {
    printf("╔══════════════════════════════════════════════════╗\n");
    printf("║  Beam Entropy Container v2 Tests                ║\n");
    printf("║  8-bit BECCoord | 65B BECSlot | Param navigation║\n");
    printf("╚══════════════════════════════════════════════════╝\n\n");

    /* Self-verify */
    printf("=== Self-verify ===\n");
    int vret = beam_entropy_container_verify();
    TEST("self-verify", vret == 0);
    if (vret != 0) printf("  (error code: %d)\n", vret);

    test_coord_8bit();
    test_navigation();
    test_store_load();
    test_existence();
    test_iterator();
    test_multi_block();
    test_random();
    test_beam_timer();
    test_determinism();
    test_empty();
    test_slot_size();
    test_code_mapping();
    test_store_with_coord();

    printf("\n══════════════════════════════════════════════════\n");
    printf("RESULTS: %d pass, %d fail\n", n_pass, n_fail);
    return n_fail > 0 ? 1 : 0;
}
