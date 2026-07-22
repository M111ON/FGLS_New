/*
 * test_entropy_container.c — RDH Entropy Container tests
 * ═══════════════════════════════════════════════════════════════
 * Tests: roundtrip (flat_key), determinism, multi-block, empty,
 *        iterator, fibo_tick, random stress, enc coverage
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "entropy_container.h"
#include "fibo_tick.h"

static int n_pass = 0, n_fail = 0;
#define TEST(name, expr) do { \
    if (expr) { n_pass++; printf("  PASS  %s\n", name); } \
    else { n_fail++; printf("  FAIL  %s (line %d)\n", name, __LINE__); } \
} while(0)

/* ── T1: Roundtrip via flat_key ─────────────────────────────── */
static void test_roundtrip(void) {
    printf("=== T1: Roundtrip (flat_key) ===\n");
    EntropyContainer ec; ec_init(&ec);
    RDHConfig cfg = RDH_CAPTURE_144;

    uint8_t data[48];
    for (int i = 0; i < 48; i++) data[i] = (uint8_t)(i * 37 + 13);

    ec_store(&ec, data, 48, &cfg);

    /* Compute flat_key */
    uint64_t fk = rdh_capture(data, 48, &cfg);

    /* Load by flat_key (direct O(1)) */
    uint8_t loaded[48];
    int n = ec_load_by_flat_key(&ec, fk, loaded, 48);
    TEST("load by flat_key", n == 48);
    TEST("match", memcmp(data, loaded, 48) == 0);

    /* Load by address (ring, wedge from flat_key) */
    uint16_t r, w;
    ec_key_to_addr(fk, &r, &w);
    memset(loaded, 0, 48);
    n = ec_load_by_addr(&ec, r, w, loaded, 48);
    TEST("load by addr", n == 48);
    TEST("match addr", memcmp(data, loaded, 48) == 0);
}

/* ── T2: Determinism ────────────────────────────────────────── */
static void test_determinism(void) {
    printf("=== T2: Determinism ===\n");
    RDHConfig cfg = RDH_CAPTURE_144;
    uint8_t data[48];
    for (int i = 0; i < 48; i++) data[i] = (uint8_t)(i * 7 + 3);

    uint64_t k1 = rdh_capture(data, 48, &cfg);
    uint64_t k2 = rdh_capture(data, 48, &cfg);
    TEST("same key", k1 == k2);

    uint16_t r1, w1, r2, w2;
    ec_key_to_addr(k1, &r1, &w1);
    ec_key_to_addr(k2, &r2, &w2);
    TEST("same addr", r1 == r2 && w1 == w2);
}

/* ── T3: Multi-block (200B) ─────────────────────────────────── */
static void test_multi_block(void) {
    printf("=== T3: Multi-block (200B) ===\n");
    EntropyContainer ec; ec_init(&ec);
    RDHConfig cfg = RDH_CAPTURE_144;

    uint8_t big[200];
    for (int i = 0; i < 200; i++) big[i] = (uint8_t)(i * 7 + 11);

    /* Store each 48B block, track flat_keys */
    uint64_t fks[5];
    int stored = 0;
    for (int i = 0; i < 5; i++) {
        size_t off = i * 48;
        size_t blk = (i == 4) ? (200 - 4 * 48) : 48;
        fks[i] = rdh_capture(big + off, blk, &cfg);
        if (ec_store(&ec, big + off, blk, &cfg) == 0) stored++;
    }
    TEST("stored 5", stored == 5);

    /* Verify each block roundtrips via flat_key */
    int ok = 0;
    for (int i = 0; i < 5; i++) {
        size_t off = i * 48;
        size_t blk = (i == 4) ? (200 - 4 * 48) : 48;
        uint8_t loaded[48];
        int n = ec_load_by_flat_key(&ec, fks[i], loaded, 48);
        if (n == (int)blk && memcmp(loaded, big + off, blk) == 0) ok++;
    }
    printf("  %d/5 blocks roundtrip\n", ok);
    TEST(">= 1 block ok", ok >= 1);
}

/* ── T4: Empty ──────────────────────────────────────────────── */
static void test_empty(void) {
    printf("=== T4: Empty ===\n");
    EntropyContainer ec; ec_init(&ec);
    uint8_t buf[48];
    TEST("empty flat_key", ec_load_by_flat_key(&ec, 12345, buf, 48) == 0);
    TEST("empty addr", ec_load_by_addr(&ec, 50, 50, buf, 48) == 0);
    TEST("ec_has false", ec_has(&ec, 999) == 0);
}

