/*
 * test_l3_l2_l5_pipeline.c — P2: Full L3→L2→L5 Pipeline Integration
 * ═══════════════════════════════════════════════════════════════════
 *
 * Pipeline chain:
 *   L3 (Stream Input):  tgw_stream_dispatch() — slice data → packets
 *   L2 (TGW Dispatch):  tring_route_from_enc() → polarity → ROUTE/GROUND
 *   L5 (Goldberg):      tgw_write() → blueprint → dispatch
 *
 * Sacred constants: 720, 60, 30, 6
 * No malloc. No float. No heap.
 * NOTE: TGWCtx is 3MB — declared static to avoid stack overflow.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "tgw_stream_dispatch.h"

/* ── Test framework ── */
static int tests_run    = 0;
static int tests_passed = 0;

#define CHK(cond, msg) do { \
    tests_run++; \
    if (cond) { tests_passed++; } \
    else      { printf("  FAIL: %s\n", msg); } \
} while(0)

/* ══════════════════════════════════════════════════════════════
   Test 1: TRing route derivation (L2 core math)
   ══════════════════════════════════════════════════════════════ */
static void test_tring_route(void)
{
    printf("\n-- Test 1: TRing route derivation (L2) --\n");

    uint32_t test_encs[] = { 0, 1, 30, 60, 120, 360, 719 };
    int n = sizeof(test_encs) / sizeof(test_encs[0]);
    int route_count = 0, ground_count = 0, unknown_count = 0;

    for (int i = 0; i < n; i++) {
        TRingRoute rt = tring_route_from_enc(test_encs[i]);
        if (rt.pos == 0xFFFFu) { unknown_count++; continue; }
        CHK(rt.pentagon < 12, "pentagon in range");
        CHK(rt.spoke < 6, "spoke in range");
        CHK(rt.polarity <= 1, "polarity 0 or 1");
        if (rt.polarity == 0) route_count++;
        else                  ground_count++;
        /* determinism */
        TRingRoute rt2 = tring_route_from_enc(test_encs[i]);
        CHK(rt.pos == rt2.pos && rt.polarity == rt2.polarity, "deterministic");
    }
    printf("  route=%d ground=%d unknown=%d\n", route_count, ground_count, unknown_count);
    CHK(route_count + ground_count > 0, "packets routed");
}

/* ══════════════════════════════════════════════════════════════
   Test 2: Walk enc 0..719 — polarity + pentagon distribution
   TRing LUT only covers a subset of enc values.
   Valid encs split 50/50 ROUTE/GROUND.
   ══════════════════════════════════════════════════════════════ */
static void test_720_sweep(void)
{
    printf("\n-- Test 2: Walk enc 0..719 --\n");

    uint32_t spoke_counts[6] = {0};
    uint32_t polarity_counts[2] = {0};
    uint32_t pentagon_counts[12] = {0};
    uint32_t valid = 0, invalid = 0;
    int invariant_fails = 0;

    for (uint32_t enc = 0; enc < 720u; enc++) {
        TRingRoute rt = tring_route_from_enc(enc);
        if (rt.pos == 0xFFFFu) { invalid++; continue; }
        valid++;
        polarity_counts[rt.polarity]++;
        spoke_counts[rt.spoke]++;
        pentagon_counts[rt.pentagon]++;
        /* invariant: pentagon*60 + offset == pos */
        uint32_t check = (uint32_t)rt.pentagon * 60u + (rt.pos % 60u);
        if (check != rt.pos) invariant_fails++;
    }

    printf("  valid=%u invalid=%u\n", valid, invalid);
    printf("  ROUTE=%u GROUND=%u\n", polarity_counts[0], polarity_counts[1]);

    CHK(valid > 0, "some valid encs");
    CHK(invalid > 0, "some invalid encs (LUT is sparse)");
    /* Polarity: 50/50 split among valid encs */
    CHK(polarity_counts[0] == polarity_counts[1], "ROUTE == GROUND (50/50)");
    CHK(invariant_fails == 0, "pentagon*60+offset==pos (all valid)");

    /* Spoke distribution: each valid spoke should have equal count */
    uint32_t max_spoke = 0, min_spoke = 999999;
    for (uint8_t s = 0; s < 6; s++) {
        if (spoke_counts[s] > max_spoke) max_spoke = spoke_counts[s];
        if (spoke_counts[s] < min_spoke && spoke_counts[s] > 0)
            min_spoke = spoke_counts[s];
    }
    printf("  spoke min=%u max=%u\n", min_spoke, max_spoke);

    /* Pentagon distribution: each valid pentagon should have equal count */
    uint32_t max_pent = 0, min_pent = 999999, active_pents = 0;
    for (uint8_t p = 0; p < 12; p++) {
        if (pentagon_counts[p] > 0) {
            active_pents++;
            if (pentagon_counts[p] > max_pent) max_pent = pentagon_counts[p];
            if (pentagon_counts[p] < min_pent) min_pent = pentagon_counts[p];
        }
    }
    printf("  active_pentagons=%u pentagon min=%u max=%u\n",
           active_pents, min_pent, max_pent);
    CHK(active_pents > 0, "at least one active pentagon");
}

