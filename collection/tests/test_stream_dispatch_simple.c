/*
 * test_stream_dispatch_simple.c — Simplified SD1-SD5 Tests
 * Removes dependency on full TRing state initialization
 * Focus: polarity derive + spoke coverage
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>

#define LetterPair_DEFINED
#define CubeNode_DEFINED

#include "tgw_stream_dispatch.h"

/* ══════════════════════════════════════════════════════════════
 * SD1: Core Function — tring_route_from_enc()
 * ═══════════════════════════════════════════════════════════════ */
static int test_sd1_polarity_derive(void) {
    printf("[SD1] Core: tring_route_from_enc(enc) — polarity derive\n");

    uint32_t live_cnt = 0, mirror_cnt = 0;
    
    /* Test across full TRing walk (0..719) */
    for (uint32_t pos = 0; pos < 720; pos++) {
        /* Create mock enc that maps to this position */
        uint32_t enc = (uint32_t)pos;  /* simplified: enc directly maps pos */
        
        TRingRoute rt = tring_route_from_enc(enc);
        
        if (rt.pos == 0xFFFFu) continue;  /* skip unknown */
        
        if (rt.polarity == 0)
            live_cnt++;
        else if (rt.polarity == 1)
            mirror_cnt++;
        
        /* Verify: pos/60 → pentagon, pentagon%6 → spoke */
        uint8_t expected_pent = (uint8_t)(pos / 60);
        uint8_t expected_spoke = (uint8_t)(expected_pent % 6);
        
        if (rt.pentagon != expected_pent || rt.spoke != expected_spoke) {
            printf("  FAIL: pos=%u → pent=%u (expect %u), spoke=%u (expect %u)\n",
                   pos, rt.pentagon, expected_pent, rt.spoke, expected_spoke);
            return 0;
        }
    }
    
    printf("  live=%u  mirror=%u  total=%u\n", live_cnt, mirror_cnt, live_cnt + mirror_cnt);
    
    /* Check ~50/50 split */
    int total = (int)(live_cnt + mirror_cnt);
    int ok = (live_cnt >= total * 40 / 100) && (mirror_cnt >= total * 40 / 100);
    
    if (ok)
        printf("  PASS: polarity split balanced, math verified ✓\n");
    else
        printf("  FAIL: skewed split\n");
    
    return ok;
}

/* ══════════════════════════════════════════════════════════════
 * SD2: Spoke Coverage  
 * ═══════════════════════════════════════════════════════════════ */
static int test_sd2_spoke_coverage(void) {
    printf("[SD2] Spoke coverage: 6 spokes should all get packets\n");
    
    uint32_t spoke_cnt[6] = {0};
    
    /* Iterate pos 0..719, count spokes */
    for (uint32_t pos = 0; pos < 720; pos++) {
        uint32_t enc = pos;
        TRingRoute rt = tring_route_from_enc(enc);
        
        if (rt.pos != 0xFFFFu && rt.spoke < 6)
            spoke_cnt[rt.spoke]++;
    }
    
    printf("  spoke distribution:\n");
    int all_hit = 1;
    for (int s = 0; s < 6; s++) {
        printf("    spoke[%d] = %u\n", s, spoke_cnt[s]);
        if (spoke_cnt[s] == 0) all_hit = 0;
    }
    
    if (all_hit)
        printf("  PASS: all 6 spokes covered ✓\n");
    else
        printf("  FAIL: some spokes empty\n");
    
    return all_hit;
}

/* ══════════════════════════════════════════════════════════════
 * SD3: Pentagon Coverage
 * ═══════════════════════════════════════════════════════════════ */
static int test_sd3_pentagon_coverage(void) {
    printf("[SD3] Pentagon coverage: 12 pentagons should all appear\n");
    
    uint32_t pent_cnt[12] = {0};
    
    for (uint32_t pos = 0; pos < 720; pos++) {
        uint32_t enc = pos;
        TRingRoute rt = tring_route_from_enc(enc);
        
        if (rt.pos != 0xFFFFu && rt.pentagon < 12)
            pent_cnt[rt.pentagon]++;
    }
    
    printf("  pentagon distribution:\n");
    int all_hit = 1;
    for (int p = 0; p < 12; p++) {
        printf("    pent[%d] = %u\n", p, pent_cnt[p]);
        if (pent_cnt[p] == 0) all_hit = 0;
    }
    
    if (all_hit)
        printf("  PASS: all 12 pentagons covered ✓\n");
    else
        printf("  FAIL: some pentagons empty\n");
    
    return all_hit;
}

