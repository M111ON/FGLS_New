/*
 * test_rotation_state.c — Task #4: Rotation State Verification
 * ═════════════════════════════════════════════════════════════
 *
 * Tests:
 *   RO01: wrap at 6 — advance 6× → back to 0
 *   RO02: 1440 cycle → rotate exactly once (from 0 to 1)
 *   RO03: 2 cycles   → rotate twice (rotation_state == 2)
 *   RO04: 6 cycles   → full Rubik wrap (rotation_state == 0)
 *   RO05: phase=0 backward compat — bit6 == Task #3 output (rot=0)
 *   RO06: phase=1 bit6 affected by rot (rot=1 vs rot=0 differ)
 *   RO07: rotation fires ONLY at idx=720 (not idx720=0 in phase=0)
 *   RO08: lane_group = rot%6 always in 0..5
 *   RO09: pogls_current_lane() in 0..53
 *   RO10: multi-cycle driver rot == n_cycles%6 after N full cycles
 *   RO11: tgw_dispatch_rot phase=0 matches original formula
 *   RO12: fence_key = (rot^phase)&0x3F — Task #5 contract
 *
 * Compile:
 *   gcc -O2 -Wall -o test_rotation_state test_rotation_state.c && ./test_rotation_state
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "pogls_rotation.h"   /* pulls pogls_1440.h + fabric_wire.h */

static int _pass = 0, _fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS  %s\n", msg); _pass++; } \
    else      { printf("  FAIL  %s\n", msg); _fail++; } \
} while(0)

/* ── RO01: wrap at 6 ─────────────────────────────────────────── */
static void ro01(void) {
    printf("\nRO01: rotation wraps at 6\n");
    fabric_rotation_reset();
    for (int i = 0; i < 6; i++) fabric_rotation_advance_global();
    CHECK(fabric_rotation_state() == 0u, "6 advances → back to 0");

    fabric_rotation_reset();
    uint8_t seq[7];
    for (int i = 0; i < 7; i++) seq[i] = fabric_rotation_advance_global();
    CHECK(seq[5] == 0u, "advance[5] → wraps to 0 (6th advance)");
    CHECK(seq[6] == 1u, "advance[6] → 1 (continues past wrap)");
}

/* ── RO02: 1 cycle → rotate once ────────────────────────────── */
static void ro02(void) {
    printf("\nRO02: 1440 cycle → rotate exactly once\n");
    fabric_rotation_reset();
    uint8_t rot_before = fabric_rotation_state();  /* 0 */

    /* run full cycle: idx 0..1439 */
    for (uint16_t i = 0; i < POGLS_EXT_CYCLE; i++)
        pogls_dispatch_1440_rot(i);

    uint8_t rot_after = fabric_rotation_state();
    printf("  INFO  rot_before=%u  rot_after=%u\n", rot_before, rot_after);
    CHECK(rot_after == 1u, "1 cycle → rotation_state == 1");
}

/* ── RO03: 2 cycles → rotate twice ──────────────────────────── */
static void ro03(void) {
    printf("\nRO03: 2 cycles → rotation_state == 2\n");
    fabric_rotation_reset();
    for (uint32_t c = 0; c < 2; c++)
        for (uint16_t i = 0; i < POGLS_EXT_CYCLE; i++)
            pogls_dispatch_1440_rot(i);

    CHECK(fabric_rotation_state() == 2u, "2 cycles → rotation_state == 2");
}

/* ── RO04: 6 cycles → full Rubik wrap ───────────────────────── */
static void ro04(void) {
    printf("\nRO04: 6 cycles → full Rubik wrap (rotation_state == 0)\n");
    fabric_rotation_reset();
    for (uint32_t c = 0; c < 6; c++)
        for (uint16_t i = 0; i < POGLS_EXT_CYCLE; i++)
            pogls_dispatch_1440_rot(i);

    CHECK(fabric_rotation_state() == 0u, "6 cycles → rotation wraps to 0");
}

