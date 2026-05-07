/*
 * test_tgw_lc_bridge.c
 * gcc -O2 -I/tmp/lc_gcfs/src -o test_bridge test_tgw_lc_bridge.c && ./test_bridge
 *
 * Invariants verified:
 *   T1: enc → pos → spoke/polarity (P0 math preserved)
 *   T2: enc → LCNodeHdr → route (round-trip match)
 *   T3: 720 full-cycle distribution (50/50, 6 spokes ×120)
 *   T4: gate enc_addr/enc_value → GROUND when polarity differs
 *   T5: LCNodeHdr is active (non-ghost) for all valid enc
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>

#include "tgw_lc_bridge.h"

static int pass = 0, fail = 0;
#define CHK(cond, msg) do { \
    if (cond) { printf("  ✓ %s\n", msg); pass++; } \
    else      { printf("  ✗ FAIL: %s\n", msg); fail++; } \
} while(0)

int main(void) {
    printf("╔══════════════════════════════════════════════╗\n");
    printf("║  tgw_lc_bridge.h — Round-Trip Test          ║\n");
    printf("╚══════════════════════════════════════════════╝\n\n");

    /* ── T1: core math vs P0 verified formula ── */
    printf("▶ T1: core math (P0 sacred numbers)\n");
    /* pos=0   → pentagon=0, spoke=0, polarity=0 (ROUTE) */
    CHK(tgwlc_pos_to_spoke(0)    == 0, "pos=0   → spoke=0");
    CHK(tgwlc_pos_to_polarity(0) == 0, "pos=0   → ROUTE");
    /* pos=30  → pentagon=0, spoke=0, polarity=1 (GROUND) */
    CHK(tgwlc_pos_to_polarity(30) == 1, "pos=30  → GROUND");
    /* pos=60  → pentagon=1, spoke=1 */
    CHK(tgwlc_pos_to_spoke(60)   == 1, "pos=60  → spoke=1");
    /* pos=360 → pentagon=6, spoke=0 (wraps) */
    CHK(tgwlc_pos_to_spoke(360)  == 0, "pos=360 → spoke=0 (wrap)");
    /* pos=719 → last position */
    CHK(tgwlc_pos_to_spoke(719)  == 5, "pos=719 → spoke=5");

    /* ── T2: round-trip enc → hdr → route ── */
    printf("\n▶ T2: round-trip enc → LCNodeHdr → TGWLCRoute\n");
    uint32_t test_encs[] = {0, 1, 29, 30, 60, 359, 360, 719, 720, 1000, 0xDEADBEEF};
    int n = sizeof(test_encs)/sizeof(test_encs[0]);
    int rt_ok = 1;
    for (int i = 0; i < n; i++) {
        uint32_t enc   = test_encs[i];
        uint16_t pos   = tgwlc_enc_to_pos(enc);
        uint8_t  spk_direct = tgwlc_pos_to_spoke(pos);
        uint8_t  pol_direct = tgwlc_pos_to_polarity(pos);

        TGWLCRoute r = tgwlc_route(enc);

        if (r.spoke != spk_direct || r.polarity != pol_direct) {
            printf("  ✗ enc=%-12u pos=%3u spoke_direct=%u bridge=%u "
                   "pol_direct=%u bridge=%u\n",
                   enc, pos, spk_direct, r.spoke, pol_direct, r.polarity);
            rt_ok = 0;
        }
    }
    CHK(rt_ok, "all test_encs round-trip match");

    /* ── T3: 720-cycle full distribution ── */
    printf("\n▶ T3: 720-position distribution (P1 invariants)\n");
    uint32_t spoke_cnt[6] = {0};
    uint32_t route_cnt = 0, ground_cnt = 0;

    for (uint32_t enc = 0; enc < 720; enc++) {
        TGWLCRoute r = tgwlc_route(enc);
        spoke_cnt[r.spoke]++;
        if (r.polarity == 0) route_cnt++;
        else                 ground_cnt++;
    }

    CHK(route_cnt  == 360, "360 ROUTE (50%)");
    CHK(ground_cnt == 360, "360 GROUND (50%)");

    int uniform = 1;
    for (int s = 0; s < 6; s++)
        if (spoke_cnt[s] != 120) { uniform = 0; break; }
    CHK(uniform, "6 spokes × 120 packets each");

    /* ── T4: gate polarity check → GROUND ── */
    printf("\n▶ T4: gate — polarity mismatch → GROUND\n");
    /* enc=0 (pos=0, polarity=0=ROUTE) vs enc=30 (pos=30, polarity=1=GROUND) */
    BridgeGate g = tgwlc_gate(0, 30);
    CHK(g == BRIDGE_GATE_GROUND, "enc=0 vs enc=30 → GROUND (polarity differ)");

    /* enc=0 vs enc=1: both ROUTE (same polarity) → ROUTE or WARP */
    g = tgwlc_gate(0, 1);
    CHK(g == BRIDGE_GATE_ROUTE || g == BRIDGE_GATE_WARP,
        "enc=0 vs enc=1 (same polarity) → ROUTE or WARP");

    /* ── T5: all valid enc produce active (non-ghost) node ── */
    printf("\n▶ T5: active node (mag>0) for all enc 0..719\n");
    int all_active = 1;
    for (uint32_t enc = 0; enc < 720; enc++) {
        LCNodeHdr hdr = tgwlc_enc_to_node_hdr(enc);
        if (lch_is_ghost(hdr)) { all_active = 0; break; }
    }
    CHK(all_active, "no ghost nodes in 0..719");

    /* ── summary ── */
    printf("\n══════════════════════════════════════════════\n");
    printf("  PASS: %d   FAIL: %d\n", pass, fail);
    if (!fail) printf("✅ All PASS — bridge invariants hold\n");
    else       printf("❌ %d FAIL\n", fail);
    return fail ? 1 : 0;
}