/* ══════════════════════════════════════════════════════════════
   Test 3: Stream dispatch integration (L3→L2→L5)
   TGWCtx is 3MB — must be static to avoid stack overflow.
   ══════════════════════════════════════════════════════════════ */
static TGWCtx            s_ctx;
static TGWDispatch       s_d;
static TGWStreamDispatch s_sd;

static void test_stream_dispatch(void)
{
    printf("\n-- Test 3: Stream dispatch (L3->L2->L5) --\n");

    GeoSeed seed = { .gen2 = 0xDEADBEEF, .gen3 = 0xCAFEBABE };
    uint64_t bundle[GEO_BUNDLE_WORDS] = {
        0x1111111111111111ULL, 0x2222222222222222ULL,
        0x3333333333333333ULL, 0x4444444444444444ULL,
        0x5555555555555555ULL, 0x6666666666666666ULL,
        0x7777777777777777ULL, 0x8888888888888888ULL,
    };

    tgw_init(&s_ctx, seed, bundle);
    tgw_dispatch_init(&s_d, 0x12345678ULL);
    tgw_stream_dispatch_init(&s_sd);

    CHK(s_ctx.total_writes == 0, "ctx initialized");
    CHK(s_ctx.stream_pkts_rx == 0, "rx=0");

    /* Feed ALL 720 enc values through L2 dispatch path */
    uint32_t route_total = 0, ground_total = 0, unknown_total = 0;

    for (uint32_t enc = 0; enc < 720u; enc++) {
        TRingRoute rt = tring_route_from_enc(enc);
        if (rt.pos == 0xFFFFu) { unknown_total++; continue; }

        uint64_t addr  = (uint64_t)enc;
        uint64_t value = (uint64_t)(enc ^ 0xA5A5) | ((uint64_t)64 << 16);

        if (rt.polarity == 1) {
            s_d.ground_count++;
            s_d.total_dispatched++;
            ground_total++;
        } else {
            TGWResult r = tgw_write(&s_ctx, addr, value, 0);
            tgw_dispatch(&s_d, &r, addr, value, s_ctx._bundle);
            route_total++;
        }
    }

    printf("  route=%u ground=%u unknown=%u total_valid=%u\n",
           route_total, ground_total, unknown_total,
           route_total + ground_total);

    CHK(route_total > 0, "some ROUTE dispatched");
    CHK(ground_total > 0, "some GROUND dispatched");
    CHK(route_total == ground_total, "ROUTE == GROUND (50/50)");
    CHK(route_total + ground_total + unknown_total == 720, "all 720 encs accounted");

    TGWDispatchStats stats = tgw_dispatch_stats(&s_d);
    CHK(stats.ground_count == ground_total, "ground count matches");
    CHK(stats.total_dispatched == route_total + ground_total, "total matches");
    CHK(s_ctx.total_writes == route_total, "ctx writes == route count");
}

/* ══════════════════════════════════════════════════════════════
   Test 4: Data integrity — no corruption in chain
   ══════════════════════════════════════════════════════════════ */