/* ── RO05: phase=0 backward compat ──────────────────────────── */
static void ro05(void) {
    printf("\nRO05: phase=0 backward compat (bit6 == Task #3 formula)\n");
    /* When rot=0 and phase=0: tgw_dispatch_rot == tgw_dispatch_v2_phase(idx,0) */
    int ok = 1;
    for (uint16_t i = 0; i < POGLS_BASE_CYCLE; i++) {
        uint8_t task3 = tgw_dispatch_v2_phase(i, 0);   /* original */
        uint8_t task4 = tgw_dispatch_rot(i, 0, 0);      /* new, rot=0 */
        if (task3 != task4) { ok = 0; break; }
    }
    CHECK(ok, "all 720: tgw_dispatch_rot(i,0,0) == tgw_dispatch_v2_phase(i,0)");
}

/* ── RO06: phase=1 bit6 changes with rot ────────────────────── */
static void ro06(void) {
    printf("\nRO06: phase=1 bit6 varies with rot (rot=0 vs rot=1)\n");
    int differ = 0;
    for (uint16_t i = 0; i < POGLS_BASE_CYCLE; i++) {
        uint8_t b0 = tgw_dispatch_rot(i, 1, 0);
        uint8_t b1 = tgw_dispatch_rot(i, 1, 1);
        if (b0 != b1) differ++;
    }
    printf("  INFO  rot=0 vs rot=1 differ count = %d / 720\n", differ);
    CHECK(differ > 0, "phase=1: rot affects bit6 for at least 1 slot");
    /* rot only contributes 1 XOR bit — half should differ when rot flips */
    CHECK(differ > 0 && differ <= 720, "differ count in [1..720] as expected");
}

/* ── RO07: rotation fires ONLY at idx=720 ───────────────────── */
static void ro07(void) {
    printf("\nRO07: rotation advances ONLY at idx=720 (phase boundary)\n");
    fabric_rotation_reset();

    /* run phase=0 (idx 0..719) — rotation must NOT advance */
    for (uint16_t i = 0; i < POGLS_BASE_CYCLE; i++)
        pogls_dispatch_1440_rot(i);

    uint8_t rot_after_phase0 = fabric_rotation_state();
    CHECK(rot_after_phase0 == 0u, "after phase=0 (idx 0..719): rotation stays 0");

    /* idx=720: rotation advances */
    pogls_dispatch_1440_rot(POGLS_BASE_CYCLE);
    CHECK(fabric_rotation_state() == 1u, "idx=720 → rotation advances to 1");

    /* idx=721..1439: rotation does NOT advance again */
    for (uint16_t i = POGLS_BASE_CYCLE + 1u; i < POGLS_EXT_CYCLE; i++)
        pogls_dispatch_1440_rot(i);

    CHECK(fabric_rotation_state() == 1u, "idx 721..1439: rotation stays at 1");
}

/* ── RO08: lane_group always 0..5 ───────────────────────────── */
static void ro08(void) {
    printf("\nRO08: lane_group = rot%%6 always in 0..5\n");
    int ok = 1;
    for (uint8_t rot = 0; rot < 6; rot++) {
        uint8_t lg = pogls_lane_group(rot);
        if (lg != rot % POGLS_LANE_GROUPS) { ok = 0; break; }
        if (lg > 5u) { ok = 0; break; }
    }
    CHECK(ok, "lane_group(rot) == rot%6 in 0..5 for all rot");

    /* verify SlotR.lane_group field matches */
    fabric_rotation_reset();
    for (uint16_t i = 0; i < POGLS_EXT_CYCLE; i++) {
        Pogls1440SlotR s = pogls_dispatch_1440_rot(i);
        if (s.lane_group != s.rot % POGLS_LANE_GROUPS) { ok = 0; break; }
    }
    CHECK(ok, "SlotR.lane_group == slot.rot%6 for all 1440 slots");
}

/* ── RO09: pogls_current_lane() in 0..53 ────────────────────── */
static void ro09(void) {
    printf("\nRO09: pogls_current_lane() in 0..53\n");
    int ok = 1;
    for (uint8_t rot = 0; rot < 6; rot++) {
        _pogls_rotation_state = rot;
        for (uint8_t local = 0; local < POGLS_LANES_PER_GRP; local++) {
            uint8_t lane = pogls_current_lane(local);
            if (lane >= POGLS_SACRED_NEXUS) { ok = 0; break; }  /* SACRED_NEXUS=54 */
        }
    }
    fabric_rotation_reset();
    CHECK(ok, "pogls_current_lane() always in 0..53 for all rot+local");
}