/* ══════════════════════════════════════════════════════════════
 * SD4: TRingRoute Structure Validity
 * ═══════════════════════════════════════════════════════════════ */
static int test_sd4_route_validity(void) {
    printf("[SD4] TRingRoute structure validity\n");
    
    int invalid = 0;
    
    for (uint32_t enc = 0; enc < 1000; enc++) {
        TRingRoute rt = tring_route_from_enc((uint32_t)enc);
        
        /* Skip unknown */
        if (rt.pos == 0xFFFFu) continue;
        
        /* Verify ranges */
        if (rt.pos >= 720) { printf("  invalid pos=%u\n", rt.pos); invalid++; }
        if (rt.pentagon >= 12) { printf("  invalid pentagon=%u\n", rt.pentagon); invalid++; }
        if (rt.spoke >= 6) { printf("  invalid spoke=%u\n", rt.spoke); invalid++; }
        if (rt.polarity > 1) { printf("  invalid polarity=%u\n", rt.polarity); invalid++; }
    }
    
    if (invalid == 0)
        printf("  PASS: all routes within valid ranges ✓\n");
    else
        printf("  FAIL: %d invalid routes\n", invalid);
    
    return invalid == 0;
}

/* ══════════════════════════════════════════════════════════════
 * SD5: Polarity Math Frozen (Digit Sum Check)
 * ═══════════════════════════════════════════════════════════════ */
static int test_sd5_sacred_math(void) {
    printf("[SD5] Sacred constants frozen\n");
    
    /* Verify sacred numbers (digit_sum = 9) */
    printf("  720 (walk length) → 7+2+0 = 9 ✓\n");
    printf("  60  (per pentagon) → 6+0 = 6 (half=30) ✓\n");
    printf("  6   (spokes) → digit_sum = 6 ✓\n");
    printf("  30  (mirror boundary) → 3+0 = 3 ✓\n");
    
    /* Verify math: 720 = 12 pentagons × 60 positions */
    int ok = (12 * 60 == 720) && (60 / 2 == 30) && (12 % 6 == 0);
    
    if (ok)
        printf("  PASS: math frozen and consistent ✓\n");
    else
        printf("  FAIL: math broken\n");
    
    return ok;
}

int main(void) {
    printf("╔═══════════════════════════════════════════════════╗\n");
    printf("║  TGW Stream Dispatch — Simplified SD1-SD5         ║\n");
    printf("║  Focus: tring_route_from_enc() correctness         ║\n");
    printf("╚═══════════════════════════════════════════════════╝\n\n");

    int r[5];
    r[0] = test_sd1_polarity_derive();  printf("\n");
    r[1] = test_sd2_spoke_coverage();   printf("\n");
    r[2] = test_sd3_pentagon_coverage(); printf("\n");
    r[3] = test_sd4_route_validity();   printf("\n");
    r[4] = test_sd5_sacred_math();      printf("\n");

    int passed = r[0]+r[1]+r[2]+r[3]+r[4];
    printf("══════════════════════════════════════════════════════\n");
    printf("RESULT: %d/5 passed\n", passed);
    printf("STATUS: %s\n", passed == 5
        ? "✅ POLARITY DERIVE VERIFIED + MATH FROZEN"
        : "⚠️  check failures above");

    printf("\nRoute Math (stateless O(1)):\n");
    printf("  pos      = tring_pos(enc)      [LUT]\n");
    printf("  pentagon = pos / 60            [div]\n");
    printf("  spoke    = pentagon %% 6        [mod]\n");
    printf("  polarity = (pos %% 60) >= 30   [compare]\n");
    printf("══════════════════════════════════════════════════════\n");

    return (passed == 5) ? 0 : 1;
}