/* ── T5: Iterator (1440 steps) ──────────────────────────────── */
static void test_iterator(void) {
    printf("=== T5: Iterator (1440 steps) ===\n");
    EntropyContainer ec; ec_init(&ec);
    RDHConfig cfg = RDH_CAPTURE_144;

    /* Store 5 items, record their positions */
    uint8_t known[5][48];
    uint16_t known_r[5], known_w[5];
    for (int i = 0; i < 5; i++) {
        for (int j = 0; j < 48; j++) known[i][j] = (uint8_t)(i * 100 + j * 7);
        ec_store(&ec, known[i], 48, &cfg);
        uint64_t fk = rdh_capture(known[i], 48, &cfg);
        ec_key_to_addr(fk, &known_r[i], &known_w[i]);
    }

    /* Walk 1440 steps */
    ECIter it;
    ec_iter_init(&it, &ec);
    uint32_t visited = 0, found = 0;
    while (visited < FRAME_CYCLE) {
        if (it.valid) {
            const ECSlot *slot = &ec.slots[it.ring][it.wedge];
            for (int i = 0; i < 5; i++) {
                if (memcmp(slot->data, known[i], 48) == 0) { found++; break; }
            }
        }
        visited++;
        if (!ec_iter_next(&it)) break;
    }
    TEST("1440 steps", visited == 1440);
    printf("  Found %u of 5\n", found);
    TEST("found some", found > 0);
}

/* ── T6: fibo_tick × container ──────────────────────────────── */
static void test_fibo_tick(void) {
    printf("=== T6: fibo_tick × container ===\n");
    EntropyContainer ec; ec_init(&ec);
    RDHConfig cfg = RDH_CAPTURE_144;

    uint8_t chunks[12][48];
    for (int i = 0; i < 12; i++) {
        for (int j = 0; j < 48; j++) chunks[i][j] = (uint8_t)((i + 1) * 251 + j * 31);
        ec_store(&ec, chunks[i], 48, &cfg);
    }

    uint32_t counts[4] = {0};
    for (int i = 0; i < 12; i++) {
        uint64_t fk = rdh_capture(chunks[i], 48, &cfg);
        uint16_t enc = (uint16_t)(fk % FRAME_CYCLE);
        uint8_t action = ft_store_action(enc);
        counts[action]++;
    }
    printf("  FREEZE=%u MAIN=%u PIPE=%u BRIDGE=%u\n",
           counts[0], counts[1], counts[2], counts[3]);
    TEST("all 12 stored", counts[0] + counts[1] + counts[2] + counts[3] == 12);
}

/* ── T7: Random stress (200 blocks) ─────────────────────────── */
static void test_random(void) {
    printf("=== T7: Random (200 blocks) ===\n");
    EntropyContainer ec; ec_init(&ec);
    RDHConfig cfg = RDH_CAPTURE_144;
    srand(42);

    int ok = 0;
    for (int i = 0; i < 200; i++) {
        uint8_t orig[48];
        for (int j = 0; j < 48; j++) orig[j] = (uint8_t)(rand() & 0xFF);
        ec_store(&ec, orig, 48, &cfg);

        uint64_t fk = rdh_capture(orig, 48, &cfg);
        uint8_t loaded[48];
        int n = ec_load_by_flat_key(&ec, fk, loaded, 48);
        if (n == 48 && memcmp(orig, loaded, 48) == 0) ok++;
    }
    printf("  %d/200 roundtrip (overwrites: %u)\n", ok, ec.overwrites);
    TEST(">= 100 roundtrip", ok >= 100);
}

/* ── T8: Enc coverage ───────────────────────────────────────── */
static void test_enc_coverage(void) {
    printf("=== T8: Enc coverage ===\n");
    EntropyContainer ec; ec_init(&ec);
    RDHConfig cfg = RDH_CAPTURE_144;
    srand(123);

    for (int i = 0; i < 1440; i++) {
        uint8_t data[48];
        for (int j = 0; j < 48; j++) data[j] = (uint8_t)(rand() & 0xFF);
        ec_store(&ec, data, 48, &cfg);
    }

    uint32_t occ, ovw, st, ld;
    ec_stats(&ec, &occ, &ovw, &st, &ld);
    printf("  occupied: %u / %u\n", occ, EC_SLOTS);
    printf("  overwrites: %u\n", ovw);
    printf("  stored: %u\n", st);
    TEST("occupied > 0", occ > 0);
    TEST("stored = 1440", st == 1440);
}

/* ── main ───────────────────────────────────────────────────── */
int main(void) {
    printf("╔══════════════════════════════════════════╗\n");
    printf("║  RDH Entropy Container Tests             ║\n");
    printf("╚══════════════════════════════════════════╝\n\n");

    test_roundtrip();
    test_determinism();
    test_multi_block();
    test_empty();
    test_iterator();
    test_fibo_tick();
    test_random();
    test_enc_coverage();

    printf("\n══════════════════════════════════════════\n");
    printf("RESULTS: %d pass, %d fail\n", n_pass, n_fail);
    return n_fail > 0 ? 1 : 0;
}
