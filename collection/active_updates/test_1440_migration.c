/*
 * test_1440_migration.c — Task #3: 1440 cycle migration verification
 *
 * Tests:
 *   M01: constants — EXT=2×BASE, BASE=720
 *   M02: phase extraction — idx<720→phase=0, idx>=720→phase=1
 *   M03: backward compat — phase=0 == base dispatch for all 720
 *   M04: phase behavior differs — bit6 flipped in phase 1
 *   M05: full 1440 loop — no out-of-bounds, bit6 always 0 or 1
 *
 * Compile:
 *   gcc -O2 -Wall -o test_1440_migration test_1440_migration.c && ./test_1440_migration
 */

#include <stdio.h>
#include <stdint.h>
#include <assert.h>
#include "pogls_1440.h"

static int _pass = 0, _fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS  %s\n", msg); _pass++; } \
    else      { printf("  FAIL  %s\n", msg); _fail++; } \
} while(0)

/* M01 */
static void m01(void) {
    printf("\nM01: constants\n");
    CHECK(POGLS_BASE_CYCLE == 720u,  "BASE_CYCLE = 720");
    CHECK(POGLS_EXT_CYCLE  == 1440u, "EXT_CYCLE  = 1440");
    CHECK(POGLS_EXT_CYCLE  == POGLS_BASE_CYCLE * 2u, "EXT = 2 × BASE");
}

/* M02 */
static void m02(void) {
    printf("\nM02: phase extraction\n");
    CHECK(pogls_dispatch_1440(0).phase   == 0u, "idx=0   → phase=0");
    CHECK(pogls_dispatch_1440(719).phase == 0u, "idx=719 → phase=0");
    CHECK(pogls_dispatch_1440(720).phase == 1u, "idx=720 → phase=1");
    CHECK(pogls_dispatch_1440(1439).phase== 1u, "idx=1439→ phase=1");

    /* idx720 wraps correctly */
    CHECK(pogls_dispatch_1440(720).idx720  == 0u,   "idx=720  → idx720=0");
    CHECK(pogls_dispatch_1440(1439).idx720 == 719u, "idx=1439 → idx720=719");
}

/* M03: backward compat — phase=0 must match tgw_dispatch_v2_phase(idx,0) */
static void m03(void) {
    printf("\nM03: backward compat (phase=0 == base)\n");
    int ok = 1;
    for (uint16_t i = 0; i < POGLS_BASE_CYCLE; i++) {
        uint8_t base = tgw_dispatch_v2_phase(i, 0);
        uint8_t wrap = pogls_dispatch_1440(i).bit6;
        if (base != wrap) { ok = 0; break; }
    }
    CHECK(ok, "all 720: dispatch_1440(i) == base_dispatch(i) for phase=0");
}

/* M04: phase 1 differs from phase 0 */
static void m04(void) {
    printf("\nM04: phase 1 differs from phase 0\n");
    int diff = 0;
    for (uint16_t i = 0; i < POGLS_BASE_CYCLE; i++) {
        uint8_t p0 = tgw_dispatch_v2_phase(i, 0);
        uint8_t p1 = tgw_dispatch_v2_phase(i, 1);
        if (p0 != p1) diff++;
    }
    printf("  INFO  diff count = %d / 720\n", diff);
    CHECK(diff > 0, "phase 1 produces different bit6 for at least 1 idx");
    /* phase XOR flips all bits → diff should be exactly 720 */
    CHECK(diff == 720, "phase XOR flips ALL 720 slots (diff=720)");
}

/* M05: full 1440 loop, bit6 always valid */
static void m05(void) {
    printf("\nM05: full 1440 loop — bit6 ∈ {0,1}\n");
    int ok = 1;
    uint32_t count0 = 0, count1 = 0;
    for (uint16_t i = 0; i < POGLS_EXT_CYCLE; i++) {
        Pogls1440Slot s = pogls_dispatch_1440(i);
        if (s.bit6 > 1u)           { ok = 0; break; }
        if (s.idx720 >= POGLS_BASE_CYCLE) { ok = 0; break; }
        if (s.phase  > 1u)         { ok = 0; break; }
        if (s.bit6 == 0) count0++; else count1++;
    }
    printf("  INFO  bit6=0: %u  bit6=1: %u  total: %u\n",
           count0, count1, count0 + count1);
    CHECK(ok, "all 1440 slots: bit6∈{0,1}, idx720<720, phase∈{0,1}");
    CHECK(count0 + count1 == POGLS_EXT_CYCLE, "total = 1440");
}

int main(void) {
    printf("=== 1440 Migration (Task #3) ===\n");
    printf("1440 = 2×720 | phase XOR overlay | core 720 untouched\n");

    m01(); m02(); m03(); m04(); m05();

    printf("\n=== RESULT: %d PASS  %d FAIL ===\n", _pass, _fail);
    if (_fail == 0)
        printf("\n✅ Task #3 verified — 1440 overlay locked, 720 intact\n");
    else
        printf("\n❌ Fix before Task #4\n");
    return _fail ? 1 : 0;
}