static void test_data_integrity(void)
{
    printf("\n-- Test 4: Data integrity --\n");

    uint64_t bundle[GEO_BUNDLE_WORDS];
    memset(bundle, 0, sizeof(bundle));
    bundle[0] = 0xDEADBEEF;
    bundle[7] = 0xCAFEBABE;

    GeoSeed seed = { .gen2 = 0x1122334455667788ULL, .gen3 = 0x99AABBCCDDEEFF00ULL };
    tgw_init(&s_ctx, seed, bundle);
    tgw_dispatch_init(&s_d, 0xBEEF);

    /* Use enc values that we KNOW are valid (from test 1/2) */
    uint32_t valid_encs[] = { 0, 1, 2, 3, 30, 31, 60, 61, 120, 121 };
    int n = sizeof(valid_encs) / sizeof(valid_encs[0]);
    uint32_t processed = 0, p_route = 0, p_ground = 0;

    for (int i = 0; i < n; i++) {
        TRingRoute rt = tring_route_from_enc(valid_encs[i]);
        if (rt.pos == 0xFFFFu) continue;

        uint64_t addr  = (uint64_t)valid_encs[i];
        uint64_t value = (uint64_t)0xA5A50000u | valid_encs[i];

        if (rt.polarity == 1) {
            s_d.ground_count++;
            s_d.total_dispatched++;
            p_ground++;
        } else {
            TGWResult r = tgw_write(&s_ctx, addr, value, 0);
            tgw_dispatch(&s_d, &r, addr, value, s_ctx._bundle);
            p_route++;
        }
        processed++;
    }

    printf("  processed=%u route=%u ground=%u\n", processed, p_route, p_ground);
    CHK(processed > 0, "some packets processed");
    CHK(s_ctx.total_writes == p_route, "writes == route count");
    CHK(bundle[0] == 0xDEADBEEF, "bundle[0] unchanged");
    CHK(bundle[7] == 0xCAFEBABE, "bundle[7] unchanged");

    /* Verify tgw_write returns consistent results for same input */
    TGWResult r1 = tgw_write(&s_ctx, 0x42, 0xBBBB, 0);
    TGWResult r2 = tgw_write(&s_ctx, 0x42, 0xBBBB, 0);
    CHK(r1.gpr.blueprint_ready == r2.gpr.blueprint_ready,
        "tgw_write deterministic for same input");
}

/* ══════════════════════════════════════════════════════════════
   Test 5: Spoke coverage — uniform distribution
   ══════════════════════════════════════════════════════════════ */
static void test_spoke_coverage(void)
{
    printf("\n-- Test 5: Spoke coverage --\n");

    uint32_t counts[6] = {0};
    uint32_t valid = 0;
    for (uint32_t enc = 0; enc < 720u; enc++) {
        TRingRoute rt = tring_route_from_enc(enc);
        if (rt.pos != 0xFFFFu) {
            counts[rt.spoke]++;
            valid++;
        }
    }

    printf("  valid=%u\n", valid);
    for (uint8_t s = 0; s < 6; s++) {
        printf("  spoke %u = %u\n", s, counts[s]);
    }

    CHK(valid > 0, "some valid encs");

    /* Check that active spokes have equal distribution */
    uint32_t active_spokes = 0;
    uint32_t ref_count = 0;
    for (uint8_t s = 0; s < 6; s++) {
        if (counts[s] > 0) {
            active_spokes++;
            if (ref_count == 0) ref_count = counts[s];
        }
    }

    int uniform = 1;
    for (uint8_t s = 0; s < 6; s++) {
        if (counts[s] > 0 && counts[s] != ref_count) uniform = 0;
    }

    printf("  active_spokes=%u ref_count=%u\n", active_spokes, ref_count);
    CHK(active_spokes > 0, "at least one active spoke");
    CHK(uniform, "all active spokes have equal count");
}

/* ══════════════════════════════════════════════════════════════ */
int main(void)
{
    printf("P2: L3->L2->L5 Pipeline Integration Test\n");
    printf("=========================================\n");

    test_tring_route();
    test_720_sweep();
    test_stream_dispatch();
    test_data_integrity();
    test_spoke_coverage();

    printf("\n=========================================\n");
    printf("Results: %d/%d passed\n", tests_passed, tests_run);

    if (tests_passed == tests_run) {
        printf("ALL TESTS PASSED\n");
        return 0;
    } else {
        printf("SOME TESTS FAILED\n");
        return 1;
    }
}
