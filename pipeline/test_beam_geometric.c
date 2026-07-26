/*
 * test_beam_geometric.c — Geometric Beam Encoding Tests
 * ═══════════════════════════════════════════════════════════════════
 * Weight = Position. No storage needed.
 * beam_length = distance from center = |weight|
 * sign = direction (ceiling/floor)
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define FIELD_SIZE    20736u
#define FIELD_CENTER  (FIELD_SIZE / 2)  /* 10368 */
#define Q8_MIN        (-128)
#define Q8_MAX        127

static int n_pass = 0, n_fail = 0;
#define TEST(name, expr) do { \
    if (expr) { n_pass++; printf("  PASS  %s\n", name); } \
    else { n_fail++; printf("  FAIL  %s (line %d)\n", name, __LINE__); } \
} while(0)

/* ── weight ↔ position ──────────────────────────────────────── */
static inline uint32_t w2p(int32_t w) { return FIELD_CENTER + w; }
static inline int32_t  p2w(uint32_t p) { return (int32_t)p - (int32_t)FIELD_CENTER; }

/* ── T1: roundtrip all Q8 values ────────────────────────────── */
static void test_roundtrip(void) {
    printf("=== T1: Roundtrip all Q8 (-128..127) ===\n");
    int ok = 0;
    for (int32_t w = Q8_MIN; w <= Q8_MAX; w++) {
        uint32_t pos = w2p(w);
        int32_t back = p2w(pos);
        if (back == w) ok++;
    }
    TEST("256/256 roundtrip", ok == 256);
}

/* ── T2: position range ─────────────────────────────────────── */
static void test_position_range(void) {
    printf("=== T2: Position range ===\n");
    uint32_t min_pos = w2p(Q8_MIN);
    uint32_t max_pos = w2p(Q8_MAX);
    TEST("min_pos = 10240", min_pos == 10240);
    TEST("max_pos = 10495", max_pos == 10495);
    TEST("min_pos < FIELD_CENTER", min_pos < FIELD_CENTER);
    TEST("max_pos > FIELD_CENTER", max_pos > FIELD_CENTER);
    TEST("all in range", min_pos >= 0 && max_pos < FIELD_SIZE);
}

/* ── T3: beam_length = |weight| ─────────────────────────────── */
static void test_beam_length(void) {
    printf("=== T3: beam_length = |weight| ===\n");
    int ok = 0;
    for (int32_t w = Q8_MIN; w <= Q8_MAX; w++) {
        uint32_t pos = w2p(w);
        uint32_t blen = (pos >= FIELD_CENTER) ? (pos - FIELD_CENTER) : (FIELD_CENTER - pos);
        uint32_t expected = (w < 0) ? (uint32_t)(-w) : (uint32_t)w;
        if (blen == expected) ok++;
    }
    TEST("256/256 beam_length", ok == 256);
}

/* ── T4: sign from position ─────────────────────────────────── */
static void test_sign(void) {
    printf("=== T4: Sign from position ===\n");
    int ok = 0;
    for (int32_t w = Q8_MIN; w <= Q8_MAX; w++) {
        uint32_t pos = w2p(w);
        int sign = (pos > FIELD_CENTER) ? 1 : (pos < FIELD_CENTER) ? -1 : 0;
        int expected = (w > 0) ? 1 : (w < 0) ? -1 : 0;
        if (sign == expected) ok++;
    }
    TEST("256/256 sign", ok == 256);
}

/* ── T5: zero at center ─────────────────────────────────────── */
static void test_zero(void) {
    printf("=== T5: Zero at center ===\n");
    uint32_t pos = w2p(0);
    TEST("zero pos = FIELD_CENTER", pos == FIELD_CENTER);
    TEST("zero beam_length = 0", (FIELD_CENTER - pos) == 0);
    TEST("zero sign = 0",
         (pos > FIELD_CENTER ? 1 : pos < FIELD_CENTER ? -1 : 0) == 0);
}

/* ── T6: boundary values ────────────────────────────────────── */
static void test_boundary(void) {
    printf("=== T6: Boundary values ===\n");
    /* Q8 min */
    int32_t w_min = Q8_MIN;
    uint32_t pos_min = w2p(w_min);
    TEST("Q8_MIN roundtrip", p2w(pos_min) == w_min);
    TEST("Q8_MIN sign = -1", pos_min < FIELD_CENTER);
    /* Q8 max */
    int32_t w_max = Q8_MAX;
    uint32_t pos_max = w2p(w_max);
    TEST("Q8_MAX roundtrip", p2w(pos_max) == w_max);
    TEST("Q8_MAX sign = +1", pos_max > FIELD_CENTER);
}

/* ── T7: determinism ────────────────────────────────────────── */
static void test_determinism(void) {
    printf("=== T7: Determinism ===\n");
    for (int32_t w = -100; w <= 100; w++) {
        uint32_t p1 = w2p(w);
        uint32_t p2 = w2p(w);
        if (p1 != p2 || p2w(p1) != w) { n_fail++; printf("  FAIL  deterministic at %d\n", w); return; }
    }
    TEST("101 deterministic", 1);
}

/* ── T8: no collision for unique weights ────────────────────── */
static void test_no_collision(void) {
    printf("=== T8: No collision for unique weights ===\n");
    uint8_t seen[FIELD_SIZE];
    memset(seen, 0, sizeof(seen));
    int collisions = 0;
    for (int32_t w = Q8_MIN; w <= Q8_MAX; w++) {
        uint32_t pos = w2p(w);
        if (seen[pos]) collisions++;
        seen[pos] = 1;
    }
    TEST("0 collisions", collisions == 0);
}

/* ── T9: stress — random weights ────────────────────────────── */
static void test_stress(void) {
    printf("=== T9: Stress (10000 random) ===\n");
    srand(42);
    int ok = 0;
    for (int i = 0; i < 10000; i++) {
        int32_t w = (rand() % 256) - 128;  /* Q8 range */
        uint32_t pos = w2p(w);
        int32_t back = p2w(pos);
        if (back == w) ok++;
    }
    TEST("10000/10000 roundtrip", ok == 10000);
}

/* ── T10: memory layout ─────────────────────────────────────── */
static void test_memory_layout(void) {
    printf("=== T10: Memory layout ===\n");
    TEST("Q8 weights fit in field", (Q8_MAX - Q8_MIN + 1) <= FIELD_SIZE);
    TEST("center in middle", FIELD_CENTER == FIELD_SIZE / 2);
    TEST("no overflow on w2p(Q8_MAX)", w2p(Q8_MAX) < FIELD_SIZE);
    TEST("no underflow on w2p(Q8_MIN)", w2p(Q8_MIN) < FIELD_SIZE);
}

/* ── main ───────────────────────────────────────────────────── */
int main(void) {
    printf("╔═══════════════════════════════════════════════════════════╗\n");
    printf("║  Geometric Beam Encoding Tests                           ║\n");
    printf("║  Weight = Position. Storage = 0. Access = O(1).          ║\n");
    printf("╚═══════════════════════════════════════════════════════════╝\n\n");

    test_roundtrip();
    test_position_range();
    test_beam_length();
    test_sign();
    test_zero();
    test_boundary();
    test_determinism();
    test_no_collision();
    test_stress();
    test_memory_layout();

    printf("\n═══════════════════════════════════════════════════════════\n");
    printf("RESULTS: %d pass, %d fail\n", n_pass, n_fail);
    return n_fail > 0 ? 1 : 0;
}