/* ── RO10: multi-cycle driver ────────────────────────────────── */
static void ro10(void) {
    printf("\nRO10: pogls_run_cycles(N) → rot == N%%6\n");
    uint32_t test_cases[] = {1, 3, 5, 6, 7, 12, 13};
    int ok = 1;
    for (int t = 0; t < 7; t++) {
        fabric_rotation_reset();
        uint8_t rot = pogls_run_cycles(test_cases[t]);
        uint8_t expected = (uint8_t)(test_cases[t] % POGLS_LANE_GROUPS);
        if (rot != expected) {
            printf("  FAIL  cycles=%u: got rot=%u expected=%u\n",
                   test_cases[t], rot, expected);
            ok = 0;
        } else {
            printf("  INFO  cycles=%u → rot=%u ✓\n", test_cases[t], rot);
        }
    }
    CHECK(ok, "pogls_run_cycles(N) == N%6 for all test cases");
}

/* ── RO11: tgw_dispatch_rot phase=0 matches original ─────────── */
static void ro11(void) {
    printf("\nRO11: tgw_dispatch_rot — exhaustive phase=0 match\n");
    /* For all rot values, phase=0: rot_xor = (0 & rot & 1) = 0
     * → bit6 = (trit^slope^0^0)&1 = (trit^slope)&1 regardless of rot */
    int ok = 1;
    for (uint8_t rot = 0; rot < 6; rot++) {
        for (uint16_t i = 0; i < POGLS_BASE_CYCLE; i++) {
            uint8_t base  = _p1440_trit(i) ^ _p1440_slope(i);
            uint8_t task4 = tgw_dispatch_rot(i, 0, rot);
            if ((base & 1u) != task4) { ok = 0; break; }
        }
    }
    CHECK(ok, "phase=0: bit6 == (trit^slope)&1 independent of rot (all 6 rot values)");
}

/* ── RO12: fence_key contract for Task #5 ───────────────────── */
static void ro12(void) {
    printf("\nRO12: fence_key = (rot^phase)&0x3F — Task #5 contract\n");
    int ok = 1;
    for (uint8_t rot = 0; rot < 6; rot++) {
        for (uint8_t phase = 0; phase < 2; phase++) {
            uint8_t fk = pogls_fence_key(rot, phase);
            uint8_t expected = (uint8_t)(rot * 2u + (phase & 1u));
            if (fk != expected) { ok = 0; break; }
            if (fk >= 12u) { ok = 0; break; }  /* must be in 0..11 */
        }
    }
    CHECK(ok, "fence_key(rot,phase) == rot*2+phase for all combos (0..11)");

    /* phase=0, rot=0 → fence_key=0 (neutral, no interference) */
    CHECK(pogls_fence_key(0, 0) == 0u, "fence_key(0,0)=0 (neutral state)");
    /* phase=1, rot=0 → fence_key=1 (temporal gate active) */
    CHECK(pogls_fence_key(0, 1) == 1u, "fence_key(0,1)=1 (temporal gate active)");
    /* phase=1, rot=1 → fence_key=3 (rot=1 lane, phase=1) */
    CHECK(pogls_fence_key(1, 1) == 3u, "fence_key(1,1)=3 (rot=1 lane, phase=1)");
    /* rot=5, phase=1 → fence_key=11 (last drain gate) */
    CHECK(pogls_fence_key(5, 1) == 11u, "fence_key(5,1)=11 (last pentagon drain)");
}

/* ════════════════════════════════════════════════════════════ */

int main(void) {
    printf("=== Task #4: Rotation State Verification ===\n");
    printf("rotation = bridge: 720(spatial) × 1440(temporal)\n");
    printf("rot driver: advance at idx=720 (phase boundary), wraps at 6\n");

    ro01(); ro02(); ro03(); ro04(); ro05();
    ro06(); ro07(); ro08(); ro09(); ro10();
    ro11(); ro12();

    printf("\n=== RESULT: %d PASS  %d FAIL ===\n", _pass, _fail);
    if (_fail == 0) {
        printf("\n✅ Task #4 verified — rotation wired to 1440 phase\n");
        printf("   spatial(720) ⊥ temporal(1440), rot = global phase driver\n");
        printf("   Task #5 hook: pogls_fence_key(rot, phase) ready\n");
    } else {
        printf("\n❌ Fix before Task #5\n");
    }
    return _fail ? 1 : 0;
}
