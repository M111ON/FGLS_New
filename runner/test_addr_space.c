/*
 * test_addr_space.c — Verify Power-of-N address space system
 *
 * Build: gcc -O2 -std=c11 -I. -o test_addr_space.exe test_addr_space.c
 */

#include <stdio.h>
#include <assert.h>
#include <string.h>
#include "addr_space.h"

#define PASS(name) printf("  PASS: %s\n", name)
#define FAIL(name, msg) printf("  FAIL: %s — %s\n", name, msg)

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { \
    tests_run++; \
    printf("[%d] %s\n", tests_run, name); \
} while(0)

#define CHECK(cond, name, msg) do { \
    if (cond) { tests_passed++; PASS(name); } \
    else { FAIL(name, msg); } \
} while(0)

/* ── Test 1: Core constants ── */
static void test_constants(void) {
    TEST("Core geometry constants");

    CHECK(ADDR_DIM_A == 128, "DIM_A=128", "expected128");
    CHECK(ADDR_DIM_B == 162, "DIM_B=162", "expected162");
    CHECK(ADDR_BASE == 20736, "BASE=20736", "expected 20736");
    CHECK(ADDR_N == 144, "N=144", "expected 144");
    CHECK(ADDR_BASE == ADDR_N * ADDR_N, "BASE=N^2", "144^2 != 20736");
    CHECK(128 * 162 == 20736, "128*162=20736", "math check");
}

/* ── Test 2: Tier capacities ── */
static void test_tier_capacities(void) {
    TEST("Tier capacities");

    CHECK(addr_tier_capacity(0) == 20736ULL, "Tier0 cap", "expected 20736");
    CHECK(addr_tier_capacity(1) == 429981696ULL, "Tier1 cap", "expected 429981696");

    /* Verify Tier1 = Tier0^2 */
    CHECK(addr_tier_capacity(1) == 20736ULL * 20736ULL, "Tier1 = Tier0^2", "20736^2 mismatch");

    /* Tier2 should be Tier0^3 */
    uint64_t t2 = addr_tier_capacity(2);
    uint64_t t2_expected = 20736ULL * 20736ULL * 20736ULL;
    CHECK(t2 == t2_expected, "Tier2 = Tier0^3", "20736^3 mismatch");
}

/* ── Test 3: Tier selection ── */
static void test_tier_selection(void) {
    TEST("Tier selection by model size");

    /* All current models fit in Tier0 */
    CHECK(addr_select_tier(290, 960) == 0, "SmolLM2->Tier0", "290 tensors");
    CHECK(addr_select_tier(148, 2048) == 0, "LFM2->Tier0", "148 tensors");
    CHECK(addr_select_tier(291, 4096) == 0, "Llama3-8B->Tier0", "291 tensors");
    CHECK(addr_select_tier(800, 16384) == 0, "Many tensors->Tier0", "800 tensors < 20736");

    /* Hypothetical massive model */
    CHECK(addr_select_tier(30000, 7168) == 1, "Huge->Tier1", "30000 > 20736");
}

/* ── Test 4: Address composition/decomposition roundtrip ── */
static void test_roundtrip(void) {
    TEST("Address compose/decompose roundtrip");

    for (uint8_t tier = 0; tier <= 1; tier++) {
        uint32_t cap = (tier == 0) ? 20736u : 429981696u;

        /* Test a sample of addresses */
        uint32_t addrs[] = {0, 1, 127, 128, 255, 256, 1000, 20735, 20736};
        int n = (tier == 0) ? 9 : 5; /* Tier1: skip addresses > capacity */

        for (int i = 0; i < n; i++) {
            uint32_t a = addrs[i];
            if (a >= cap) continue;

            AddrDecomp d = addr_decompose(a, tier);
            uint32_t recomposed = addr_compose(d.macro, d.micro, tier);

            char buf[64];
            snprintf(buf, sizeof(buf), "tier%d addr=%u roundtrip", tier, a);
            CHECK(recomposed == a, buf, "recomposed != original");
        }
    }
}

/* ── Test 5: Macro/micro bit ranges ── */
static void test_bit_ranges(void) {
    TEST("Macro/micro bit ranges");

    for (uint8_t tier = 0; tier <= 1; tier++) {
        const AddrTier *t = &ADDR_TIERS[tier];

        /* Macro should fit in macro_bits */
        CHECK(t->macro_slots > 0, "macro_slots > 0", "zero macro slots");

        /* Micro should fit in micro_bits */
        CHECK(t->micro_slots > 0, "micro_slots > 0", "zero micro slots");

        /* Total bits should be correct */
        uint64_t cap = addr_tier_capacity(tier);
        uint8_t needed = 0;
        uint64_t v = cap;
        while (v > 0) { needed++; v >>= 1; }

        char buf[64];
        snprintf(buf, sizeof(buf), "Tier%d total_bits", tier);
        CHECK(t->total_bits >= needed - 1 && t->total_bits <= needed + 1, buf, "bits mismatch");

        /* Macro x micro should >= capacity */
        uint64_t macro_micro = (uint64_t)t->macro_slots * t->micro_slots;
        snprintf(buf, sizeof(buf), "Tier%d macro*micro >= cap", tier);
        CHECK(macro_micro >= cap, buf, "macro*micro < capacity");
    }
}

/* ── Test 6: Tensor name -> address mapping ── */
static void test_tensor_mapping(void) {
    TEST("Tensor name -> address mapping");

    /* Different names should give different addresses */
    uint32_t a1 = addr_from_tensor_name("blk.0.attn_q", 0);
    uint32_t a2 = addr_from_tensor_name("blk.0.attn_k", 0);
    uint32_t a3 = addr_from_tensor_name("blk.1.attn_q", 0);
    uint32_t a4 = addr_from_tensor_name("token_embd.weight", 0);

    CHECK(a1 != a2, "q != k within same layer", "same addr for q and k");
    CHECK(a1 != a3, "blk.0 != blk.1", "same addr for different layers");
    CHECK(a1 != a4, "block != embedding", "same addr for block and embedding");

    /* All should be within capacity */
    CHECK(addr_valid(a1, 0), "blk.0.attn_q valid", "addr out of range");
    CHECK(addr_valid(a2, 0), "blk.0.attn_k valid", "addr out of range");
    CHECK(addr_valid(a3, 0), "blk.1.attn_q valid", "addr out of range");
    CHECK(addr_valid(a4, 0), "token_embd valid", "addr out of range");

    printf("    blk.0.attn_q  = %u\n", a1);
    printf("    blk.0.attn_k  = %u\n", a2);
    printf("    blk.1.attn_q  = %u\n", a3);
    printf("    token_embd    = %u\n", a4);
}

/* ── Test 7: Capo face rotation ── */
static void test_capo(void) {
    TEST("Capo face rotation");

    uint32_t base = 1000;
    for (uint32_t f = 0; f < 12; f++) {
        uint32_t rotated = addr_capo(base, f, 0);
        CHECK(addr_valid(rotated, 0), "rotated addr valid", "out of range");

        /* Different faces should give different addresses */
        if (f > 0) {
            uint32_t prev = addr_capo(base, f - 1, 0);
            char buf[64];
            snprintf(buf, sizeof(buf), "face %u != face %u", f, f - 1);
            CHECK(rotated != prev, buf, "same address for different faces");
        }
    }

    /* Face 0 = base (no rotation) */
    CHECK(addr_capo(base, 0, 0) == base, "face 0 = base", "face 0 should be identity");
}

/* ── Test 8: Backward compatibility with GEO_FULL=20736 ── */
static void test_backward_compat(void) {
    TEST("Backward compatibility with GEO_FULL");

    /* The old system used GEO_FULL=20736, CAPO_TOWER=144, CAPO_STRIDE=12 */
    CHECK(ADDR_BASE == 20736, "ADDR_BASE = old GEO_FULL", "mismatch");
    CHECK(ADDR_TOWER == 144, "ADDR_TOWER = old CAPO_TOWER", "mismatch");
    CHECK(ADDR_STRIDE == 12, "ADDR_STRIDE = old CAPO_STRIDE", "mismatch");

    /* Old capo formula: (base + f * 12 * 144) % 20736 */
    uint32_t old_base = 5000;
    for (uint32_t f = 0; f < 12; f++) {
        uint32_t old_addr = (old_base + f * 12 * 144) % 20736;
        uint32_t new_addr = addr_capo(old_base, f, 0);
        char buf[64];
        snprintf(buf, sizeof(buf), "capo face %u compat", f);
        CHECK(old_addr == new_addr, buf, "capo formula mismatch");
    }
}

/* ── Test 9: Geometry decomposition ── */
static void test_geometry(void) {
    TEST("Geometry decomposition (spoke/layer/slot)");

    /* Tier0: addr -> (layer, slot) = (addr/128, addr%128)
     *   layer range: 0-161 (20736/128 = 162)
     *   slot range:  0-127
     */
    for (uint32_t addr = 0; addr < 20736; addr += 128) {
        GeoDecomp g = addr_to_geo(addr, 0);
        CHECK(g.spoke < 162, "layer < 162", "layer out of range");
        CHECK(g.slot < 128, "slot < 128", "slot out of range");
    }

    /* Tier0: addr -> (layer, slot) mapping:
     *   layer = addr / 128, slot = addr % 128
     *   addr 0    -> layer 0, slot 0
     *   addr 127  -> layer 0, slot 127
     *   addr 128  -> layer 1, slot 0
     *   addr 20735 -> layer 161, slot 127
     */
    GeoDecomp g0 = addr_to_geo(0, 0);
    CHECK(g0.spoke == 0 && g0.slot == 0, "addr 0 -> layer 0 slot 0", "wrong position");

    GeoDecomp g127 = addr_to_geo(127, 0);
    CHECK(g127.spoke == 0 && g127.slot == 127, "addr 127 -> layer 0 slot 127", "wrong position");

    GeoDecomp g128 = addr_to_geo(128, 0);
    CHECK(g128.spoke == 1 && g128.slot == 0, "addr 128 -> layer 1 slot 0", "wrong position");
}

/* ══════════════════════════════════════════════════════════════════ */
int main(void) {
    printf("=== addr_space.h Test Suite ===\n\n");

    test_constants();
    test_tier_capacities();
    test_tier_selection();
    test_roundtrip();
    test_bit_ranges();
    test_tensor_mapping();
    test_capo();
    test_backward_compat();
    test_geometry();

    printf("\n=== Results: %d/%d passed ===\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
